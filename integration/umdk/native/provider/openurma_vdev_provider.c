// SPDX-License-Identifier: MIT
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "urma_api.h"
#include "urma_provider.h"
#include "openurma_vdev_q5.h"

#ifndef OPENURMA_VDEV_PROVIDER_NAME
#define OPENURMA_VDEV_PROVIDER_NAME "openurma_vdev"
#endif

struct vdev_context {
	urma_context_t base;
	struct openurma_vdev_context_resp response;
	struct openurma_vdev_control_page *control;
	struct openurma_vdev_sqe *sq;
	struct openurma_vdev_cqe *cq;
	pthread_mutex_t sq_mutex;
	pthread_mutex_t cq_mutex;
};

struct vdev_target_seg {
	urma_target_seg_t base;
	struct openurma_vdev_mr_resp mr;
	uint64_t remote_generation;
	bool imported;
};

struct vdev_jfs {
	urma_jfs_t base;
};

struct vdev_target_jetty {
	urma_target_jetty_t base;
	urma_jetty_t *bound_jetty;
};

static int log_enabled;
static urma_ops_t g_ops;

#define VDEV_LOG(...) do { if (log_enabled) fprintf(stderr, __VA_ARGS__); } while (0)

static urma_status_t status_from_cmd(int ret)
{
	return ret == 0 ? URMA_SUCCESS : URMA_FAIL;
}

static uint64_t vdev_monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t vdev_effective_qd(const struct vdev_context *ctx)
{
	uint64_t qd = __atomic_load_n(&ctx->control->reserved[0],
		__ATOMIC_ACQUIRE);

	if (!qd || qd > OPENURMA_VDEV_RING_DEPTH)
		qd = OPENURMA_VDEV_RING_DEPTH;
	return qd;
}

static void vdev_ring_doorbell(struct vdev_context *ctx, uint64_t tail)
{
	struct openurma_vdev_doorbell req = {
		.context_id = ctx->response.context_id,
		.sq_tail = tail,
	};
	urma_user_ctl_in_t in = {
		.addr = (uint64_t)(uintptr_t)&req,
		.len = sizeof(req),
		.opcode = OPENURMA_VDEV_USER_CTL_DOORBELL,
	};
	urma_user_ctl_out_t out = {0};
	urma_udrv_t data = {0};

	if (urma_cmd_user_ctl(&ctx->base, &in, &out, &data) != 0)
		VDEV_LOG("[openurma_vdev] doorbell failed context=%" PRIu64
			" tail=%" PRIu64 " errno=%d\n",
			req.context_id, tail, errno);
}

/* The Q8 virtual device publishes two stable EIDs (index 0 -> ...01 and
 * index 1 -> ...02).  A target jetty carries the full EID while the compact
 * ring ABI carries the device-local EID index. */
static int vdev_target_eid_index(const urma_eid_t *eid)
{
	if (!eid || eid->raw[0] != 0xfd || eid->raw[15] < 1 || eid->raw[15] > 2)
		return -1;
	for (size_t i = 1; i < 15; ++i)
		if (eid->raw[i] != 0)
			return -1;
	return (int)eid->raw[15] - 1;
}

static int validate_context_response(const struct openurma_vdev_context_resp *r)
{
	if (r->magic != OPENURMA_VDEV_ABI_MAGIC ||
	    r->abi_major != OPENURMA_VDEV_ABI_MAJOR ||
	    r->abi_minor > OPENURMA_VDEV_ABI_MINOR ||
	    r->header_size != sizeof(*r) ||
	    r->page_size != OPENURMA_VDEV_PAGE_SIZE ||
	    r->control_bytes != OPENURMA_VDEV_CONTROL_BYTES ||
	    r->sq_bytes != OPENURMA_VDEV_SQ_BYTES ||
	    r->cq_bytes != OPENURMA_VDEV_CQ_BYTES ||
	    r->ring_size == 0 || (r->ring_size & (r->ring_size - 1)) != 0 ||
	    r->ring_size != OPENURMA_VDEV_RING_DEPTH ||
	    r->sqe_size != sizeof(struct openurma_vdev_sqe) ||
	    r->cqe_size != sizeof(struct openurma_vdev_cqe) ||
	    r->statistics_generation == 0 ||
	    r->device_state != OPENURMA_VDEV_DEVICE_READY || r->fatal_status != 0 ||
	    (r->features & OPENURMA_VDEV_ABI_FEATURES) !=
		OPENURMA_VDEV_ABI_FEATURES)
		return EPROTO;
	return 0;
}

