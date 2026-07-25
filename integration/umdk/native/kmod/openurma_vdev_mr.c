// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <ub/urma/ubcore_api.h>

#include "openurma_vdev.h"

static bool ranges_overlap(u64 a, u64 alen, u64 b, u64 blen)
{
	return a < b + blen && b < a + alen;
}

struct ubcore_target_seg *openurma_vdev_register_seg(
	struct ubcore_device *dev, struct ubcore_seg_cfg *cfg,
	struct ubcore_udata *udata)
{
	struct vdev_ucontext *owner = openurma_vdev_owner_from_udata(udata);
	struct openurma_vdev *vdev = openurma_vdev_from_dev(dev);
	struct openurma_vdev_mr_resp resp = {0};
	union ubcore_umem_flag flag = { .value = 0 };
	struct vdev_token *token;
	struct vdev_mr *mr, *iter;
	u64 end;

	if (!owner || current->mm != owner->owner_mm || !cfg || !cfg->token_id ||
	    cfg->len == 0 ||
	    cfg->flag.bs.non_pin || cfg->flag.bs.pa || cfg->flag.bs.dsva)
		return NULL;
	if (check_add_overflow(cfg->va, cfg->len, &end))
		return NULL;
	token = container_of(cfg->token_id, struct vdev_token, base);
	if (token->owner != owner || token->vdev != vdev)
		return NULL;
	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return NULL;
	mr->va = cfg->va;
	mr->len = cfg->len;
	mr->owner = owner;
	mr->vdev = vdev;
	mr->access = cfg->flag.bs.access;
	mr->token_id = cfg->token_id->token_id;
	mr->eid_index = owner->base.eid_index;
	mr->state = OPENURMA_VDEV_MR_ALLOCATING;
	refcount_set(&mr->refs, 1);
	init_waitqueue_head(&mr->ref_wait);
	INIT_LIST_HEAD(&mr->global_node);
	mr->pinned_pages = DIV_ROUND_UP((cfg->va & ~PAGE_MASK) + cfg->len,
		PAGE_SIZE);
	mutex_lock(&owner->mr_lock);
	list_for_each_entry(iter, &owner->mr_list, node) {
		if (ranges_overlap(mr->va, mr->len, iter->va, iter->len)) {
			mutex_unlock(&owner->mr_lock);
			kfree(mr);
			return NULL;
		}
	}
	flag.bs.writable = !!(cfg->flag.bs.access & (1U << 2));
	mr->umem = ubcore_umem_get(dev, cfg->va, cfg->len, flag);
	if (IS_ERR(mr->umem)) {
		mr->umem = NULL;
		mutex_unlock(&owner->mr_lock);
		kfree(mr);
		return NULL;
	}
	{
		struct scatterlist *sg = mr->umem->sg_head.sgl;
		unsigned int i;

		for (i = 0; i < mr->umem->sg_head.nents && sg;
		     i++, sg = sg_next(sg)) {
			int nid;

			if (!sg_page(sg))
				continue;
			nid = page_to_nid(sg_page(sg));
			if (nid == 0)
				mr->pages_node0++;
			else if (nid == 1)
				mr->pages_node1++;
			else
				mr->pages_other++;
		}
	}
	mr->generation = atomic64_inc_return(&vdev->next_mr_generation);
	mr->base.ub_dev = dev;
	mr->base.uctx = &owner->base;
	mr->base.seg.ubva.va = cfg->va;
	mr->base.seg.len = cfg->len;
	mr->state = OPENURMA_VDEV_MR_LIVE;
	list_add_tail(&mr->node, &owner->mr_list);
	mutex_unlock(&owner->mr_lock);
	mutex_lock(&vdev->mr_index_lock);
	list_add_tail(&mr->global_node, &vdev->mr_index);
	mutex_unlock(&vdev->mr_index_lock);
	openurma_vdev_object_get(vdev, owner, &vdev->logical_segments);
	atomic_inc(&vdev->mr);
	atomic_add(mr->pinned_pages, &vdev->pinned_pages);
	atomic64_add(mr->pages_node0, &vdev->mr_pages_node0);
	atomic64_add(mr->pages_node1, &vdev->mr_pages_node1);
	atomic64_add(mr->pages_other, &vdev->mr_pages_other);
	resp.magic = OPENURMA_VDEV_ABI_MAGIC;
	resp.abi_major = OPENURMA_VDEV_ABI_MAJOR;
	resp.abi_minor = OPENURMA_VDEV_ABI_MINOR;
	resp.generation = mr->generation;
	resp.va = mr->va;
	resp.len = mr->len;
	resp.token_id = mr->token_id;
	resp.access = mr->access;
	resp.pinned_pages = mr->pinned_pages;
	resp.eid_index = mr->eid_index;
	resp.state = mr->state;
	if (udata->udrv_data && udata->udrv_data->out_addr &&
	    udata->udrv_data->out_len >= sizeof(resp) &&
	    copy_to_user((void __user *)(uintptr_t)udata->udrv_data->out_addr,
		&resp, sizeof(resp))) {
		openurma_vdev_unregister_seg(&mr->base);
		return NULL;
	}
	return &mr->base;
}

