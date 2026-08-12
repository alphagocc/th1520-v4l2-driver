// SPDX-License-Identifier: GPL-2.0
/*
 * TH1520 VC8000D — H.264 (G1 legacy, DEC_MODE = 0) stateless 后端。
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * 控件到寄存器的映射来自 VC8000D 产品表（product id 0x8001，
 * 表 @0x4CBB80；位域图 analysis/vc8000d-register-config/swreg-map-vc8000d.md，
 * 分析与 golden 对照见 analysis/vc8000d-product-table.md）。
 * G1 表（@0x4D40A0）的字段位置对 TH1520 无效——产品表把 H.264 的
 * 地址寄存器统一到了 HEVC 风格位置（STREAM@168/169、OUT@64/65、
 * REF@66+2i/67+2i、DIRMV@132/133、QTABLE@174/175），START_CODE_E/INIT_QP
 * 统一在 swreg13[31]/[30:24]，STREAM_LEN 为 32 bit。
 *
 * DPB 匹配、参考列表构建与 CABAC/POC/缩放矩阵表的布局，参考上游
 * drivers/media/platform/verisilicon/{hantro_h264.c,hantro_g1_h264_dec.c}
 * （GPL-2.0，Copyright (c) 2014 Rockchip Electronics / Google Inc.，
 *  Linux commit 8ba098e6b6ff0db8edf28528d1552be261af30d4）。
 *
 * 产品表与上游 G1 布局同位置的寄存器（已逐 id 核对，见
 * analysis/vc8000d-product-table.md §5.4）：
 *   REFERn_NBR         swreg30+i（每个寄存器两个 16 bit frame_num）
 *   REFER_LTERM_E      swreg38
 *   REFER_VALID_E      swreg39
 *   BINIT_RLIST_*      swreg42+i（B 帧 L0/L1 初始列表）
 *   B_REF_PIC 第15项+P前4项 swreg47
 *   PINIT_RLIST_F4..15 swreg10/11（P 帧 L0 初始列表）
 *   PRED_BC_TAP_0_x    swreg49
 * 与上游 G1 布局不同、已按产品表改写的：全部地址寄存器、
 * START_CODE_E/INIT_QP、STRM_START_BIT（7 bit）、STREAM_LEN（32 bit）、
 * APF_THRESHOLD（16 bit）、swreg2（只有 swap 域+CLK_GATE_E）、
 * MAX_BURST/BUSWIDTH/AXI_RD_ID_E（swreg58）、超时（swreg318/319）。
 */

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/swab.h>

#include <media/v4l2-h264.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "th1520_vdec.h"

/* POC 表：16 个参考帧各两个场的 POC，外加当前帧的两个场 */
#define POC_BUFFER_LEN			34
/* 缩放矩阵：6 个 4x4 表 + 2 个 8x8 表（只有 Intra/Inter Y） */
#define SCALING_LIST_LEN		(6 * 16 + 2 * 64)

/*
 * 硬件读取的私有表布局。QTABLE_BASE（swreg40/140）指向本结构开头。
 * 三段的顺序与上游 struct hantro_h264_dec_priv_tbl 一致。
 */
struct th1520_vdec_h264_priv_tbl {
	u32 cabac_table[TH1520_H264_CABAC_TABLE_LEN];
	u32 poc[POC_BUFFER_LEN];
	u8 scaling_list[SCALING_LIST_LEN];
};

/*
 * REFER_VALID_E / REFER_LTERM_E（swreg39 / swreg38）中位序是反的：
 * bit31 对应 DPB 槽 0。帧解码用 bit31..bit16，场解码用全部 32 bit
 * （bit31 顶场、bit30 底场，依此类推）。
 */
#define REF_BIT(i)			BIT(32 - 1 - (i))

/* ---------------------------------------------------------------------- */

