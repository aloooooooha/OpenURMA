// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>

#include "openurma_vdev.h"

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

void openurma_vdev_ring_doorbell(struct vdev_ucontext *ctx)
{
	struct openurma_vdev_control_page *ctrl =
		(void *)ctx->control_page;

	atomic64_inc(&ctx->vdev->doorbells);
	vdev_update_max(&ctx->vdev->max_sq_outstanding,
		(int)min_t(u64, smp_load_acquire(&ctrl->sq_tail) -
			smp_load_acquire(&ctrl->sq_head),
			OPENURMA_VDEV_RING_DEPTH));
	wake_up_interruptible(&ctx->sq_wait);
}

static int vdev_ring_worker(void *data)
{
	struct vdev_ucontext *ctx = data;
	struct openurma_vdev_control_page *ctrl =
		(void *)ctx->control_page;
	struct openurma_vdev_sqe *sq = (void *)ctx->sq_page;
	struct openurma_vdev_cqe *cq = (void *)ctx->cq_page;

	openurma_vdev_model_bind_worker(ctx->vdev);
	WRITE_ONCE(ctrl->worker_starts, READ_ONCE(ctrl->worker_starts) + 1);
	atomic_inc(&ctx->vdev->worker_starts);

	while (!kthread_should_stop()) {
		u64 head = READ_ONCE(ctrl->sq_head);
		u64 tail = smp_load_acquire(&ctrl->sq_tail);

		if (head != tail) {
			WRITE_ONCE(ctrl->worker_wakeups,
				READ_ONCE(ctrl->worker_wakeups) + 1);
			atomic_inc(&ctx->vdev->worker_wakeups);
		}

		while (head != tail) {
			struct openurma_vdev_sqe *src;
			struct openurma_vdev_cqe result;
			u64 expected_generation = READ_ONCE(ctrl->generation);
			bool reported_full = false;

			src = &sq[head & (OPENURMA_VDEV_RING_DEPTH - 1)];
			memset(&result, 0, sizeof(result));
			vdev_update_max(&ctx->vdev->max_inflight,
				atomic_inc_return(&ctx->vdev->inflight));
			atomic64_inc(&ctx->vdev->submitted_total);
			result.wr_id = READ_ONCE(src->wr_id);
			result.sequence = head;
			result.generation = expected_generation;
			result.user_ctx = READ_ONCE(src->user_ctx);
			result.jfc_id = READ_ONCE(src->jfc_id);
			result.local_jfs_id = READ_ONCE(src->local_jfs_id);
			if (READ_ONCE(src->sequence) != head ||
			    READ_ONCE(src->generation) != expected_generation)
				result.status = OPENURMA_VDEV_CQE_BAD_GENERATION;
			else if (READ_ONCE(src->opcode) == OPENURMA_VDEV_SQE_NOP)
				result.status = OPENURMA_VDEV_CQE_SUCCESS;
			else
				result.status = openurma_vdev_execute_rw(ctx, src, &result);
			if (result.status) {
				result.error_detail = result.status;
				WRITE_ONCE(ctrl->errors, READ_ONCE(ctrl->errors) + 1);
				atomic_inc(&ctx->vdev->completed_failed);
			}
			else
				atomic_inc(&ctx->vdev->completed_success);
			/* Test-only deterministic window after copy/accounting but before
			 * the release-published CQE becomes visible to user space. */
			(void)openurma_vdev_fault_wait(ctx->vdev,
				OPENURMA_VDEV_FAULT_BEFORE_CQ_PUBLISH);
			result.completion_timestamp = ktime_get_ns();
			for (;;) {
				u64 cq_head;
				u64 cq_tail;
				struct openurma_vdev_cqe *dst;

				mutex_lock(&ctx->vdev->cq_publish_lock);
				cq_head = smp_load_acquire(&ctrl->cq_head);
				cq_tail = READ_ONCE(ctrl->cq_tail);
				if (cq_tail - cq_head < OPENURMA_VDEV_RING_DEPTH) {
					dst = &cq[cq_tail &
						(OPENURMA_VDEV_RING_DEPTH - 1)];
					*dst = result;
					smp_store_release(&ctrl->cq_tail, cq_tail + 1);
					WRITE_ONCE(ctrl->cq_produced,
						READ_ONCE(ctrl->cq_produced) + 1);
					atomic_inc(&ctx->vdev->cq_produced_total);
					mutex_unlock(&ctx->vdev->cq_publish_lock);
					break;
				}
				mutex_unlock(&ctx->vdev->cq_publish_lock);
				if (!reported_full) {
					WRITE_ONCE(ctrl->cq_full,
						READ_ONCE(ctrl->cq_full) + 1);
					atomic_inc(&ctx->vdev->cq_full_total);
					reported_full = true;
				}
				if (kthread_should_stop())
					break;
				usleep_range(500, 1000);
			}
			if (kthread_should_stop()) {
				atomic_dec(&ctx->vdev->inflight);
				break;
			}
			head++;
			smp_store_release(&ctrl->sq_head, head);
			WRITE_ONCE(ctrl->sq_consumed,
				READ_ONCE(ctrl->sq_consumed) + 1);
			atomic_inc(&ctx->vdev->sq_consumed_total);
			atomic64_inc(&ctx->vdev->consumed_total);
			atomic_dec(&ctx->vdev->inflight);
			tail = smp_load_acquire(&ctrl->sq_tail);
		}
		wait_event_interruptible_timeout(ctx->sq_wait,
			kthread_should_stop() ||
			smp_load_acquire(&ctrl->sq_head) !=
				smp_load_acquire(&ctrl->sq_tail),
			msecs_to_jiffies(1));
	}
	WRITE_ONCE(ctrl->worker_stops, READ_ONCE(ctrl->worker_stops) + 1);
	atomic_inc(&ctx->vdev->worker_stops);
	return 0;
}

