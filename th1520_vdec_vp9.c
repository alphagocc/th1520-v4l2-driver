// SPDX-License-Identifier: GPL-2.0-only
/*
 * TH1520 VC8000D VP9 stateless decoder, hardware mode 13.
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 * Codec helpers adapted from Linux hantro_g2_vp9_dec.c:
 * Copyright (C) 2021 Collabora Ltd.
 * Linux commit 8ba098e6b6ff0db8edf28528d1552be261af30d4.
 * Register coordinates and buffer sizes were checked independently against
 * the TH1520 SDK. See docs/vp9.md for sources and validation limits.
 */

#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <media/videobuf2-dma-contig.h>

#include "th1520_vdec.h"
#include "th1520_vdec_vp9.h"
#include "th1520_vdec_vp9_regs.h"

#define INTRA_FRAME  0
#define LAST_FRAME   1
#define GOLDEN_FRAME 2
#define ALTREF_FRAME 3

/* Vp9AsicAllocateMem: 3744 bytes of probabilities, 13264 of counters. */
static_assert(sizeof(struct th1520_vp9_all_probs) == 3744);
static_assert(sizeof(struct th1520_vp9_counts) == 13264);

static void config_loop_filter(struct th1520_vdec_ctx *ctx, const struct v4l2_ctrl_vp9_frame *dec_params)
{
	bool d = dec_params->lf.flags & V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED;

	th1520_vdec_reg_write(ctx->dev, &vp9_filt_level, dec_params->lf.level);
	th1520_vdec_reg_write(ctx->dev, &hevc_filtering_dis, dec_params->lf.level == 0);
	th1520_vdec_reg_write(ctx->dev, &vp9_filt_sharpness, dec_params->lf.sharpness);

	th1520_vdec_reg_write(ctx->dev, &vp9_filt_ref_adj_0, d ? dec_params->lf.ref_deltas[0] : 0);
	th1520_vdec_reg_write(ctx->dev, &vp9_filt_ref_adj_1, d ? dec_params->lf.ref_deltas[1] : 0);
	th1520_vdec_reg_write(ctx->dev, &vp9_filt_ref_adj_2, d ? dec_params->lf.ref_deltas[2] : 0);
	th1520_vdec_reg_write(ctx->dev, &vp9_filt_ref_adj_3, d ? dec_params->lf.ref_deltas[3] : 0);
	th1520_vdec_reg_write(ctx->dev, &vp9_filt_mb_adj_0, d ? dec_params->lf.mode_deltas[0] : 0);
	th1520_vdec_reg_write(ctx->dev, &vp9_filt_mb_adj_1, d ? dec_params->lf.mode_deltas[1] : 0);
}

static inline bool is_lossless(const struct v4l2_vp9_quantization *quant)
{
	return quant->base_q_idx == 0 && quant->delta_q_uv_ac == 0 &&
	       quant->delta_q_uv_dc == 0 && quant->delta_q_y_dc == 0;
}

static void
config_quant(struct th1520_vdec_ctx *ctx, const struct v4l2_ctrl_vp9_frame *dec_params)
{
	th1520_vdec_reg_write(ctx->dev, &vp9_qp_delta_y_dc, dec_params->quant.delta_q_y_dc);
	th1520_vdec_reg_write(ctx->dev, &vp9_qp_delta_ch_dc, dec_params->quant.delta_q_uv_dc);
	th1520_vdec_reg_write(ctx->dev, &vp9_qp_delta_ch_ac, dec_params->quant.delta_q_uv_ac);
	th1520_vdec_reg_write(ctx->dev, &vp9_lossless_e, is_lossless(&dec_params->quant));
}

static u32
th1520_interp_filter(unsigned int interpolation_filter)
{
	switch (interpolation_filter) {
	case V4L2_VP9_INTERP_FILTER_EIGHTTAP:
		return 0x1;
	case V4L2_VP9_INTERP_FILTER_EIGHTTAP_SMOOTH:
		return 0;
	case V4L2_VP9_INTERP_FILTER_EIGHTTAP_SHARP:
		return 0x2;
	case V4L2_VP9_INTERP_FILTER_BILINEAR:
		return 0x3;
	case V4L2_VP9_INTERP_FILTER_SWITCHABLE:
		return 0x4;
	}

	return 0;
}