static int validate_and_touch_range(uint64_t va, uint64_t len)
{
	char *line = NULL;
	size_t line_cap = 0;
	uintptr_t cursor, end;
	FILE *maps;
	int ret = EFAULT;

	if (len == 0 || va > UINTPTR_MAX || len > UINTPTR_MAX ||
	    __builtin_add_overflow((uintptr_t)va, (uintptr_t)len, &end) || end == 0)
		return EOVERFLOW;
	cursor = (uintptr_t)va;
	maps = fopen("/proc/self/maps", "re");
	if (!maps)
		return errno;
	while (cursor < end && getline(&line, &line_cap, maps) >= 0) {
		unsigned long map_start, map_end;
		char perms[5] = {0};

		if (sscanf(line, "%lx-%lx %4s", &map_start, &map_end, perms) != 3)
			continue;
		if (cursor < map_start)
			break;
		if (cursor >= map_end)
			continue;
		if (perms[0] != 'r' || perms[1] != 'w')
			break;
		cursor = map_end < end ? map_end : end;
	}
	if (cursor == end) {
		uintptr_t pos = (uintptr_t)va;

		while (pos < end) {
			volatile unsigned char *byte = (volatile unsigned char *)pos;
			unsigned char value = *byte;

			*byte = value;
			if (end - pos <= OPENURMA_VDEV_PAGE_SIZE)
				break;
			pos = (pos | (OPENURMA_VDEV_PAGE_SIZE - 1)) + 1;
		}
		ret = 0;
	}
	free(line);
	(void)fclose(maps);
	return ret;
}

static urma_status_t openurma_vdev_init(urma_init_attr_t *conf)
{
	(void)conf;
	VDEV_LOG("[openurma_vdev] Q8 provider init\n");
	return URMA_SUCCESS;
}

static urma_status_t openurma_vdev_uninit(void)
{
	return URMA_SUCCESS;
}

static urma_status_t openurma_vdev_query_device(urma_device_t *dev,
						urma_device_attr_t *attr)
{
	(void)dev;
	if (!attr)
		return URMA_EINVAL;
	memset(attr, 0, sizeof(*attr));
	attr->port_cnt = 1;
	attr->port_attr[0].state = URMA_PORT_ACTIVE;
	attr->port_attr[0].max_mtu = URMA_MTU_4096;
	attr->port_attr[0].active_mtu = URMA_MTU_4096;
	attr->dev_cap.max_jfc = 256;
	attr->dev_cap.max_jfr = 256;
	attr->dev_cap.max_jetty = 256;
	attr->dev_cap.max_jfc_depth = 256;
	attr->dev_cap.max_jfr_depth = 256;
	attr->dev_cap.max_jfs_depth = 256;
	attr->dev_cap.max_jfr_sge = 1;
	attr->dev_cap.max_jfs_sge = 1;
	attr->dev_cap.max_jfs_rsge = 1;
	attr->dev_cap.trans_mode = URMA_TM_RC;
	attr->dev_cap.max_jfs = 256;
	attr->dev_cap.max_msg_size = 10U * 1024U * 1024U;
	attr->dev_cap.max_read_size = 10U * 1024U * 1024U;
	attr->dev_cap.max_write_size = 10U * 1024U * 1024U;
	attr->dev_cap.max_cas_size = sizeof(uint64_t);
	attr->dev_cap.max_fetch_and_add_size = sizeof(uint64_t);
	attr->dev_cap.atomic_feat.bs.cas = 1;
	attr->dev_cap.atomic_feat.bs.fetch_and_add = 1;
	attr->dev_cap.max_jfs_inline_len = 0;
	return URMA_SUCCESS;
}

static urma_context_t *openurma_vdev_create_context(urma_device_t *dev,
						     uint32_t eid_index,
						     int dev_fd)
{
	struct vdev_context *ctx = calloc(1, sizeof(*ctx));
	struct openurma_vdev_context_req req;
	urma_context_cfg_t cfg;
	urma_cmd_udrv_priv_t udata;
	const char *fail_mmap_env;
	int fail_mmap_step = 0;

	if (!ctx)
		return NULL;
	memset(&cfg, 0, sizeof(cfg));
	cfg.dev = dev;
	cfg.ops = &g_ops;
	cfg.eid_index = eid_index;
	cfg.dev_fd = dev_fd;
	memset(&req, 0, sizeof(req));
	req.magic = OPENURMA_VDEV_ABI_MAGIC;
	req.abi_major = OPENURMA_VDEV_ABI_MAJOR;
	req.abi_minor = OPENURMA_VDEV_ABI_MINOR;
	req.features = OPENURMA_VDEV_ABI_FEATURES;
	if (getenv("OPENURMA_VDEV_FORCE_ABI_MISMATCH"))
		req.abi_major++;
	if (getenv("OPENURMA_VDEV_FORCE_UNSUPPORTED_FEATURE"))
		req.features |= 1ULL << 63;
	if (getenv("OPENURMA_VDEV_FAIL_WORKER"))
		req.flags |= OPENURMA_VDEV_CONTEXT_FAIL_WORKER;
	memset(&udata, 0, sizeof(udata));
	udata.in_addr = (uint64_t)(uintptr_t)&req;
	udata.in_len = sizeof(req);
	udata.out_addr = (uint64_t)(uintptr_t)&ctx->response;
	udata.out_len = sizeof(ctx->response);
	if (urma_cmd_create_context(&ctx->base, &cfg, &udata) != 0) {
		VDEV_LOG("[openurma_vdev] create_context failed errno=%d\n", errno);
		free(ctx);
		return NULL;
	}
	if (getenv("OPENURMA_VDEV_FORCE_RESPONSE_MAGIC"))
		ctx->response.magic++;
	if (getenv("OPENURMA_VDEV_FORCE_RESPONSE_RING_SIZE"))
		ctx->response.ring_size = 3;
	if (getenv("OPENURMA_VDEV_FORCE_RESPONSE_ENTRY_SIZE"))
		ctx->response.sqe_size++;
	if (validate_context_response(&ctx->response) != 0) {
		errno = EPROTO;
		goto err_cmd;
	}
#define MAP_Q5(member, key, protection) do { \
	ctx->member = mmap(NULL, ctx->response.member##_bytes, protection, \
		MAP_SHARED, dev_fd, (off_t)(ctx->response.key * ctx->response.page_size)); \
	if (ctx->member == MAP_FAILED) { ctx->member = NULL; goto err_map; } \
} while (0)
	fail_mmap_env = getenv("OPENURMA_VDEV_FAIL_MMAP_STEP");
	if (fail_mmap_env)
		fail_mmap_step = atoi(fail_mmap_env);
	MAP_Q5(control, control_pgoff, PROT_READ | PROT_WRITE);
	if (fail_mmap_step == 1) { errno = EIO; goto err_map; }
	MAP_Q5(sq, sq_pgoff, PROT_READ | PROT_WRITE);
	if (fail_mmap_step == 2) { errno = EIO; goto err_map; }
	MAP_Q5(cq, cq_pgoff, PROT_READ);
	if (fail_mmap_step == 3) { errno = EIO; goto err_map; }
