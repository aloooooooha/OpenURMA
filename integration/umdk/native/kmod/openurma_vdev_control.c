// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "openurma_vdev.h"

#define VDEV_ID_MIN 1
#define VDEV_ID_MAX 0x00ffffff

struct vdev_tseg {
	struct ubcore_target_seg base;
	struct vdev_ucontext *owner;
	struct openurma_vdev *vdev;
};

struct vdev_jfc {
	struct ubcore_jfc base;
	struct vdev_ucontext *owner;
	struct openurma_vdev *vdev;
};

struct vdev_tjetty {
	struct ubcore_tjetty base;
	struct vdev_ucontext *owner;
	struct openurma_vdev *vdev;
};

struct vdev_vtpn {
	struct ubcore_vtpn base;
	struct openurma_vdev *vdev;
};

struct openurma_vdev *openurma_vdev_from_dev(struct ubcore_device *dev)
{
	return container_of(dev, struct openurma_vdev, ubdev);
}

struct vdev_ucontext *openurma_vdev_owner_from_udata(struct ubcore_udata *udata)
{
	if (!udata || !udata->uctx)
		return NULL;
	{
		struct vdev_ucontext *owner = container_of(udata->uctx,
			struct vdev_ucontext, base);

		return current->mm == owner->owner_mm ? owner : NULL;
	}
}

void openurma_vdev_object_get(struct openurma_vdev *vdev,
			struct vdev_ucontext *owner, atomic_t *kind)
{
	atomic_inc(&vdev->objects);
	atomic_inc(kind);
	if (owner)
		atomic_inc(&owner->owned_objects);
}

void openurma_vdev_object_put(struct openurma_vdev *vdev,
			struct vdev_ucontext *owner, atomic_t *kind)
{
	if (owner)
		atomic_dec(&owner->owned_objects);
	atomic_dec(kind);
	atomic_dec(&vdev->objects);
}

void openurma_vdev_control_init(struct openurma_vdev *vdev)
{
	ida_init(&vdev->token_ida);
	ida_init(&vdev->jfc_ida);
	ida_init(&vdev->jfs_ida);
	ida_init(&vdev->jfr_ida);
	ida_init(&vdev->jetty_ida);
	ida_init(&vdev->vtpn_ida);
	atomic64_set(&vdev->statistics_generation, 0);
	atomic_set(&vdev->sq_consumed_total, 0);
	atomic_set(&vdev->cq_produced_total, 0);
	atomic_set(&vdev->cq_full_total, 0);
	atomic_set(&vdev->worker_wakeups, 0);
	atomic_set(&vdev->worker_starts, 0);
	atomic_set(&vdev->worker_stops, 0);
	atomic64_set(&vdev->doorbells, 0);
	atomic_set(&vdev->max_inflight, 0);
	atomic_set(&vdev->max_sq_outstanding, 0);
	atomic_set(&vdev->completed_success, 0);
	atomic_set(&vdev->completed_failed, 0);
	atomic64_set(&vdev->submitted_total, 0);
	atomic64_set(&vdev->consumed_total, 0);
	atomic64_set(&vdev->completed_bytes, 0);
	atomic64_set(&vdev->read_bytes, 0);
	atomic64_set(&vdev->write_bytes, 0);
	atomic64_set(&vdev->mr_lookup_errors, 0);
	atomic64_set(&vdev->mr_range_errors, 0);
	atomic64_set(&vdev->mr_access_errors, 0);
	atomic64_set(&vdev->copy_errors, 0);
	atomic64_set(&vdev->mr_operation_refs, 0);
	atomic64_set(&vdev->mr_closing_total, 0);
	atomic64_set(&vdev->mr_destroyed_total, 0);
	atomic64_set(&vdev->mr_pages_node0, 0);
	atomic64_set(&vdev->mr_pages_node1, 0);
	atomic64_set(&vdev->mr_pages_other, 0);
	atomic_set(&vdev->completed_cancelled, 0);
	atomic_set(&vdev->recv_wrs, 0);
	atomic_set(&vdev->fault_pause_after_ref, 0);
	atomic_set(&vdev->fault_copy_error_once, 0);
	atomic_set(&vdev->fault_phase, OPENURMA_VDEV_FAULT_OFF);
	atomic64_set(&vdev->fault_generation, 0);
	atomic64_set(&vdev->fault_claimed_generation, 0);
	atomic64_set(&vdev->fault_reached_generation, 0);
	atomic64_set(&vdev->fault_release_generation, 0);
	atomic64_set(&vdev->fault_reached_total, 0);
	atomic64_set(&vdev->fault_released_total, 0);
	init_waitqueue_head(&vdev->fault_wait);
	mutex_init(&vdev->mr_index_lock);
	INIT_LIST_HEAD(&vdev->mr_index);
	mutex_init(&vdev->endpoint_lock);
	INIT_LIST_HEAD(&vdev->recv_list);
	INIT_LIST_HEAD(&vdev->jfr_list);
	INIT_LIST_HEAD(&vdev->jetty_list);
	mutex_init(&vdev->cq_publish_lock);
	mutex_init(&vdev->atomic_lock);
}