int openurma_vdev_unregister_seg(struct ubcore_target_seg *tseg)
{
	struct vdev_mr *mr = container_of(tseg, struct vdev_mr, base);

	mutex_lock(&mr->vdev->mr_index_lock);
	mutex_lock(&mr->owner->mr_lock);
	if (mr->state != OPENURMA_VDEV_MR_LIVE) {
		mutex_unlock(&mr->owner->mr_lock);
		mutex_unlock(&mr->vdev->mr_index_lock);
		return -EINVAL;
	}
	mr->state = OPENURMA_VDEV_MR_CLOSING;
	list_del(&mr->node);
	list_del(&mr->global_node);
	atomic64_inc(&mr->vdev->mr_closing_total);
	mutex_unlock(&mr->owner->mr_lock);
	mutex_unlock(&mr->vdev->mr_index_lock);
	openurma_vdev_recv_cancel_mr(mr);
	wait_event(mr->ref_wait, refcount_read(&mr->refs) == 1);
	ubcore_umem_release(mr->umem);
	atomic64_sub(mr->pages_node0, &mr->vdev->mr_pages_node0);
	atomic64_sub(mr->pages_node1, &mr->vdev->mr_pages_node1);
	atomic64_sub(mr->pages_other, &mr->vdev->mr_pages_other);
	atomic_sub(mr->pinned_pages, &mr->vdev->pinned_pages);
	atomic_dec(&mr->vdev->mr);
	openurma_vdev_object_put(mr->vdev, mr->owner,
		&mr->vdev->logical_segments);
	mr->state = OPENURMA_VDEV_MR_DEAD;
	atomic64_inc(&mr->vdev->mr_destroyed_total);
	WARN_ON(!refcount_dec_and_test(&mr->refs));
	kfree(mr);
	return 0;
}

int openurma_vdev_user_ctl(struct ubcore_device *dev,
	struct ubcore_user_ctl *user_ctl)
{
	struct openurma_vdev_mr_validate req;
	struct openurma_vdev_recv_post recv_req;
	struct openurma_vdev_doorbell doorbell;
	struct vdev_ucontext *owner;
	struct vdev_mr *mr;
	int ret = -ESTALE;

	(void)dev;
	if (!user_ctl || !user_ctl->uctx || !user_ctl->in.addr)
		return -EINVAL;
	owner = container_of(user_ctl->uctx, struct vdev_ucontext, base);
	if (current->mm != owner->owner_mm)
		return -EACCES;
	if (user_ctl->in.opcode == OPENURMA_VDEV_USER_CTL_DOORBELL) {
		struct openurma_vdev_control_page *ctrl =
			(void *)owner->control_page;

		if (user_ctl->in.len != sizeof(doorbell) ||
		    copy_from_user(&doorbell,
		    (void __user *)(uintptr_t)user_ctl->in.addr,
		    sizeof(doorbell)))
			return -EFAULT;
		if (doorbell.context_id != owner->context_cookie ||
		    doorbell.sq_tail != smp_load_acquire(&ctrl->sq_tail))
			return -ESTALE;
		openurma_vdev_ring_doorbell(owner);
		return 0;
	}
	if (user_ctl->in.opcode == OPENURMA_VDEV_USER_CTL_POST_RECV) {
		if (user_ctl->in.len != sizeof(recv_req) ||
		    copy_from_user(&recv_req,
		    (void __user *)(uintptr_t)user_ctl->in.addr,
		    sizeof(recv_req)))
			return -EFAULT;
		if (recv_req.reserved[0] || recv_req.reserved[1] ||
		    recv_req.local_eid != owner->base.eid_index)
			return -EACCES;
		return openurma_vdev_recv_post(owner->vdev, owner, &recv_req);
	}
	if (user_ctl->in.opcode != OPENURMA_VDEV_USER_CTL_VALIDATE_MR ||
	    user_ctl->in.len != sizeof(req) ||
	    copy_from_user(&req, (void __user *)(uintptr_t)user_ctl->in.addr,
	    sizeof(req)))
		return -EINVAL;
	if (req.reserved != 0 || req.eid_index != owner->base.eid_index)
		return -EACCES;
	mutex_lock(&owner->vdev->mr_index_lock);
	list_for_each_entry(mr, &owner->vdev->mr_index, global_node) {
		if (mr->eid_index != req.eid_index || mr->token_id != req.token_id ||
		    mr->owner != owner || mr->state != OPENURMA_VDEV_MR_LIVE)
			continue;
		ret = mr->generation == req.generation ? 0 : -ESTALE;
		break;
	}
	mutex_unlock(&owner->vdev->mr_index_lock);
	return ret;
}

