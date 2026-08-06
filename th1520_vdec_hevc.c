// SPDX-License-Identifier: GPL-2.0
/*
 * TH1520 VC8000D — HEVC (G2, DEC_MODE = 12) stateless 后端。
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * 控件到寄存器的映射来自 analysis/vc8000d-register-config/swreg-map-hevc.md
 * （由真实二进制的 HEVC 规格表 @0x4DC5C0 dump 得到）。
 *
 * 语法元素 → 寄存器的对应关系另有上游
 * drivers/media/platform/verisilicon/hantro_g2_hevc_dec.c（GPL-2.0，
 * Copyright (C) 2020 Safran Passenger Innovations LLC，
 * Linux commit 8ba098e6b6ff0db8edf28528d1552be261af30d4）作交叉验证：
 * 本文件用到的 G2 位域中，上游同名定义与二进制规格表逐位吻合。
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

static void th1520_hevc_ref_init(struct th1520_vdec_ctx *ctx)
{
	ctx->hevc.ref_bufs_used = 0;
}

static dma_addr_t th1520_hevc_get_ref_buf(struct th1520_vdec_ctx *ctx, s32 poc)
{
	struct th1520_vdec_hevc_ctx *hevc = &ctx->hevc;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hevc->ref_bufs); i++) {
		if (hevc->ref_bufs_poc[i] == poc) {
			hevc->ref_bufs_used |= BIT(i);
			return hevc->ref_bufs[i];
		}
	}

	return 0;
}

static int th1520_hevc_add_ref_buf(struct th1520_vdec_ctx *ctx, s32 poc,
				   dma_addr_t addr)
{
	struct th1520_vdec_hevc_ctx *hevc = &ctx->hevc;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hevc->ref_bufs); i++) {
		if (!(hevc->ref_bufs_used & BIT(i))) {
			hevc->ref_bufs_used |= BIT(i);
			hevc->ref_bufs_poc[i] = poc;
			hevc->ref_bufs[i] = addr;
			return 0;
		}
	}

	return -EINVAL;
}

/*
 * CAPTURE buffer 内部布局（与 th1520_vdec_fill_pixfmt_cap() 保持一致）：
 *
 *   +0                       luma (Y)
 *   +chroma_offset           chroma (interleaved CbCr)
 *   +mv_offset               direct-MV (colocated) 缓冲
 */
static size_t th1520_hevc_chroma_offset(struct th1520_vdec_ctx *ctx)
{
	return ctx->dst_fmt.plane_fmt[0].bytesperline *
	       ALIGN(ctx->dst_fmt.height, TH1520_MB_DIM);
}

static size_t th1520_hevc_mv_offset(struct th1520_vdec_ctx *ctx)
{
	return ALIGN(th1520_hevc_chroma_offset(ctx) * 3 / 2, 16);
}

/* ---------------------------------------------------------------------- */

