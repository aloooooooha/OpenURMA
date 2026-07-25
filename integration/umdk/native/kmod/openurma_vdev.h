/* SPDX-License-Identifier: GPL-2.0 */
#ifndef OPENURMA_VDEV_INTERNAL_H
#define OPENURMA_VDEV_INTERNAL_H

#include <linux/debugfs.h>
#include <linux/idr.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <ub/urma/ubcore_types.h>

#include "../include/openurma_vdev_abi.h"

struct vdev_ucontext;
struct openurma_vdev;

struct vdev_mr {
	struct ubcore_target_seg base;
	struct vdev_ucontext *owner;
	struct openurma_vdev *vdev;
	struct ubcore_umem *umem;
	struct list_head node;
	struct list_head global_node;
	refcount_t refs;
	wait_queue_head_t ref_wait;
	u64 va;
	u64 len;
	u64 generation;
	u32 pinned_pages;
	u32 pages_node0;
	u32 pages_node1;
	u32 pages_other;
	u32 access;
	u32 token_id;
	u32 eid_index;
	u32 state;
};

struct vdev_recv_wr {
	struct list_head node;
	struct vdev_mr *mr;
	struct vdev_ucontext *owner;
	u32 jfr_id;
	u32 jfc_id;
	u64 va;
	u64 len;
	u64 user_ctx;
};

struct vdev_ucontext {
	struct ubcore_ucontext base;
	struct openurma_vdev *vdev;
	atomic_t owned_objects;
	struct mutex mr_lock;
	struct list_head mr_list;
	unsigned long control_page;
	unsigned long sq_page;
	unsigned long cq_page;
	struct task_struct *worker;
	wait_queue_head_t sq_wait;
	struct mm_struct *owner_mm;
	u64 context_cookie;
	u64 control_pgoff;
	u64 sq_pgoff;
	u64 cq_pgoff;
};

enum openurma_vdev_model_mode {
	OPENURMA_VDEV_MODEL_FUNCTIONAL = 0,
	OPENURMA_VDEV_MODEL_TIMED_NUMA = 1,
};

struct vdev_token {
	struct ubcore_token_id base;
	struct vdev_ucontext *owner;
	struct openurma_vdev *vdev;
};

struct vdev_jfr {
	struct ubcore_jfr base;
	struct vdev_ucontext *owner;
	struct openurma_vdev *vdev;
	struct list_head node;
};

struct vdev_jfs {
	struct ubcore_jfs base;
	struct vdev_ucontext *owner;
	struct openurma_vdev *vdev;
};

struct vdev_jetty {
	struct ubcore_jetty base;
	struct vdev_ucontext *owner;
	struct openurma_vdev *vdev;
	u32 jfr_id;
	struct list_head node;
};

/* Debug-only, root-controlled pause points.  A pause is identified by a
 * monotonic generation so a test never mistakes a stale reached event for
 * its newly armed window. */
enum openurma_vdev_fault_phase {
	OPENURMA_VDEV_FAULT_OFF = 0,
	OPENURMA_VDEV_FAULT_AFTER_MR_REFS = 1,
	OPENURMA_VDEV_FAULT_BEFORE_CQ_PUBLISH = 2,
};