struct vdev_sg_cursor {
	struct scatterlist *sg;
	size_t offset;
};

static int vdev_cursor_init(struct vdev_mr *mr, u64 va,
	struct vdev_sg_cursor *cur)
{
	u64 skip = (mr->va & ~PAGE_MASK) + (va - mr->va);
	struct scatterlist *sg = mr->umem->sg_head.sgl;
	unsigned int i;

	for (i = 0; i < mr->umem->sg_head.nents && sg; i++, sg = sg_next(sg)) {
		if (!sg_page(sg) || sg->offset >= PAGE_SIZE || !sg->length ||
		    sg->length > PAGE_SIZE - sg->offset)
			return -EUCLEAN;
		if (skip < sg->length) {
			cur->sg = sg;
			cur->offset = skip;
			return 0;
		}
		skip -= sg->length;
	}
	return -ERANGE;
}

int openurma_vdev_copy_pages(struct vdev_mr *src_mr, u64 src_va,
	struct vdev_mr *dst_mr, u64 dst_va, u64 length)
{
	struct vdev_sg_cursor src, dst;
	u64 left = length;
	u64 copy_start = ktime_get_ns();

	if (vdev_cursor_init(src_mr, src_va, &src) ||
	    vdev_cursor_init(dst_mr, dst_va, &dst)) {
		atomic64_add(ktime_get_ns() - copy_start,
			&src_mr->vdev->model_copy_ns);
		return -ERANGE;
	}
	while (left) {
		size_t src_avail;
		size_t dst_avail;
		size_t chunk;
		void *src_map, *dst_map;

		if (!src.sg || !dst.sg || !sg_page(src.sg) || !sg_page(dst.sg) ||
		    src.sg->offset >= PAGE_SIZE || dst.sg->offset >= PAGE_SIZE ||
		    src.offset >= src.sg->length || dst.offset >= dst.sg->length ||
		    src.sg->length > PAGE_SIZE - src.sg->offset ||
		    dst.sg->length > PAGE_SIZE - dst.sg->offset) {
			atomic64_add(ktime_get_ns() - copy_start,
				&src_mr->vdev->model_copy_ns);
			return -EUCLEAN;
		}
		src_avail = src.sg->length - src.offset;
		dst_avail = dst.sg->length - dst.offset;
		chunk = min_t(u64, left, min(src_avail, dst_avail));

		if (!chunk) {
			atomic64_add(ktime_get_ns() - copy_start,
				&src_mr->vdev->model_copy_ns);
			return -EFAULT;
		}
		src_map = kmap_local_page(sg_page(src.sg));
		dst_map = kmap_local_page(sg_page(dst.sg));
		memcpy((char *)dst_map + dst.sg->offset + dst.offset,
		       (char *)src_map + src.sg->offset + src.offset, chunk);
		kunmap_local(dst_map);
		kunmap_local(src_map);
		set_page_dirty_lock(sg_page(dst.sg));
		left -= chunk;
		src.offset += chunk;
		dst.offset += chunk;
		if (src.offset == src.sg->length && left) {
			src.sg = sg_next(src.sg);
			src.offset = 0;
		}
		if (dst.offset == dst.sg->length && left) {
			dst.sg = sg_next(dst.sg);
			dst.offset = 0;
		}
		if (left && (!src.sg || !dst.sg)) {
			atomic64_add(ktime_get_ns() - copy_start,
				&src_mr->vdev->model_copy_ns);
			return -ERANGE;
		}
	}
	smp_wmb();
	atomic64_add(ktime_get_ns() - copy_start,
		&src_mr->vdev->model_copy_ns);
	return 0;
}

static bool vdev_range_valid(struct vdev_mr *mr, u64 va, u64 len)
{
	u64 end, mr_end;

	return len && !check_add_overflow(va, len, &end) &&
		!check_add_overflow(mr->va, mr->len, &mr_end) &&
		va >= mr->va && end <= mr_end;
}

static void vdev_recv_release(struct vdev_recv_wr *recv)
{
	struct openurma_vdev *vdev = recv->owner->vdev;

	openurma_vdev_mr_put(recv->mr);
	atomic64_dec(&vdev->mr_operation_refs);
	atomic_dec(&vdev->recv_wrs);
	atomic_dec(&vdev->objects);
	atomic_dec(&recv->owner->owned_objects);
	kfree(recv);
}

void openurma_vdev_recv_cancel_mr(struct vdev_mr *mr)
{
	struct openurma_vdev *vdev;
	struct vdev_recv_wr *recv, *tmp;

	if (!mr)
		return;
	vdev = mr->vdev;
	mutex_lock(&vdev->endpoint_lock);
	list_for_each_entry_safe(recv, tmp, &vdev->recv_list, node) {
		if (recv->mr != mr)
			continue;
		list_del_init(&recv->node);
		vdev_recv_release(recv);
	}
	mutex_unlock(&vdev->endpoint_lock);
}

