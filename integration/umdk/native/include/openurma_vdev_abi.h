/* SPDX-License-Identifier: MIT */
#ifndef OPENURMA_VDEV_ABI_H
#define OPENURMA_VDEV_ABI_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
#define OV_U8 __u8
#define OV_U16 __u16
#define OV_U32 __u32
#define OV_U64 __u64
#else
#include <stddef.h>
#include <stdint.h>
#define OV_U8 uint8_t
#define OV_U16 uint16_t
#define OV_U32 uint32_t
#define OV_U64 uint64_t
#endif

#define OV_CACHE_ALIGNED __attribute__((aligned(64)))

#define OPENURMA_VDEV_ABI_MAGIC 0x4f565135U /* "OVQ5" */
#define OPENURMA_VDEV_ABI_MAJOR 1U
#define OPENURMA_VDEV_ABI_MINOR 4U
#define OPENURMA_VDEV_ABI_VERSION ((OPENURMA_VDEV_ABI_MAJOR << 16) | OPENURMA_VDEV_ABI_MINOR)

#define OPENURMA_VDEV_FEATURE_SAFE_MR       (1ULL << 0)
#define OPENURMA_VDEV_FEATURE_CONTEXT_MMAP  (1ULL << 1)
#define OPENURMA_VDEV_FEATURE_SQ_CQ         (1ULL << 2)
#define OPENURMA_VDEV_FEATURE_NOP_SELFTEST  (1ULL << 3)
#define OPENURMA_VDEV_FEATURE_FORK_NOCOPY   (1ULL << 4)
#define OPENURMA_VDEV_FEATURE_BYTE_TRUE_RW  (1ULL << 5)
#define OPENURMA_VDEV_FEATURE_REAL_CQE      (1ULL << 6)
#define OPENURMA_VDEV_FEATURE_SEND_RECV     (1ULL << 7)
#define OPENURMA_VDEV_FEATURE_ATOMIC        (1ULL << 8)
#define OPENURMA_VDEV_FEATURE_DOORBELL      (1ULL << 9)
#define OPENURMA_VDEV_FEATURE_TIMED_MODEL   (1ULL << 10)
#define OPENURMA_VDEV_ABI_FEATURES (OPENURMA_VDEV_FEATURE_SAFE_MR | \
	OPENURMA_VDEV_FEATURE_CONTEXT_MMAP | OPENURMA_VDEV_FEATURE_SQ_CQ | \
	OPENURMA_VDEV_FEATURE_NOP_SELFTEST | OPENURMA_VDEV_FEATURE_FORK_NOCOPY | \
	OPENURMA_VDEV_FEATURE_BYTE_TRUE_RW | OPENURMA_VDEV_FEATURE_REAL_CQE | \
	OPENURMA_VDEV_FEATURE_SEND_RECV | OPENURMA_VDEV_FEATURE_ATOMIC | \
	OPENURMA_VDEV_FEATURE_DOORBELL | OPENURMA_VDEV_FEATURE_TIMED_MODEL)