static void th1520_h264_assemble_scaling_list(struct th1520_vdec_ctx *ctx)
{
	const struct th1520_vdec_h264_ctrls *ctrls = &ctx->h264.ctrls;
	const struct v4l2_ctrl_h264_scaling_matrix *scaling = ctrls->scaling;
	const struct v4l2_ctrl_h264_pps *pps = ctrls->pps;
	struct th1520_vdec_h264_priv_tbl *tbl = ctx->h264.priv.cpu;
	const size_t list_len_4x4 = ARRAY_SIZE(scaling->scaling_list_4x4[0]);
	const size_t list_len_8x8 = ARRAY_SIZE(scaling->scaling_list_8x8[0]);
	u32 *dst = (u32 *)tbl->scaling_list;
	const u32 *src;
	unsigned int i, j;

	if (!(pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT))
		return;

	/*
	 * 硬件按大端读取每个 4 字节组，因此写入前做 32 bit 字节交换
	 * （与上游 assemble_scaling_list() 一致）。
	 */
	for (i = 0; i < ARRAY_SIZE(scaling->scaling_list_4x4); i++) {
		src = (const u32 *)&scaling->scaling_list_4x4[i];
		for (j = 0; j < list_len_4x4 / 4; j++)
			*dst++ = swab32(src[j]);
	}

	/* 只有 Intra Y 与 Inter Y 两张 8x8 表 */
	for (i = 0; i < 2; i++) {
		src = (const u32 *)&scaling->scaling_list_8x8[i];
		for (j = 0; j < list_len_8x8 / 4; j++)
			*dst++ = swab32(src[j]);
	}
}

static void th1520_h264_prepare_table(struct th1520_vdec_ctx *ctx)
{
	const struct th1520_vdec_h264_ctrls *ctrls = &ctx->h264.ctrls;
	const struct v4l2_ctrl_h264_decode_params *dec = ctrls->decode;
	const struct v4l2_ctrl_h264_sps *sps = ctrls->sps;
	struct th1520_vdec_h264_priv_tbl *tbl = ctx->h264.priv.cpu;
	const struct v4l2_h264_dpb_entry *dpb = ctx->h264.dpb;
	u32 dpb_longterm = 0;
	u32 dpb_valid = 0;
	unsigned int i;

	for (i = 0; i < TH1520_DPB_SIZE; i++) {
		tbl->poc[i * 2] = dpb[i].top_field_order_cnt;
		tbl->poc[i * 2 + 1] = dpb[i].bottom_field_order_cnt;

		if (!(dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID))
			continue;

		if (dec->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC) {
			if (dpb[i].fields & V4L2_H264_TOP_FIELD_REF)
				dpb_valid |= REF_BIT(i * 2);
			if (dpb[i].fields & V4L2_H264_BOTTOM_FIELD_REF)
				dpb_valid |= REF_BIT(i * 2 + 1);
			if (dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM) {
				dpb_longterm |= REF_BIT(i * 2);
				dpb_longterm |= REF_BIT(i * 2 + 1);
			}
		} else {
			dpb_valid |= REF_BIT(i);
			if (dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM)
				dpb_longterm |= REF_BIT(i);
		}
	}

	ctx->h264.dpb_valid = dpb_valid;
	ctx->h264.dpb_longterm = dpb_longterm;

	if ((dec->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC) ||
	    !(sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD)) {
		tbl->poc[32] = ctx->h264.cur_poc;
		tbl->poc[33] = 0;
	} else {
		tbl->poc[32] = dec->top_field_order_cnt;
		tbl->poc[33] = dec->bottom_field_order_cnt;
	}

	th1520_h264_assemble_scaling_list(ctx);
}

/*
 * 把 request 里的新 DPB 与驱动维护的 DPB 做匹配，尽量保持槽位稳定：
 * 参考帧地址寄存器是按槽位编号的，槽位漂移会让参考列表索引失效。
 * 匹配依据是 reference_ts（与 CAPTURE buffer 的 timestamp 对应）。
 */
static bool th1520_h264_dpb_match(const struct v4l2_h264_dpb_entry *a,
				  const struct v4l2_h264_dpb_entry *b)
{
	return a->reference_ts == b->reference_ts;
}