static int th1520_hevc_tile_buffers_realloc(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct th1520_vdec_hevc_ctx *hevc = &ctx->hevc;
	const struct v4l2_ctrl_hevc_pps *pps = hevc->ctrls.pps;
	const struct v4l2_ctrl_hevc_sps *sps = hevc->ctrls.sps;
	unsigned int num_tile_cols = pps->num_tile_columns_minus1 + 1;
	unsigned int height64 = ALIGN(sps->pic_height_in_luma_samples, 64);
	unsigned int size;

	if (num_tile_cols <= 1 ||
	    num_tile_cols <= hevc->num_tile_cols_allocated)
		return 0;

	if (hevc->tile_filter.cpu) {
		dma_free_coherent(vpu->dev, hevc->tile_filter.size,
				  hevc->tile_filter.cpu, hevc->tile_filter.dma);
		hevc->tile_filter.cpu = NULL;
	}
	if (hevc->tile_sao.cpu) {
		dma_free_coherent(vpu->dev, hevc->tile_sao.size,
				  hevc->tile_sao.cpu, hevc->tile_sao.dma);
		hevc->tile_sao.cpu = NULL;
	}
	if (hevc->tile_bsd.cpu) {
		dma_free_coherent(vpu->dev, hevc->tile_bsd.size,
				  hevc->tile_bsd.cpu, hevc->tile_bsd.dma);
		hevc->tile_bsd.cpu = NULL;
	}

	size = (VERT_FILTER_RAM_SIZE * height64 * (num_tile_cols - 1) *
		ctx->bit_depth) / 8;
	hevc->tile_filter.cpu = dma_alloc_coherent(vpu->dev, size,
						   &hevc->tile_filter.dma,
						   GFP_KERNEL);
	if (!hevc->tile_filter.cpu)
		return -ENOMEM;
	hevc->tile_filter.size = size;

	size = (VERT_SAO_RAM_SIZE * height64 * (num_tile_cols - 1) *
		ctx->bit_depth) / 8;
	hevc->tile_sao.cpu = dma_alloc_coherent(vpu->dev, size,
						&hevc->tile_sao.dma,
						GFP_KERNEL);
	if (!hevc->tile_sao.cpu)
		goto err_free_filter;
	hevc->tile_sao.size = size;

	size = BSD_CTRL_RAM_SIZE * height64 * (num_tile_cols - 1);
	hevc->tile_bsd.cpu = dma_alloc_coherent(vpu->dev, size,
						&hevc->tile_bsd.dma,
						GFP_KERNEL);
	if (!hevc->tile_bsd.cpu)
		goto err_free_sao;
	hevc->tile_bsd.size = size;

	hevc->num_tile_cols_allocated = num_tile_cols;
	return 0;

err_free_sao:
	dma_free_coherent(vpu->dev, hevc->tile_sao.size, hevc->tile_sao.cpu,
			  hevc->tile_sao.dma);
	hevc->tile_sao.cpu = NULL;
err_free_filter:
	dma_free_coherent(vpu->dev, hevc->tile_filter.size,
			  hevc->tile_filter.cpu, hevc->tile_filter.dma);
	hevc->tile_filter.cpu = NULL;
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

	if (!tiles_enabled) {
		/* 同 INIT_QP：_V0 是与新版重叠的旧修订位域，不能一起写。 */
		th1520_vdec_reg_write(vpu, &hevc_num_tile_rows, 1);
		th1520_vdec_reg_write(vpu, &hevc_num_tile_cols, 1);

		/* 只有一个 tile，尺寸等于整幅图像。 */
		p[0] = pic_width_in_ctbs;
		p[1] = pic_height_in_ctbs;
		return;
	}

	th1520_vdec_reg_write(vpu, &hevc_num_tile_rows, num_tile_rows);
	th1520_vdec_reg_write(vpu, &hevc_num_tile_cols, num_tile_cols);

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
	th1520_vdec_reg_write(vpu, &hevc_slice_hdr_ebits,
			      pps->num_extra_slice_header_bits);
	th1520_vdec_reg_write(vpu, &hevc_slice_chqp_flag,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT));
	th1520_vdec_reg_write(vpu, &hevc_weight_pred_e,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED));
	th1520_vdec_reg_write(vpu, &hevc_weight_bipr_idc,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED));
	th1520_vdec_reg_write(vpu, &hevc_depend_slice_e,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_filt_override_e,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_pcm_filt_disable,
			      !!(sps->flags & V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED));
	th1520_vdec_reg_write(vpu, &hevc_cabac_init_present,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT));
	/* 单色（4:0:0）在 try_ctrl 里已被拒绝，这里恒为 0。 */
	th1520_vdec_reg_write(vpu, &hevc_blackwhite_e, 0);

	/* swreg12 —— CB / PCM / 变换标志 */
	th1520_vdec_reg_write(vpu, &hevc_transform_skip_e,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_transq_bypass_e,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_refpiclist_mod_e,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT));
	th1520_vdec_reg_write(vpu, &hevc_pcm_e,
			      !!(sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED));
	if (sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED) {
		th1520_vdec_reg_write(vpu, &hevc_max_pcm_size,
				      sps->log2_diff_max_min_pcm_luma_coding_block_size +
				      sps->log2_min_pcm_luma_coding_block_size_minus3 + 3);
		th1520_vdec_reg_write(vpu, &hevc_min_pcm_size,
				      sps->log2_min_pcm_luma_coding_block_size_minus3 + 3);
		th1520_vdec_reg_write(vpu, &hevc_pcm_bitdepth_y,
				      sps->pcm_sample_bit_depth_luma_minus1 + 1);
		th1520_vdec_reg_write(vpu, &hevc_pcm_bitdepth_c,
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

	th1520_vdec_reg_write(vpu, &hevc_entr_code_synch_e,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED));
	/*
	 * 只写新版（7 bit）INIT_QP。
	 *
	 * 规格表里 swreg10 同时存在旧修订的窄位域 INIT_QP_V0[30:25]，
	 * 它与 INIT_QP[30:24] **重叠**。.so 之所以两套都写，是为了兼容不同
	 * HW 修订，但那依赖特定的写入顺序 —— 两个都写等于把值破坏掉。
	 *
	 * 目标板实测：product id = 0x8001（新版 VC8000D），
	 * 且 INIT_QP[30:24] 与上游 mainline 的 g2_init_qp（shift 24, mask 0x7f）
	 * 定义一致。因此这里只写新版位域。
	 *
	 * （曾经两个都写，导致 QP 26 被写成 52，见 README §6.0 的调试记录。）
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
	if (index > dec->num_active_dpb_entries)
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
	size_t cr_offset = th1520_hevc_chroma_offset(ctx);
	size_t mv_offset = th1520_hevc_mv_offset(ctx);
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

	th1520_vdec_reg_write(vpu, &hevc_filt_slice_border,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED));
	th1520_vdec_reg_write(vpu, &hevc_filt_tile_border,
			      !!(pps->flags & V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED));

	/* 写各参考帧与当前帧的 POC 差 */
	for (i = 0; i < dec->num_active_dpb_entries &&
	     i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++) {
		s8 poc_diff = dec->pic_order_cnt_val -
			      dpb[i].pic_order_cnt_val;

		th1520_hevc_write_cur_poc(vpu, i, (u8)poc_diff);
	}
	/* 紧跟参考帧之后放一项指向自身（差为 0） */
	if (i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
		th1520_hevc_write_cur_poc(vpu, i++, 0);
	/* 其余槽位填当前帧的 POC */
	for (; i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++)
		th1520_hevc_write_cur_poc(vpu, i, dec->pic_order_cnt_val);

	th1520_hevc_set_ref_pic_list(ctx);

	/* 只保留仍在使用的参考帧记录 */
	th1520_hevc_ref_init(ctx);

	for (i = 0; i < dec->num_active_dpb_entries &&
	     i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX - 1; i++) {
		luma_addr = th1520_hevc_get_ref_buf(ctx,
						    dpb[i].pic_order_cnt_val);
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
	luma_addr = vb2_dma_contig_plane_dma_addr(&vb2_dst->vb2_buf, 0);
	if (!luma_addr)
		return -EFAULT;

	if (th1520_hevc_add_ref_buf(ctx, dec->pic_order_cnt_val, luma_addr))
		return -EINVAL;

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

	/* 影子寄存器已清零，剩余槽位天然为 0，无需再显式写。 */

	th1520_vdec_reg_write(vpu, &hevc_refer_lterm_e, dpb_longterm);

	return 0;
}

static void th1520_hevc_set_buffers(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src_buf = th1520_vdec_get_src_buf(ctx);
	dma_addr_t src_dma;
	u32 src_len, src_buf_len;

	src_dma = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	src_len = vb2_get_plane_payload(&src_buf->vb2_buf, 0);
	src_buf_len = vb2_plane_size(&src_buf->vb2_buf, 0);

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
				(int)min_t(u32, src_len, 16), p);
	}

	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_STREAM, src_dma);
	th1520_vdec_reg_write(vpu, &hevc_stream_len, src_len);
	th1520_vdec_reg_write(vpu, &hevc_strm_buffer_len, src_buf_len);
	th1520_vdec_reg_write(vpu, &hevc_strm_start_offset, 0);
	th1520_vdec_reg_write(vpu, &hevc_strm_start_bit, 0);
	/* 硬件自行搜索 Annex-B 起始码。 */
	th1520_vdec_reg_write(vpu, &hevc_start_code_e, 1);
	/* 必须写 MV，否则后续帧无法做时域预测。 */
	th1520_vdec_reg_write(vpu, &hevc_write_mvs_e, 1);

	/*
	 * LAST_BUFFER_E：告诉硬件"整帧数据都在这一个 buffer 里，后面没有了"。
	 *
	 * 本驱动一次提交一个完整 AU，不做环形缓冲续流，所以恒为 1。
	 * 上游 mainline 不写这一位（依赖复位值），但本驱动每帧把影子寄存器清零，
	 * 任何不显式写的位都会变成 0 —— 若该位复位值为 1，就会被我们错误地清掉，
	 * 硬件会一直等待后续数据而不产生中断。
	 *
	 * BUFFER_EMPTY_INT_E 一并打开：万一硬件仍然认为码流不足，
	 * 它会产生 DEC_BUFFER_INT 让驱动能报错，而不是静默挂死。
	 */
	th1520_vdec_reg_write(vpu, &hevc_last_buffer_e, 1);
	th1520_vdec_reg_write(vpu, &hevc_buffer_empty_int_e, 1);

	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_TILE_SIZES,
				    ctx->hevc.tile_sizes.dma);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_TILE_FILTER,
				    ctx->hevc.tile_filter.dma);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_TILE_SAO,
				    ctx->hevc.tile_sao.dma);
	th1520_vdec_write_addr_pair(vpu, TH1520_HEVC_ADDR_TILE_BSD,
				    ctx->hevc.tile_bsd.dma);

	/* swreg314 —— 输出 stride */
	th1520_vdec_reg_write(vpu, &hevc_dec_out_y_stride,
			      ctx->dst_fmt.plane_fmt[0].bytesperline);
	th1520_vdec_reg_write(vpu, &hevc_dec_out_c_stride,
			      ctx->dst_fmt.plane_fmt[0].bytesperline);
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

static int th1520_hevc_prepare_run(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_hevc_ctrls *ctrls = &ctx->hevc.ctrls;

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

	/*
	 * CAPTURE 队列的分辨率是按 OUTPUT 格式配置好的，
	 * 码流里的实际尺寸必须能装得下，否则硬件会写越界。
	 */
	if (ctrls->sps->pic_width_in_luma_samples > ctx->dst_fmt.width ||
	    ctrls->sps->pic_height_in_luma_samples > ctx->dst_fmt.height)
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

	th1520_hevc_ref_init(ctx);

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