static void
config_compound_reference(struct th1520_vdec_ctx *ctx,
			  const struct v4l2_ctrl_vp9_frame *dec_params)
{
	u32 comp_fixed_ref, comp_var_ref[2];
	bool last_ref_frame_sign_bias;
	bool golden_ref_frame_sign_bias;
	bool alt_ref_frame_sign_bias;
	bool comp_ref_allowed = 0;

	comp_fixed_ref = 0;
	comp_var_ref[0] = 0;
	comp_var_ref[1] = 0;

	last_ref_frame_sign_bias = dec_params->ref_frame_sign_bias & V4L2_VP9_SIGN_BIAS_LAST;
	golden_ref_frame_sign_bias = dec_params->ref_frame_sign_bias & V4L2_VP9_SIGN_BIAS_GOLDEN;
	alt_ref_frame_sign_bias = dec_params->ref_frame_sign_bias & V4L2_VP9_SIGN_BIAS_ALT;

	/* 6.3.12 Frame reference mode syntax */
	comp_ref_allowed |= golden_ref_frame_sign_bias != last_ref_frame_sign_bias;
	comp_ref_allowed |= alt_ref_frame_sign_bias != last_ref_frame_sign_bias;

	if (comp_ref_allowed) {
		if (last_ref_frame_sign_bias ==
		    golden_ref_frame_sign_bias) {
			comp_fixed_ref = ALTREF_FRAME;
			comp_var_ref[0] = LAST_FRAME;
			comp_var_ref[1] = GOLDEN_FRAME;
		} else if (last_ref_frame_sign_bias ==
			   alt_ref_frame_sign_bias) {
			comp_fixed_ref = GOLDEN_FRAME;
			comp_var_ref[0] = LAST_FRAME;
			comp_var_ref[1] = ALTREF_FRAME;
		} else {
			comp_fixed_ref = LAST_FRAME;
			comp_var_ref[0] = GOLDEN_FRAME;
			comp_var_ref[1] = ALTREF_FRAME;
		}
	}

	th1520_vdec_reg_write(ctx->dev, &vp9_comp_pred_fixed_ref, comp_fixed_ref);
	th1520_vdec_reg_write(ctx->dev, &vp9_comp_pred_var_ref0, comp_var_ref[0]);
	th1520_vdec_reg_write(ctx->dev, &vp9_comp_pred_var_ref1, comp_var_ref[1]);
}

/* Vp9CalculateBufSize: native 4x4 tiles, 64-byte alignment, 1024/SB MV. */
static u32 vp9_stride(u32 width)
{
	return ALIGN(ALIGN(width, 8) * 4, 64);
}

static u32 vp9_chroma_offset(u32 width, u32 height)
{
	return ALIGN(vp9_stride(width) * ALIGN(height, 8) / 4, 64);
}

static u32 vp9_mv_offset(u32 width, u32 height)
{
	u32 luma = vp9_chroma_offset(width, height);

	return luma + ALIGN(vp9_stride(width) * ALIGN(height, 8) / 8, 64) + 64;
}

static u32 vp9_mv_size(u32 width, u32 height)
{
	return DIV_ROUND_UP(width, 64) * DIV_ROUND_UP(height, 64) * 1024;
}

size_t th1520_vdec_vp9_native_size(const struct th1520_vdec_ctx *ctx)
{
	return vp9_mv_offset(ctx->src_fmt.width, ctx->src_fmt.height) +
		vp9_mv_size(ctx->src_fmt.width, ctx->src_fmt.height);
}