static void vdev_free_ring(struct vdev_ucontext *ctx)
{
	if (ctx->worker)
		kthread_stop(ctx->worker);
	if (ctx->cq_page)
		free_page(ctx->cq_page);
	if (ctx->sq_page)
		free_pages(ctx->sq_page, get_order(OPENURMA_VDEV_SQ_BYTES));
	if (ctx->control_page)
		free_page(ctx->control_page);
	if (ctx->owner_mm)
		mmdrop(ctx->owner_mm);
	ctx->worker = NULL;
	ctx->owner_mm = NULL;
	ctx->cq_page = ctx->sq_page = ctx->control_page = 0;
}

struct ubcore_ucontext *openurma_vdev_alloc_ucontext(
	struct ubcore_device *dev, u32 eid_index,
	struct ubcore_udrv_priv *udrv_data)
{
	struct openurma_vdev_context_req req = {0};
	struct openurma_vdev_context_resp resp = {0};
	struct openurma_vdev_control_page *ctrl;
	struct vdev_ucontext *ctx;
	struct openurma_vdev *vdev = openurma_vdev_from_dev(dev);
	u64 cookie;

	if (!udrv_data || !udrv_data->in_addr ||
	    udrv_data->in_len != sizeof(req) || !udrv_data->out_addr ||
	    udrv_data->out_len < sizeof(resp))
		return NULL;
	if (copy_from_user(&req,
		(void __user *)(uintptr_t)udrv_data->in_addr, sizeof(req)))
		return NULL;
	if (req.magic != OPENURMA_VDEV_ABI_MAGIC ||
	    req.abi_major != OPENURMA_VDEV_ABI_MAJOR ||
	    req.abi_minor > OPENURMA_VDEV_ABI_MINOR ||
	    (req.features & OPENURMA_VDEV_ABI_FEATURES) != req.features ||
	    (req.flags & ~OPENURMA_VDEV_CONTEXT_FAIL_WORKER) || req.reserved ||
	    PAGE_SIZE != OPENURMA_VDEV_PAGE_SIZE ||
	    OPENURMA_VDEV_RING_DEPTH >
		OPENURMA_VDEV_SQ_BYTES / sizeof(struct openurma_vdev_sqe) ||
	    OPENURMA_VDEV_RING_DEPTH >
		OPENURMA_VDEV_CQ_BYTES / sizeof(struct openurma_vdev_cqe))
		return NULL;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	ctx->owner_mm = current->mm;
	mmgrab(ctx->owner_mm);
	init_waitqueue_head(&ctx->sq_wait);
	ctx->control_page = openurma_vdev_model_alloc_pages(vdev, 0);
	ctx->sq_page = openurma_vdev_model_alloc_pages(vdev,
		get_order(OPENURMA_VDEV_SQ_BYTES));
	ctx->cq_page = openurma_vdev_model_alloc_pages(vdev, 0);
	if (!ctx->control_page || !ctx->sq_page || !ctx->cq_page)
		goto err;
	cookie = atomic64_inc_return(&vdev->next_context_cookie);
	ctx->vdev = vdev;
	ctx->base.ub_dev = dev;
	ctx->base.eid_index = eid_index;
	ctx->context_cookie = cookie;
	ctx->control_pgoff = 0x500000ULL +
		cookie * OPENURMA_VDEV_CONTEXT_MMAP_STRIDE_PAGES;
	ctx->sq_pgoff = ctx->control_pgoff + 1;
	ctx->cq_pgoff = ctx->sq_pgoff +
		OPENURMA_VDEV_SQ_BYTES / OPENURMA_VDEV_PAGE_SIZE;
	atomic_set(&ctx->owned_objects, 0);
	mutex_init(&ctx->mr_lock);
	INIT_LIST_HEAD(&ctx->mr_list);
	ctrl = (void *)ctx->control_page;
	ctrl->magic = OPENURMA_VDEV_ABI_MAGIC;
	ctrl->abi_major = OPENURMA_VDEV_ABI_MAJOR;
	ctrl->abi_minor = OPENURMA_VDEV_ABI_MINOR;
	ctrl->header_size = sizeof(*ctrl);
	ctrl->device_state = OPENURMA_VDEV_DEVICE_READY;
	ctrl->features = OPENURMA_VDEV_ABI_FEATURES;
	ctrl->context_id = cookie;
	ctrl->statistics_generation = atomic64_inc_return(
		&vdev->statistics_generation);
	ctrl->eid_index = eid_index;
	ctrl->sq_size = OPENURMA_VDEV_RING_DEPTH;
	ctrl->sq_mask = OPENURMA_VDEV_RING_DEPTH - 1;
	ctrl->cq_size = OPENURMA_VDEV_RING_DEPTH;
	ctrl->cq_mask = OPENURMA_VDEV_RING_DEPTH - 1;
	ctrl->sqe_size = sizeof(struct openurma_vdev_sqe);
	ctrl->cqe_size = sizeof(struct openurma_vdev_cqe);
	ctrl->generation = cookie;
	ctrl->reserved[0] = vdev->model_queue_depth;
	ctrl->reserved[1] = vdev->model_mode;
	ctrl->reserved[2] = vdev->model_numa_node;
	ctrl->reserved[3] = vdev->model_fixed_latency_ns;
	ctrl->reserved[4] = vdev->model_bandwidth_bps;
	ctrl->reserved[5] = vdev->model_completion_overhead_ns;
	resp.magic = OPENURMA_VDEV_ABI_MAGIC;
	resp.abi_major = OPENURMA_VDEV_ABI_MAJOR;
	resp.abi_minor = OPENURMA_VDEV_ABI_MINOR;
	resp.features = OPENURMA_VDEV_ABI_FEATURES;
	resp.context_id = cookie;
	resp.eid_index = eid_index;
	resp.header_size = sizeof(resp);
	resp.control_pgoff = ctx->control_pgoff;
	resp.sq_pgoff = ctx->sq_pgoff;
	resp.cq_pgoff = ctx->cq_pgoff;
	resp.page_size = PAGE_SIZE;
	resp.ring_size = OPENURMA_VDEV_RING_DEPTH;
	resp.control_bytes = OPENURMA_VDEV_CONTROL_BYTES;
	resp.sq_bytes = OPENURMA_VDEV_SQ_BYTES;
	resp.cq_bytes = OPENURMA_VDEV_CQ_BYTES;
	resp.sqe_size = sizeof(struct openurma_vdev_sqe);
	resp.cqe_size = sizeof(struct openurma_vdev_cqe);
	resp.statistics_generation = ctrl->statistics_generation;
	resp.device_state = OPENURMA_VDEV_DEVICE_READY;
	if (copy_to_user((void __user *)(uintptr_t)udrv_data->out_addr,
			 &resp, sizeof(resp)))
		goto err;
	if (req.flags & OPENURMA_VDEV_CONTEXT_FAIL_WORKER)
		goto err;
	ctx->worker = kthread_run(vdev_ring_worker, ctx, "ovq6/%llu", cookie);
	if (IS_ERR(ctx->worker)) {
		ctx->worker = NULL;
		goto err;
	}
	if (!try_module_get(THIS_MODULE))
		goto err;
	atomic_inc(&vdev->contexts);
	atomic_inc(&vdev->sq);
	atomic_inc(&vdev->cq);
	return &ctx->base;
err:
	vdev_free_ring(ctx);
	kfree(ctx);
	return NULL;
}