#undef MAP_Q5
	if (ctx->control->magic != OPENURMA_VDEV_ABI_MAGIC ||
	    ctx->control->context_id != ctx->response.context_id ||
	    ctx->control->header_size != sizeof(*ctx->control) ||
	    ctx->control->sq_size != ctx->response.ring_size ||
	    ctx->control->sq_mask != ctx->response.ring_size - 1 ||
	    ctx->control->cq_size != ctx->response.ring_size ||
	    ctx->control->cq_mask != ctx->response.ring_size - 1 ||
	    ctx->control->sqe_size != ctx->response.sqe_size ||
	    ctx->control->cqe_size != ctx->response.cqe_size ||
	    ctx->control->generation == 0) {
		errno = EPROTO;
		goto err_map;
	}
	if (pthread_mutex_init(&ctx->sq_mutex, NULL) != 0 ||
	    pthread_mutex_init(&ctx->cq_mutex, NULL) != 0) {
		errno = EIO;
		goto err_map;
	}
	return &ctx->base;
err_map:
	if (ctx->cq)
		(void)munmap(ctx->cq, ctx->response.cq_bytes);
	if (ctx->sq)
		(void)munmap(ctx->sq, ctx->response.sq_bytes);
	if (ctx->control)
		(void)munmap(ctx->control, ctx->response.control_bytes);
err_cmd:
	(void)urma_cmd_delete_context(&ctx->base);
	free(ctx);
	return NULL;
}

static urma_status_t openurma_vdev_delete_context(urma_context_t *ctx)
{
	int ret;

	if (!ctx)
		return URMA_EINVAL;
	ret = urma_cmd_delete_context(ctx);
	if (ret == 0) {
		struct vdev_context *private = (struct vdev_context *)ctx;
		(void)pthread_mutex_destroy(&private->cq_mutex);
		(void)pthread_mutex_destroy(&private->sq_mutex);
		(void)munmap(private->cq, private->response.cq_bytes);
		(void)munmap(private->sq, private->response.sq_bytes);
		(void)munmap(private->control, private->response.control_bytes);
		free(ctx);
	}
	return status_from_cmd(ret);
}

static urma_token_id_t *vdev_alloc_token_id(urma_context_t *ctx)
{
	urma_token_id_t *token = calloc(1, sizeof(*token));
	urma_cmd_udrv_priv_t udata = {0};

	if (!token)
		return NULL;
	token->urma_ctx = ctx;
	if (urma_cmd_alloc_token_id(ctx, token, &udata) != 0) {
		free(token);
		return NULL;
	}
	return token;
}

static urma_status_t vdev_free_token_id(urma_token_id_t *token)
{
	int ret;

	if (!token)
		return URMA_EINVAL;
	ret = urma_cmd_free_token_id(token);
	if (ret == 0)
		free(token);
	return status_from_cmd(ret);
}

static urma_target_seg_t *vdev_register_seg(urma_context_t *ctx,
						    urma_seg_cfg_t *cfg)
{
	struct vdev_target_seg *private;
	urma_target_seg_t *seg;
	urma_cmd_udrv_priv_t udata;

	if (!ctx || !cfg || !cfg->token_id || !cfg->flag.bs.token_id_valid ||
	    cfg->flag.bs.non_pin || cfg->flag.bs.dsva || cfg->flag.bs.user_iova ||
	    cfg->flag.bs.reserved || (cfg->flag.bs.access & ~0xfU) ||
	    validate_and_touch_range(cfg->va, cfg->len) != 0) {
		errno = EINVAL;
		return NULL;
	}
	private = calloc(1, sizeof(*private));
	if (!private)
		return NULL;
	seg = &private->base;
	seg->urma_ctx = ctx;
	seg->token_id = cfg->token_id;
	memset(&udata, 0, sizeof(udata));
	udata.out_addr = (uint64_t)(uintptr_t)&private->mr;
	udata.out_len = sizeof(private->mr);
	if (getenv("OPENURMA_VDEV_FAIL_MR_COPYOUT"))
		udata.out_addr = 1;
	if (urma_cmd_register_seg(ctx, seg, cfg, &udata) != 0) {
		free(private);
		return NULL;
	}
	if (private->mr.magic != OPENURMA_VDEV_ABI_MAGIC) {
		(void)urma_cmd_unregister_seg(seg);
		free(private);
		errno = EPROTO;
		return NULL;
	}
	seg->seg.ubva.uasid = private->mr.eid_index;
	seg->seg.ubva.va = private->mr.va;
	seg->seg.len = private->mr.len;
	return seg;
}