void openurma_vdev_recv_cancel_jfr(struct openurma_vdev *vdev,
	struct vdev_ucontext *owner, u32 jfr_id)
{
	struct vdev_recv_wr *recv, *tmp;

	if (!vdev || !owner)
		return;
	mutex_lock(&vdev->endpoint_lock);
	list_for_each_entry_safe(recv, tmp, &vdev->recv_list, node) {
		if (recv->owner != owner || recv->jfr_id != jfr_id)
			continue;
		list_del_init(&recv->node);
		vdev_recv_release(recv);
	}
	mutex_unlock(&vdev->endpoint_lock);
}

int openurma_vdev_recv_post(struct openurma_vdev *vdev,
	struct vdev_ucontext *owner,
	const struct openurma_vdev_recv_post *req)
{
	struct vdev_recv_wr *recv;
	struct vdev_mr *mr;
	struct vdev_jfr *jfr;
	bool jfr_valid = false;

	if (!vdev || !owner || !req || !req->length ||
	    req->length > U32_MAX || req->local_eid != owner->base.eid_index)
		return -EINVAL;
	mutex_lock(&vdev->endpoint_lock);
	list_for_each_entry(jfr, &vdev->jfr_list, node) {
		if (jfr->owner == owner && jfr->base.jfr_id.id == req->jfr_id) {
			jfr_valid = true;
			break;
		}
	}
	mutex_unlock(&vdev->endpoint_lock);
	if (!jfr_valid)
		return -ENOENT;
	recv = kzalloc(sizeof(*recv), GFP_KERNEL);
	if (!recv)
		return -ENOMEM;
	mutex_lock(&vdev->mr_index_lock);
	mr = openurma_vdev_find_mr_locked(vdev, req->local_eid,
		req->local_token, req->local_generation);
	if (!mr || mr->owner != owner ||
	    !vdev_range_valid(mr, req->local_va, req->length)) {
		mutex_unlock(&vdev->mr_index_lock);
		kfree(recv);
		return -ESTALE;
	}
	refcount_inc(&mr->refs);
	atomic64_inc(&vdev->mr_operation_refs);
	mutex_unlock(&vdev->mr_index_lock);
	recv->mr = mr;
	recv->owner = owner;
	recv->jfr_id = req->jfr_id;
	recv->jfc_id = req->jfc_id;
	recv->va = req->local_va;
	recv->len = req->length;
	recv->user_ctx = req->user_ctx;
	INIT_LIST_HEAD(&recv->node);
	mutex_lock(&vdev->endpoint_lock);
	jfr_valid = false;
	list_for_each_entry(jfr, &vdev->jfr_list, node) {
		if (jfr->owner == owner && jfr->base.jfr_id.id == req->jfr_id) {
			jfr_valid = true;
			break;
		}
	}
	if (!jfr_valid) {
		mutex_unlock(&vdev->endpoint_lock);
		openurma_vdev_mr_put(recv->mr);
		atomic64_dec(&vdev->mr_operation_refs);
		kfree(recv);
		return -ENOENT;
	}
	list_add_tail(&recv->node, &vdev->recv_list);
	mutex_unlock(&vdev->endpoint_lock);
	atomic_inc(&vdev->recv_wrs);
	atomic_inc(&vdev->objects);
	atomic_inc(&owner->owned_objects);
	return 0;
}

struct vdev_mr *openurma_vdev_find_mr_locked(struct openurma_vdev *vdev,
	u32 eid, u32 token, u64 generation)
{
	struct vdev_mr *mr;

	list_for_each_entry(mr, &vdev->mr_index, global_node)
		if (mr->state == OPENURMA_VDEV_MR_LIVE &&
		    mr->eid_index == eid && mr->token_id == token &&
		    mr->generation == generation)
			return mr;
	return NULL;
}

void openurma_vdev_mr_put(struct vdev_mr *mr)
{
	refcount_dec(&mr->refs);
	wake_up_all(&mr->ref_wait);
}

static int vdev_copy_mr_to_buf(struct vdev_mr *src_mr, u64 src_va,
	void *buf, u64 length)
{
	struct vdev_sg_cursor src;
	u64 left = length;
	char *dst = buf;

	if (vdev_cursor_init(src_mr, src_va, &src))
		return -ERANGE;
	while (left) {
		size_t avail, chunk;
		void *map;

		if (!src.sg || !sg_page(src.sg) || src.sg->offset >= PAGE_SIZE ||
		    src.offset >= src.sg->length ||
		    src.sg->length > PAGE_SIZE - src.sg->offset)
			return -EUCLEAN;
		avail = src.sg->length - src.offset;
		chunk = min_t(u64, left, avail);
		map = kmap_local_page(sg_page(src.sg));
		memcpy(dst, (char *)map + src.sg->offset + src.offset, chunk);
		kunmap_local(map);
		dst += chunk;
		left -= chunk;
		src.offset += chunk;
		if (src.offset == src.sg->length && left) {
			src.sg = sg_next(src.sg);
			src.offset = 0;
		}
	}
	return 0;
}