#define OPENURMA_VDEV_PAGE_SIZE 4096U
#define OPENURMA_VDEV_RING_DEPTH 64U
#define OPENURMA_VDEV_CONTROL_BYTES OPENURMA_VDEV_PAGE_SIZE
#define OPENURMA_VDEV_SQ_BYTES (2U * OPENURMA_VDEV_PAGE_SIZE)
#define OPENURMA_VDEV_CQ_BYTES OPENURMA_VDEV_PAGE_SIZE
#define OPENURMA_VDEV_CONTEXT_MMAP_STRIDE_PAGES 8U
#define OPENURMA_VDEV_SQE_WRITE 0x00U
#define OPENURMA_VDEV_SQE_READ 0x10U
#define OPENURMA_VDEV_SQE_CAS 0x20U
#define OPENURMA_VDEV_SQE_FADD 0x22U
#define OPENURMA_VDEV_SQE_SEND 0x40U
#define OPENURMA_VDEV_SQE_NOP 0x51U
#define OPENURMA_VDEV_CQE_SUCCESS 0U
#define OPENURMA_VDEV_CQE_BAD_OPCODE 1U
#define OPENURMA_VDEV_CQE_BAD_GENERATION 2U
#define OPENURMA_VDEV_CQE_STALE_LOCAL_MR 3U
#define OPENURMA_VDEV_CQE_STALE_REMOTE_MR 4U
#define OPENURMA_VDEV_CQE_LOCAL_RANGE 5U
#define OPENURMA_VDEV_CQE_REMOTE_RANGE 6U
#define OPENURMA_VDEV_CQE_REMOTE_ACCESS 7U
#define OPENURMA_VDEV_CQE_COPY_ERROR 8U
#define OPENURMA_VDEV_CQE_CONTEXT_CLOSING 9U
#define OPENURMA_VDEV_CQE_RNR 10U
#define OPENURMA_VDEV_CQE_REMOTE_RESP_LEN 11U
#define OPENURMA_VDEV_CQE_CQ_FULL 12U
#define OPENURMA_VDEV_CQE_FLAG_RECV (1U << 0)
#define OPENURMA_VDEV_USER_CTL_VALIDATE_MR 0x51350001U
#define OPENURMA_VDEV_USER_CTL_POST_RECV 0x51350002U
#define OPENURMA_VDEV_USER_CTL_DOORBELL 0x51350003U
#define OPENURMA_VDEV_CONTEXT_FAIL_WORKER (1U << 0)
#define OPENURMA_VDEV_DEVICE_READY 1U

#define OPENURMA_VDEV_MR_ALLOCATING 1U
#define OPENURMA_VDEV_MR_LIVE 2U
#define OPENURMA_VDEV_MR_CLOSING 3U
#define OPENURMA_VDEV_MR_DEAD 4U

struct openurma_vdev_context_req {
	OV_U32 magic;
	OV_U16 abi_major;
	OV_U16 abi_minor;
	OV_U64 features;
	OV_U32 flags;
	OV_U32 reserved;
};

struct openurma_vdev_context_resp {
	OV_U32 magic;
	OV_U16 abi_major;
	OV_U16 abi_minor;
	OV_U64 features;
	OV_U64 context_id;
	OV_U32 eid_index;
	OV_U32 header_size;
	OV_U64 control_pgoff;
	OV_U64 sq_pgoff;
	OV_U64 cq_pgoff;
	OV_U32 page_size;
	OV_U32 ring_size;
	OV_U32 control_bytes;
	OV_U32 sq_bytes;
	OV_U32 cq_bytes;
	OV_U32 sqe_size;
	OV_U32 cqe_size;
	OV_U64 statistics_generation;
	OV_U32 device_state;
	OV_U32 fatal_status;
	OV_U64 reserved[3];
};

struct OV_CACHE_ALIGNED openurma_vdev_control_page {
	OV_U32 magic;
	OV_U16 abi_major;
	OV_U16 abi_minor;
	OV_U32 header_size;
	OV_U32 device_state;
	OV_U64 features;
	OV_U64 context_id;
	OV_U64 statistics_generation;
	OV_U32 eid_index;
	OV_U32 fatal_status;
	OV_U32 sq_size;
	OV_U32 sq_mask;
	OV_U32 cq_size;
	OV_U32 cq_mask;
	OV_U32 sqe_size;
	OV_U32 cqe_size;
	OV_U64 generation;
	OV_U64 sq_head;
	OV_U64 sq_tail;
	OV_U64 cq_head;
	OV_U64 cq_tail;
	OV_U64 sq_full;
	OV_U64 cq_full;
	OV_U64 sq_submitted;
	OV_U64 sq_consumed;
	OV_U64 cq_produced;
	OV_U64 cq_consumed;
	OV_U64 worker_wakeups;
	OV_U64 worker_starts;
	OV_U64 worker_stops;
	OV_U64 errors;
	OV_U64 reserved[8];
};