struct openurma_vdev {
	struct platform_device *pdev;
	struct ubcore_device ubdev;
	struct dentry *debugfs_root;
	u32 abi_version;
	u32 devices;
	atomic_t contexts;
	atomic_t objects;
	atomic_t token_ids;
	atomic_t logical_segments;
	atomic_t imported_segments;
	atomic_t jfcs;
	atomic_t jfs;
	atomic_t jfrs;
	atomic_t jetties;
	atomic_t imported_jetties;
	atomic_t binds;
	atomic_t vtpns;
	atomic_t mr;
	atomic_t pinned_pages;
	atomic64_t mr_pages_node0;
	atomic64_t mr_pages_node1;
	atomic64_t mr_pages_other;
	atomic_t sq;
	atomic_t cq;
	atomic_t inflight;
	atomic_t errors;
	atomic64_t statistics_generation;
	atomic_t sq_consumed_total;
	atomic_t cq_produced_total;
	atomic_t cq_full_total;
	atomic_t worker_wakeups;
	atomic_t worker_starts;
	atomic_t worker_stops;
	atomic64_t doorbells;
	atomic_t max_inflight;
	atomic_t max_sq_outstanding;
	atomic_t completed_success;
	atomic_t completed_failed;
	atomic64_t submitted_total;
	atomic64_t consumed_total;
	atomic64_t completed_bytes;
	atomic64_t read_bytes;
	atomic64_t write_bytes;
	atomic64_t mr_lookup_errors;
	atomic64_t mr_range_errors;
	atomic64_t mr_access_errors;
	atomic64_t copy_errors;
	atomic64_t mr_operation_refs;
	atomic64_t mr_closing_total;
	atomic64_t mr_destroyed_total;
	enum openurma_vdev_model_mode model_mode;
	char model_mode_name[16];
	u64 model_fixed_latency_ns;
	u64 model_bandwidth_bps;
	u64 model_completion_overhead_ns;
	u32 model_queue_depth;
	int model_numa_node;
	int model_worker_cpu;
	spinlock_t model_timeline_lock;
	u64 model_direction_ready_ns[2];
	atomic_t model_inflight;
	atomic_t model_max_inflight;
	atomic64_t model_operations;
	atomic64_t model_bytes;
	atomic64_t model_base_service_ns;
	atomic64_t model_planned_delay_ns;
	atomic64_t model_actual_wait_ns;
	atomic64_t model_observed_service_ns;
	atomic64_t model_wakeup_late_ns;
	atomic64_t model_copy_ns;
	atomic_t model_last_worker_cpu;
	atomic_t model_last_worker_node;
	atomic_t completed_cancelled;
	atomic_t recv_wrs;
	atomic_t fault_pause_after_ref;
	atomic_t fault_copy_error_once;
	atomic_t fault_phase;
	atomic64_t fault_generation;
	atomic64_t fault_claimed_generation;
	atomic64_t fault_reached_generation;
	atomic64_t fault_release_generation;
	atomic64_t fault_reached_total;
	atomic64_t fault_released_total;
	wait_queue_head_t fault_wait;
	struct mutex mr_index_lock;
	struct list_head mr_index;
	struct mutex endpoint_lock;
	struct list_head recv_list;
	struct list_head jfr_list;
	struct list_head jetty_list;
	struct mutex cq_publish_lock;
	struct mutex atomic_lock;
	atomic64_t next_context_cookie;
	atomic64_t next_mr_generation;
	u32 reserved_jetty_id_min;
	u32 reserved_jetty_id_max;
	struct ida token_ida;
	struct ida jfc_ida;
	struct ida jfs_ida;
	struct ida jfr_ida;
	struct ida jetty_ida;
	struct ida vtpn_ida;
};

int openurma_vdev_debugfs_init(struct openurma_vdev *vdev);
void openurma_vdev_debugfs_fini(struct openurma_vdev *vdev);
int openurma_vdev_fault_wait(struct openurma_vdev *vdev,
	enum openurma_vdev_fault_phase phase);
int openurma_vdev_model_init(struct openurma_vdev *vdev);
void openurma_vdev_model_fini(struct openurma_vdev *vdev);
int openurma_vdev_model_wait(struct openurma_vdev *vdev,
	const struct openurma_vdev_sqe *sqe);
void openurma_vdev_model_bind_worker(struct openurma_vdev *vdev);
unsigned long openurma_vdev_model_alloc_pages(struct openurma_vdev *vdev,
	unsigned int order);
void openurma_vdev_ring_doorbell(struct vdev_ucontext *ctx);
void openurma_vdev_control_init(struct openurma_vdev *vdev);
void openurma_vdev_control_fini(struct openurma_vdev *vdev);
struct openurma_vdev *openurma_vdev_from_dev(struct ubcore_device *dev);
struct vdev_ucontext *openurma_vdev_owner_from_udata(struct ubcore_udata *udata);
void openurma_vdev_object_get(struct openurma_vdev *vdev,
	struct vdev_ucontext *owner, atomic_t *kind);