static int vdev_copy_buf_to_mr(const void *buf, struct vdev_mr *dst_mr,
	u64 dst_va, u64 length)
{
	struct vdev_sg_cursor dst;
	u64 left = length;
	const char *src = buf;

	if (vdev_cursor_init(dst_mr, dst_va, &dst))
		return -ERANGE;
	while (left) {
		size_t avail, chunk;
		void *map;

		if (!dst.sg || !sg_page(dst.sg) || dst.sg->offset >= PAGE_SIZE ||
		    dst.offset >= dst.sg->length ||
		    dst.sg->length > PAGE_SIZE - dst.sg->offset)
			return -EUCLEAN;
		avail = dst.sg->length - dst.offset;
		chunk = min_t(u64, left, avail);
		map = kmap_local_page(sg_page(dst.sg));
		memcpy((char *)map + dst.sg->offset + dst.offset, src, chunk);
		kunmap_local(map);
		set_page_dirty_lock(sg_page(dst.sg));
		src += chunk;
		left -= chunk;
		dst.offset += chunk;
		if (dst.offset == dst.sg->length && left) {
			dst.sg = sg_next(dst.sg);
			dst.offset = 0;
		}
	}
	smp_wmb();
	return 0;
}

static struct vdev_recv_wr *vdev_find_recv_locked(
	struct openurma_vdev *vdev, u32 remote_eid, u32 jetty_id)
{
	struct vdev_recv_wr *recv;
	struct vdev_jetty *jetty;

	list_for_each_entry(recv, &vdev->recv_list, node) {
		if (recv->owner->base.eid_index != remote_eid)
			continue;
		list_for_each_entry(jetty, &vdev->jetty_list, node) {
			if (jetty->owner == recv->owner &&
			    jetty->base.jetty_id.id == jetty_id &&
			    jetty->jfr_id == recv->jfr_id)
				return recv;
		}
	}
	return NULL;
}