void openurma_vdev_control_fini(struct openurma_vdev *vdev)
{
	mutex_destroy(&vdev->atomic_lock);
	mutex_destroy(&vdev->cq_publish_lock);
	mutex_destroy(&vdev->endpoint_lock);
	mutex_destroy(&vdev->mr_index_lock);
	ida_destroy(&vdev->vtpn_ida);
	ida_destroy(&vdev->jetty_ida);
	ida_destroy(&vdev->jfr_ida);
	ida_destroy(&vdev->jfc_ida);
	ida_destroy(&vdev->jfs_ida);
	ida_destroy(&vdev->token_ida);
}

struct ubcore_token_id *openurma_vdev_alloc_token_id(
	struct ubcore_device *dev, union ubcore_token_id_flag flag,
	struct ubcore_udata *udata)
{
	struct openurma_vdev *vdev = openurma_vdev_from_dev(dev);
	struct vdev_ucontext *owner = openurma_vdev_owner_from_udata(udata);
	struct vdev_token *token;
	int id;

	if (!owner)
		return NULL;
	id = ida_alloc_range(&vdev->token_ida, VDEV_ID_MIN, VDEV_ID_MAX,
			     GFP_KERNEL);
	if (id < 0)
		return NULL;
	token = kzalloc(sizeof(*token), GFP_KERNEL);
	if (!token) {
		ida_free(&vdev->token_ida, id);
		return NULL;
	}
	token->owner = owner;
	token->vdev = vdev;
	token->base.ub_dev = dev;
	token->base.uctx = &owner->base;
	token->base.token_id = id;
	token->base.flag = flag;
	openurma_vdev_object_get(vdev, owner, &vdev->token_ids);
	return &token->base;
}

int openurma_vdev_free_token_id(struct ubcore_token_id *token_id)
{
	struct vdev_token *token = container_of(token_id, struct vdev_token, base);

	ida_free(&token->vdev->token_ida, token_id->token_id);
	openurma_vdev_object_put(token->vdev, token->owner, &token->vdev->token_ids);
	kfree(token);
	return 0;
}

struct ubcore_target_seg *openurma_vdev_import_seg(
	struct ubcore_device *dev, struct ubcore_target_seg_cfg *cfg,
	struct ubcore_udata *udata)
{
	struct openurma_vdev *vdev = openurma_vdev_from_dev(dev);
	struct vdev_ucontext *owner = openurma_vdev_owner_from_udata(udata);
	struct vdev_tseg *seg;

	if (!owner || !cfg || cfg->seg.len == 0)
		return NULL;
	seg = kzalloc(sizeof(*seg), GFP_KERNEL);
	if (!seg)
		return NULL;
	seg->owner = owner;
	seg->vdev = vdev;
	seg->base.ub_dev = dev;
	seg->base.uctx = &owner->base;
	seg->base.seg = cfg->seg;
	seg->base.mva = cfg->mva;
	openurma_vdev_object_get(vdev, owner, &vdev->imported_segments);
	return &seg->base;
}

int openurma_vdev_unimport_seg(struct ubcore_target_seg *tseg)
{
	struct vdev_tseg *seg = container_of(tseg, struct vdev_tseg, base);

	openurma_vdev_object_put(seg->vdev, seg->owner, &seg->vdev->imported_segments);
	kfree(seg);
	return 0;
}

struct ubcore_jfc *openurma_vdev_create_jfc(
	struct ubcore_device *dev, struct ubcore_jfc_cfg *cfg,
	struct ubcore_udata *udata)
{
	struct openurma_vdev *vdev = openurma_vdev_from_dev(dev);
	struct vdev_ucontext *owner = openurma_vdev_owner_from_udata(udata);
	struct vdev_jfc *obj;
	int id;

	if (!owner)
		return NULL;
	id = ida_alloc_range(&vdev->jfc_ida, VDEV_ID_MIN, VDEV_ID_MAX,
			     GFP_KERNEL);
	if (id < 0)
		return NULL;
	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		ida_free(&vdev->jfc_ida, id);
		return NULL;
	}
	obj->owner = owner;
	obj->vdev = vdev;
	obj->base.ub_dev = dev;
	obj->base.uctx = &owner->base;
	obj->base.id = id;
	obj->base.jfc_cfg = *cfg;
	openurma_vdev_object_get(vdev, owner, &vdev->jfcs);
	return &obj->base;
}

