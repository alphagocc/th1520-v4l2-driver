// SPDX-License-Identifier: GPL-2.0-only
/*
 * TH1520 VC8000D HEVC stateless decoding, hardware mode 12.
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * Tile and scaling-list handling adapted from Linux hantro_hevc.c and
 * hantro_g2_hevc_dec.c:
 * Copyright (C) 2020 Safran Passenger Innovations LLC
 *
 * Linux commit 8ba098e6b6ff0db8edf28528d1552be261af30d4.
 * See docs/sources.md for public upstream links. TH1520 uses its own
 * register placement and native buffers as documented in docs/hardware.md.
 */

#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "th1520_vdec.h"

/* tile 边界中间缓冲的每像素行字节数（与上游 hantro_hevc.c 同值） */
#define VERT_FILTER_RAM_SIZE		8
#define BSD_CTRL_RAM_SIZE		4
#define VERT_SAO_RAM_SIZE		48

/* 缩放矩阵在 DMA buffer 中的布局大小 */
#define SCALING_LIST_SIZE		(16 * 64)

/* HEVC 规范允许的 tile 数量上限 */
#define MAX_TILE_COLS			20
#define MAX_TILE_ROWS			22

/* ---------------------------------------------------------------------- */

/* The Request API identifies references by CAPTURE timestamps, not POC.
 * Keep the tiled decode surface separate from the linear PP output.
 */
static dma_addr_t th1520_hevc_get_ref_buf(struct th1520_vdec_ctx *ctx, u64 ts)
{
	struct vb2_queue *q = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	struct th1520_vdec_buffer *buf;
	struct vb2_buffer *vb = vb2_find_buffer(q, ts);

	if (!vb)
		return 0;
	buf = th1520_vdec_vbuf_to_buffer(to_vb2_v4l2_buffer(vb));
	return buf->native.cpu ? buf->native.dma : 0;
}

/* ---------------------------------------------------------------------- */

static int th1520_hevc_tile_buffers_realloc(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct th1520_vdec_hevc_ctx *hevc = &ctx->hevc;
	const struct v4l2_ctrl_hevc_pps *pps = hevc->ctrls.pps;
	struct th1520_vdec_aux_buf filter = { 0 };
	struct th1520_vdec_aux_buf sao = { 0 };
	struct th1520_vdec_aux_buf bsd = { 0 };
	unsigned int num_tile_cols = pps->num_tile_columns_minus1 + 1;
	unsigned int height64 = ALIGN(ctx->src_fmt.height, 64);

	if (num_tile_cols <= 1 ||
	    num_tile_cols <= hevc->num_tile_cols_allocated)
		return 0;

	/* Keep the previous allocation usable if any replacement fails. */
	filter.size = (VERT_FILTER_RAM_SIZE * height64 * (num_tile_cols - 1) *
		       ctx->bit_depth) / 8;
	filter.cpu = dma_alloc_coherent(vpu->dev, filter.size, &filter.dma,
				       GFP_KERNEL);
	if (!filter.cpu)
		return -ENOMEM;

	sao.size = (VERT_SAO_RAM_SIZE * height64 * (num_tile_cols - 1) *
		    ctx->bit_depth) / 8;
	sao.cpu = dma_alloc_coherent(vpu->dev, sao.size, &sao.dma, GFP_KERNEL);
	if (!sao.cpu)
		goto err_free_filter;

	bsd.size = BSD_CTRL_RAM_SIZE * height64 * (num_tile_cols - 1);
	bsd.cpu = dma_alloc_coherent(vpu->dev, bsd.size, &bsd.dma, GFP_KERNEL);
	if (!bsd.cpu)
		goto err_free_sao;

	if (hevc->tile_filter.cpu)
		dma_free_coherent(vpu->dev, hevc->tile_filter.size,
				  hevc->tile_filter.cpu, hevc->tile_filter.dma);
	if (hevc->tile_sao.cpu)
		dma_free_coherent(vpu->dev, hevc->tile_sao.size,
				  hevc->tile_sao.cpu, hevc->tile_sao.dma);
	if (hevc->tile_bsd.cpu)
		dma_free_coherent(vpu->dev, hevc->tile_bsd.size,
				  hevc->tile_bsd.cpu, hevc->tile_bsd.dma);
	hevc->tile_filter = filter;
	hevc->tile_sao = sao;
	hevc->tile_bsd = bsd;
	hevc->num_tile_cols_allocated = num_tile_cols;
	return 0;

err_free_sao:
	dma_free_coherent(vpu->dev, sao.size, sao.cpu, sao.dma);
err_free_filter:
	dma_free_coherent(vpu->dev, filter.size, filter.cpu, filter.dma);
	return -ENOMEM;
}

/*
 * tile 尺寸表：硬件从 TILE_BASE（swreg166/167）读取每个 tile 的
 * (宽, 高)，单位为 CTB，各 16 bit。
 */