static urma_status_t vdev_unregister_seg(urma_target_seg_t *seg)
{
	int ret;

	if (!seg)
		return URMA_EINVAL;
	ret = urma_cmd_unregister_seg(seg);
	if (ret == 0)
		free(seg);
	return status_from_cmd(ret);
}

int openurma_vdev_q5_context_info(urma_context_t *ctx,
	struct openurma_vdev_q5_context_info *info)
{
	struct vdev_context *private;

	if (!ctx || !info || ctx->ops != &g_ops)
		return EINVAL;
	private = (struct vdev_context *)ctx;
	memset(info, 0, sizeof(*info));
	info->response = private->response;
	info->control = private->control;
	info->sq = private->sq;
	info->cq = private->cq;
	info->dev_fd = ctx->dev_fd;
	return 0;
}

int openurma_vdev_q5_post_nop(urma_context_t *ctx, uint64_t wr_id)
{
	struct vdev_context *private;
	struct openurma_vdev_control_page *ctrl;
	struct openurma_vdev_sqe *sqe;
	uint64_t head, tail;

	if (!ctx || ctx->ops != &g_ops)
		return EINVAL;
	private = (struct vdev_context *)ctx;
	ctrl = private->control;
	head = __atomic_load_n(&ctrl->sq_head, __ATOMIC_ACQUIRE);
	tail = __atomic_load_n(&ctrl->sq_tail, __ATOMIC_RELAXED);
	if (tail - head >= vdev_effective_qd(private)) {
		__atomic_add_fetch(&ctrl->sq_full, 1, __ATOMIC_RELAXED);
		return EAGAIN;
	}
	sqe = &private->sq[tail & (OPENURMA_VDEV_RING_DEPTH - 1)];
	memset(sqe, 0, sizeof(*sqe));
	sqe->wr_id = wr_id;
	sqe->opcode = OPENURMA_VDEV_SQE_NOP;
	sqe->sequence = tail;
	sqe->generation = __atomic_load_n(&ctrl->generation, __ATOMIC_ACQUIRE);
	__atomic_add_fetch(&ctrl->sq_submitted, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&ctrl->sq_tail, tail + 1, __ATOMIC_RELEASE);
	vdev_ring_doorbell(private, tail + 1);
	return 0;
}

int openurma_vdev_q5_poll(urma_context_t *ctx,
	struct openurma_vdev_cqe *cqe)
{
	struct vdev_context *private;
	struct openurma_vdev_control_page *ctrl;
	uint64_t head, tail;

	if (!ctx || !cqe || ctx->ops != &g_ops)
		return EINVAL;
	private = (struct vdev_context *)ctx;
	ctrl = private->control;
	head = __atomic_load_n(&ctrl->cq_head, __ATOMIC_RELAXED);
	tail = __atomic_load_n(&ctrl->cq_tail, __ATOMIC_ACQUIRE);
	if (head == tail)
		return EAGAIN;
	*cqe = private->cq[head & (OPENURMA_VDEV_RING_DEPTH - 1)];
	__atomic_store_n(&ctrl->cq_head, head + 1, __ATOMIC_RELEASE);
	__atomic_add_fetch(&ctrl->cq_consumed, 1, __ATOMIC_RELAXED);
	return 0;
}

int openurma_vdev_q5_mr_info(urma_target_seg_t *seg,
	struct openurma_vdev_mr_resp *info)
{
	if (!seg || !info || seg->urma_ctx->ops != &g_ops)
		return EINVAL;
	*info = ((struct vdev_target_seg *)seg)->mr;
	return 0;
}

int openurma_vdev_q5_validate_mr(urma_context_t *ctx, uint32_t token_id,
	uint64_t generation)

{
	return openurma_vdev_q5_validate_mr_eid(ctx, token_id, generation,
		ctx ? ctx->eid_index : UINT32_MAX);
}

int openurma_vdev_q5_validate_mr_eid(urma_context_t *ctx, uint32_t token_id,
	uint64_t generation, uint32_t eid_index)
{
	struct openurma_vdev_mr_validate req = {
		.generation = generation,
		.token_id = token_id,
		.eid_index = eid_index,
	};
	urma_user_ctl_in_t in = {
		.addr = (uint64_t)(uintptr_t)&req,
		.len = sizeof(req),
		.opcode = OPENURMA_VDEV_USER_CTL_VALIDATE_MR,
	};
	urma_user_ctl_out_t out = {0};
	urma_udrv_t data = {0};

	if (!ctx || ctx->ops != &g_ops)
		return EINVAL;
	return urma_cmd_user_ctl(ctx, &in, &out, &data);
}