static void vp9_config_output(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct th1520_vdec_buffer *dst =
		th1520_vdec_vbuf_to_buffer(th1520_vdec_get_dst_buf(ctx));
	u32 width = ctx->vp9->cur.width, height = ctx->vp9->cur.height;

	dst->vp9.width = width;
	dst->vp9.height = height;
	dst->vp9.stride = vp9_stride(width);
	dst->vp9.chroma_offset = vp9_chroma_offset(width, height);
	dst->vp9.mv_offset = vp9_mv_offset(width, height);
	dst->vp9.valid = false;
	/* Vp9AsicInitPicture clears the 32-byte sync area before every frame. */
	memset(dst->native.cpu + dst->vp9.mv_offset - 32, 0, 32);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_OUT_LUMA,
				   dst->native.dma);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_OUT_CHROMA,
				   dst->native.dma + dst->vp9.chroma_offset);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_OUT_MV,
				   dst->native.dma + dst->vp9.mv_offset);
	th1520_vdec_reg_write(vpu, &hevc_native_luma_stride, dst->vp9.stride);
	th1520_vdec_reg_write(vpu, &hevc_native_chroma_stride, dst->vp9.stride);
}

static bool vp9_intra(const struct v4l2_ctrl_vp9_frame *f)
{
	return f->flags & (V4L2_VP9_FRAME_FLAG_KEY_FRAME |
			   V4L2_VP9_FRAME_FLAG_INTRA_ONLY);
}

static int vp9_config_refs(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct th1520_vdec_vp9_ctx *vp9 = ctx->vp9;
	const struct v4l2_ctrl_vp9_frame *f = &vp9->frame;
	struct vb2_queue *q = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	struct vb2_v4l2_buffer *dst = th1520_vdec_get_dst_buf(ctx);
	const u64 timestamps[] = { f->last_frame_ts, f->golden_frame_ts,
				   f->alt_frame_ts };
	const unsigned int slots[] = { 0, 4, 5 };
	unsigned int i;
	bool temporal;

	if (vp9_intra(f))
		return 0;
	for (i = 0; i < ARRAY_SIZE(slots); i++) {
		struct th1520_vdec_buffer *ref;
		struct vb2_buffer *vb = vb2_find_buffer(q, timestamps[i]);
		u32 width, height;

		if (!vb || vb == &dst->vb2_buf)
			return -EINVAL;
		ref = th1520_vdec_vbuf_to_buffer(to_vb2_v4l2_buffer(vb));
		if (!ref->native.cpu || !ref->vp9.valid)
			return -EINVAL;
		width = ref->vp9.width;
		height = ref->vp9.height;
		/* VP9 valid_ref_frame_size(), using the original coded dimensions. */
		if (width > 2 * vp9->cur.width || height > 2 * vp9->cur.height ||
		    vp9->cur.width > 16 * width || vp9->cur.height > 16 * height)
			return -EINVAL;
		th1520_vdec_reg_write_raw(vpu, 33 + i, (width << 16) | height);
		th1520_vdec_reg_write_raw(vpu, 36 + i,
			(((width << 14) / vp9->cur.width) << 16) |
			((height << 14) / vp9->cur.height));
		/* SDK has explicit Y/C strides for LAST, GOLDEN and ALTREF. */
		th1520_vdec_reg_write_raw(vpu, 42 + i,
			(ref->vp9.stride << 16) | ref->vp9.stride);
		th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_REF_LUMA(slots[i]),
					   ref->native.dma);
		th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_REF_CHROMA(slots[i]),
					   ref->native.dma + ref->vp9.chroma_offset);
		/* Slot 0 is previous decode MV; slot 1 holds LAST reference MV. */
		th1520_vdec_write_addr_pair(vpu,
			TH1520_HEVC_ADDR_REF_MV(i ? slots[i] : 1),
			ref->native.dma + ref->vp9.mv_offset);
	}
	temporal = vp9->last.valid &&
		!(f->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT) &&
		!(vp9->last.flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) &&
		(vp9->last.flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME) &&
		vp9->last.width == vp9->cur.width &&
		vp9->last.height == vp9->cur.height;
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_REF_MV(0),
				   vp9->previous_mv.dma);
	th1520_vdec_reg_write(vpu, &hevc_tempor_mvp_e, temporal);
	th1520_vdec_reg_write(vpu, &vp9_last_sign_bias,
		!!(f->ref_frame_sign_bias & V4L2_VP9_SIGN_BIAS_LAST));
	th1520_vdec_reg_write(vpu, &vp9_gref_sign_bias,
		!!(f->ref_frame_sign_bias & V4L2_VP9_SIGN_BIAS_GOLDEN));
	th1520_vdec_reg_write(vpu, &vp9_aref_sign_bias,
		!!(f->ref_frame_sign_bias & V4L2_VP9_SIGN_BIAS_ALT));
	return 0;
}