static void th1520_h264_update_dpb(struct th1520_vdec_ctx *ctx)
{
	const struct v4l2_ctrl_h264_decode_params *dec = ctx->h264.ctrls.decode;
	DECLARE_BITMAP(new_entries, TH1520_DPB_SIZE) = { 0 };
	DECLARE_BITMAP(used, TH1520_DPB_SIZE) = { 0 };
	unsigned int i, j;

	for (i = 0; i < TH1520_DPB_SIZE; i++)
		ctx->h264.dpb[i].flags = 0;

	for (i = 0; i < ARRAY_SIZE(dec->dpb) && i < TH1520_DPB_SIZE; i++) {
		const struct v4l2_h264_dpb_entry *ndpb = &dec->dpb[i];

		if (!(ndpb->flags & V4L2_H264_DPB_ENTRY_FLAG_VALID))
			continue;

		for_each_clear_bit(j, used, TH1520_DPB_SIZE) {
			if (!th1520_h264_dpb_match(&ctx->h264.dpb[j], ndpb))
				continue;

			ctx->h264.dpb[j] = *ndpb;
			__set_bit(j, used);
			break;
		}

		if (j == TH1520_DPB_SIZE)
			__set_bit(i, new_entries);
	}

	/* 匹配不上的条目占用剩余空槽。 */
	for_each_set_bit(i, new_entries, TH1520_DPB_SIZE) {
		j = find_first_zero_bit(used, TH1520_DPB_SIZE);
		if (WARN_ON(j >= TH1520_DPB_SIZE))
			return;

		ctx->h264.dpb[j] = dec->dpb[i];
		__set_bit(j, used);
	}
}

/*
 * 参考帧地址寄存器（swreg14..29）的低 2 bit 被复用为标志位：
 *   bit1 = REFERn_FIELD_E（该参考项是场）
 *   bit0 = REFERn_TOPC_E （使用顶场）
 * 解码缓冲至少 16 字节对齐，所以低 2 bit 一定为 0，可以安全复用。
 */
static dma_addr_t th1520_h264_get_ref_buf(struct th1520_vdec_ctx *ctx,
					  unsigned int dpb_idx)
{
	const struct v4l2_h264_dpb_entry *dpb = ctx->h264.dpb;
	s32 cur_poc = ctx->h264.cur_poc;
	dma_addr_t dma_addr = 0;
	u32 flags;

	if (dpb[dpb_idx].flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE)
		dma_addr = th1520_vdec_get_ref(ctx, dpb[dpb_idx].reference_ts);

	if (!dma_addr) {
		/*
		 * 槽位无效或未使用时指向当前输出帧，避免硬件读到随机地址。
		 */
		struct vb2_v4l2_buffer *dst_buf = th1520_vdec_get_dst_buf(ctx);

		dma_addr = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	}

	flags = (dpb[dpb_idx].flags & V4L2_H264_DPB_ENTRY_FLAG_FIELD) ? 0x2 : 0;
	flags |= abs(dpb[dpb_idx].top_field_order_cnt - cur_poc) <
		 abs(dpb[dpb_idx].bottom_field_order_cnt - cur_poc) ? 0x1 : 0;

	return dma_addr | flags;
}