static int openurma_vdev_execute_send(struct vdev_ucontext *ctx,
	const struct openurma_vdev_sqe *sqe, struct openurma_vdev_cqe *cqe)
{
	struct openurma_vdev *vdev = ctx->vdev;
	struct vdev_mr *local;
	struct vdev_recv_wr *recv;
	struct openurma_vdev_control_page *target_ctrl;
	struct openurma_vdev_cqe *target_cqe;
	u64 target_tail;
	int ret;

	mutex_lock(&vdev->mr_index_lock);
	local = openurma_vdev_find_mr_locked(vdev, READ_ONCE(sqe->local_eid),
		READ_ONCE(sqe->local_token), READ_ONCE(sqe->local_generation));
	if (!local || local->owner != ctx) {
		atomic64_inc(&vdev->mr_lookup_errors);
		mutex_unlock(&vdev->mr_index_lock);
		return OPENURMA_VDEV_CQE_STALE_LOCAL_MR;
	}
	if (!vdev_range_valid(local, READ_ONCE(sqe->local_va),
		READ_ONCE(sqe->length))) {
		atomic64_inc(&vdev->mr_range_errors);
		mutex_unlock(&vdev->mr_index_lock);
		return OPENURMA_VDEV_CQE_LOCAL_RANGE;
	}
	refcount_inc(&local->refs);
	atomic64_inc(&vdev->mr_operation_refs);
	mutex_unlock(&vdev->mr_index_lock);
	ret = openurma_vdev_fault_wait(vdev, OPENURMA_VDEV_FAULT_AFTER_MR_REFS);
	if (ret)
		goto out_local;
	ret = openurma_vdev_model_wait(vdev, sqe);
	if (ret)
		goto out_local;
	mutex_lock(&vdev->endpoint_lock);
	recv = vdev_find_recv_locked(vdev, READ_ONCE(sqe->remote_eid),
		READ_ONCE(sqe->remote_token));
	if (!recv) {
		mutex_unlock(&vdev->endpoint_lock);
		ret = OPENURMA_VDEV_CQE_RNR;
		goto out_local;
	}
	target_ctrl = (void *)recv->owner->control_page;
	mutex_lock(&vdev->cq_publish_lock);
	if (READ_ONCE(target_ctrl->cq_tail) -
	    smp_load_acquire(&target_ctrl->cq_head) >= OPENURMA_VDEV_RING_DEPTH) {
		mutex_unlock(&vdev->cq_publish_lock);
		mutex_unlock(&vdev->endpoint_lock);
		ret = OPENURMA_VDEV_CQE_CQ_FULL;
		goto out_local;
	}
	if (READ_ONCE(sqe->length) > recv->len) {
		target_tail = READ_ONCE(target_ctrl->cq_tail);
		target_cqe = &((struct openurma_vdev_cqe *)recv->owner->cq_page)[
			target_tail & (OPENURMA_VDEV_RING_DEPTH - 1)];
		list_del_init(&recv->node);
		mutex_unlock(&vdev->endpoint_lock);
		ret = OPENURMA_VDEV_CQE_REMOTE_RESP_LEN;
		memset(target_cqe, 0, sizeof(*target_cqe));
		target_cqe->wr_id = recv->user_ctx;
		target_cqe->sequence = target_tail;
		target_cqe->generation = READ_ONCE(target_ctrl->generation);
		target_cqe->status = ret;
		target_cqe->error_detail = ret;
		target_cqe->flags = OPENURMA_VDEV_CQE_FLAG_RECV;
		target_cqe->jfc_id = recv->jfc_id;
		target_cqe->user_ctx = recv->user_ctx;
		target_cqe->local_jfs_id = recv->jfr_id;
		target_cqe->completion_timestamp = ktime_get_ns();
		smp_store_release(&target_ctrl->cq_tail, target_tail + 1);
		WRITE_ONCE(target_ctrl->cq_produced,
			READ_ONCE(target_ctrl->cq_produced) + 1);
		atomic_inc(&vdev->cq_produced_total);
		mutex_unlock(&vdev->cq_publish_lock);
		vdev_recv_release(recv);
		goto out_local;
	}
	target_tail = READ_ONCE(target_ctrl->cq_tail);
	target_cqe = &((struct openurma_vdev_cqe *)recv->owner->cq_page)[
		target_tail & (OPENURMA_VDEV_RING_DEPTH - 1)];
	list_del_init(&recv->node);
	mutex_unlock(&vdev->endpoint_lock);
	ret = 0;
	if (atomic_cmpxchg(&vdev->fault_copy_error_once, 1, 0) == 1 ||
	    openurma_vdev_copy_pages(local, READ_ONCE(sqe->local_va), recv->mr,
	    recv->va, READ_ONCE(sqe->length)))
		ret = OPENURMA_VDEV_CQE_COPY_ERROR;
	memset(target_cqe, 0, sizeof(*target_cqe));
	target_cqe->wr_id = recv->user_ctx;
	target_cqe->sequence = target_tail;
	target_cqe->generation = READ_ONCE(target_ctrl->generation);
	target_cqe->status = ret;
	target_cqe->completion_len = ret ? 0 : READ_ONCE(sqe->length);
	target_cqe->error_detail = ret;
	target_cqe->flags = OPENURMA_VDEV_CQE_FLAG_RECV;
	target_cqe->jfc_id = recv->jfc_id;
	target_cqe->user_ctx = recv->user_ctx;
	target_cqe->local_jfs_id = recv->jfr_id;
	target_cqe->completion_timestamp = ktime_get_ns();
	smp_store_release(&target_ctrl->cq_tail, target_tail + 1);
	WRITE_ONCE(target_ctrl->cq_produced, READ_ONCE(target_ctrl->cq_produced) + 1);
	atomic_inc(&vdev->cq_produced_total);
	mutex_unlock(&vdev->cq_publish_lock);
	if (ret)
		atomic64_inc(&vdev->copy_errors);
	else {
		cqe->completion_len = READ_ONCE(sqe->length);
		atomic64_add(READ_ONCE(sqe->length), &vdev->completed_bytes);
	}
	vdev_recv_release(recv);
	if (ret)
		goto out_local;
	ret = OPENURMA_VDEV_CQE_SUCCESS;
out_local:
	openurma_vdev_mr_put(local);
	atomic64_dec(&vdev->mr_operation_refs);
	return ret == -ESHUTDOWN ? OPENURMA_VDEV_CQE_CONTEXT_CLOSING : ret;
}