static void th1520_hevc_prepare_tile_info(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	const struct th1520_vdec_hevc_ctrls *ctrls = &ctx->hevc.ctrls;
	const struct v4l2_ctrl_hevc_pps *pps = ctrls->pps;
	const struct v4l2_ctrl_hevc_sps *sps = ctrls->sps;
	u16 *p = ctx->hevc.tile_sizes.cpu;
	unsigned int num_tile_rows = pps->num_tile_rows_minus1 + 1;
	unsigned int num_tile_cols = pps->num_tile_columns_minus1 + 1;
	unsigned int pic_width_in_ctbs, pic_height_in_ctbs;
	unsigned int max_log2_ctb_size;
	bool tiles_enabled, uniform_spacing;
	unsigned int i, j, h;

	tiles_enabled = !!(pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED);
	uniform_spacing = !!(pps->flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING);

	th1520_vdec_reg_write(vpu, &hevc_tile_enable, tiles_enabled);

	max_log2_ctb_size = sps->log2_min_luma_coding_block_size_minus3 + 3 +
			    sps->log2_diff_max_min_luma_coding_block_size;
	pic_width_in_ctbs = DIV_ROUND_UP(sps->pic_width_in_luma_samples,
					 1 << max_log2_ctb_size);
	pic_height_in_ctbs = DIV_ROUND_UP(sps->pic_height_in_luma_samples,
					  1 << max_log2_ctb_size);

	/*
	 * Tile columns occupy register 10 bits 23:17 and rows bits 16:12.
	 * Use only these fields to avoid overlapping encodings from other cores.
	 */
	if (!tiles_enabled) {
		th1520_vdec_reg_write(vpu, &hevc_num_tile_rows_8k, 1);
		th1520_vdec_reg_write(vpu, &hevc_num_tile_cols_8k, 1);

		/* 只有一个 tile，尺寸等于整幅图像。 */
		p[0] = pic_width_in_ctbs;
		p[1] = pic_height_in_ctbs;
		return;
	}

	th1520_vdec_reg_write(vpu, &hevc_num_tile_rows_8k, num_tile_rows);
	th1520_vdec_reg_write(vpu, &hevc_num_tile_cols_8k, num_tile_cols);

	if (!uniform_spacing) {
		unsigned int tmp_w, tmp_h = 0;

		for (i = 0; i < num_tile_rows; i++) {
			if (i == num_tile_rows - 1)
				h = pic_height_in_ctbs - tmp_h;
			else
				h = pps->row_height_minus1[i] + 1;
			tmp_h += h;

			for (j = 0, tmp_w = 0; j < num_tile_cols - 1; j++) {
				tmp_w += pps->column_width_minus1[j] + 1;
				*p++ = pps->column_width_minus1[j] + 1;
				*p++ = h;
			}
			/* 最后一列 */
			*p++ = pic_width_in_ctbs - tmp_w;
			*p++ = h;
		}
	} else {
		unsigned int tmp, prev_h, prev_w;

		for (i = 0, prev_h = 0; i < num_tile_rows; i++) {
			tmp = (i + 1) * pic_height_in_ctbs / num_tile_rows;
			h = tmp - prev_h;
			prev_h = tmp;

			for (j = 0, prev_w = 0; j < num_tile_cols; j++) {
				tmp = (j + 1) * pic_width_in_ctbs /
				      num_tile_cols;
				*p++ = tmp - prev_w;
				*p++ = h;
				prev_w = tmp;
			}
		}
	}
}

/*
 * HDR_SKIP_LENGTH（swreg9[13:0]）：硬件要跳过的 slice segment header
 * 前缀比特数。V4L2 不直接提供该值，需按 H.265 7.3.6.1 的语法顺序累加。
 * 计算方式与上游 hantro_g2_hevc_dec.c 的 compute_header_skip_length() 一致。
 */
static int th1520_hevc_header_skip_length(struct th1520_vdec_ctx *ctx)
{
	const struct th1520_vdec_hevc_ctrls *ctrls = &ctx->hevc.ctrls;
	const struct v4l2_ctrl_hevc_decode_params *dec = ctrls->decode_params;
	const struct v4l2_ctrl_hevc_sps *sps = ctrls->sps;
	const struct v4l2_ctrl_hevc_pps *pps = ctrls->pps;
	int skip = 0;

	if (pps->flags & V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT)
		skip++;					/* pic_output_flag */

	if (sps->flags & V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE)
		skip += 2;				/* colour_plane_id */

	if (!(dec->flags & V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC)) {
		skip += sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
		skip++;			/* short_term_ref_pic_set_sps_flag */

		if (dec->short_term_ref_pic_set_size)
			skip += dec->short_term_ref_pic_set_size;
		else if (sps->num_short_term_ref_pic_sets > 1)
			skip += fls(sps->num_short_term_ref_pic_sets - 1);

		skip += dec->long_term_ref_pic_set_size;
	}

	return skip;
}