void openurma_vdev_object_put(struct openurma_vdev *vdev,
	struct vdev_ucontext *owner, atomic_t *kind);
void openurma_vdev_recv_cancel_mr(struct vdev_mr *mr);
void openurma_vdev_recv_cancel_jfr(struct openurma_vdev *vdev,
	struct vdev_ucontext *owner, u32 jfr_id);
int openurma_vdev_recv_post(struct openurma_vdev *vdev,
	struct vdev_ucontext *owner,
	const struct openurma_vdev_recv_post *req);

struct ubcore_ucontext *openurma_vdev_alloc_ucontext(
	struct ubcore_device *dev, u32 eid_index,
	struct ubcore_udrv_priv *udrv_data);
int openurma_vdev_free_ucontext(struct ubcore_ucontext *uctx);
int openurma_vdev_mmap(struct ubcore_ucontext *uctx,
	struct vm_area_struct *vma);
struct ubcore_token_id *openurma_vdev_alloc_token_id(
	struct ubcore_device *dev, union ubcore_token_id_flag flag,
	struct ubcore_udata *udata);
int openurma_vdev_free_token_id(struct ubcore_token_id *token_id);
struct ubcore_target_seg *openurma_vdev_register_seg(
	struct ubcore_device *dev, struct ubcore_seg_cfg *cfg,
	struct ubcore_udata *udata);
int openurma_vdev_unregister_seg(struct ubcore_target_seg *tseg);
int openurma_vdev_user_ctl(struct ubcore_device *dev,
	struct ubcore_user_ctl *user_ctl);
struct ubcore_target_seg *openurma_vdev_import_seg(
	struct ubcore_device *dev, struct ubcore_target_seg_cfg *cfg,
	struct ubcore_udata *udata);
int openurma_vdev_unimport_seg(struct ubcore_target_seg *tseg);
struct ubcore_jfc *openurma_vdev_create_jfc(
	struct ubcore_device *dev, struct ubcore_jfc_cfg *cfg,
	struct ubcore_udata *udata);
int openurma_vdev_destroy_jfc(struct ubcore_jfc *jfc);
struct ubcore_jfs *openurma_vdev_create_jfs(
	struct ubcore_device *dev, struct ubcore_jfs_cfg *cfg,
	struct ubcore_udata *udata);
int openurma_vdev_destroy_jfs(struct ubcore_jfs *jfs);
int openurma_vdev_execute_rw(struct vdev_ucontext *ctx,
	const struct openurma_vdev_sqe *sqe, struct openurma_vdev_cqe *cqe);
struct vdev_mr *openurma_vdev_find_mr_locked(struct openurma_vdev *vdev,
	u32 eid, u32 token, u64 generation);
void openurma_vdev_mr_put(struct vdev_mr *mr);
int openurma_vdev_copy_pages(struct vdev_mr *src_mr, u64 src_va,
	struct vdev_mr *dst_mr, u64 dst_va, u64 length);
struct ubcore_jfr *openurma_vdev_create_jfr(
	struct ubcore_device *dev, struct ubcore_jfr_cfg *cfg,
	struct ubcore_udata *udata);
int openurma_vdev_destroy_jfr(struct ubcore_jfr *jfr);
struct ubcore_jetty *openurma_vdev_create_jetty(
	struct ubcore_device *dev, struct ubcore_jetty_cfg *cfg,
	struct ubcore_udata *udata);
int openurma_vdev_destroy_jetty(struct ubcore_jetty *jetty);
struct ubcore_tjetty *openurma_vdev_import_jetty(
	struct ubcore_device *dev, struct ubcore_tjetty_cfg *cfg,
	struct ubcore_udata *udata);
int openurma_vdev_unimport_jetty(struct ubcore_tjetty *tjetty);
int openurma_vdev_bind_jetty(struct ubcore_jetty *jetty,
	struct ubcore_tjetty *tjetty, struct ubcore_udata *udata);
int openurma_vdev_unbind_jetty(struct ubcore_jetty *jetty);
struct ubcore_vtpn *openurma_vdev_alloc_vtpn(struct ubcore_device *dev);
int openurma_vdev_free_vtpn(struct ubcore_vtpn *vtpn);

#endif