static void vp9_config_tiles(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct th1520_vdec_vp9_ctx *vp9 = ctx->vp9;
	u32 cols = 1U << vp9->frame.tile_cols_log2;
	u32 rows = 1U << vp9->frame.tile_rows_log2;
	u32 sb_cols = DIV_ROUND_UP(vp9->cur.width, 64);
	u32 sb_rows = DIV_ROUND_UP(vp9->cur.height, 64);
	u32 row, col, first = 0, previous = 0;
	__le16 *sizes = vp9->tiles.cpu;

	/* Vp9AsicSetTileInfoRegs skips and merges leading empty tile rows. */
	if (rows == sb_rows + 1)
		first = 1;
	else if (rows == sb_rows + 2)
		first = 2;
	memset(sizes, 0, vp9->tiles.size);
	for (row = first; row < rows; row++) {
		u32 end = (row + 1) * sb_rows / rows;

		for (col = 0; col < cols; col++) {
			*sizes++ = cpu_to_le16((col + 1) * sb_cols / cols -
					       col * sb_cols / cols);
			*sizes++ = cpu_to_le16(end - previous);
		}
		previous = end;
	}
	th1520_vdec_reg_write(vpu, &hevc_tile_enable, cols > 1 || rows > 1);
	th1520_vdec_reg_write(vpu, &hevc_num_tile_cols_8k, cols);
	th1520_vdec_reg_write(vpu, &hevc_num_tile_rows_8k,
			     sb_rows > 2 ? min(rows, sb_rows) : rows);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_TILE_SIZES, vp9->tiles.dma);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_TILE_FILTER,
				   vp9->tile_filter.dma);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_TILE_BSD, vp9->tile_bsd.dma);
}