static u16 th1520_h264_get_ref_nbr(struct th1520_vdec_ctx *ctx,
				   unsigned int dpb_idx)
{
	const struct v4l2_h264_dpb_entry *dpb = &ctx->h264.dpb[dpb_idx];

	if (!(dpb->flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
		return 0;

	return dpb->frame_num;
}

/*
 * 场解码时同一个缓冲会在参考列表里出现两次（顶场和底场各一次）。
 * 去掉与当前图像同奇偶性的项，剩下的就是互补场。
 * 与上游 deduplicate_reflist() 一致（上游注明这是实测一致性得分最高的做法）。
 */
static void th1520_h264_dedup_reflist(struct v4l2_h264_reflist_builder *b,
				      struct v4l2_h264_reference *reflist)
{
	unsigned int write_idx = 0;
	unsigned int i;

	if (b->cur_pic_fields == V4L2_H264_FRAME_REF) {
		write_idx = b->num_valid;
		goto done;
	}

	for (i = 0; i < b->num_valid; i++)
		if (b->cur_pic_fields != reflist[i].fields)
			reflist[write_idx++] = reflist[i];

done:
	if (WARN_ON(write_idx > TH1520_DPB_SIZE))
		write_idx = TH1520_DPB_SIZE;

	/* 清掉剩余项，否则部分码流会解码失败。 */
	for (; write_idx < TH1520_DPB_SIZE; write_idx++)
		reflist[write_idx].index = TH1520_DPB_SIZE - 1;
}

/* ---------------------------------------------------------------------- */

static void th1520_h264_set_params(struct th1520_vdec_ctx *ctx)
{
	const struct th1520_vdec_h264_ctrls *ctrls = &ctx->h264.ctrls;
	const struct v4l2_ctrl_h264_decode_params *dec = ctrls->decode;
	const struct v4l2_ctrl_h264_sps *sps = ctrls->sps;
	const struct v4l2_ctrl_h264_pps *pps = ctrls->pps;
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src_buf = th1520_vdec_get_src_buf(ctx);
	unsigned int mb_height;
	u32 stream_len;

	/* swreg3 —— 图像结构 */
	th1520_vdec_reg_write(vpu, &h264_seq_mbaff_e,
			      !!(sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD));
	if (sps->profile_idc > 66) {
		th1520_vdec_reg_write(vpu, &h264_picord_count_e, 1);
		if (dec->nal_ref_idc)
			th1520_vdec_reg_write(vpu, &h264_write_mvs_e, 1);
	}

	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) &&
	    ((sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD) ||
	     (dec->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC)))
		th1520_vdec_reg_write(vpu, &h264_pic_interlace_e, 1);

	if (dec->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC)
		th1520_vdec_reg_write(vpu, &h264_pic_fieldmode_e, 1);

	if (!(dec->flags & V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD))
		th1520_vdec_reg_write(vpu, &h264_pic_topfield_e, 1);

	/* swreg4 —— 图像尺寸（单位：宏块）与参考帧数 */
	mb_height = TH1520_MB_HEIGHT(ctx->src_fmt.height);
	th1520_vdec_reg_write(vpu, &h264_pic_mb_width,
			      TH1520_MB_WIDTH(ctx->src_fmt.width));
	th1520_vdec_reg_write(vpu, &h264_pic_mb_height_p, mb_height);
	/* PIC_MB_HEIGHT_P 只有 8 bit，>255 时高位放在 swreg7[25]。 */
	th1520_vdec_reg_write(vpu, &h264_pic_mb_h_ext, mb_height >> 8);
	th1520_vdec_reg_write(vpu, &h264_ref_frames, sps->max_num_ref_frames);

	/* swreg5 —— 色度 QP 偏移与场图像标志 */
	th1520_vdec_reg_write(vpu, &h264_ch_qp_offset,
			      pps->chroma_qp_index_offset);
	th1520_vdec_reg_write(vpu, &h264_ch_qp_offset2,
			      pps->second_chroma_qp_index_offset);
	th1520_vdec_reg_write(vpu, &h264_type1_quant_e,
			      !!(pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT));
	th1520_vdec_reg_write(vpu, &h264_fieldpic_flag_e,
			      !(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY));
	/* 每个 OUTPUT buffer 从字节边界开始，起始位偏移恒为 0。 */
	th1520_vdec_reg_write(vpu, &h264_strm_start_bit, 0);

	/* swreg6 —— 起始码搜索、初始 QP 与码流长度 */
	th1520_vdec_reg_write(vpu, &h264_start_code_e, 1);
	th1520_vdec_reg_write(vpu, &h264_init_qp,
			      pps->pic_init_qp_minus26 + 26);
	stream_len = vb2_get_plane_payload(&src_buf->vb2_buf, 0);
	th1520_vdec_reg_write(vpu, &h264_stream_len, stream_len);

	/* swreg7 —— 熵编码与 frame_num */
	th1520_vdec_reg_write(vpu, &h264_framenum_len,
			      sps->log2_max_frame_num_minus4 + 4);
	th1520_vdec_reg_write(vpu, &h264_framenum, dec->frame_num);
	th1520_vdec_reg_write(vpu, &h264_weight_bipr_idc,
			      pps->weighted_bipred_idc);
	th1520_vdec_reg_write(vpu, &h264_cabac_e,
			      !!(pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE));
	th1520_vdec_reg_write(vpu, &h264_dir_8x8_infer_e,
			      !!(sps->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE));
	th1520_vdec_reg_write(vpu, &h264_blackwhite_e,
			      sps->profile_idc >= 100 &&
			      sps->chroma_format_idc == 0);
	th1520_vdec_reg_write(vpu, &h264_weight_pred_e,
			      !!(pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED));

	/* swreg8 —— PPS 标志、dec_ref_pic_marking 长度与 IDR */
	th1520_vdec_reg_write(vpu, &h264_refpic_mk_len,
			      dec->dec_ref_pic_marking_bit_size);
	th1520_vdec_reg_write(vpu, &h264_idr_pic_id, dec->idr_pic_id);
	th1520_vdec_reg_write(vpu, &h264_const_intra_e,
			      !!(pps->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED));
	th1520_vdec_reg_write(vpu, &h264_filt_ctrl_pres,
			      !!(pps->flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT));
	th1520_vdec_reg_write(vpu, &h264_rdpic_cnt_pres,
			      !!(pps->flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT));
	th1520_vdec_reg_write(vpu, &h264_8x8trans_flag_e,
			      !!(pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE));
	th1520_vdec_reg_write(vpu, &h264_idr_pic_e,
			      !!(dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC));

	/* swreg9 —— PPS id、活动参考索引数与 POC 语法长度 */
	th1520_vdec_reg_write(vpu, &h264_pps_id, pps->pic_parameter_set_id);
	th1520_vdec_reg_write(vpu, &h264_refidx0_active,
			      pps->num_ref_idx_l0_default_active_minus1 + 1);
	th1520_vdec_reg_write(vpu, &h264_refidx1_active,
			      pps->num_ref_idx_l1_default_active_minus1 + 1);
	th1520_vdec_reg_write(vpu, &h264_poc_length,
			      dec->pic_order_cnt_bit_size);

	/* swreg314 —— 输出 stride */
	th1520_vdec_reg_write(vpu, &h264_dec_out_y_stride,
			      ctx->dst_fmt.plane_fmt[0].bytesperline);
	th1520_vdec_reg_write(vpu, &h264_dec_out_c_stride,
			      ctx->dst_fmt.plane_fmt[0].bytesperline);
}