static urma_target_seg_t *vdev_import_seg(urma_context_t *ctx,
						 urma_seg_t *seg, urma_token_t *token,
						 uint64_t addr,
						 urma_import_seg_flag_t flag)
{
	struct vdev_target_seg *private;
	urma_target_seg_t *target;
	urma_import_tseg_cfg_t cfg;
	urma_cmd_udrv_priv_t udata = {0};

	/* Q6 uses NOMAP; addr carries the generation from the exchanged
	 * provider descriptor.  Mapped imports and all unused flag bits are
	 * intentionally rejected so addr can never be confused with an MVA.
	 */
	if (!ctx || !seg || !token || !addr || flag.value != 0 || !seg->len ||
	    !seg->token_id || seg->attr.bs.reserved) {
		errno = EINVAL;
		return NULL;
	}
	private = calloc(1, sizeof(*private));
	if (!private)
		return NULL;
	target = &private->base;
	private->imported = true;
	private->remote_generation = addr;
	memset(&cfg, 0, sizeof(cfg));
	target->urma_ctx = ctx;
	target->seg = *seg;
	target->mva = seg->ubva.va;
	cfg.ubva = seg->ubva;
	cfg.len = seg->len;
	cfg.token_id = seg->token_id;
	cfg.token = token;
	cfg.flag = flag;
	cfg.mva = target->mva;
	if (urma_cmd_import_seg(ctx, target, &cfg, &udata) != 0) {
		free(private);
		return NULL;
	}
	return target;
}

static urma_jfs_t *vdev_create_jfs(urma_context_t *ctx, urma_jfs_cfg_t *cfg)
{
	struct vdev_jfs *private;
	urma_cmd_udrv_priv_t udata = {0};

	if (!ctx || !cfg || !cfg->jfc || cfg->jfc->urma_ctx != ctx)
		return NULL;
	private = calloc(1, sizeof(*private));
	if (!private)
		return NULL;
	private->base.urma_ctx = ctx;
	private->base.jfs_cfg = *cfg;
	if (urma_cmd_create_jfs(ctx, &private->base, cfg, &udata) != 0) {
		free(private);
		return NULL;
	}
	return &private->base;
}

static urma_status_t vdev_delete_jfs(urma_jfs_t *jfs)
{
	int ret;

	if (!jfs)
		return URMA_EINVAL;
	ret = urma_cmd_delete_jfs(jfs);
	if (ret == 0)
		free(jfs);
	return status_from_cmd(ret);
}