static void th1520_hevc_set_params(struct th1520_vdec_ctx *ctx)
{
	const struct th1520_vdec_hevc_ctrls *ctrls = &ctx->hevc.ctrls;
	const struct v4l2_ctrl_hevc_sps *sps = ctrls->sps;
	const struct v4l2_ctrl_hevc_pps *pps = ctrls->pps;
	const struct v4l2_ctrl_hevc_decode_params *dec = ctrls->decode_params;
	struct th1520_vdec_dev *vpu = ctx->dev;
	u32 min_log2_cb_size, max_log2_ctb_size, min_cb_size, max_ctb_size;
	u32 pic_width_in_min_cbs, pic_height_in_min_cbs;
	u32 pic_width_aligned, pic_height_aligned;

	/* swreg8 —— 位深。本驱动只支持 8 bit，两个字段都为 0。 */
	th1520_vdec_reg_write(vpu, &hevc_bit_depth_y_minus8,
			      sps->bit_depth_luma_minus8);
	th1520_vdec_reg_write(vpu, &hevc_bit_depth_c_minus8,
			      sps->bit_depth_chroma_minus8);
	th1520_vdec_reg_write(vpu, &hevc_output_8_bits, 0);
	th1520_vdec_reg_write(vpu, &hevc_output_format, 0);

	th1520_vdec_reg_write(vpu, &hevc_hdr_skip_length,
			      th1520_hevc_header_skip_length(ctx));

	min_log2_cb_size = sps->log2_min_luma_coding_block_size_minus3 + 3;
	max_log2_ctb_size = min_log2_cb_size +
			    sps->log2_diff_max_min_luma_coding_block_size;

	th1520_vdec_reg_write(vpu, &hevc_min_cb_size, min_log2_cb_size);
	th1520_vdec_reg_write(vpu, &hevc_max_cb_size, max_log2_ctb_size);

	min_cb_size = 1 << min_log2_cb_size;
	max_ctb_size = 1 << max_log2_ctb_size;

	pic_width_in_min_cbs = sps->pic_width_in_luma_samples / min_cb_size;
	pic_height_in_min_cbs = sps->pic_height_in_luma_samples / min_cb_size;
	pic_width_aligned = ALIGN(sps->pic_width_in_luma_samples, max_ctb_size);
	pic_height_aligned = ALIGN(sps->pic_height_in_luma_samples,
				   max_ctb_size);

	/* swreg20 —— 图像是否以非整 CTB 结尾 */
	th1520_vdec_reg_write(vpu, &hevc_partial_ctb_x,
			      sps->pic_width_in_luma_samples !=
			      pic_width_aligned);
	th1520_vdec_reg_write(vpu, &hevc_partial_ctb_y,
			      sps->pic_height_in_luma_samples !=
			      pic_height_aligned);

	/* swreg4 —— 以最小 CB 为单位的尺寸 */
	th1520_vdec_reg_write(vpu, &hevc_pic_width_in_cbs,
			      pic_width_in_min_cbs);
	th1520_vdec_reg_write(vpu, &hevc_pic_height_in_cbs,
			      pic_height_in_min_cbs);

	/* swreg20 —— 以 4x4 块为单位的尺寸 */
	th1520_vdec_reg_write(vpu, &hevc_pic_width_4x4,
			      (pic_width_in_min_cbs * min_cb_size) / 4);
	th1520_vdec_reg_write(vpu, &hevc_pic_height_4x4,
			      (pic_height_in_min_cbs * min_cb_size) / 4);

	/* swreg13 —— 变换树层级 */
	th1520_vdec_reg_write(vpu, &hevc_max_inter_hierdepth,
			      sps->max_transform_hierarchy_depth_inter);
	th1520_vdec_reg_write(vpu, &hevc_max_intra_hierdepth,
			      sps->max_transform_hierarchy_depth_intra);
	th1520_vdec_reg_write(vpu, &hevc_min_trb_size,
			      sps->log2_min_luma_transform_block_size_minus2 + 2);
	th1520_vdec_reg_write(vpu, &hevc_max_trb_size,
			      sps->log2_min_luma_transform_block_size_minus2 + 2 +
			      sps->log2_diff_max_min_luma_transform_block_size);
	th1520_vdec_reg_write(vpu, &hevc_parallel_merge,
			      pps->log2_parallel_merge_level_minus2 + 2);

	/* swreg5 —— SPS/PPS 标志 */
	th1520_vdec_reg_write(vpu, &hevc_tempor_mvp_e,
			      !!(sps->flags & V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED) &&
			      !(dec->flags & V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC));
	th1520_vdec_reg_write(vpu, &hevc_sign_data_hide,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_cb_qp_offset, pps->pps_cb_qp_offset);
	th1520_vdec_reg_write(vpu, &hevc_cr_qp_offset, pps->pps_cr_qp_offset);

	if (pps->flags & V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED) {
		th1520_vdec_reg_write(vpu, &hevc_cu_qpd_e, 1);
		th1520_vdec_reg_write(vpu, &hevc_max_cu_qpd_depth,
				      pps->diff_cu_qp_delta_depth);
	} else {
		th1520_vdec_reg_write(vpu, &hevc_cu_qpd_e, 0);
		th1520_vdec_reg_write(vpu, &hevc_max_cu_qpd_depth, 0);
	}

	/* swreg7 —— slice / 滤波参数 */
	th1520_vdec_reg_write(vpu, &hevc_strong_smooth_e,
			      !!(sps->flags & V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_asym_pred_e,
			      !!(sps->flags & V4L2_HEVC_SPS_FLAG_AMP_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_sao_e,
			      !!(sps->flags & V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET));
	th1520_vdec_reg_write(vpu, &hevc_filt_offset_beta,
			      pps->pps_beta_offset_div2);
	th1520_vdec_reg_write(vpu, &hevc_filt_offset_tc,
			      pps->pps_tc_offset_div2);
	th1520_vdec_reg_write(vpu, &hevc_slice_hdr_ext_e,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT));
	th1520_vdec_reg_write(vpu, &hevc_slice_header_extra_bits,
			      pps->num_extra_slice_header_bits);
	th1520_vdec_reg_write(vpu, &hevc_slice_chroma_qp_offsets_present,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT));
	th1520_vdec_reg_write(vpu, &hevc_weight_pred_e,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED));
	th1520_vdec_reg_write(vpu, &hevc_weight_bipr_idc,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED));
	th1520_vdec_reg_write(vpu, &hevc_dependent_segments_enabled,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_deblocking_override_enabled,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_pcm_loop_filter_disabled,
			      !!(sps->flags & V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED));
	th1520_vdec_reg_write(vpu, &hevc_cabac_init_present,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT));
	/* 单色（4:0:0）在 try_ctrl 里已被拒绝，这里恒为 0。 */
	th1520_vdec_reg_write(vpu, &hevc_blackwhite_e, 0);

	/* swreg12 —— CB / PCM / 变换标志 */
	th1520_vdec_reg_write(vpu, &hevc_transform_skip_enabled,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_transquant_bypass_enabled,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_reference_list_modification_enabled,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT));
	th1520_vdec_reg_write(vpu, &hevc_pcm_e,
			      !!(sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED));
	if (sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED) {
		th1520_vdec_reg_write(vpu, &hevc_max_pcm_size,
				      sps->log2_diff_max_min_pcm_luma_coding_block_size +
				      sps->log2_min_pcm_luma_coding_block_size_minus3 + 3);
		th1520_vdec_reg_write(vpu, &hevc_min_pcm_size,
				      sps->log2_min_pcm_luma_coding_block_size_minus3 + 3);
		th1520_vdec_reg_write(vpu, &hevc_pcm_luma_sample_depth,
				      sps->pcm_sample_bit_depth_luma_minus1 + 1);
		th1520_vdec_reg_write(vpu, &hevc_pcm_chroma_sample_depth,
				      sps->pcm_sample_bit_depth_chroma_minus1 + 1);
	}

	/* swreg8 / swreg10 */
	th1520_vdec_reg_write(vpu, &hevc_const_intra_e,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED));
	th1520_vdec_reg_write(vpu, &hevc_filt_ctrl_pres,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT));
	th1520_vdec_reg_write(vpu, &hevc_idr_pic_e,
			      !!(dec->flags & V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC));
	th1520_vdec_reg_write(vpu, &hevc_filtering_dis,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER));

	th1520_vdec_reg_write(vpu, &hevc_entropy_row_sync_enabled,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED));
	/*
	 * The initial QP occupies register 13 bits 30:24 on this hardware.
	 */
	th1520_vdec_reg_write(vpu, &hevc_init_qp, pps->init_qp_minus26 + 26);

	/* swreg9 —— 活动参考索引数 */
	th1520_vdec_reg_write(vpu, &hevc_refidx0_active,
			      pps->num_ref_idx_l0_default_active_minus1 + 1);
	th1520_vdec_reg_write(vpu, &hevc_refidx1_active,
			      pps->num_ref_idx_l1_default_active_minus1 + 1);
}

