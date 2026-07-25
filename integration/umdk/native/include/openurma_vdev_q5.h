/* SPDX-License-Identifier: MIT */
#ifndef OPENURMA_VDEV_Q5_H
#define OPENURMA_VDEV_Q5_H

#include "openurma_vdev_abi.h"
#include "urma_api.h"

struct openurma_vdev_q5_context_info {
	struct openurma_vdev_context_resp response;
	struct openurma_vdev_control_page *control;
	struct openurma_vdev_sqe *sq;
	struct openurma_vdev_cqe *cq;
	int dev_fd;
};

int openurma_vdev_q5_context_info(urma_context_t *ctx,
	struct openurma_vdev_q5_context_info *info);
int openurma_vdev_q5_post_nop(urma_context_t *ctx, uint64_t wr_id);
int openurma_vdev_q5_poll(urma_context_t *ctx,
	struct openurma_vdev_cqe *cqe);
int openurma_vdev_q5_mr_info(urma_target_seg_t *seg,
	struct openurma_vdev_mr_resp *info);
int openurma_vdev_q5_validate_mr(urma_context_t *ctx, uint32_t token_id,
	uint64_t generation);
int openurma_vdev_q5_validate_mr_eid(urma_context_t *ctx, uint32_t token_id,
	uint64_t generation, uint32_t eid_index);

#endif