int openurma_vdev_destroy_jfc(struct ubcore_jfc *jfc)
{
	struct vdev_jfc *obj = container_of(jfc, struct vdev_jfc, base);
	struct openurma_vdev_control_page *ctrl =
		(void *)obj->owner->control_page;

	if (smp_load_acquire(&ctrl->sq_head) !=
	    smp_load_acquire(&ctrl->sq_tail) ||
	    atomic_read(&obj->vdev->inflight) != 0)
		return -EBUSY;

	ida_free(&obj->vdev->jfc_ida, jfc->id);
	openurma_vdev_object_put(obj->vdev, obj->owner, &obj->vdev->jfcs);
	kfree(obj);
	return 0;
}

struct ubcore_jfs *openurma_vdev_create_jfs(
	struct ubcore_device *dev, struct ubcore_jfs_cfg *cfg,
	struct ubcore_udata *udata)
{
	struct openurma_vdev *vdev = openurma_vdev_from_dev(dev);
	struct vdev_ucontext *owner = openurma_vdev_owner_from_udata(udata);
	struct vdev_jfs *obj;
	int id;

	if (!owner || !cfg || !cfg->jfc || cfg->jfc->uctx != &owner->base)
		return NULL;
	id = ida_alloc_range(&vdev->jfs_ida, VDEV_ID_MIN, VDEV_ID_MAX,
		GFP_KERNEL);
	if (id < 0)
		return NULL;
	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		ida_free(&vdev->jfs_ida, id);
		return NULL;
	}
	obj->owner = owner;
	obj->vdev = vdev;
	obj->base.ub_dev = dev;
	obj->base.uctx = &owner->base;
	obj->base.jfs_id.id = id;
	obj->base.jfs_cfg = *cfg;
	openurma_vdev_object_get(vdev, owner, &vdev->jfs);
	return &obj->base;
}

int openurma_vdev_destroy_jfs(struct ubcore_jfs *jfs)
{
	struct vdev_jfs *obj = container_of(jfs, struct vdev_jfs, base);
	struct openurma_vdev_control_page *ctrl =
		(void *)obj->owner->control_page;

	if (smp_load_acquire(&ctrl->sq_head) !=
	    smp_load_acquire(&ctrl->sq_tail) ||
	    atomic_read(&obj->vdev->inflight) != 0)
		return -EBUSY;

	ida_free(&obj->vdev->jfs_ida, jfs->jfs_id.id);
	openurma_vdev_object_put(obj->vdev, obj->owner, &obj->vdev->jfs);
	kfree(obj);
	return 0;
}

struct ubcore_jfr *openurma_vdev_create_jfr(
	struct ubcore_device *dev, struct ubcore_jfr_cfg *cfg,
	struct ubcore_udata *udata)
{
	struct openurma_vdev *vdev = openurma_vdev_from_dev(dev);
	struct vdev_ucontext *owner = openurma_vdev_owner_from_udata(udata);
	struct vdev_jfr *obj;
	int id;

	if (!owner)
		return NULL;
	id = ida_alloc_range(&vdev->jfr_ida, VDEV_ID_MIN, VDEV_ID_MAX,
			     GFP_KERNEL);
	if (id < 0)
		return NULL;
	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		ida_free(&vdev->jfr_ida, id);
		return NULL;
	}
	obj->owner = owner;
	obj->vdev = vdev;
	obj->base.ub_dev = dev;
	obj->base.uctx = &owner->base;
	obj->base.jfr_id.id = id;
	obj->base.jfr_cfg = *cfg;
	INIT_LIST_HEAD(&obj->node);
	mutex_lock(&vdev->endpoint_lock);
	list_add_tail(&obj->node, &vdev->jfr_list);
	mutex_unlock(&vdev->endpoint_lock);
	openurma_vdev_object_get(vdev, owner, &vdev->jfrs);
	return &obj->base;
}

int openurma_vdev_destroy_jfr(struct ubcore_jfr *jfr)
{
	struct vdev_jfr *obj = container_of(jfr, struct vdev_jfr, base);

	openurma_vdev_recv_cancel_jfr(obj->vdev, obj->owner, jfr->jfr_id.id);
	mutex_lock(&obj->vdev->endpoint_lock);
	list_del_init(&obj->node);
	mutex_unlock(&obj->vdev->endpoint_lock);
	ida_free(&obj->vdev->jfr_ida, jfr->jfr_id.id);
	openurma_vdev_object_put(obj->vdev, obj->owner, &obj->vdev->jfrs);
	kfree(obj);
	return 0;
}

struct ubcore_jetty *openurma_vdev_create_jetty(
	struct ubcore_device *dev, struct ubcore_jetty_cfg *cfg,
	struct ubcore_udata *udata)
{
	struct openurma_vdev *vdev = openurma_vdev_from_dev(dev);
	struct vdev_ucontext *owner = openurma_vdev_owner_from_udata(udata);
	struct vdev_jetty *obj;
	int id;