static void vp9_config_segmentation(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct th1520_vdec_vp9_ctx *vp9 = ctx->vp9;
	const struct v4l2_ctrl_vp9_frame *f = &vp9->frame;
	struct v4l2_vp9_segmentation *seg = &vp9->next_segmentation;
	bool enabled = f->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED;
	bool update = enabled && (f->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP);
	bool absolute;
	u32 i;

	*seg = vp9->segmentation;
	if (vp9_intra(f) || (f->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT)) {
		memset(seg, 0, sizeof(*seg));
		memset(vp9->segment_map.cpu, 0, vp9->segment_map.size);
	} else if (vp9->last.valid && (vp9->last.width != vp9->cur.width ||
				     vp9->last.height != vp9->cur.height)) {
		/* SDK Vp9AsicRun clears the map when its superblock grid changes. */
		memset(vp9->segment_map.cpu, 0, vp9->segment_map.size);
	}
	if (enabled && (f->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_UPDATE_DATA))
		*seg = f->seg;
	absolute = seg->flags & V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE;
	th1520_vdec_reg_write(vpu, &vp9_segment_e, enabled);
	th1520_vdec_reg_write(vpu, &vp9_segment_upd_e, update);
	th1520_vdec_reg_write(vpu, &vp9_segment_temp_upd_e, update &&
		!!(f->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE));
	for (i = 0; i < 8; i++) {
		u8 features = enabled ? seg->feature_enabled[i] : 0;
		s32 quant = f->quant.base_q_idx, filter = f->lf.level;
		u32 ref = 0, skip;
		u16 reg = i < 6 ? 14 + i : 31 + i - 6;

		if (features & V4L2_VP9_SEGMENT_FEATURE_ENABLED(V4L2_VP9_SEG_LVL_ALT_Q))
			quant = clamp_t(s32, (absolute ? 0 : quant) +
				seg->feature_data[i][V4L2_VP9_SEG_LVL_ALT_Q], 0, 255);
		if (features & V4L2_VP9_SEGMENT_FEATURE_ENABLED(V4L2_VP9_SEG_LVL_ALT_L))
			filter = clamp_t(s32, (absolute ? 0 : filter) +
				seg->feature_data[i][V4L2_VP9_SEG_LVL_ALT_L], 0, 63);
		if (!(f->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) &&
		    (features & V4L2_VP9_SEGMENT_FEATURE_ENABLED(V4L2_VP9_SEG_LVL_REF_FRAME)))
			ref = seg->feature_data[i][V4L2_VP9_SEG_LVL_REF_FRAME] + 1;
		skip = !!(features & V4L2_VP9_SEGMENT_FEATURE_ENABLED(V4L2_VP9_SEG_LVL_SKIP));
		th1520_vdec_reg_write_raw(vpu, reg,
			(vpu->regs[reg] & ~GENMASK(17, 0)) |
			(ref << 15) | (skip << 14) | (filter << 8) | quant);
	}
	vp9->next_segment = update ? 1 - vp9->active_segment : vp9->active_segment;
	th1520_vdec_write_addr_pair(vpu, TH1520_VP9_ADDR_SEG_READ,
		vp9->segment_map.dma + vp9->active_segment * vp9->segment_map_size);
	th1520_vdec_write_addr_pair(vpu, TH1520_VP9_ADDR_SEG_WRITE,
		vp9->segment_map.dma + (1 - vp9->active_segment) * vp9->segment_map_size);
}

static void vp9_config_frame(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct th1520_vdec_vp9_ctx *vp9 = ctx->vp9;
	const struct v4l2_ctrl_vp9_frame *f = &vp9->frame;
	u32 width = ALIGN(vp9->cur.width, 8), height = ALIGN(vp9->cur.height, 8);

	th1520_vdec_reg_write(vpu, &hevc_pic_width_in_cbs, width / 8);
	th1520_vdec_reg_write(vpu, &hevc_pic_height_in_cbs, height / 8);
	th1520_vdec_reg_write(vpu, &hevc_pic_width_4x4, width / 4);
	th1520_vdec_reg_write(vpu, &hevc_pic_height_4x4, height / 4);
	th1520_vdec_reg_write(vpu, &hevc_min_cb_size, 3);
	th1520_vdec_reg_write(vpu, &hevc_max_cb_size, 6);
	th1520_vdec_reg_write(vpu, &hevc_idr_pic_e, vp9_intra(f));
	th1520_vdec_reg_write(vpu, &hevc_write_mvs_e,
			     !(f->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME));
	th1520_vdec_reg_write(vpu, &vp9_transform_mode, vp9->cur.tx_mode);
	th1520_vdec_reg_write(vpu, &vp9_mcomp_filt_type,
		vp9_intra(f) ? 0 : th1520_interp_filter(f->interpolation_filter));
	th1520_vdec_reg_write(vpu, &vp9_high_prec_mv_e,
			     !!(f->flags & V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV));
	th1520_vdec_reg_write(vpu, &vp9_comp_pred_mode, f->reference_mode);
	config_loop_filter(ctx, f);
	config_quant(ctx, f);
	config_compound_reference(ctx, f);
	vp9_config_segmentation(ctx);
}