static void th1520_h264_set_ref(struct th1520_vdec_ctx *ctx)
{
	const struct v4l2_h264_reference *b0, *b1, *p;
	struct th1520_vdec_dev *vpu = ctx->dev;
	unsigned int i, reg_num;
	u32 reg;

	th1520_vdec_reg_write_raw(vpu, TH1520_H264_SWREG_VALID_REF,
				  ctx->h264.dpb_valid);
	th1520_vdec_reg_write_raw(vpu, TH1520_H264_SWREG_LT_REF,
				  ctx->h264.dpb_longterm);

	/* swreg30..37：每个寄存器两个参考帧的 frame_num */
	for (i = 0; i < TH1520_DPB_SIZE; i += 2) {
		reg = TH1520_H264_REF_NBR_EVEN(th1520_h264_get_ref_nbr(ctx, i)) |
		      TH1520_H264_REF_NBR_ODD(th1520_h264_get_ref_nbr(ctx, i + 1));
		th1520_vdec_reg_write_raw(vpu,
					  TH1520_H264_SWREG_REF_PIC(i / 2), reg);
	}

	b0 = ctx->h264.reflists.b0;
	b1 = ctx->h264.reflists.b1;
	p = ctx->h264.reflists.p;

	/* swreg42..46：B 帧 L0/L1 的第 0..14 项，每个寄存器三组 */
	reg_num = 0;
	for (i = 0; i < 15; i += 3) {
		reg = ((b0[i].index     & 0x1f) << 0) |
		      ((b1[i].index     & 0x1f) << 5) |
		      ((b0[i + 1].index & 0x1f) << 10) |
		      ((b1[i + 1].index & 0x1f) << 15) |
		      ((b0[i + 2].index & 0x1f) << 20) |
		      ((b1[i + 2].index & 0x1f) << 25);
		th1520_vdec_reg_write_raw(vpu,
					  TH1520_H264_SWREG_BD_REF_PIC(reg_num++),
					  reg);
	}

	/* swreg47：B 列表第 15 项 + P 列表第 0..3 项 */
	reg = ((b0[15].index & 0x1f) << 0) |
	      ((b1[15].index & 0x1f) << 5) |
	      ((p[0].index   & 0x1f) << 10) |
	      ((p[1].index   & 0x1f) << 15) |
	      ((p[2].index   & 0x1f) << 20) |
	      ((p[3].index   & 0x1f) << 25);
	th1520_vdec_reg_write_raw(vpu, TH1520_H264_SWREG_BD_P_REF_PIC, reg);

	/* swreg10/11：P 列表第 4..9 与 10..15 项 */
	reg_num = 0;
	for (i = 4; i < TH1520_DPB_SIZE; i += 6) {
		reg = ((p[i].index     & 0x1f) << 0) |
		      ((p[i + 1].index & 0x1f) << 5) |
		      ((p[i + 2].index & 0x1f) << 10) |
		      ((p[i + 3].index & 0x1f) << 15) |
		      ((p[i + 4].index & 0x1f) << 20) |
		      ((p[i + 5].index & 0x1f) << 25);
		th1520_vdec_reg_write_raw(vpu,
					  TH1520_H264_SWREG_FWD_PIC(reg_num++),
					  reg);
	}

	/* swreg14..29 / swreg124..139：16 路参考帧基址（低 2 bit 为标志位） */
	for (i = 0; i < TH1520_DPB_SIZE; i++)
		th1520_vdec_write_addr(vpu, TH1520_H264_ADDR_REF_LSB(i),
				       TH1520_H264_ADDR_REF_MSB(i),
				       th1520_h264_get_ref_buf(ctx, i));
}

