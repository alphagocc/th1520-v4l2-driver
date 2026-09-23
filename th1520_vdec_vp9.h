/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 th1520-v4l2 contributors */
#ifndef TH1520_VDEC_VP9_H_
#define TH1520_VDEC_VP9_H_

#include <media/v4l2-vp9.h>

#include "th1520_vdec_vp9_probs.h"

struct th1520_vp9_frame_info {
	bool valid;
	u8 frame_context_idx;
	u8 reference_mode;
	u8 tx_mode;
	u8 interpolation_filter;
	u32 flags;
	u32 width;
	u32 height;
};

struct th1520_vdec_vp9_ctx {
	struct th1520_vdec_aux_buf probs;
	struct th1520_vdec_aux_buf counts;
	struct th1520_vdec_aux_buf tiles;
	struct th1520_vdec_aux_buf tile_filter;
	struct th1520_vdec_aux_buf tile_bsd;
	struct th1520_vdec_aux_buf segment_map;
	struct th1520_vdec_aux_buf previous_mv;
	struct v4l2_vp9_frame_symbol_counts cnts;
	struct v4l2_vp9_frame_context probability_tables;
	struct v4l2_vp9_frame_context frame_context[4];
	/* Publish resets and adaptation only after a successful decode. */
	struct v4l2_vp9_frame_context working_context[4];
	struct th1520_vp9_frame_info cur;
	struct th1520_vp9_frame_info last;
	struct v4l2_ctrl_vp9_frame frame;
	struct v4l2_vp9_segmentation segmentation;
	struct v4l2_vp9_segmentation next_segmentation;
	u32 segment_map_size;
	u8 active_segment;
	u8 next_segment;
	bool need_keyframe;
};

void th1520_vp9_init_counts(struct th1520_vdec_ctx *ctx);
void th1520_vp9_pack_probs(struct th1520_vdec_ctx *ctx,
			 const struct v4l2_ctrl_vp9_frame *frame);
void th1520_vp9_update_probs(struct th1520_vdec_ctx *ctx);

#endif