static int vp9_validate_frame(struct th1520_vdec_ctx *ctx,
			    const struct v4l2_ctrl_vp9_frame *f,
			    const struct v4l2_ctrl_vp9_compressed_hdr *hdr)
{
	struct vb2_buffer *src = &th1520_vdec_get_src_buf(ctx)->vb2_buf;
	u32 width = f->frame_width_minus_1 + 1, height = f->frame_height_minus_1 + 1;
	u32 payload = vb2_get_plane_payload(src, 0), offset = src->planes[0].data_offset;
	u32 headers = (u32)f->uncompressed_header_size + f->compressed_header_size;
	u32 sb_cols = DIV_ROUND_UP(width, 64);
	u32 subsampling = V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING |
			  V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;

	if (f->profile || f->bit_depth != 8 || (f->flags & subsampling) != subsampling ||
	    width < TH1520_MIN_WIDTH || height < TH1520_MIN_HEIGHT ||
	    width > ctx->src_fmt.width || height > ctx->src_fmt.height ||
	    (width & 1) || (height & 1))
		return -EINVAL;
	if (f->tile_cols_log2 > 6 || f->tile_rows_log2 > 2 ||
	    hdr->tx_mode > V4L2_VP9_TX_MODE_SELECT || f->frame_context_idx >= 4 ||
	    f->reference_mode > V4L2_VP9_REFERENCE_MODE_SELECT ||
	    f->interpolation_filter > V4L2_VP9_INTERP_FILTER_SWITCHABLE)
		return -EINVAL;
	/* Section 6.2.14: tile columns contain between 4 and 64 superblocks. */
	if ((sb_cols >> f->tile_cols_log2) < 4 && f->tile_cols_log2)
		return -EINVAL;
	if (sb_cols > (64U << f->tile_cols_log2))
		return -EINVAL;
	if (offset >= payload || payload > vb2_plane_size(src, 0) ||
	    !f->uncompressed_header_size || !f->compressed_header_size ||
	    headers >= payload - offset)
		return -EINVAL;
	/* The stream window uses a 16-byte aligned DMA allocation base. */
	if (vb2_dma_contig_plane_dma_addr(src, 0) & 15)
		return -EINVAL;
	if (is_lossless(&f->quant) && hdr->tx_mode != V4L2_VP9_TX_MODE_ONLY_4X4)
		return -EINVAL;
	if ((f->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT) &&
	    (!(f->flags & V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE) ||
	     (f->flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX)))
		return -EINVAL;
	if (ctx->vp9->need_keyframe && !(f->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME))
		return -EINVAL;
	return 0;
}

static void vp9_config_stream(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	const struct v4l2_ctrl_vp9_frame *f = &ctx->vp9->frame;
	struct vb2_buffer *src = &th1520_vdec_get_src_buf(ctx)->vb2_buf;
	dma_addr_t base = vb2_dma_contig_plane_dma_addr(src, 0);
	u32 start = src->planes[0].data_offset + f->uncompressed_header_size +
		    f->compressed_header_size;
	u32 aligned = round_down(start, 16);

	/* Vp9AsicStrmPosUpdate: whole allocation base plus an aligned offset. */
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_STREAM, base);
	th1520_vdec_reg_write(vpu, &hevc_strm_start_offset, aligned);
	th1520_vdec_reg_write(vpu, &hevc_strm_start_bit, (start - aligned) * 8);
	th1520_vdec_reg_write(vpu, &hevc_stream_len, vb2_get_plane_payload(src, 0) - aligned);
	th1520_vdec_reg_write(vpu, &hevc_strm_buffer_len, vb2_plane_size(src, 0));
}