/*
 * swreg14..19：初始参考列表，每项 5 bit。
 * 每个 swreg 存 3 组 (F, B)：F 在 shift 0/10/20，B 在 shift 5/15/25。
 * 与上游 hevc_rlist_f0..f15 / b0..b15 的定义一致。
 */
static void th1520_hevc_write_rlist(struct th1520_vdec_dev *vpu,
				    unsigned int idx, u32 f, u32 b)
{
	u16 swreg = TH1520_HEVC_SWREG_RLIST(idx / 3);
	unsigned int group = idx % 3;
	const struct th1520_vdec_reg reg_f =
		TH1520_REG(swreg, group * 10, 0x1f);
	const struct th1520_vdec_reg reg_b =
		TH1520_REG(swreg, group * 10 + 5, 0x1f);

	th1520_vdec_reg_write(vpu, &reg_f, f);
	th1520_vdec_reg_write(vpu, &reg_b, b);
}

static u32 th1520_hevc_dpb_index(const struct v4l2_ctrl_hevc_decode_params *dec,
				 u32 index)
{
	if (index >= dec->num_active_dpb_entries)
		return 0;

	return index;
}

static void th1520_hevc_set_ref_pic_list(struct th1520_vdec_ctx *ctx)
{
	const struct v4l2_ctrl_hevc_decode_params *dec =
		ctx->hevc.ctrls.decode_params;
	struct th1520_vdec_dev *vpu = ctx->dev;
	u32 list0[V4L2_HEVC_DPB_ENTRIES_NUM_MAX] = {};
	u32 list1[V4L2_HEVC_DPB_ENTRIES_NUM_MAX] = {};
	unsigned int i, j;

	/* L0：short-term before → short-term after → long-term */
	j = 0;
	for (i = 0; i < dec->num_poc_st_curr_before && j < ARRAY_SIZE(list0); i++)
		list0[j++] = dec->poc_st_curr_before[i];
	for (i = 0; i < dec->num_poc_st_curr_after && j < ARRAY_SIZE(list0); i++)
		list0[j++] = dec->poc_st_curr_after[i];
	for (i = 0; i < dec->num_poc_lt_curr && j < ARRAY_SIZE(list0); i++)
		list0[j++] = dec->poc_lt_curr[i];
	/* 反复拷贝填满整张表 */
	for (i = 0; j < ARRAY_SIZE(list0); i++)
		list0[j++] = list0[i];

	/* L1：short-term after → short-term before → long-term */
	j = 0;
	for (i = 0; i < dec->num_poc_st_curr_after && j < ARRAY_SIZE(list1); i++)
		list1[j++] = dec->poc_st_curr_after[i];
	for (i = 0; i < dec->num_poc_st_curr_before && j < ARRAY_SIZE(list1); i++)
		list1[j++] = dec->poc_st_curr_before[i];
	for (i = 0; i < dec->num_poc_lt_curr && j < ARRAY_SIZE(list1); i++)
		list1[j++] = dec->poc_lt_curr[i];
	for (i = 0; j < ARRAY_SIZE(list1); i++)
		list1[j++] = list1[i];

	for (i = 0; i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++)
		th1520_hevc_write_rlist(vpu, i,
					th1520_hevc_dpb_index(dec, list0[i]),
					th1520_hevc_dpb_index(dec, list1[i]));
}

