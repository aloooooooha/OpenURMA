// SPDX-License-Identifier: GPL-2.0
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/nodemask.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/topology.h>

#include "openurma_vdev.h"

static char *model_mode = "functional";
module_param(model_mode, charp, 0444);
MODULE_PARM_DESC(model_mode, "functional or timed-numa");

static unsigned long long model_fixed_latency_ns;
module_param(model_fixed_latency_ns, ullong, 0444);
MODULE_PARM_DESC(model_fixed_latency_ns, "minimum fixed operation latency in ns");

static unsigned long long model_bandwidth_bps;
module_param(model_bandwidth_bps, ullong, 0444);
MODULE_PARM_DESC(model_bandwidth_bps, "per-direction serialization bandwidth in bits/s");

static unsigned long long model_completion_overhead_ns;
module_param(model_completion_overhead_ns, ullong, 0444);
MODULE_PARM_DESC(model_completion_overhead_ns, "completion publication overhead in ns");

static unsigned int model_queue_depth = OPENURMA_VDEV_RING_DEPTH;
module_param(model_queue_depth, uint, 0444);
MODULE_PARM_DESC(model_queue_depth, "effective outstanding SQ depth (1..64)");

static int model_numa_node = -1;
module_param(model_numa_node, int, 0444);
MODULE_PARM_DESC(model_numa_node, "NUMA node for ring pages and worker affinity");

static int model_worker_cpu = -1;
module_param(model_worker_cpu, int, 0444);
MODULE_PARM_DESC(model_worker_cpu, "explicit worker CPU; overrides model_numa_node affinity");

static u64 vdev_serialization_ns(u64 bytes, u64 bandwidth_bps)
{
	u64 numerator;
	u64 quotient;

	if (!bytes || !bandwidth_bps)
		return 0;
	if (check_mul_overflow(bytes, 8000000000ULL, &numerator))
		return U64_MAX;
	quotient = div64_u64(numerator, bandwidth_bps);
	if (numerator % bandwidth_bps)
		quotient++;
	return quotient;
}

static int vdev_model_direction(const struct openurma_vdev_sqe *sqe)
{
	u32 opcode = READ_ONCE(sqe->opcode);
	u32 source_eid;

	if (opcode == OPENURMA_VDEV_SQE_READ)
		source_eid = READ_ONCE(sqe->remote_eid);
	else
		source_eid = READ_ONCE(sqe->local_eid);
	return source_eid & 1U;
}

static void vdev_update_max(atomic_t *maximum, int value)
{
	int old = atomic_read(maximum);

	while (value > old) {
		int observed = atomic_cmpxchg(maximum, old, value);

		if (observed == old)
			break;
		old = observed;
	}
}

int openurma_vdev_model_init(struct openurma_vdev *vdev)
{
	if (!strcmp(model_mode, "functional")) {
		vdev->model_mode = OPENURMA_VDEV_MODEL_FUNCTIONAL;
	} else if (!strcmp(model_mode, "timed-numa")) {
		vdev->model_mode = OPENURMA_VDEV_MODEL_TIMED_NUMA;
	} else {
		pr_err("openurma_vdev: invalid model_mode=%s\n", model_mode);
		return -EINVAL;
	}
	if (!model_queue_depth ||
	    model_queue_depth > OPENURMA_VDEV_RING_DEPTH)
		return -EINVAL;
	if (model_numa_node >= 0 && !node_online(model_numa_node))
		return -ENODEV;
	if (model_worker_cpu >= 0 && !cpu_online(model_worker_cpu))
		return -ENODEV;
	if (vdev->model_mode == OPENURMA_VDEV_MODEL_TIMED_NUMA &&
	    !model_bandwidth_bps)
		return -EINVAL;
	if (model_fixed_latency_ns > 60000000000ULL ||
	    model_completion_overhead_ns > 60000000000ULL)
		return -ERANGE;

	strscpy(vdev->model_mode_name, model_mode,
		sizeof(vdev->model_mode_name));
	vdev->model_fixed_latency_ns = model_fixed_latency_ns;
	vdev->model_bandwidth_bps = model_bandwidth_bps;
	vdev->model_completion_overhead_ns = model_completion_overhead_ns;
	vdev->model_queue_depth = model_queue_depth;
	vdev->model_numa_node = model_numa_node;
	vdev->model_worker_cpu = model_worker_cpu;
	spin_lock_init(&vdev->model_timeline_lock);
	vdev->model_direction_ready_ns[0] = 0;
	vdev->model_direction_ready_ns[1] = 0;
	atomic_set(&vdev->model_inflight, 0);
	atomic_set(&vdev->model_max_inflight, 0);
	atomic64_set(&vdev->model_operations, 0);
	atomic64_set(&vdev->model_bytes, 0);
	atomic64_set(&vdev->model_base_service_ns, 0);
	atomic64_set(&vdev->model_planned_delay_ns, 0);
	atomic64_set(&vdev->model_actual_wait_ns, 0);
	atomic64_set(&vdev->model_observed_service_ns, 0);
	atomic64_set(&vdev->model_wakeup_late_ns, 0);
	atomic64_set(&vdev->model_copy_ns, 0);
	atomic_set(&vdev->model_last_worker_cpu, -1);
	atomic_set(&vdev->model_last_worker_node, -1);
	return 0;
}