	if (!owner)
		return NULL;
	id = ida_alloc_range(&vdev->jetty_ida, VDEV_ID_MIN, VDEV_ID_MAX,
			     GFP_KERNEL);
	if (id < 0)
		return NULL;
	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		ida_free(&vdev->jetty_ida, id);
		return NULL;
	}
	obj->owner = owner;
	obj->vdev = vdev;
	obj->base.ub_dev = dev;
	obj->base.uctx = &owner->base;
	obj->base.jetty_id.id = id;
	obj->base.jetty_cfg = *cfg;
	obj->jfr_id = (cfg->jfr != NULL) ? cfg->jfr->jfr_id.id : 0;
	INIT_LIST_HEAD(&obj->node);
	mutex_lock(&vdev->endpoint_lock);
	list_add_tail(&obj->node, &vdev->jetty_list);
	mutex_unlock(&vdev->endpoint_lock);
	openurma_vdev_object_get(vdev, owner, &vdev->jetties);
	return &obj->base;
}

int openurma_vdev_destroy_jetty(struct ubcore_jetty *jetty)
{
	struct vdev_jetty *obj = container_of(jetty, struct vdev_jetty, base);

	mutex_lock(&obj->vdev->endpoint_lock);
	list_del_init(&obj->node);
	mutex_unlock(&obj->vdev->endpoint_lock);
	ida_free(&obj->vdev->jetty_ida, jetty->jetty_id.id);
	openurma_vdev_object_put(obj->vdev, obj->owner, &obj->vdev->jetties);
	kfree(obj);
	return 0;
}

struct ubcore_tjetty *openurma_vdev_import_jetty(
	struct ubcore_device *dev, struct ubcore_tjetty_cfg *cfg,
	struct ubcore_udata *udata)
{
	struct openurma_vdev *vdev = openurma_vdev_from_dev(dev);
	struct vdev_ucontext *owner = openurma_vdev_owner_from_udata(udata);
	struct vdev_tjetty *obj;

	if (!owner || !cfg)
		return NULL;
	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return NULL;
	obj->owner = owner;
	obj->vdev = vdev;
	obj->base.ub_dev = dev;
	obj->base.uctx = &owner->base;
	obj->base.cfg = *cfg;
	openurma_vdev_object_get(vdev, owner, &vdev->imported_jetties);
	return &obj->base;
}

int openurma_vdev_unimport_jetty(struct ubcore_tjetty *tjetty)
{
	struct vdev_tjetty *obj = container_of(tjetty, struct vdev_tjetty, base);

	openurma_vdev_object_put(obj->vdev, obj->owner, &obj->vdev->imported_jetties);
	kfree(obj);
	return 0;
}

int openurma_vdev_bind_jetty(struct ubcore_jetty *jetty,
			     struct ubcore_tjetty *tjetty,
			     struct ubcore_udata *udata)
{
	struct openurma_vdev *vdev;

	(void)udata;
	if (!jetty || !tjetty || jetty->ub_dev != tjetty->ub_dev)
		return -EINVAL;
	vdev = openurma_vdev_from_dev(jetty->ub_dev);
	atomic_inc(&vdev->binds);
	return 0;
}

int openurma_vdev_unbind_jetty(struct ubcore_jetty *jetty)
{
	struct openurma_vdev *vdev;

	if (!jetty || !jetty->ub_dev)
		return -EINVAL;
	vdev = openurma_vdev_from_dev(jetty->ub_dev);
	if (atomic_read(&vdev->binds) > 0)
		atomic_dec(&vdev->binds);
	return 0;
}

struct ubcore_vtpn *openurma_vdev_alloc_vtpn(struct ubcore_device *dev)
{
	struct openurma_vdev *vdev = openurma_vdev_from_dev(dev);
	struct vdev_vtpn *obj;
	int id;

	id = ida_alloc_range(&vdev->vtpn_ida, VDEV_ID_MIN, VDEV_ID_MAX,
			     GFP_KERNEL);
	if (id < 0)
		return NULL;
	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		ida_free(&vdev->vtpn_ida, id);
		return NULL;
	}
	obj->vdev = vdev;
	obj->base.ub_dev = dev;
	obj->base.vtpn = id;
	atomic_inc(&vdev->vtpns);
	return &obj->base;
}

int openurma_vdev_free_vtpn(struct ubcore_vtpn *vtpn)
{
	struct vdev_vtpn *obj = container_of(vtpn, struct vdev_vtpn, base);

	ida_free(&obj->vdev->vtpn_ida, vtpn->vtpn);
	atomic_dec(&obj->vdev->vtpns);
	kfree(obj);
	return 0;
}