/* swreg46..49：16 个参考帧相对当前帧的 POC 差，每项 8 bit。 */
static void th1520_hevc_write_cur_poc(struct th1520_vdec_dev *vpu,
				      unsigned int idx, u32 val)
{
	const struct th1520_vdec_reg reg =
		TH1520_REG(TH1520_HEVC_SWREG_CUR_POC(idx / 4),
			   24 - (idx % 4) * 8, 0xff);

	th1520_vdec_reg_write(vpu, &reg, val);
}

static int th1520_hevc_set_ref(struct th1520_vdec_ctx *ctx)
{
	const struct th1520_vdec_hevc_ctrls *ctrls = &ctx->hevc.ctrls;
	const struct v4l2_ctrl_hevc_pps *pps = ctrls->pps;
	const struct v4l2_ctrl_hevc_decode_params *dec = ctrls->decode_params;
	const struct v4l2_hevc_dpb_entry *dpb = dec->dpb;
	struct th1520_vdec_dev *vpu = ctx->dev;
	size_t cr_offset = th1520_vdec_hevc_native_chroma_offset(ctx);
	size_t mv_offset = th1520_vdec_hevc_native_mv_offset(ctx);
	struct th1520_vdec_buffer *dst;
	struct vb2_v4l2_buffer *vb2_dst;
	dma_addr_t luma_addr, chroma_addr, mv_addr;
	u32 max_ref_frames;
	u16 dpb_longterm = 0;
	unsigned int i;

	max_ref_frames = dec->num_poc_lt_curr + dec->num_poc_st_curr_before +
			 dec->num_poc_st_curr_after;
	/*
	 * REF_FRAMES 为 0 会让硬件在遇到标记错误的 I 帧时挂死。
	 * .so 与上游 mainline 都强制至少为 1。
	 */
	th1520_vdec_reg_write(vpu, &hevc_num_ref_frames,
			      max_ref_frames ? max_ref_frames : 1);

	th1520_vdec_reg_write(vpu, &hevc_loop_filter_across_slices,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_loop_filter_across_tiles,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED));

	/* 写各参考帧与当前帧的 POC 差 */
	for (i = 0; i < dec->num_active_dpb_entries &&
	     i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++) {
		s64 poc_diff = (s64)dec->pic_order_cnt_val -
			       dpb[i].pic_order_cnt_val;

		/*
		 * The signed 8-bit POC-difference fields require saturation before packing.
		 */
		th1520_hevc_write_cur_poc(vpu, i,
					 (u8)clamp_t(s64, poc_diff, -128, 127));
	}
	/* 紧跟参考帧之后放一项指向自身（差为 0） */
	if (i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
		th1520_hevc_write_cur_poc(vpu, i++, 0);
	/* 其余槽位填当前帧的 POC */
	for (; i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++)
		th1520_hevc_write_cur_poc(vpu, i, dec->pic_order_cnt_val);

	th1520_hevc_set_ref_pic_list(ctx);

	for (i = 0; i < dec->num_active_dpb_entries &&
	     i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX - 1; i++) {
		luma_addr = th1520_hevc_get_ref_buf(ctx,
						    dpb[i].timestamp);
		if (!luma_addr)
			return -ENOENT;

		if (dpb[i].flags & V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE)
			dpb_longterm |=
				BIT(V4L2_HEVC_DPB_ENTRIES_NUM_MAX - 1 - i);

		th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_REF_LUMA(i),
					    luma_addr);
		th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_REF_CHROMA(i),
					    luma_addr + cr_offset);
		th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_REF_MV(i),
					    luma_addr + mv_offset);
	}

	vb2_dst = th1520_vdec_get_dst_buf(ctx);
	dst = th1520_vdec_vbuf_to_buffer(vb2_dst);
	if (!dst->native.cpu)
		return -EFAULT;
	luma_addr = dst->native.dma;
	/*
	 * Clear the 32-byte picture synchronization area before buffer reuse.
	 */
	memset((u8 *)dst->native.cpu + mv_offset - 32, 0, 32);

	chroma_addr = luma_addr + cr_offset;
	mv_addr = luma_addr + mv_offset;

	/* 当前帧同时占用参考槽 i（硬件要求当前帧可被自身引用） */
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_REF_LUMA(i),
				    luma_addr);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_REF_CHROMA(i),
				    chroma_addr);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_REF_MV(i),
				    mv_addr);
	i++;

	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_OUT_LUMA, luma_addr);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_OUT_CHROMA,
				    chroma_addr);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_OUT_MV, mv_addr);

	/* Clear unused slots in hardware as well when sparse flushing is used. */
	for (; i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++) {
		th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_REF_LUMA(i), 0);
		th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_REF_CHROMA(i), 0);
		th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_REF_MV(i), 0);
	}

	th1520_vdec_reg_write(vpu, &hevc_refer_lterm_e, dpb_longterm);

	return 0;
}