static urma_status_t vdev_post_jfs_wr(urma_jfs_t *jfs, urma_jfs_wr_t *wr,
	urma_jfs_wr_t **bad_wr)
{
	struct vdev_context *ctx;
	urma_jfs_wr_t *current;
	urma_status_t ret = URMA_SUCCESS;

	if (!jfs || !wr || !bad_wr || !jfs->jfs_cfg.jfc)
		return URMA_EINVAL;
	ctx = (struct vdev_context *)jfs->urma_ctx;
	*bad_wr = NULL;
	pthread_mutex_lock(&ctx->sq_mutex);
	for (current = wr; current; current = current->next) {
		struct vdev_target_seg *local = NULL, *remote = NULL;
		urma_sge_t *local_sge = NULL, *remote_sge = NULL;
		struct openurma_vdev_sqe *sqe;
		uint64_t head, tail;

		urma_jfs_wr_flag_t expected_flag = {0};

		expected_flag.bs.complete_enable = 1;
		if (!current->tjetty || !current->flag.bs.complete_enable ||
		    current->flag.value != expected_flag.value ||
		    current->tjetty->urma_ctx != jfs->urma_ctx)
			goto bad;
		if (current->opcode == URMA_OPC_SEND) {
			if (current->flag.bs.inline_flag ||
			    current->send.src.num_sge != 1 || !current->send.src.sge ||
			    !current->send.src.sge->tseg ||
			    current->send.src.sge->len == 0)
				goto bad;
			local_sge = &current->send.src.sge[0];
			local = (struct vdev_target_seg *)local_sge->tseg;
			if (local->imported || local->base.urma_ctx != jfs->urma_ctx ||
			    local->mr.magic != OPENURMA_VDEV_ABI_MAGIC)
				goto bad;
		} else if (current->opcode == URMA_OPC_WRITE) {
			if (current->rw.src.num_sge != 1 || current->rw.dst.num_sge != 1 ||
			    !current->rw.src.sge || !current->rw.dst.sge)
				goto bad;
			local_sge = &current->rw.src.sge[0];
			remote_sge = &current->rw.dst.sge[0];
		} else if (current->opcode == URMA_OPC_READ) {
			if (current->rw.src.num_sge != 1 || current->rw.dst.num_sge != 1 ||
			    !current->rw.src.sge || !current->rw.dst.sge)
				goto bad;
			remote_sge = &current->rw.src.sge[0];
			local_sge = &current->rw.dst.sge[0];
		} else if (current->opcode == URMA_OPC_CAS) {
			if (current->cas.dst == NULL || current->cas.src == NULL)
				goto bad;
			remote_sge = current->cas.dst;
			local_sge = current->cas.src;
		} else if (current->opcode == URMA_OPC_FADD) {
			if (current->faa.dst == NULL || current->faa.src == NULL)
				goto bad;
			remote_sge = current->faa.dst;
			local_sge = current->faa.src;
		} else {
			goto bad;
		}
		if (!local_sge || !local_sge->tseg || local_sge->len == 0)
			goto bad;
		if (current->opcode != URMA_OPC_SEND) {
			if (!remote_sge || !remote_sge->tseg ||
			    local_sge->len != remote_sge->len)
				goto bad;
			if (!local)
				local = (struct vdev_target_seg *)local_sge->tseg;
			remote = (struct vdev_target_seg *)remote_sge->tseg;
			if (local->imported || !remote->imported ||
			    local->base.urma_ctx != jfs->urma_ctx ||
			    remote->base.urma_ctx != jfs->urma_ctx ||
			    local->mr.magic != OPENURMA_VDEV_ABI_MAGIC ||
			    remote->remote_generation == 0 ||
			    memcmp(&remote->base.seg.ubva.eid, &current->tjetty->id.eid,
			    sizeof(remote->base.seg.ubva.eid)) != 0)
				goto bad;
			if ((current->opcode == URMA_OPC_CAS || current->opcode == URMA_OPC_FADD) &&
			    local_sge->len != sizeof(uint64_t))
				goto bad;
		}
		head = __atomic_load_n(&ctx->control->sq_head, __ATOMIC_ACQUIRE);
		tail = __atomic_load_n(&ctx->control->sq_tail, __ATOMIC_RELAXED);
		if (tail - head >= vdev_effective_qd(ctx)) {
			__atomic_add_fetch(&ctx->control->sq_full, 1, __ATOMIC_RELAXED);
			ret = URMA_EAGAIN;
			goto bad_ret;
		}
		sqe = &ctx->sq[tail & ctx->control->sq_mask];
		memset(sqe, 0, sizeof(*sqe));
		sqe->wr_id = current->user_ctx;
		sqe->sequence = tail;
		sqe->generation = __atomic_load_n(&ctx->control->generation, __ATOMIC_ACQUIRE);
		sqe->opcode = current->opcode;
		sqe->local_eid = local->mr.eid_index;
		sqe->local_token = local->mr.token_id;
		sqe->local_generation = local->mr.generation;
		sqe->local_va = local_sge->addr;
		if (current->opcode == URMA_OPC_SEND) {
			int target_eid = vdev_target_eid_index(&current->tjetty->id.eid);
			if (target_eid < 0)
				goto bad;
			sqe->remote_eid = (uint32_t)target_eid;
			sqe->remote_token = current->tjetty->id.id;
			sqe->remote_va = 0;
			sqe->remote_generation = 0;
		} else {
			sqe->remote_eid = remote->base.seg.ubva.uasid;
			sqe->remote_token = remote->base.seg.token_id;
			sqe->remote_generation = remote->remote_generation;
			sqe->remote_va = remote_sge->addr;
		}
		sqe->length = local_sge->len;
		sqe->local_jfs_id = jfs->jfs_id.id;
		sqe->jfc_id = jfs->jfs_cfg.jfc->jfc_id.id;
		sqe->user_ctx = current->user_ctx;
		if (current->opcode == URMA_OPC_CAS) {
			sqe->atomic_compare = current->cas.cmp_data;
			sqe->atomic_swap_add = current->cas.swap_data;
		} else if (current->opcode == URMA_OPC_FADD) {
			sqe->atomic_swap_add = current->faa.operand;
		} else {
			sqe->submit_timestamp = vdev_monotonic_ns();
		}
		__atomic_add_fetch(&ctx->control->sq_submitted, 1, __ATOMIC_RELAXED);
		__atomic_store_n(&ctx->control->sq_tail, tail + 1, __ATOMIC_RELEASE);
		vdev_ring_doorbell(ctx, tail + 1);
	}
	pthread_mutex_unlock(&ctx->sq_mutex);
	return URMA_SUCCESS;
bad:
	ret = URMA_EINVAL;
bad_ret:
	*bad_wr = current;
	pthread_mutex_unlock(&ctx->sq_mutex);
	return ret;
}

static urma_status_t vdev_post_jfr_wr(urma_jfr_t *jfr, urma_jfr_wr_t *wr,
	urma_jfr_wr_t **bad_wr)
{
	struct vdev_target_seg *local;
	struct openurma_vdev_recv_post req;
	urma_user_ctl_in_t in;
	urma_user_ctl_out_t out = {0};
	urma_udrv_t data = {0};

	if (!jfr || !wr || !bad_wr || !jfr->jfr_cfg.jfc ||
	    wr->src.num_sge != 1 || !wr->src.sge || !wr->src.sge->tseg ||
	    wr->src.sge->len == 0 || wr->src.sge->tseg->urma_ctx != jfr->urma_ctx)
		return URMA_EINVAL;
	local = (struct vdev_target_seg *)wr->src.sge->tseg;
	if (local->imported || local->mr.magic != OPENURMA_VDEV_ABI_MAGIC)
		return URMA_EINVAL;
	memset(&req, 0, sizeof(req));
	req.jfr_id = jfr->jfr_id.id;
	req.jfc_id = jfr->jfr_cfg.jfc->jfc_id.id;
	req.local_eid = local->mr.eid_index;
	req.local_token = local->mr.token_id;
	req.local_generation = local->mr.generation;
	req.local_va = wr->src.sge->addr;
	req.length = wr->src.sge->len;
	req.user_ctx = wr->user_ctx;
	in.addr = (uint64_t)(uintptr_t)&req;
	in.len = sizeof(req);
	in.opcode = OPENURMA_VDEV_USER_CTL_POST_RECV;
	*bad_wr = NULL;
	return status_from_cmd(urma_cmd_user_ctl(jfr->urma_ctx, &in, &out, &data));
}