struct OV_CACHE_ALIGNED openurma_vdev_sqe {
	OV_U64 wr_id;
	OV_U64 sequence;
	OV_U64 generation;
	OV_U32 opcode;
	OV_U32 flags;
	OV_U32 local_eid;
	OV_U32 local_token;
	OV_U64 local_va;
	OV_U32 remote_eid;
	OV_U32 remote_token;
	OV_U64 remote_va;
	OV_U64 length;
	OV_U32 local_jfs_id;
	OV_U32 jfc_id;
	OV_U64 user_ctx;
	OV_U32 immediate;
	OV_U32 reserved0;
	/* Atomic operands live in the trailing 16-byte timestamp/reserved slot so
	 * the MR generations remain present for stale-handle validation. */
	OV_U64 local_generation;
	OV_U64 remote_generation;
	union {
		struct {
			OV_U64 submit_timestamp;
			OV_U64 reserved[1];
		};
		struct {
			OV_U64 atomic_compare;
			OV_U64 atomic_swap_add;
		};
	};
};

struct OV_CACHE_ALIGNED openurma_vdev_cqe {
	OV_U64 wr_id;
	OV_U64 sequence;
	OV_U64 generation;
	OV_U32 status;
	OV_U32 completion_len;
	OV_U32 error_detail;
	OV_U32 jfc_id;
	OV_U64 user_ctx;
	OV_U32 local_jfs_id;
	OV_U32 flags;
	OV_U64 completion_timestamp;
};

struct openurma_vdev_mr_resp {
	OV_U32 magic;
	OV_U16 abi_major;
	OV_U16 abi_minor;
	OV_U64 generation;
	OV_U64 va;
	OV_U64 len;
	OV_U32 token_id;
	OV_U32 access;
	OV_U32 pinned_pages;
	OV_U32 eid_index;
	OV_U32 state;
	OV_U32 reserved0;
	OV_U64 reserved[1];
};

struct openurma_vdev_mr_validate {
	OV_U64 generation;
	OV_U32 token_id;
	OV_U32 eid_index;
	OV_U64 reserved;
};

struct openurma_vdev_doorbell {
	OV_U64 context_id;
	OV_U64 sq_tail;
};

struct openurma_vdev_recv_post {
	OV_U32 jfr_id;
	OV_U32 jfc_id;
	OV_U32 local_eid;
	OV_U32 local_token;
	OV_U64 local_generation;
	OV_U64 local_va;
	OV_U64 length;
	OV_U64 user_ctx;
	OV_U64 reserved[2];
};

#ifdef __KERNEL__
#include <linux/build_bug.h>
static_assert(sizeof(struct openurma_vdev_context_resp) == 128);
static_assert(sizeof(struct openurma_vdev_control_page) == 256);
static_assert(sizeof(struct openurma_vdev_sqe) == 128);
static_assert(sizeof(struct openurma_vdev_cqe) == 64);
static_assert(sizeof(struct openurma_vdev_mr_resp) == 64);
static_assert(sizeof(struct openurma_vdev_doorbell) == 16);
static_assert(sizeof(struct openurma_vdev_recv_post) == 64);
static_assert(sizeof(struct openurma_vdev_context_req) == 24);
static_assert(__alignof__(struct openurma_vdev_context_resp) == 8);
static_assert(__alignof__(struct openurma_vdev_control_page) == 64);
static_assert(__alignof__(struct openurma_vdev_sqe) == 64);
static_assert(__alignof__(struct openurma_vdev_cqe) == 64);
static_assert(__alignof__(struct openurma_vdev_mr_resp) == 8);
static_assert(offsetof(struct openurma_vdev_context_resp, features) == 8);
static_assert(offsetof(struct openurma_vdev_context_resp, control_pgoff) == 32);
static_assert(offsetof(struct openurma_vdev_context_resp, statistics_generation) == 88);
static_assert(offsetof(struct openurma_vdev_control_page, context_id) == 24);
static_assert(offsetof(struct openurma_vdev_control_page, sq_head) == 80);
static_assert(offsetof(struct openurma_vdev_control_page, cq_tail) == 104);
static_assert(offsetof(struct openurma_vdev_sqe, generation) == 16);
static_assert(offsetof(struct openurma_vdev_sqe, local_generation) == 96);
static_assert(offsetof(struct openurma_vdev_cqe, generation) == 16);
static_assert(offsetof(struct openurma_vdev_cqe, completion_len) == 28);
static_assert(offsetof(struct openurma_vdev_mr_resp, generation) == 8);
static_assert((OPENURMA_VDEV_RING_DEPTH & (OPENURMA_VDEV_RING_DEPTH - 1)) == 0);
static_assert(OPENURMA_VDEV_RING_DEPTH * sizeof(struct openurma_vdev_sqe) ==
	OPENURMA_VDEV_SQ_BYTES);