static void th1520_h264_set_buffers(struct th1520_vdec_ctx *ctx)
{
	const struct th1520_vdec_h264_ctrls *ctrls = &ctx->h264.ctrls;
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src_buf = th1520_vdec_get_src_buf(ctx);
	struct vb2_v4l2_buffer *dst_buf = th1520_vdec_get_dst_buf(ctx);
	dma_addr_t src_dma, dst_dma;
	size_t offset = 0;

	src_dma = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	th1520_vdec_write_addr(vpu, TH1520_H264_ADDR_STREAM_LSB,
			       TH1520_H264_ADDR_STREAM_MSB, src_dma);

	dst_dma = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	/* 底场从第二行开始写。 */
	if (ctrls->decode->flags & V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD)
		offset = ctx->dst_fmt.plane_fmt[0].bytesperline;
	th1520_vdec_write_addr(vpu, TH1520_H264_ADDR_DST_LSB,
			       TH1520_H264_ADDR_DST_MSB, dst_dma + offset);

	/*
	 * direct-MV 缓冲紧跟在 NV12 帧之后。
	 * 帧大小 = bytesperline * ALIGN(height,16) * 3/2 = 384 * MB_W * MB_H，
	 * 与 th1520_vdec_fill_pixfmt_cap() 的布局一致。
	 *
	 * 与上游的差异：上游对单色码流用 256 字节/MB 计算偏移，因为它不为
	 * 单色分配色度平面；本驱动的 CAPTURE 始终是完整 NV12，
	 * 因此偏移统一按 384 字节/MB 计算，与自身的缓冲布局自洽。
	 */
	if (ctrls->sps->profile_idc > 66 && ctrls->decode->nal_ref_idc) {
		unsigned int mb_w = TH1520_MB_WIDTH(ctx->src_fmt.width);
		unsigned int mb_h = TH1520_MB_HEIGHT(ctx->src_fmt.height);

		offset = 384 * mb_w * mb_h;

		/* 场编码时 DMV 缓冲一分为二，底场用后半段。 */
		if (ctrls->decode->flags &
		    V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD)
			offset += 32 * mb_w * mb_h;

		th1520_vdec_write_addr(vpu, TH1520_H264_ADDR_DIR_MV_LSB,
				       TH1520_H264_ADDR_DIR_MV_MSB,
				       dst_dma + offset);
	}

	/* CABAC 表 / POC 表 / 缩放矩阵所在的私有 DMA 缓冲 */
	th1520_vdec_write_addr(vpu, TH1520_H264_ADDR_QTABLE_LSB,
			       TH1520_H264_ADDR_QTABLE_MSB,
			       ctx->h264.priv.dma);
}