static void th1520_hevc_set_buffers(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src_buf = th1520_vdec_get_src_buf(ctx);
	dma_addr_t src_dma;
	u32 src_len, src_buf_len, data_offset, start_byte;

	src_dma = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	data_offset = src_buf->vb2_buf.planes[0].data_offset;
	src_dma += data_offset;
	start_byte = src_dma & 0xf;
	src_len = vb2_get_plane_payload(&src_buf->vb2_buf, 0) - data_offset + start_byte;
	src_buf_len = vb2_plane_size(&src_buf->vb2_buf, 0) - data_offset + start_byte;
	src_dma -= start_byte;

	/*
	 * 调试用：确认送进硬件的确实是带 Annex-B 起始码的 slice NAL。
	 * 打开方式：
	 *   echo 'module th1520_vdec +p' > /sys/kernel/debug/dynamic_debug/control
	 */
	if (IS_ENABLED(CONFIG_DYNAMIC_DEBUG)) {
		const u8 *p = vb2_plane_vaddr(&src_buf->vb2_buf, 0);

		if (p)
			dev_dbg(vpu->dev,
				"stream len=%u buf=%u head=%*ph\n",
				src_len, src_buf_len,
				(int)min_t(u32, src_len - start_byte, 16), p + data_offset);
	}

	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_STREAM, src_dma);
	th1520_vdec_reg_write(vpu, &hevc_stream_len, src_len);
	th1520_vdec_reg_write(vpu, &hevc_strm_buffer_len, src_buf_len);
	th1520_vdec_reg_write(vpu, &hevc_strm_start_offset, 0);
	th1520_vdec_reg_write(vpu, &hevc_strm_start_bit, start_byte * 8);
	/*
	 * The negotiated HEVC input contains Annex-B start codes. Register 13
	 * bit 31 enables hardware start-code parsing.
	 */
	th1520_vdec_reg_write(vpu, &hevc_start_code_e, 1);
	/* 必须写 MV，否则后续帧无法做时域预测。 */
	th1520_vdec_reg_write(vpu, &hevc_write_mvs_e, 1);

	/*
	 * Common configuration enables the input-exhausted interrupt. Each
	 * request supplies a complete frame and its available input-buffer size.
	 */

	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_TILE_SIZES,
				    ctx->hevc.tile_sizes.dma);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_TILE_FILTER,
				    ctx->hevc.tile_filter.dma);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_TILE_SAO,
				    ctx->hevc.tile_sao.dma);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_TILE_BSD,
				    ctx->hevc.tile_bsd.dma);

	/*
	 * Native tiled stride is the number of bytes in four luma or chroma rows.
	 */
	th1520_vdec_reg_write(vpu, &hevc_native_luma_stride,
			      ALIGN(ctx->src_fmt.width * 4, 64));
	th1520_vdec_reg_write(vpu, &hevc_native_chroma_stride,
			      ALIGN(ctx->src_fmt.width * 4, 64));
}

/*
 * 缩放矩阵在 DMA buffer 中的排布：先是 16x16/32x32 的 DC 系数，
 * 对齐到 128 bit 边界后，各尺寸的系数按列优先写入。
 * 布局与上游 hantro_g2_hevc_dec.c 的 prepare_scaling_list_buffer() 一致。
 */
static void th1520_hevc_prepare_scaling_list(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	const struct th1520_vdec_hevc_ctrls *ctrls = &ctx->hevc.ctrls;
	const struct v4l2_ctrl_hevc_scaling_matrix *sc = ctrls->scaling;
	const struct v4l2_ctrl_hevc_sps *sps = ctrls->sps;
	u8 *p = ctx->hevc.scaling_lists.cpu;
	bool enabled;
	unsigned int i, j, k;

	enabled = !!(sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED);
	th1520_vdec_reg_write(vpu, &hevc_scaling_list_e, enabled);

	if (!enabled)
		return;

	for (i = 0; i < ARRAY_SIZE(sc->scaling_list_dc_coef_16x16); i++)
		*p++ = sc->scaling_list_dc_coef_16x16[i];
	for (i = 0; i < ARRAY_SIZE(sc->scaling_list_dc_coef_32x32); i++)
		*p++ = sc->scaling_list_dc_coef_32x32[i];

	/* 补齐到 128 bit 边界 */
	p += 8;

	for (i = 0; i < 6; i++)
		for (j = 0; j < 4; j++)
			for (k = 0; k < 4; k++)
				*p++ = sc->scaling_list_4x4[i][4 * k + j];

	for (i = 0; i < 6; i++)
		for (j = 0; j < 8; j++)
			for (k = 0; k < 8; k++)
				*p++ = sc->scaling_list_8x8[i][8 * k + j];

	for (i = 0; i < 6; i++)
		for (j = 0; j < 8; j++)
			for (k = 0; k < 8; k++)
				*p++ = sc->scaling_list_16x16[i][8 * k + j];

	for (i = 0; i < 2; i++)
		for (j = 0; j < 8; j++)
			for (k = 0; k < 8; k++)
				*p++ = sc->scaling_list_32x32[i][8 * k + j];

	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_SCALING_LIST,
				    ctx->hevc.scaling_lists.dma);
}

