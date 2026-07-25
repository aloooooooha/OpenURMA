// SPDX-License-Identifier: GPL-2.0
#include <linux/err.h>
#include <linux/capability.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include "openurma_vdev.h"

static int vdev_atomic64_show(struct seq_file *seq, void *unused)
{
	atomic64_t *counter = seq->private;

	(void)unused;
	seq_printf(seq, "%lld\n", atomic64_read(counter));
	return 0;
}

static int vdev_atomic64_open(struct inode *inode, struct file *file)
{
	return single_open(file, vdev_atomic64_show, inode->i_private);
}

static const struct file_operations vdev_atomic64_fops = {
	.owner = THIS_MODULE,
	.open = vdev_atomic64_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void vdev_debugfs_create_atomic64(const char *name,
	struct dentry *parent, atomic64_t *counter)
{
	debugfs_create_file(name, 0444, parent, counter, &vdev_atomic64_fops);
}

static ssize_t vdev_fault_arm_write(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct openurma_vdev *vdev = file->private_data;
	char buf[32];
	unsigned int phase;
	u64 generation;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	ret = simple_write_to_buffer(buf, sizeof(buf) - 1, ppos, user_buf, count);
	if (ret < 0)
		return ret;
	buf[ret] = '\0';
	ret = kstrtouint(buf, 0, &phase);
	if (ret)
		return ret;
	if (phase > OPENURMA_VDEV_FAULT_BEFORE_CQ_PUBLISH)
		return -EINVAL;
	/* Disarm first so an old generation cannot become visible as the new
	 * test's handshake.  Waking here also guarantees a safe unwind. */
	atomic_set(&vdev->fault_phase, OPENURMA_VDEV_FAULT_OFF);
	wake_up_all(&vdev->fault_wait);
	generation = atomic64_inc_return(&vdev->fault_generation);
	atomic64_set(&vdev->fault_claimed_generation, 0);
	atomic64_set(&vdev->fault_reached_generation, 0);
	atomic64_set(&vdev->fault_release_generation, 0);
	smp_wmb();
	if (phase)
		atomic_set(&vdev->fault_phase, phase);
	pr_debug("openurma_vdev: fault phase=%u generation=%llu\n", phase,
		 (unsigned long long)generation);
	return count;
}

static ssize_t vdev_fault_release_write(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct openurma_vdev *vdev = file->private_data;
	char buf[32];
	u64 generation;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	ret = simple_write_to_buffer(buf, sizeof(buf) - 1, ppos, user_buf, count);
	if (ret < 0)
		return ret;
	buf[ret] = '\0';
	ret = kstrtou64(buf, 0, &generation);
	if (ret)
		return ret;
	if (generation != atomic64_read(&vdev->fault_generation))
		return -ESTALE;
	atomic64_set(&vdev->fault_release_generation, generation);
	/* A fault arm is one-shot.  The waiter checks release before treating
	 * this phase change as cancellation, while future WRs see OFF. */
	atomic_set(&vdev->fault_phase, OPENURMA_VDEV_FAULT_OFF);
	wake_up_all(&vdev->fault_wait);
	return count;
}

static int vdev_fault_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static const struct file_operations vdev_fault_arm_fops = {
	.owner = THIS_MODULE,
	.open = vdev_fault_open,
	.write = vdev_fault_arm_write,
	.llseek = no_llseek,
};

static const struct file_operations vdev_fault_release_fops = {
	.owner = THIS_MODULE,
	.open = vdev_fault_open,
	.write = vdev_fault_release_write,
	.llseek = no_llseek,
};

static int vdev_snapshot_show(struct seq_file *seq, void *unused)
{
	struct openurma_vdev *vdev = seq->private;
	u64 generation = atomic64_read(&vdev->statistics_generation);

	(void)unused;
	seq_printf(seq, "snapshot_generation=%llu\n",
		   (unsigned long long)generation);
	seq_printf(seq, "devices=%u contexts=%d objects=%d mr=%d pinned_pages=%d\n",
		   READ_ONCE(vdev->devices), atomic_read(&vdev->contexts),
		   atomic_read(&vdev->objects), atomic_read(&vdev->mr),
		   atomic_read(&vdev->pinned_pages));
	seq_printf(seq, "mr_pages_node0=%lld mr_pages_node1=%lld mr_pages_other=%lld\n",
		   (long long)atomic64_read(&vdev->mr_pages_node0),
		   (long long)atomic64_read(&vdev->mr_pages_node1),
		   (long long)atomic64_read(&vdev->mr_pages_other));
	seq_printf(seq, "sq=%d cq=%d inflight=%d submitted=%lld consumed=%lld\n",
		   atomic_read(&vdev->sq), atomic_read(&vdev->cq),
		   atomic_read(&vdev->inflight),
		   (long long)atomic64_read(&vdev->submitted_total),
		   (long long)atomic64_read(&vdev->consumed_total));
	seq_printf(seq, "doorbells=%lld max_inflight=%d max_sq_outstanding=%d\n",
		   (long long)atomic64_read(&vdev->doorbells),
		   atomic_read(&vdev->max_inflight),
		   atomic_read(&vdev->max_sq_outstanding));
	seq_printf(seq, "model_mode=%s model_qd=%u model_node=%d model_cpu=%d fixed_latency_ns=%llu bandwidth_bps=%llu completion_overhead_ns=%llu\n",
		   vdev->model_mode_name, vdev->model_queue_depth,
		   vdev->model_numa_node, vdev->model_worker_cpu,
		   (unsigned long long)vdev->model_fixed_latency_ns,
		   (unsigned long long)vdev->model_bandwidth_bps,
		   (unsigned long long)vdev->model_completion_overhead_ns);
	seq_printf(seq, "model_operations=%lld model_bytes=%lld model_inflight=%d model_max_inflight=%d model_base_service_ns=%lld model_planned_delay_ns=%lld model_actual_wait_ns=%lld model_observed_service_ns=%lld model_wakeup_late_ns=%lld model_copy_ns=%lld last_worker_cpu=%d last_worker_node=%d\n",
		   (long long)atomic64_read(&vdev->model_operations),
		   (long long)atomic64_read(&vdev->model_bytes),
		   atomic_read(&vdev->model_inflight),
		   atomic_read(&vdev->model_max_inflight),
		   (long long)atomic64_read(&vdev->model_base_service_ns),
		   (long long)atomic64_read(&vdev->model_planned_delay_ns),
		   (long long)atomic64_read(&vdev->model_actual_wait_ns),
		   (long long)atomic64_read(&vdev->model_observed_service_ns),
		   (long long)atomic64_read(&vdev->model_wakeup_late_ns),
		   (long long)atomic64_read(&vdev->model_copy_ns),
		   atomic_read(&vdev->model_last_worker_cpu),
		   atomic_read(&vdev->model_last_worker_node));
	seq_printf(seq, "success=%d failed=%d cancelled=%d bytes=%lld read_bytes=%lld write_bytes=%lld\n",
		   atomic_read(&vdev->completed_success),
		   atomic_read(&vdev->completed_failed),
		   atomic_read(&vdev->completed_cancelled),
		   (long long)atomic64_read(&vdev->completed_bytes),
		   (long long)atomic64_read(&vdev->read_bytes),
		   (long long)atomic64_read(&vdev->write_bytes));
	seq_printf(seq, "fault_phase=%d fault_generation=%lld fault_claimed_generation=%lld fault_reached_generation=%lld fault_release_generation=%lld fault_reached_total=%lld fault_released_total=%lld\n",
		   atomic_read(&vdev->fault_phase),
		   (long long)atomic64_read(&vdev->fault_generation),
		   (long long)atomic64_read(&vdev->fault_claimed_generation),
		   (long long)atomic64_read(&vdev->fault_reached_generation),
		   (long long)atomic64_read(&vdev->fault_release_generation),
		   (long long)atomic64_read(&vdev->fault_reached_total),
		   (long long)atomic64_read(&vdev->fault_released_total));
	seq_printf(seq, "mr_operation_refs=%lld mr_closing_total=%lld mr_destroyed_total=%lld\n",
		   (long long)atomic64_read(&vdev->mr_operation_refs),
		   (long long)atomic64_read(&vdev->mr_closing_total),
		   (long long)atomic64_read(&vdev->mr_destroyed_total));
	return 0;
}

static int vdev_snapshot_open(struct inode *inode, struct file *file)
{
	return single_open(file, vdev_snapshot_show, inode->i_private);
}

static const struct file_operations vdev_snapshot_fops = {
	.owner = THIS_MODULE,
	.open = vdev_snapshot_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

int openurma_vdev_fault_wait(struct openurma_vdev *vdev,
	enum openurma_vdev_fault_phase phase)
{
	u64 generation;

	if (atomic_read(&vdev->fault_phase) != phase)
		return 0;
	generation = atomic64_read(&vdev->fault_generation);
	if (atomic64_cmpxchg(&vdev->fault_claimed_generation, 0, generation) != 0)
		return 0;
	atomic64_set(&vdev->fault_reached_generation, generation);
	atomic64_inc(&vdev->fault_reached_total);
	wake_up_all(&vdev->fault_wait);
	wait_event(vdev->fault_wait,
		kthread_should_stop() ||
		atomic_read(&vdev->fault_phase) != phase ||
		atomic64_read(&vdev->fault_release_generation) == generation);
	if (kthread_should_stop())
		return -ESHUTDOWN;
	if (atomic64_read(&vdev->fault_release_generation) == generation) {
		atomic64_inc(&vdev->fault_released_total);
		return 0;
	}
	return -ECANCELED;
}

int openurma_vdev_debugfs_init(struct openurma_vdev *vdev)
{
	vdev->debugfs_root = debugfs_create_dir("openurma_vdev", NULL);
	if (IS_ERR(vdev->debugfs_root))
		return PTR_ERR(vdev->debugfs_root);
	if (!vdev->debugfs_root)
		return -ENODEV;

	debugfs_create_u32("abi_version", 0444, vdev->debugfs_root,
			   &vdev->abi_version);
	debugfs_create_u32("devices", 0444, vdev->debugfs_root, &vdev->devices);
	debugfs_create_atomic_t("contexts", 0444, vdev->debugfs_root, &vdev->contexts);
	debugfs_create_atomic_t("objects", 0444, vdev->debugfs_root, &vdev->objects);
	debugfs_create_atomic_t("token_ids", 0444, vdev->debugfs_root, &vdev->token_ids);
	debugfs_create_atomic_t("logical_segments", 0444, vdev->debugfs_root, &vdev->logical_segments);
	debugfs_create_atomic_t("imported_segments", 0444, vdev->debugfs_root, &vdev->imported_segments);
	debugfs_create_atomic_t("jfcs", 0444, vdev->debugfs_root, &vdev->jfcs);
	debugfs_create_atomic_t("jfs", 0444, vdev->debugfs_root, &vdev->jfs);
	debugfs_create_atomic_t("jfrs", 0444, vdev->debugfs_root, &vdev->jfrs);
	debugfs_create_atomic_t("jetties", 0444, vdev->debugfs_root, &vdev->jetties);
	debugfs_create_atomic_t("imported_jetties", 0444, vdev->debugfs_root, &vdev->imported_jetties);
	debugfs_create_atomic_t("binds", 0444, vdev->debugfs_root, &vdev->binds);
	debugfs_create_atomic_t("vtpns", 0444, vdev->debugfs_root, &vdev->vtpns);
	debugfs_create_atomic_t("mr", 0444, vdev->debugfs_root, &vdev->mr);
	debugfs_create_atomic_t("pinned_pages", 0444, vdev->debugfs_root,
			   &vdev->pinned_pages);
	debugfs_create_atomic_t("sq", 0444, vdev->debugfs_root, &vdev->sq);
	debugfs_create_atomic_t("cq", 0444, vdev->debugfs_root, &vdev->cq);
	debugfs_create_atomic_t("inflight", 0444, vdev->debugfs_root, &vdev->inflight);
	debugfs_create_atomic_t("errors", 0444, vdev->debugfs_root, &vdev->errors);
	debugfs_create_atomic_t("sq_consumed_total", 0444, vdev->debugfs_root,
			   &vdev->sq_consumed_total);
	debugfs_create_atomic_t("cq_produced_total", 0444, vdev->debugfs_root,
			   &vdev->cq_produced_total);
	debugfs_create_atomic_t("cq_full_total", 0444, vdev->debugfs_root,
			   &vdev->cq_full_total);
	debugfs_create_atomic_t("worker_wakeups", 0444, vdev->debugfs_root,
			   &vdev->worker_wakeups);
	debugfs_create_atomic_t("worker_starts", 0444, vdev->debugfs_root,
			   &vdev->worker_starts);
	debugfs_create_atomic_t("worker_stops", 0444, vdev->debugfs_root,
			   &vdev->worker_stops);
	vdev_debugfs_create_atomic64("doorbells", vdev->debugfs_root,
		&vdev->doorbells);
	debugfs_create_atomic_t("max_inflight", 0444, vdev->debugfs_root,
		&vdev->max_inflight);
	debugfs_create_atomic_t("max_sq_outstanding", 0444,
		vdev->debugfs_root, &vdev->max_sq_outstanding);
	debugfs_create_atomic_t("completed_success", 0444, vdev->debugfs_root,
			   &vdev->completed_success);
	debugfs_create_atomic_t("completed_failed", 0444, vdev->debugfs_root,
			   &vdev->completed_failed);
	debugfs_create_atomic_t("completed_cancelled", 0444, vdev->debugfs_root,
			   &vdev->completed_cancelled);
	vdev_debugfs_create_atomic64("submitted_total", vdev->debugfs_root,
		&vdev->submitted_total);
	vdev_debugfs_create_atomic64("consumed_total", vdev->debugfs_root,
		&vdev->consumed_total);
	vdev_debugfs_create_atomic64("completed_bytes", vdev->debugfs_root,
		&vdev->completed_bytes);
	vdev_debugfs_create_atomic64("read_bytes", vdev->debugfs_root,
		&vdev->read_bytes);
	vdev_debugfs_create_atomic64("write_bytes", vdev->debugfs_root,
		&vdev->write_bytes);
	vdev_debugfs_create_atomic64("mr_lookup_errors", vdev->debugfs_root,
		&vdev->mr_lookup_errors);
	vdev_debugfs_create_atomic64("mr_range_errors", vdev->debugfs_root,
		&vdev->mr_range_errors);
	vdev_debugfs_create_atomic64("mr_access_errors", vdev->debugfs_root,
		&vdev->mr_access_errors);
	vdev_debugfs_create_atomic64("copy_errors", vdev->debugfs_root,
		&vdev->copy_errors);
	vdev_debugfs_create_atomic64("mr_operation_refs", vdev->debugfs_root,
		&vdev->mr_operation_refs);
	vdev_debugfs_create_atomic64("mr_closing_total", vdev->debugfs_root,
		&vdev->mr_closing_total);
	vdev_debugfs_create_atomic64("mr_destroyed_total", vdev->debugfs_root,
		&vdev->mr_destroyed_total);
	vdev_debugfs_create_atomic64("mr_pages_node0", vdev->debugfs_root,
		&vdev->mr_pages_node0);
	vdev_debugfs_create_atomic64("mr_pages_node1", vdev->debugfs_root,
		&vdev->mr_pages_node1);
	vdev_debugfs_create_atomic64("mr_pages_other", vdev->debugfs_root,
		&vdev->mr_pages_other);
	vdev_debugfs_create_atomic64("model_operations", vdev->debugfs_root,
		&vdev->model_operations);
	vdev_debugfs_create_atomic64("model_bytes", vdev->debugfs_root,
		&vdev->model_bytes);
	vdev_debugfs_create_atomic64("model_base_service_ns",
		vdev->debugfs_root, &vdev->model_base_service_ns);
	debugfs_create_atomic_t("model_inflight", 0444, vdev->debugfs_root,
		&vdev->model_inflight);
	debugfs_create_atomic_t("model_max_inflight", 0444, vdev->debugfs_root,
		&vdev->model_max_inflight);
	vdev_debugfs_create_atomic64("model_planned_delay_ns",
		vdev->debugfs_root, &vdev->model_planned_delay_ns);
	vdev_debugfs_create_atomic64("model_actual_wait_ns",
		vdev->debugfs_root, &vdev->model_actual_wait_ns);
	vdev_debugfs_create_atomic64("model_observed_service_ns",
		vdev->debugfs_root, &vdev->model_observed_service_ns);
	vdev_debugfs_create_atomic64("model_wakeup_late_ns",
		vdev->debugfs_root, &vdev->model_wakeup_late_ns);
	vdev_debugfs_create_atomic64("model_copy_ns", vdev->debugfs_root,
		&vdev->model_copy_ns);
	debugfs_create_atomic_t("model_last_worker_cpu", 0444,
		vdev->debugfs_root, &vdev->model_last_worker_cpu);
	debugfs_create_atomic_t("model_last_worker_node", 0444,
		vdev->debugfs_root, &vdev->model_last_worker_node);
	debugfs_create_atomic_t("fault_pause_after_ref", 0600,
		vdev->debugfs_root, &vdev->fault_pause_after_ref);
	debugfs_create_atomic_t("fault_copy_error_once", 0600,
		vdev->debugfs_root, &vdev->fault_copy_error_once);
	vdev_debugfs_create_atomic64("fault_generation", vdev->debugfs_root,
		&vdev->fault_generation);
	vdev_debugfs_create_atomic64("fault_reached_generation", vdev->debugfs_root,
		&vdev->fault_reached_generation);
	vdev_debugfs_create_atomic64("fault_release_generation", vdev->debugfs_root,
		&vdev->fault_release_generation);
	vdev_debugfs_create_atomic64("fault_reached_total", vdev->debugfs_root,
		&vdev->fault_reached_total);
	vdev_debugfs_create_atomic64("fault_released_total", vdev->debugfs_root,
		&vdev->fault_released_total);
	debugfs_create_file("fault_arm", 0600, vdev->debugfs_root, vdev,
		&vdev_fault_arm_fops);
	debugfs_create_file("fault_release", 0600, vdev->debugfs_root, vdev,
		&vdev_fault_release_fops);
	debugfs_create_file("snapshot", 0400, vdev->debugfs_root, vdev,
		&vdev_snapshot_fops);
	return 0;
}

void openurma_vdev_debugfs_fini(struct openurma_vdev *vdev)
{
	debugfs_remove_recursive(vdev->debugfs_root);
	vdev->debugfs_root = NULL;
}