static int th1520_h264_prepare_run(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_h264_ctrls *ctrls = &ctx->h264.ctrls;
	struct v4l2_h264_reflist_builder reflist_builder;

	th1520_vdec_start_prepare_run(ctx);

	ctrls->scaling =
		th1520_vdec_get_ctrl(ctx, V4L2_CID_STATELESS_H264_SCALING_MATRIX);
	ctrls->decode =
		th1520_vdec_get_ctrl(ctx, V4L2_CID_STATELESS_H264_DECODE_PARAMS);
	ctrls->sps = th1520_vdec_get_ctrl(ctx, V4L2_CID_STATELESS_H264_SPS);
	ctrls->pps = th1520_vdec_get_ctrl(ctx, V4L2_CID_STATELESS_H264_PPS);

	if (!ctrls->scaling || !ctrls->decode || !ctrls->sps || !ctrls->pps)
		return -EINVAL;

	th1520_h264_update_dpb(ctx);

	v4l2_h264_init_reflist_builder(&reflist_builder, ctrls->decode,
				       ctrls->sps, ctx->h264.dpb);
	ctx->h264.cur_poc = reflist_builder.cur_pic_order_count;

	th1520_h264_prepare_table(ctx);

	v4l2_h264_build_p_ref_list(&reflist_builder, ctx->h264.reflists.p);
	v4l2_h264_build_b_ref_lists(&reflist_builder, ctx->h264.reflists.b0,
				    ctx->h264.reflists.b1);

	/*
	 * 参考列表最多 16 项；场解码时硬件靠 dpb_valid / dpb_longterm 位图
	 * 加上当前图像的奇偶性自行推导出实际的场列表。
	 */
	if (reflist_builder.cur_pic_fields != V4L2_H264_FRAME_REF) {
		th1520_h264_dedup_reflist(&reflist_builder,
					  ctx->h264.reflists.p);
		th1520_h264_dedup_reflist(&reflist_builder,
					  ctx->h264.reflists.b0);
		th1520_h264_dedup_reflist(&reflist_builder,
					  ctx->h264.reflists.b1);
	}

	return 0;
}

static int th1520_h264_run(struct th1520_vdec_ctx *ctx)
{
	int ret;

	ret = th1520_h264_prepare_run(ctx);
	if (ret)
		goto err_complete_request;

	/*
	 * STREAM_LEN 在产品表中是完整 32 bit（与 HEVC 相同），
	 * G1 表的 24 bit 截断在 VC8000D 上不适用，无需再做长度检查。
	 */
	th1520_vdec_set_common_config(ctx);
	th1520_h264_set_params(ctx);
	th1520_h264_set_ref(ctx);
	th1520_h264_set_buffers(ctx);

	th1520_vdec_end_prepare_run(ctx);
	th1520_vdec_start(ctx->dev);

	return 0;

err_complete_request:
	v4l2_ctrl_request_complete(th1520_vdec_get_src_buf(ctx)->vb2_buf.req_obj.req,
				   &ctx->ctrl_handler);
	return ret;
}

static void th1520_h264_reset(struct th1520_vdec_ctx *ctx)
{
	th1520_vdec_hw_reset(ctx);
}

static int th1520_h264_init(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct th1520_vdec_h264_priv_tbl *tbl;

	memset(&ctx->h264, 0, sizeof(ctx->h264));

	ctx->h264.priv.cpu = dma_alloc_coherent(vpu->dev, sizeof(*tbl),
						&ctx->h264.priv.dma,
						GFP_KERNEL);
	if (!ctx->h264.priv.cpu)
		return -ENOMEM;

	ctx->h264.priv.size = sizeof(*tbl);
	tbl = ctx->h264.priv.cpu;
	memcpy(tbl->cabac_table, th1520_vdec_h264_cabac_table,
	       sizeof(tbl->cabac_table));

	return 0;
}

static void th1520_h264_exit(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;

	if (!ctx->h264.priv.cpu)
		return;

	dma_free_coherent(vpu->dev, ctx->h264.priv.size, ctx->h264.priv.cpu,
			  ctx->h264.priv.dma);
	ctx->h264.priv.cpu = NULL;
}

const struct th1520_vdec_codec_ops th1520_vdec_h264_ops = {
	.init = th1520_h264_init,
	.exit = th1520_h264_exit,
	.run = th1520_h264_run,
	.reset = th1520_h264_reset,
};