static int th1520_hevc_validate_params(struct th1520_vdec_ctx *ctx)
{
	const struct v4l2_ctrl_hevc_sps *sps = ctx->hevc.ctrls.sps;
	const struct v4l2_ctrl_hevc_pps *pps = ctx->hevc.ctrls.pps;
	unsigned int min_cb_log2, max_ctb_log2, min_tb_log2, max_tb_log2;
	unsigned int width_in_ctbs, height_in_ctbs, min_cb_size;
	unsigned int cols = pps->num_tile_columns_minus1 + 1U;
	unsigned int rows = pps->num_tile_rows_minus1 + 1U;
	unsigned int sum, i;

	if (!sps->pic_width_in_luma_samples || !sps->pic_height_in_luma_samples ||
	    sps->chroma_format_idc != 1 || sps->bit_depth_luma_minus8 ||
	    sps->bit_depth_chroma_minus8)
		return -EINVAL;

	/* Native surfaces and tile scratch buffers share the negotiated size. */
	if (ALIGN(sps->pic_width_in_luma_samples, TH1520_MB_DIM) !=
	    ctx->src_fmt.width ||
	    ALIGN(sps->pic_height_in_luma_samples, TH1520_MB_DIM) !=
	    ctx->src_fmt.height ||
	    ctx->dst_fmt.width != ctx->src_fmt.width ||
	    ctx->dst_fmt.height != ctx->src_fmt.height)
		return -EINVAL;

	/* Validate logarithms before using them in shifts or divisors. */
	min_cb_log2 = sps->log2_min_luma_coding_block_size_minus3 + 3U;
	max_ctb_log2 = min_cb_log2 + sps->log2_diff_max_min_luma_coding_block_size;
	min_tb_log2 = sps->log2_min_luma_transform_block_size_minus2 + 2U;
	max_tb_log2 = min_tb_log2 + sps->log2_diff_max_min_luma_transform_block_size;
	if (min_cb_log2 > 6 || max_ctb_log2 < 4 || max_ctb_log2 > 6 ||
	    min_tb_log2 > 5 || min_tb_log2 >= min_cb_log2 ||
	    max_tb_log2 > 5 || max_tb_log2 > max_ctb_log2 ||
	    sps->max_transform_hierarchy_depth_inter > max_ctb_log2 - min_tb_log2 ||
	    sps->max_transform_hierarchy_depth_intra > max_ctb_log2 - min_tb_log2 ||
	    sps->log2_max_pic_order_cnt_lsb_minus4 > 12)
		return -EINVAL;

	min_cb_size = 1U << min_cb_log2;
	if (sps->pic_width_in_luma_samples % min_cb_size ||
	    sps->pic_height_in_luma_samples % min_cb_size)
		return -EINVAL;
	width_in_ctbs = DIV_ROUND_UP(sps->pic_width_in_luma_samples,
				     1U << max_ctb_log2);
	height_in_ctbs = DIV_ROUND_UP(sps->pic_height_in_luma_samples,
				      1U << max_ctb_log2);
	if (cols > MAX_TILE_COLS || rows > MAX_TILE_ROWS ||
	    cols > width_in_ctbs || rows > height_in_ctbs)
		return -EINVAL;

	if (pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) {
		if (!(pps->flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING)) {
			for (i = 0, sum = 0; i + 1 < cols; i++) {
				sum += pps->column_width_minus1[i] + 1U;
				if (sum >= width_in_ctbs)
					return -EINVAL;
			}
			for (i = 0, sum = 0; i + 1 < rows; i++) {
				sum += pps->row_height_minus1[i] + 1U;
				if (sum >= height_in_ctbs)
					return -EINVAL;
			}
		}
	} else if (cols != 1 || rows != 1) {
		return -EINVAL;
	}

	return 0;
}

static int th1520_hevc_prepare_run(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_hevc_ctrls *ctrls = &ctx->hevc.ctrls;
	struct vb2_buffer *src = &th1520_vdec_get_src_buf(ctx)->vb2_buf;
	const struct v4l2_ctrl_hevc_decode_params *dec;
	unsigned int i;
	int ret;

	th1520_vdec_start_prepare_run(ctx);

	ctrls->decode_params =
		th1520_vdec_get_ctrl(ctx, V4L2_CID_STATELESS_HEVC_DECODE_PARAMS);
	ctrls->scaling =
		th1520_vdec_get_ctrl(ctx, V4L2_CID_STATELESS_HEVC_SCALING_MATRIX);
	ctrls->sps = th1520_vdec_get_ctrl(ctx, V4L2_CID_STATELESS_HEVC_SPS);
	ctrls->pps = th1520_vdec_get_ctrl(ctx, V4L2_CID_STATELESS_HEVC_PPS);

	if (!ctrls->decode_params || !ctrls->scaling || !ctrls->sps ||
	    !ctrls->pps)
		return -EINVAL;

	ret = th1520_hevc_validate_params(ctx);
	if (ret)
		return ret;
	if (src->planes[0].data_offset >= vb2_get_plane_payload(src, 0) ||
	    vb2_get_plane_payload(src, 0) > vb2_plane_size(src, 0))
		return -EINVAL;

	dec = ctrls->decode_params;
	/* One hardware reference slot holds the current picture. */
	if (dec->num_active_dpb_entries >= V4L2_HEVC_DPB_ENTRIES_NUM_MAX ||
	    dec->num_poc_st_curr_before > dec->num_active_dpb_entries ||
	    dec->num_poc_st_curr_after > dec->num_active_dpb_entries ||
	    dec->num_poc_lt_curr > dec->num_active_dpb_entries)
		return -EINVAL;
	if ((unsigned int)dec->num_poc_st_curr_before + dec->num_poc_st_curr_after +
	    dec->num_poc_lt_curr > dec->num_active_dpb_entries)
		return -EINVAL;
	for (i = 0; i < dec->num_poc_st_curr_before; i++)
		if (dec->poc_st_curr_before[i] >= dec->num_active_dpb_entries)
			return -EINVAL;
	for (i = 0; i < dec->num_poc_st_curr_after; i++)
		if (dec->poc_st_curr_after[i] >= dec->num_active_dpb_entries)
			return -EINVAL;
	for (i = 0; i < dec->num_poc_lt_curr; i++)
		if (dec->poc_lt_curr[i] >= dec->num_active_dpb_entries)
			return -EINVAL;

	return th1520_hevc_tile_buffers_realloc(ctx);
}