static int vdev_poll_jfc(urma_jfc_t *jfc, int cr_cnt, urma_cr_t *cr)
{
	struct vdev_context *ctx;
	int count = 0;

	if (!jfc || cr_cnt < 0 || (cr_cnt && !cr))
		return -1;
	ctx = (struct vdev_context *)jfc->urma_ctx;
	pthread_mutex_lock(&ctx->cq_mutex);
	while (count < cr_cnt) {
		uint64_t head = __atomic_load_n(&ctx->control->cq_head, __ATOMIC_RELAXED);
		uint64_t tail = __atomic_load_n(&ctx->control->cq_tail, __ATOMIC_ACQUIRE);
		struct openurma_vdev_cqe cqe;

		if (head == tail)
			break;
		cqe = ctx->cq[head & ctx->control->cq_mask];
		if (cqe.jfc_id != jfc->jfc_id.id) {
			/* Leave another JFC's CQE at the head; it must not be stolen. */
			break;
		}
		memset(&cr[count], 0, sizeof(cr[count]));
		switch (cqe.status) {
		case OPENURMA_VDEV_CQE_SUCCESS:
			cr[count].status = URMA_CR_SUCCESS;
			break;
		case OPENURMA_VDEV_CQE_RNR:
			cr[count].status = URMA_CR_RNR_RETRY_CNT_EXC_ERR;
			break;
		case OPENURMA_VDEV_CQE_REMOTE_RESP_LEN:
			cr[count].status = URMA_CR_REM_RESP_LEN_ERR;
			break;
		default:
			cr[count].status = URMA_CR_REM_ACCESS_ABORT_ERR;
			break;
		}
		if (cqe.flags & OPENURMA_VDEV_CQE_FLAG_RECV) {
			cr[count].flag.bs.s_r = 1;
			cr[count].opcode = URMA_CR_OPC_SEND;
		}
		cr[count].user_ctx = cqe.user_ctx;
		cr[count].completion_len = cqe.completion_len;
		cr[count].local_id = cqe.local_jfs_id;
		__atomic_store_n(&ctx->control->cq_head, head + 1, __ATOMIC_RELEASE);
		__atomic_add_fetch(&ctx->control->cq_consumed, 1, __ATOMIC_RELAXED);
		count++;
	}
	pthread_mutex_unlock(&ctx->cq_mutex);
	return count;
}

static urma_status_t vdev_unimport_seg(urma_target_seg_t *seg)
{
	int ret;

	if (!seg)
		return URMA_EINVAL;
	ret = urma_cmd_unimport_seg(seg);
	if (ret == 0)
		free(seg);
	return status_from_cmd(ret);
}

static urma_jfc_t *vdev_create_jfc(urma_context_t *ctx, urma_jfc_cfg_t *cfg)
{
	urma_jfc_t *jfc = calloc(1, sizeof(*jfc));
	urma_cmd_udrv_priv_t udata = {0};

	if (!jfc)
		return NULL;
	jfc->urma_ctx = ctx;
	jfc->jfc_cfg = *cfg;
	if (urma_cmd_create_jfc(ctx, jfc, cfg, &udata) != 0) {
		free(jfc);
		return NULL;
	}
	return jfc;
}

static urma_status_t vdev_delete_jfc(urma_jfc_t *jfc)
{
	int ret;

	if (!jfc)
		return URMA_EINVAL;
	ret = urma_cmd_delete_jfc(jfc);
	if (ret == 0)
		free(jfc);
	return status_from_cmd(ret);
}

static urma_jfr_t *vdev_create_jfr(urma_context_t *ctx, urma_jfr_cfg_t *cfg)
{
	urma_jfr_t *jfr = calloc(1, sizeof(*jfr));
	urma_cmd_udrv_priv_t udata = {0};

	if (!jfr)
		return NULL;
	jfr->urma_ctx = ctx;
	jfr->jfr_cfg = *cfg;
	if (urma_cmd_create_jfr(ctx, jfr, cfg, &udata) != 0) {
		free(jfr);
		return NULL;
	}
	return jfr;
}

static urma_status_t vdev_delete_jfr(urma_jfr_t *jfr)
{
	int ret;

	if (!jfr)
		return URMA_EINVAL;
	ret = urma_cmd_delete_jfr(jfr);
	if (ret == 0)
		free(jfr);
	return status_from_cmd(ret);
}

static urma_jetty_t *vdev_create_jetty(urma_context_t *ctx,
						urma_jetty_cfg_t *cfg)
{
	urma_jetty_t *jetty = calloc(1, sizeof(*jetty));
	urma_cmd_udrv_priv_t udata = {0};

	if (!jetty)
		return NULL;
	jetty->urma_ctx = ctx;
	jetty->jetty_cfg = *cfg;
	if (urma_cmd_create_jetty(ctx, jetty, cfg, &udata) != 0) {
		free(jetty);
		return NULL;
	}
	return jetty;
}