static int openurma_vdev_execute_atomic(struct vdev_ucontext *ctx,
	const struct openurma_vdev_sqe *sqe, struct openurma_vdev_cqe *cqe)
{
	struct openurma_vdev *vdev = ctx->vdev;
	struct vdev_mr *local = NULL, *remote = NULL;
	u64 old_value, new_value;
	u32 opcode = READ_ONCE(sqe->opcode);
	int ret = 0;

	if (READ_ONCE(sqe->length) != sizeof(u64) ||
	    (READ_ONCE(sqe->local_va) & (sizeof(u64) - 1)) ||
	    (READ_ONCE(sqe->remote_va) & (sizeof(u64) - 1)))
		return OPENURMA_VDEV_CQE_REMOTE_RANGE;
	mutex_lock(&vdev->mr_index_lock);
	local = openurma_vdev_find_mr_locked(vdev, READ_ONCE(sqe->local_eid),
		READ_ONCE(sqe->local_token), READ_ONCE(sqe->local_generation));
	remote = openurma_vdev_find_mr_locked(vdev, READ_ONCE(sqe->remote_eid),
		READ_ONCE(sqe->remote_token), READ_ONCE(sqe->remote_generation));
	if (!local || local->owner != ctx) {
		atomic64_inc(&vdev->mr_lookup_errors);
		ret = OPENURMA_VDEV_CQE_STALE_LOCAL_MR;
		goto out_unlock;
	}
	if (!remote || remote->owner == ctx) {
		atomic64_inc(&vdev->mr_lookup_errors);
		ret = OPENURMA_VDEV_CQE_STALE_REMOTE_MR;
		goto out_unlock;
	}
	if (!vdev_range_valid(local, READ_ONCE(sqe->local_va), sizeof(u64))) {
		atomic64_inc(&vdev->mr_range_errors);
		ret = OPENURMA_VDEV_CQE_LOCAL_RANGE;
		goto out_unlock;
	}
	if (!vdev_range_valid(remote, READ_ONCE(sqe->remote_va), sizeof(u64))) {
		atomic64_inc(&vdev->mr_range_errors);
		ret = OPENURMA_VDEV_CQE_REMOTE_RANGE;
		goto out_unlock;
	}
	if (!(remote->access & (1U << 3))) {
		atomic64_inc(&vdev->mr_access_errors);
		ret = OPENURMA_VDEV_CQE_REMOTE_ACCESS;
		goto out_unlock;
	}
	refcount_inc(&local->refs);
	refcount_inc(&remote->refs);
	atomic64_add(2, &vdev->mr_operation_refs);
	mutex_unlock(&vdev->mr_index_lock);
	ret = openurma_vdev_fault_wait(vdev, OPENURMA_VDEV_FAULT_AFTER_MR_REFS);
	if (ret)
		goto out_refs;
	ret = openurma_vdev_model_wait(vdev, sqe);
	if (ret)
		goto out_refs;
	if (atomic_cmpxchg(&vdev->fault_copy_error_once, 1, 0) == 1) {
		ret = -EIO;
		goto out_refs;
	}
	mutex_lock(&vdev->atomic_lock);
	ret = vdev_copy_mr_to_buf(remote, READ_ONCE(sqe->remote_va),
		&old_value, sizeof(old_value));
	if (!ret)
		ret = vdev_copy_buf_to_mr(&old_value, local,
			READ_ONCE(sqe->local_va), sizeof(old_value));
	if (!ret) {
		if (opcode == OPENURMA_VDEV_SQE_CAS) {
			new_value = old_value;
			if (old_value == READ_ONCE(sqe->atomic_compare)) {
				new_value = READ_ONCE(sqe->atomic_swap_add);
				ret = vdev_copy_buf_to_mr(&new_value, remote,
					READ_ONCE(sqe->remote_va), sizeof(new_value));
			}
		} else if (opcode == OPENURMA_VDEV_SQE_FADD) {
			new_value = old_value + READ_ONCE(sqe->atomic_swap_add);
			ret = vdev_copy_buf_to_mr(&new_value, remote,
				READ_ONCE(sqe->remote_va), sizeof(new_value));
		} else {
			ret = -EOPNOTSUPP;
		}
	}
	mutex_unlock(&vdev->atomic_lock);
out_refs:
	atomic64_dec(&vdev->mr_operation_refs);
	openurma_vdev_mr_put(remote);
	atomic64_dec(&vdev->mr_operation_refs);
	openurma_vdev_mr_put(local);
	if (ret == -ESHUTDOWN) {
		atomic_inc(&vdev->completed_cancelled);
		return OPENURMA_VDEV_CQE_CONTEXT_CLOSING;
	}
	if (ret) {
		atomic64_inc(&vdev->copy_errors);
		return ret == -EOPNOTSUPP ? OPENURMA_VDEV_CQE_BAD_OPCODE :
			OPENURMA_VDEV_CQE_COPY_ERROR;
	}
	cqe->completion_len = sizeof(u64);
	atomic64_add(sizeof(u64), &vdev->completed_bytes);
	return OPENURMA_VDEV_CQE_SUCCESS;
out_unlock:
	mutex_unlock(&vdev->mr_index_lock);
	return ret;
}