static int th1520_hevc_run(struct th1520_vdec_ctx *ctx)
{
	int ret;

	ret = th1520_hevc_prepare_run(ctx);
	if (ret)
		goto err_complete_request;

	th1520_vdec_set_common_config(ctx);
	th1520_hevc_set_params(ctx);

	ret = th1520_hevc_set_ref(ctx);
	if (ret)
		goto err_complete_request;

	th1520_hevc_set_buffers(ctx);
	th1520_hevc_prepare_tile_info(ctx);
	th1520_hevc_prepare_scaling_list(ctx);
	th1520_vdec_set_postproc(ctx, ctx->hevc.ctrls.sps->pic_width_in_luma_samples,
				ctx->hevc.ctrls.sps->pic_height_in_luma_samples);

	th1520_vdec_end_prepare_run(ctx);
	th1520_vdec_start(ctx->dev);

	return 0;

err_complete_request:
	/*
	 * 出错时也必须把 request 的控件标记完成，否则用户态会一直等。
	 * end_prepare_run() 里的看门狗不启动，因为硬件没有被拉起来。
	 */
	v4l2_ctrl_request_complete(th1520_vdec_get_src_buf(ctx)->vb2_buf.req_obj.req,
				   &ctx->ctrl_handler);
	return ret;
}

static void th1520_hevc_reset(struct th1520_vdec_ctx *ctx)
{
	th1520_vdec_hw_reset(ctx);
}

static int th1520_hevc_init(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct th1520_vdec_hevc_ctx *hevc = &ctx->hevc;
	unsigned int size;

	memset(hevc, 0, sizeof(*hevc));

	/*
	 * tile 尺寸表：最多 MAX_TILE_COLS * MAX_TILE_ROWS 个 tile，
	 * 每个 tile 两个 u16（宽、高）。这里按上游的做法留出 4 个 u16 的余量
	 * 并多留 16 字节，再对齐到 16 字节。
	 */
	size = round_up(MAX_TILE_COLS * MAX_TILE_ROWS * 4 * sizeof(u16) + 16,
			16);
	hevc->tile_sizes.cpu = dma_alloc_coherent(vpu->dev, size,
						  &hevc->tile_sizes.dma,
						  GFP_KERNEL);
	if (!hevc->tile_sizes.cpu)
		return -ENOMEM;
	hevc->tile_sizes.size = size;

	hevc->scaling_lists.cpu = dma_alloc_coherent(vpu->dev,
						     SCALING_LIST_SIZE,
						     &hevc->scaling_lists.dma,
						     GFP_KERNEL);
	if (!hevc->scaling_lists.cpu) {
		dma_free_coherent(vpu->dev, hevc->tile_sizes.size,
				  hevc->tile_sizes.cpu, hevc->tile_sizes.dma);
		hevc->tile_sizes.cpu = NULL;
		return -ENOMEM;
	}
	hevc->scaling_lists.size = SCALING_LIST_SIZE;

	return 0;
}

static void th1520_hevc_free(struct th1520_vdec_dev *vpu,
			     struct th1520_vdec_aux_buf *buf)
{
	if (!buf->cpu)
		return;

	dma_free_coherent(vpu->dev, buf->size, buf->cpu, buf->dma);
	buf->cpu = NULL;
	buf->size = 0;
}

static void th1520_hevc_exit(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct th1520_vdec_hevc_ctx *hevc = &ctx->hevc;

	th1520_hevc_free(vpu, &hevc->tile_sizes);
	th1520_hevc_free(vpu, &hevc->scaling_lists);
	th1520_hevc_free(vpu, &hevc->tile_filter);
	th1520_hevc_free(vpu, &hevc->tile_sao);
	th1520_hevc_free(vpu, &hevc->tile_bsd);
	hevc->num_tile_cols_allocated = 0;
}

const struct th1520_vdec_codec_ops th1520_vdec_hevc_ops = {
	.init = th1520_hevc_init,
	.exit = th1520_hevc_exit,
	.run = th1520_hevc_run,
	.reset = th1520_hevc_reset,
};