static urma_status_t vdev_delete_jetty(urma_jetty_t *jetty)
{
	int ret;

	if (!jetty)
		return URMA_EINVAL;
	ret = urma_cmd_delete_jetty(jetty);
	if (ret == 0)
		free(jetty);
	return status_from_cmd(ret);
}

static urma_target_jetty_t *vdev_import_jetty(urma_context_t *ctx,
						       urma_rjetty_t *remote,
						       urma_token_t *token)
{
	struct vdev_target_jetty *private = calloc(1, sizeof(*private));
	urma_target_jetty_t *target;
	urma_tjetty_cfg_t cfg;
	urma_cmd_udrv_priv_t udata = {0};

	if (!private)
		return NULL;
	target = &private->base;
	memset(&cfg, 0, sizeof(cfg));
	target->urma_ctx = ctx;
	target->id = remote->jetty_id;
	target->trans_mode = remote->trans_mode;
	cfg.jetty_id = remote->jetty_id;
	cfg.flag = remote->flag;
	cfg.token = token;
	cfg.trans_mode = remote->trans_mode;
	cfg.type = remote->type;
	cfg.tp_type = remote->tp_type;
	if (urma_cmd_import_jetty(ctx, target, &cfg, &udata) != 0) {
		free(private);
		return NULL;
	}
	return target;
}

static urma_status_t vdev_unimport_jetty(urma_target_jetty_t *target)
{
	struct vdev_target_jetty *private;
	int ret;

	if (!target)
		return URMA_EINVAL;
	private = (struct vdev_target_jetty *)target;
	ret = urma_cmd_unimport_jetty(target);
	if (ret == 0) {
		/* Explicit unimport may cascade an official unbind in uburma. */
		if (private->bound_jetty &&
		    private->bound_jetty->remote_jetty == target)
			private->bound_jetty->remote_jetty = NULL;
		free(private);
	}
	return status_from_cmd(ret);
}

static urma_status_t vdev_bind_jetty(urma_jetty_t *jetty,
				      urma_target_jetty_t *target)
{
	urma_cmd_udrv_priv_t udata = {0};
	int ret;

	if (!jetty || !target)
		return URMA_EINVAL;
	ret = urma_cmd_bind_jetty(jetty, target, &udata);
	if (ret == 0) {
		jetty->remote_jetty = target;
		((struct vdev_target_jetty *)target)->bound_jetty = jetty;
	}
	return status_from_cmd(ret);
}

static urma_status_t vdev_unbind_jetty(urma_jetty_t *jetty)
{
	urma_target_jetty_t *target;
	int ret;

	if (!jetty)
		return URMA_EINVAL;
	target = jetty->remote_jetty;
	ret = urma_cmd_unbind_jetty(jetty);
	if (ret == 0) {
		if (target &&
		    ((struct vdev_target_jetty *)target)->bound_jetty == jetty)
			((struct vdev_target_jetty *)target)->bound_jetty = NULL;
		jetty->remote_jetty = NULL;
	}
	return status_from_cmd(ret);
}

static urma_ops_t g_ops = {
	.name = "OPENURMA_VDEV_Q6_BYTE_TRUE_RW",
	.create_jfc = vdev_create_jfc,
	.delete_jfc = vdev_delete_jfc,
	.create_jfs = vdev_create_jfs,
	.delete_jfs = vdev_delete_jfs,
	.create_jfr = vdev_create_jfr,
	.delete_jfr = vdev_delete_jfr,
	.create_jetty = vdev_create_jetty,
	.delete_jetty = vdev_delete_jetty,
	.register_seg = vdev_register_seg,
	.unregister_seg = vdev_unregister_seg,
	.import_seg = vdev_import_seg,
	.unimport_seg = vdev_unimport_seg,
	.import_jetty = vdev_import_jetty,
	.unimport_jetty = vdev_unimport_jetty,
	.bind_jetty = vdev_bind_jetty,
	.unbind_jetty = vdev_unbind_jetty,
	.alloc_token_id = vdev_alloc_token_id,
	.free_token_id = vdev_free_token_id,
	.post_jfs_wr = vdev_post_jfs_wr,
	.post_jfr_wr = vdev_post_jfr_wr,
	.poll_jfc = vdev_poll_jfc,
};

static urma_provider_ops_t openurma_vdev_provider_ops = {
	.name = OPENURMA_VDEV_PROVIDER_NAME,
	.attr = {
		.version = 1,
		.transport_type = URMA_TRANSPORT_UB,
	},
	.match_table = NULL,
	.init = openurma_vdev_init,
	.uninit = openurma_vdev_uninit,
	.query_device = openurma_vdev_query_device,
	.create_context = openurma_vdev_create_context,
	.delete_context = openurma_vdev_delete_context,
};

static __attribute__((constructor)) void openurma_vdev_register(void)
{
	const char *log_env = getenv("OPENURMA_VDEV_LOG");
	int ret;

	log_enabled = log_env != NULL && strcmp(log_env, "0") != 0;
	ret = urma_register_provider_ops(&openurma_vdev_provider_ops);
	VDEV_LOG("[openurma_vdev] register ret=%d\n", ret);
}

static __attribute__((destructor)) void openurma_vdev_unregister(void)
{
	int ret = urma_unregister_provider_ops(&openurma_vdev_provider_ops);

	VDEV_LOG("[openurma_vdev] unregister ret=%d\n", ret);
}