int openurma_vdev_execute_rw(struct vdev_ucontext *ctx,
	const struct openurma_vdev_sqe *sqe, struct openurma_vdev_cqe *cqe)
{
	struct openurma_vdev *vdev = ctx->vdev;
	struct vdev_mr *local = NULL, *remote = NULL, *src, *dst;
	u32 opcode = READ_ONCE(sqe->opcode);
	int ret = 0;

	if (opcode == OPENURMA_VDEV_SQE_SEND)
		return openurma_vdev_execute_send(ctx, sqe, cqe);
	if (opcode == OPENURMA_VDEV_SQE_CAS ||
	    opcode == OPENURMA_VDEV_SQE_FADD)
		return openurma_vdev_execute_atomic(ctx, sqe, cqe);
	if (opcode != OPENURMA_VDEV_SQE_WRITE && opcode != OPENURMA_VDEV_SQE_READ)
		return OPENURMA_VDEV_CQE_BAD_OPCODE;
	mutex_lock(&vdev->mr_index_lock);
	local = openurma_vdev_find_mr_locked(vdev, READ_ONCE(sqe->local_eid),
		READ_ONCE(sqe->local_token), READ_ONCE(sqe->local_generation));
	if (!local || local->owner != ctx) {
		atomic64_inc(&vdev->mr_lookup_errors);
		ret = OPENURMA_VDEV_CQE_STALE_LOCAL_MR;
		goto out_unlock;
	}
	remote = openurma_vdev_find_mr_locked(vdev, READ_ONCE(sqe->remote_eid),
		READ_ONCE(sqe->remote_token), READ_ONCE(sqe->remote_generation));
	if (!remote || remote->owner == ctx) {
		atomic64_inc(&vdev->mr_lookup_errors);
		ret = OPENURMA_VDEV_CQE_STALE_REMOTE_MR;
		goto out_unlock;
	}
	if (!vdev_range_valid(local, READ_ONCE(sqe->local_va), READ_ONCE(sqe->length))) {
		atomic64_inc(&vdev->mr_range_errors);
		ret = OPENURMA_VDEV_CQE_LOCAL_RANGE;
		goto out_unlock;
	}
	if (!vdev_range_valid(remote, READ_ONCE(sqe->remote_va), READ_ONCE(sqe->length))) {
		atomic64_inc(&vdev->mr_range_errors);
		ret = OPENURMA_VDEV_CQE_REMOTE_RANGE;
		goto out_unlock;
	}
	if ((opcode == OPENURMA_VDEV_SQE_WRITE && !(remote->access & (1U << 2))) ||
	    (opcode == OPENURMA_VDEV_SQE_READ && !(remote->access & (1U << 1)))) {
		atomic64_inc(&vdev->mr_access_errors);
		ret = OPENURMA_VDEV_CQE_REMOTE_ACCESS;
		goto out_unlock;
	}
	refcount_inc(&local->refs);
	refcount_inc(&remote->refs);
	atomic64_add(2, &vdev->mr_operation_refs);
	mutex_unlock(&vdev->mr_index_lock);
	ret = openurma_vdev_fault_wait(vdev,
		OPENURMA_VDEV_FAULT_AFTER_MR_REFS);
	while (atomic_read(&vdev->fault_pause_after_ref)) {
		if (kthread_should_stop()) {
			ret = -ESHUTDOWN;
			break;
		}
		usleep_range(500, 1000);
	}
	if (!ret)
		ret = openurma_vdev_model_wait(vdev, sqe);
	if (!ret && atomic_cmpxchg(&vdev->fault_copy_error_once, 1, 0) == 1)
		ret = -EIO;
	if (!ret && opcode == OPENURMA_VDEV_SQE_WRITE) {
		src = local;
		dst = remote;
		ret = openurma_vdev_copy_pages(src, READ_ONCE(sqe->local_va), dst,
			READ_ONCE(sqe->remote_va), READ_ONCE(sqe->length));
	} else if (!ret) {
		src = remote;
		dst = local;
		ret = openurma_vdev_copy_pages(src, READ_ONCE(sqe->remote_va), dst,
			READ_ONCE(sqe->local_va), READ_ONCE(sqe->length));
	}
	atomic64_dec(&vdev->mr_operation_refs);
	openurma_vdev_mr_put(remote);
	atomic64_dec(&vdev->mr_operation_refs);
	openurma_vdev_mr_put(local);
	if (ret == -ESHUTDOWN) {
		atomic_inc(&vdev->completed_cancelled);
		return OPENURMA_VDEV_CQE_CONTEXT_CLOSING;
	}
	if (ret) {
		atomic64_inc(&vdev->copy_errors);
		return OPENURMA_VDEV_CQE_COPY_ERROR;
	}
	cqe->completion_len = (u32)READ_ONCE(sqe->length);
	atomic64_add(READ_ONCE(sqe->length), &vdev->completed_bytes);
	if (opcode == OPENURMA_VDEV_SQE_WRITE)
		atomic64_add(READ_ONCE(sqe->length), &vdev->write_bytes);
	else
		atomic64_add(READ_ONCE(sqe->length), &vdev->read_bytes);
	return OPENURMA_VDEV_CQE_SUCCESS;
out_unlock:
	mutex_unlock(&vdev->mr_index_lock);
	return ret;
}