static_assert(OPENURMA_VDEV_RING_DEPTH * sizeof(struct openurma_vdev_cqe) ==
	OPENURMA_VDEV_CQ_BYTES);
#else
_Static_assert(sizeof(struct openurma_vdev_context_resp) == 128, "context ABI size");
_Static_assert(sizeof(struct openurma_vdev_control_page) == 256, "control ABI size");
_Static_assert(sizeof(struct openurma_vdev_sqe) == 128, "SQE ABI size");
_Static_assert(sizeof(struct openurma_vdev_cqe) == 64, "CQE ABI size");
_Static_assert(sizeof(struct openurma_vdev_mr_resp) == 64, "MR ABI size");
_Static_assert(sizeof(struct openurma_vdev_recv_post) == 64, "recv post ABI size");
_Static_assert(sizeof(struct openurma_vdev_context_req) == 24, "context request ABI size");
_Static_assert(_Alignof(struct openurma_vdev_context_resp) == 8, "context ABI alignment");
_Static_assert(_Alignof(struct openurma_vdev_control_page) == 64, "control ABI alignment");
_Static_assert(_Alignof(struct openurma_vdev_sqe) == 64, "SQE ABI alignment");
_Static_assert(_Alignof(struct openurma_vdev_cqe) == 64, "CQE ABI alignment");
_Static_assert(_Alignof(struct openurma_vdev_mr_resp) == 8, "MR ABI alignment");
_Static_assert(offsetof(struct openurma_vdev_context_resp, features) == 8, "context features offset");
_Static_assert(offsetof(struct openurma_vdev_context_resp, control_pgoff) == 32, "context mmap offset");
_Static_assert(offsetof(struct openurma_vdev_context_resp, statistics_generation) == 88, "context statistics offset");
_Static_assert(offsetof(struct openurma_vdev_control_page, context_id) == 24, "control context offset");
_Static_assert(offsetof(struct openurma_vdev_control_page, sq_head) == 80, "control SQ head offset");
_Static_assert(offsetof(struct openurma_vdev_control_page, cq_tail) == 104, "control CQ tail offset");
_Static_assert(offsetof(struct openurma_vdev_sqe, generation) == 16, "SQE generation offset");
_Static_assert(offsetof(struct openurma_vdev_sqe, local_generation) == 96, "SQE MR generation offset");
_Static_assert(offsetof(struct openurma_vdev_cqe, generation) == 16, "CQE generation offset");
_Static_assert(offsetof(struct openurma_vdev_cqe, completion_len) == 28, "CQE length offset");
_Static_assert(offsetof(struct openurma_vdev_mr_resp, generation) == 8, "MR generation offset");
_Static_assert((OPENURMA_VDEV_RING_DEPTH & (OPENURMA_VDEV_RING_DEPTH - 1)) == 0, "ring depth power of two");
_Static_assert(OPENURMA_VDEV_RING_DEPTH * sizeof(struct openurma_vdev_sqe) ==
	OPENURMA_VDEV_SQ_BYTES, "SQ byte size");
_Static_assert(OPENURMA_VDEV_RING_DEPTH * sizeof(struct openurma_vdev_cqe) ==
	OPENURMA_VDEV_CQ_BYTES, "CQ byte size");
#endif

#endif