void openurma_vdev_model_fini(struct openurma_vdev *vdev)
{
	if (atomic_read(&vdev->model_inflight))
		pr_warn("openurma_vdev: model shutdown with inflight=%d\n",
			atomic_read(&vdev->model_inflight));
}

void openurma_vdev_model_bind_worker(struct openurma_vdev *vdev)
{
	const struct cpumask *mask = NULL;
	int ret = 0;

	if (vdev->model_worker_cpu >= 0)
		mask = cpumask_of(vdev->model_worker_cpu);
	else if (vdev->model_numa_node >= 0)
		mask = cpumask_of_node(vdev->model_numa_node);
	if (mask && !cpumask_empty(mask))
		ret = set_cpus_allowed_ptr(current, mask);
	if (ret)
		pr_warn("openurma_vdev: worker affinity failed: %d\n", ret);
	atomic_set(&vdev->model_last_worker_cpu, task_cpu(current));
	atomic_set(&vdev->model_last_worker_node,
		cpu_to_node(task_cpu(current)));
}

unsigned long openurma_vdev_model_alloc_pages(struct openurma_vdev *vdev,
	unsigned int order)
{
	struct page *page;

	if (vdev->model_numa_node < 0)
		return __get_free_pages(GFP_KERNEL | __GFP_ZERO, order);
	page = alloc_pages_node(vdev->model_numa_node,
		GFP_KERNEL | __GFP_ZERO, order);
	return page ? (unsigned long)page_address(page) : 0;
}

int openurma_vdev_model_wait(struct openurma_vdev *vdev,
	const struct openurma_vdev_sqe *sqe)
{
	unsigned long flags;
	u64 now;
	u64 start;
	u64 serialization;
	u64 deadline;
	u64 ready;
	u64 planned;
	u64 actual;
	u64 base_service;
	u64 length;
	u64 submit;
	u64 wait_start;
	u32 opcode;
	int direction;
	int inflight;

	if (vdev->model_mode == OPENURMA_VDEV_MODEL_FUNCTIONAL)
		return 0;
	length = READ_ONCE(sqe->length);
	serialization = vdev_serialization_ns(length,
		vdev->model_bandwidth_bps);
	if (serialization == U64_MAX)
		return -EOVERFLOW;
	direction = vdev_model_direction(sqe);
	now = ktime_get_ns();
	wait_start = now;
	opcode = READ_ONCE(sqe->opcode);
	submit = READ_ONCE(sqe->submit_timestamp);
	if (opcode == OPENURMA_VDEV_SQE_CAS ||
	    opcode == OPENURMA_VDEV_SQE_FADD ||
	    !submit || submit > now || now - submit > 60000000000ULL)
		submit = now;
	spin_lock_irqsave(&vdev->model_timeline_lock, flags);
	ready = vdev->model_direction_ready_ns[direction];
	start = max(submit, ready);
	if (check_add_overflow(start, serialization, &ready) ||
	    check_add_overflow(ready, vdev->model_fixed_latency_ns,
		&deadline) ||
	    check_add_overflow(deadline,
		vdev->model_completion_overhead_ns, &deadline)) {
		spin_unlock_irqrestore(&vdev->model_timeline_lock, flags);
		return -EOVERFLOW;
	}
	vdev->model_direction_ready_ns[direction] = ready;
	spin_unlock_irqrestore(&vdev->model_timeline_lock, flags);

	planned = deadline > submit ? deadline - submit : 0;
	base_service = serialization + vdev->model_fixed_latency_ns +
		vdev->model_completion_overhead_ns;
	inflight = atomic_inc_return(&vdev->model_inflight);
	vdev_update_max(&vdev->model_max_inflight, inflight);
	atomic64_inc(&vdev->model_operations);
	atomic64_add(length, &vdev->model_bytes);
	atomic64_add(base_service, &vdev->model_base_service_ns);
	atomic64_add(planned, &vdev->model_planned_delay_ns);

	for (;;) {
		u64 remaining;
		u64 sleep_us;

		if (kthread_should_stop()) {
			atomic_dec(&vdev->model_inflight);
			return -ESHUTDOWN;
		}
		now = ktime_get_ns();
		if (now >= deadline)
			break;
		remaining = deadline - now;
		if (remaining <= 20000) {
			cpu_relax();
			continue;
		}
		sleep_us = min_t(u64, div_u64(remaining, 1000), 1000000);
		usleep_range(sleep_us, sleep_us + min_t(u64, sleep_us / 10 + 1, 50));
	}
	actual = ktime_get_ns();
	atomic64_add(actual > wait_start ? actual - wait_start : planned,
		&vdev->model_actual_wait_ns);
	atomic64_add(actual > submit ? actual - submit : planned,
		&vdev->model_observed_service_ns);
	if (actual > deadline)
		atomic64_add(actual - deadline, &vdev->model_wakeup_late_ns);
	atomic_dec(&vdev->model_inflight);
	return 0;
}