static int th1520_vp9_run(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_vp9_ctx *vp9 = ctx->vp9;
	const struct v4l2_ctrl_vp9_frame *f;
	const struct v4l2_ctrl_vp9_compressed_hdr *hdr;
	unsigned int index;
	int ret;

	th1520_vdec_start_prepare_run(ctx);
	f = th1520_vdec_get_ctrl(ctx, V4L2_CID_STATELESS_VP9_FRAME);
	hdr = th1520_vdec_get_ctrl(ctx, V4L2_CID_STATELESS_VP9_COMPRESSED_HDR);
	if (!f || !hdr) {
		ret = -EINVAL;
		goto err_complete;
	}
	ret = vp9_validate_frame(ctx, f, hdr);
	if (ret)
		goto err_complete;
	vp9->frame = *f;
	memcpy(vp9->working_context, vp9->frame_context, sizeof(vp9->frame_context));
	index = v4l2_vp9_reset_frame_ctx(f, vp9->working_context);
	vp9->probability_tables = vp9->working_context[index];
	v4l2_vp9_fw_update_probs(&vp9->probability_tables, hdr, f);
	vp9->cur = (struct th1520_vp9_frame_info) {
		.valid = true,
		.frame_context_idx = index,
		.reference_mode = f->reference_mode,
		.tx_mode = hdr->tx_mode,
		.interpolation_filter = f->interpolation_filter,
		.flags = f->flags,
		.width = f->frame_width_minus_1 + 1,
		.height = f->frame_height_minus_1 + 1,
	};
	th1520_vdec_set_common_config(ctx);
	vp9_config_output(ctx);
	ret = vp9_config_refs(ctx);
	if (ret)
		goto err_complete;
	vp9_config_frame(ctx);
	vp9_config_tiles(ctx);
	vp9_config_stream(ctx);
	memset(vp9->probs.cpu, 0, vp9->probs.size);
	memset(vp9->counts.cpu, 0, vp9->counts.size);
	th1520_vp9_pack_probs(ctx, f);
	th1520_vdec_write_addr_pair(ctx->dev, TH1520_VP9_ADDR_PROBS, vp9->probs.dma);
	th1520_vdec_write_addr_pair(ctx->dev, TH1520_VP9_ADDR_COUNTS, vp9->counts.dma);
	th1520_vdec_set_postproc(ctx, vp9->cur.width, vp9->cur.height);
	th1520_vdec_end_prepare_run(ctx);
	th1520_vdec_start(ctx->dev);
	return 0;

err_complete:
	v4l2_ctrl_request_complete(th1520_vdec_get_src_buf(ctx)->vb2_buf.req_obj.req,
				   &ctx->ctrl_handler);
	return ret;
}

static int th1520_vp9_check_result(struct th1520_vdec_ctx *ctx, u32 irq_status)
{
	struct vb2_buffer *src = &th1520_vdec_get_src_buf(ctx)->vb2_buf;
	const struct v4l2_ctrl_vp9_frame *f = &ctx->vp9->frame;
	u64 base = vb2_dma_contig_plane_dma_addr(src, 0);
	u64 start = base + src->planes[0].data_offset +
		    f->uncompressed_header_size + f->compressed_header_size;
	u64 end = base + vb2_get_plane_payload(src, 0);
	u64 position;

	/* Vp9ProcessAsicStatus also classifies ASO as a picture error. */
	if (irq_status & TH1520_IRQ_DEC_ASO_INT)
		return -EIO;
	/*
	 * A truncated VP9 tile can report only RDY while reading padding beyond
	 * bytesused. The hardware updates STREAM_BASE with its consumed position.
	 * SDK HevcRunAsic checks this address too; board VP9 measurements cover
	 * 186 valid decode jobs (end or end-1) and the truncated overrun case.
	 * Check before publishing reference contents or entropy/segment state.
	 */
	position = (u64)vdpu_read(ctx->dev, TH1520_VDEC_REG_OFF(TH1520_HEVC_ADDR_STREAM)) << 32;
	position |= vdpu_read(ctx->dev, TH1520_VDEC_REG_OFF(TH1520_HEVC_ADDR_STREAM + 1));
	if (position < start || position > end) {
		dev_err_ratelimited(ctx->dev->dev,
			"VP9 stream position outside payload: start=%llu end=%llu consumed=%llu\n",
			start - base, end - base, position - base);
		return -EIO;
	}
	return 0;
}

static void th1520_vp9_done(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_vp9_ctx *vp9 = ctx->vp9;
	struct th1520_vdec_buffer *dst =
		th1520_vdec_vbuf_to_buffer(th1520_vdec_get_dst_buf(ctx));

	dma_rmb();
	th1520_vp9_update_probs(ctx);
	memcpy(vp9->frame_context, vp9->working_context, sizeof(vp9->frame_context));
	vp9->segmentation = vp9->next_segmentation;
	vp9->active_segment = vp9->next_segment;
	/* Keep temporal MV independent of userspace CAPTURE recycling. */
	memcpy(vp9->previous_mv.cpu, dst->native.cpu + dst->vp9.mv_offset,
		vp9_mv_size(vp9->cur.width, vp9->cur.height));
	vp9->last = vp9->cur;
	vp9->need_keyframe = false;
	dst->vp9.valid = true;
}