int openurma_vdev_free_ucontext(struct ubcore_ucontext *uctx)
{
	struct vdev_ucontext *ctx = container_of(uctx, struct vdev_ucontext, base);

	if (atomic_read(&ctx->owned_objects) != 0 || !list_empty(&ctx->mr_list))
		return -EBUSY;
	vdev_free_ring(ctx);
	atomic_dec(&ctx->vdev->cq);
	atomic_dec(&ctx->vdev->sq);
	atomic_dec(&ctx->vdev->contexts);
	kfree(ctx);
	module_put(THIS_MODULE);
	return 0;
}

int openurma_vdev_mmap(struct ubcore_ucontext *uctx,
	struct vm_area_struct *vma)
{
	struct vdev_ucontext *ctx = container_of(uctx, struct vdev_ucontext, base);
	unsigned long page, map_bytes;

	if (current->mm != ctx->owner_mm)
		return -EACCES;
	if (vma->vm_flags & VM_EXEC)
		return -EACCES;
	if (vma->vm_pgoff == ctx->control_pgoff) {
		page = ctx->control_page;
		map_bytes = OPENURMA_VDEV_CONTROL_BYTES;
	} else if (vma->vm_pgoff == ctx->sq_pgoff) {
		page = ctx->sq_page;
		map_bytes = OPENURMA_VDEV_SQ_BYTES;
	} else if (vma->vm_pgoff == ctx->cq_pgoff) {
		if (vma->vm_flags & VM_WRITE)
			return -EACCES;
		page = ctx->cq_page;
		map_bytes = OPENURMA_VDEV_CQ_BYTES;
		vm_flags_clear(vma, VM_MAYWRITE);
	} else
		return -EACCES;
	if (vma->vm_end - vma->vm_start != map_bytes)
		return -EINVAL;
	vm_flags_set(vma, VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP |
		VM_IO | VM_PFNMAP);
	vm_flags_clear(vma, VM_MAYEXEC);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, virt_to_pfn((void *)page),
		map_bytes, vma->vm_page_prot);
}