static void th1520_vp9_abort(struct th1520_vdec_ctx *ctx)
{
	if (!ctx->vp9)
		return;
	ctx->vp9->need_keyframe = true;
	ctx->vp9->last.valid = false;
	ctx->vp9->cur.valid = false;
}

static void th1520_vp9_exit(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_vp9_ctx *vp9 = ctx->vp9;
	struct th1520_vdec_aux_buf *buffers[7];
	unsigned int i;

	if (!vp9)
		return;
	buffers[0] = &vp9->probs;
	buffers[1] = &vp9->counts;
	buffers[2] = &vp9->tiles;
	buffers[3] = &vp9->tile_filter;
	buffers[4] = &vp9->tile_bsd;
	buffers[5] = &vp9->segment_map;
	buffers[6] = &vp9->previous_mv;
	for (i = 0; i < ARRAY_SIZE(buffers); i++)
		if (buffers[i]->cpu)
			dma_free_coherent(ctx->dev->dev, buffers[i]->size,
					  buffers[i]->cpu, buffers[i]->dma);
	kfree(vp9);
	ctx->vp9 = NULL;
}

static int vp9_alloc(struct th1520_vdec_ctx *ctx,
		     struct th1520_vdec_aux_buf *buf, size_t size)
{
	buf->size = size;
	buf->cpu = dma_alloc_coherent(ctx->dev->dev, size, &buf->dma, GFP_KERNEL);
	if (!buf->cpu)
		return -ENOMEM;
	memset(buf->cpu, 0, size);
	return 0;
}

static int th1520_vp9_init(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_vp9_ctx *vp9;
	u32 width = ctx->src_fmt.width, height = ctx->src_fmt.height;
	u32 sb_cols = DIV_ROUND_UP(width, 64);
	u32 columns = max(1U, sb_cols / 4), edges = max(1U, columns - 1);
	unsigned int i;

	vp9 = kzalloc(sizeof(*vp9), GFP_KERNEL);
	if (!vp9)
		return -ENOMEM;
	ctx->vp9 = vp9;
	vp9->segment_map_size = DIV_ROUND_UP(width, 64) * DIV_ROUND_UP(height, 64) * 32;
	/* SDK edge storage: 24 and 4 bytes per padded row per column boundary. */
	if (vp9_alloc(ctx, &vp9->probs, 3744) ||
	    vp9_alloc(ctx, &vp9->counts, 13264) ||
	    vp9_alloc(ctx, &vp9->tiles, 3536) ||
	    vp9_alloc(ctx, &vp9->tile_filter, 24 * ALIGN(height, 64) * edges) ||
	    vp9_alloc(ctx, &vp9->tile_bsd, 4 * ALIGN(height, 64) * edges) ||
	    vp9_alloc(ctx, &vp9->segment_map, 2 * vp9->segment_map_size) ||
	    vp9_alloc(ctx, &vp9->previous_mv, vp9_mv_size(width, height))) {
		th1520_vp9_exit(ctx);
		return -ENOMEM;
	}
	for (i = 0; i < ARRAY_SIZE(vp9->frame_context); i++)
		vp9->frame_context[i] = v4l2_vp9_default_probs;
	vp9->need_keyframe = true;
	th1520_vp9_init_counts(ctx);
	return 0;
}

const struct th1520_vdec_codec_ops th1520_vdec_vp9_ops = {
	.init = th1520_vp9_init,
	.exit = th1520_vp9_exit,
	.run = th1520_vp9_run,
	.check_result = th1520_vp9_check_result,
	.done = th1520_vp9_done,
	.abort = th1520_vp9_abort,
	.reset = th1520_vdec_hw_reset,
};
