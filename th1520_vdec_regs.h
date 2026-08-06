/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TH1520 VC8000D decoder register definitions.
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * 证据来源（全部为“已验证事实”，除非另行标注）：
 *
 *   analysis/vc8000d-register-config/swreg-map-h264.md   (H.264 / G1, codec id 0x6731)
 *   analysis/vc8000d-register-config/swreg-map-hevc.md   (HEVC  / G2, codec id 0x6732)
 *
 * 这两份表由 analysis/build_swreg_map.py 直接从真实交付二进制
 * libOMX.hantro.VC8000D.video.decoder.so 的寄存器规格表
 * (H264 @0x4D40A0, HEVC @0x4DC5C0) dump 得到：swreg 编号、位宽和 shift
 * 均为二进制中的实测值，不是从参考源码推断的。
 *
 * 寄存器名来自 omx_il_g-master/.../8170enum.h，但经过三重独立验证
 * （g1/g2 规格表 LCS 匹配 1739/1742 条、SetCommonConfigRegs 调用序列
 * 逐条同名同序、.rodata 中 17 组 HWIF id 数组逐字节命中），
 * 并与上游 mainline drivers/media/platform/verisilicon/ 的 G1/G2
 * 寄存器定义做了第四重交叉对照。
 *
 * 与上游 mainline 已知不一致、且本文件以“二进制实测值”为准的位域：
 *   - DEC_PIC_SWAP：上游 swreg2 shift22/5bit，本二进制为 swreg2[27:24] 4bit。
 *   - DEC_RSCAN_SWAP：上游在 swreg3[6:2]，本二进制为 swreg2[3:0]。
 *   - DEC_TAB0..3_SWAP：上游为 swreg2/3 的 5bit “old” 变体，
 *     本二进制为 swreg2[19:16]/[15:12]/[11:8]/[7:4] 4bit。
 *   - 中断状态聚合域：上游 g2_dec_int_stat 只覆盖 swreg1[14:11]，
 *     本二进制的 DEC_IRQ_STAT (id 2127) 覆盖 [23:11] 共 13 bit。
 *   - 上游 g2_bit_depth_y/c（shift 21/17）在本二进制的规格表中不存在；
 *     本二进制使用 BIT_DEPTH_Y/C_MINUS8（与上游同名定义一致）。
 *
 * MMIO 模型：swregN 位于解码核寄存器块偏移 4*N。
 */

#ifndef TH1520_VDEC_REGS_H_
#define TH1520_VDEC_REGS_H_

#include <linux/bits.h>
#include <linux/types.h>

/*
 * 影子寄存器数组长度。
 *
 * 规格表中定义过的最大 swreg 为 H.264=336 / HEVC=317；本驱动不使用
 * 后处理（swreg320+），实际编程的最大 swreg 为 H.264 的 319（超时周期）。
 * 取 336 使得 flush 范围覆盖整张 H.264 规格表。
 *
 * 硬件侧可寻址范围更大（reference/vpu-vc8000d-kernel/linux/subsys_driver/subsys.c
 * 给出 VC8000D 的 iosize 为 1023*4），336 个寄存器 = 1344 字节，安全落在窗口内。
 */
#define TH1520_VDEC_REG_COUNT		336

#define TH1520_VDEC_REG_OFF(n)		((n) * 4)

/* swreg0：HW build id（只读）。软件影子中 [31:16] 被复用为 codec 选择器。 */
#define TH1520_VDEC_SWREG_ID		0
/* swreg1：中断状态 / 解码控制。硬件启动寄存器。 */
#define TH1520_VDEC_SWREG_IRQ		1

/**
 * struct th1520_vdec_reg - 一个 swreg 位域
 * @swreg: 寄存器编号（MMIO 偏移 = 4 * swreg）
 * @shift: 位域最低位
 * @mask:  未移位的位域掩码，即 (1 << 位宽) - 1
 */
struct th1520_vdec_reg {
	u16 swreg;
	u8 shift;
	u32 mask;
};

#define TH1520_REG(_swreg, _shift, _mask) \
	((const struct th1520_vdec_reg) { \
		.swreg = (_swreg), .shift = (_shift), .mask = (_mask), \
	})

/* ------------------------------------------------------------------------
 * swreg1 —— 中断状态与解码控制（H.264 与 HEVC 通用）
 *
 * 与内核侧 reference/vpu-vc8000d-kernel/linux/dwl/dwl_defs.h 三向吻合：
 * HANTRODEC_DEC_E=0x01, DEC_IRQ_DISABLE=0x10, DEC_ABORT=0x20, DEC_IRQ=0x100。
 * ------------------------------------------------------------------------ */

#define TH1520_IRQ_DEC_PIC_INF		BIT(24)	/* 仅 H.264 */
#define TH1520_IRQ_DEC_TILE_INT		BIT(23)	/* 仅 HEVC */
#define TH1520_IRQ_DEC_LINE_CNT_INT	BIT(22)
#define TH1520_IRQ_DEC_EXT_TIMEOUT_INT	BIT(21)	/* 仅 H.264 */
#define TH1520_IRQ_DEC_NO_SLICE_INT	BIT(20)	/* 仅 H.264 */
#define TH1520_IRQ_DEC_LAST_SLICE_INT	BIT(19)	/* H.264；HEVC 表中同位为 DEC_BUSBUSY */
#define TH1520_IRQ_DEC_BUSBUSY		BIT(19)	/* 仅 HEVC */
#define TH1520_IRQ_DEC_TIMEOUT		BIT(18)
#define TH1520_IRQ_DEC_SLICE_INT	BIT(17)	/* 仅 H.264 */
#define TH1520_IRQ_DEC_ERROR_INT	BIT(16)
#define TH1520_IRQ_DEC_ASO_INT		BIT(15)	/* 仅 H.264 */
#define TH1520_IRQ_DEC_BUFFER_INT	BIT(14)
#define TH1520_IRQ_DEC_BUS_INT		BIT(13)
#define TH1520_IRQ_DEC_RDY_INT		BIT(12)
#define TH1520_IRQ_DEC_ABORT_INT	BIT(11)
#define TH1520_IRQ_DEC_IRQ		BIT(8)
#define TH1520_IRQ_DEC_TILE_INT_E	BIT(7)	/* 仅 HEVC */
#define TH1520_IRQ_DEC_SELF_RESET_DIS	BIT(6)
#define TH1520_IRQ_DEC_ABORT_E		BIT(5)
#define TH1520_IRQ_DEC_IRQ_DIS		BIT(4)
#define TH1520_IRQ_DEC_TIMEOUT_SOURCE	BIT(3)	/* 仅 H.264 */
#define TH1520_IRQ_DEC_BUS_INT_DIS	BIT(2)	/* 仅 H.264 */
#define TH1520_IRQ_DEC_STRM_CORRUPTED	BIT(1)	/* 仅 H.264 */
#define TH1520_IRQ_DEC_E		BIT(0)

/* 所有中断状态位的集合，等于聚合域 DEC_IRQ_STAT (swreg1[23:11]) 覆盖范围。 */
#define TH1520_IRQ_STAT_MASK		GENMASK(23, 11)

/* 视为“解码失败”的状态位。 */
#define TH1520_IRQ_ERROR_MASK \
	(TH1520_IRQ_DEC_TIMEOUT | TH1520_IRQ_DEC_ERROR_INT | \
	 TH1520_IRQ_DEC_BUS_INT | TH1520_IRQ_DEC_BUFFER_INT)

/* swreg1 位域形式（用于影子寄存器 RMW） */
#define th1520_dec_irq_stat		TH1520_REG(1, 11, 0x1fff)
#define th1520_dec_irq			TH1520_REG(1, 8, 0x1)
#define th1520_dec_tile_int_e		TH1520_REG(1, 7, 0x1)
#define th1520_dec_abort_e		TH1520_REG(1, 5, 0x1)
#define th1520_dec_irq_dis		TH1520_REG(1, 4, 0x1)
#define th1520_dec_e			TH1520_REG(1, 0, 0x1)

/* ------------------------------------------------------------------------
 * DEC_MODE —— swreg3[31:27]，两个 codec 共用
 *
 * 取值由 reference/vpu-vc8000d-kernel/linux/dwl/dwl_defs.h 与
 * DWLEnableHw 的分支共同证实。
 * ------------------------------------------------------------------------ */

#define TH1520_DEC_MODE_H264		0
#define TH1520_DEC_MODE_JPEG		3
#define TH1520_DEC_MODE_HEVC		12
#define TH1520_DEC_MODE_VP9		13
#define TH1520_DEC_MODE_H264_HIGH10	15

#define th1520_dec_mode			TH1520_REG(3, 27, 0x1f)

/*
 * G1V6 兼容写法：.so 的 H264DecInit 先写 4bit 版本 DEC_MODE_G1V6[31:28]
 * 再写 5bit 版本，以兼容不同 HW 修订。H.264 baseline/main 两者都写 0，
 * 结果相同，因此本驱动只写 5bit 版本。
 * 待硬件验证：High10（值 15）只在 5bit 语义下正确。
 */

/* AXI 总线宽度编码（swreg58[10:8] DEC_BUSWIDTH） */
#define TH1520_BUS_WIDTH_32		0
#define TH1520_BUS_WIDTH_64		1
#define TH1520_BUS_WIDTH_128		2
#define TH1520_BUS_WIDTH_256		3

/* ========================================================================
 *                       H.264 (G1 legacy, DEC_MODE = 0)
 * ======================================================================== */

/* swreg2 —— 总线与字节序（由 .so 的 SetLegacyG1CommonConfigRegs 写常量） */
#define h264_dec_axi_rd_id		TH1520_REG(2, 24, 0xff)
#define h264_dec_timeout_e		TH1520_REG(2, 23, 0x1)
#define h264_dec_strswap32_e		TH1520_REG(2, 22, 0x1)
#define h264_dec_strendian_e		TH1520_REG(2, 21, 0x1)
#define h264_dec_inswap32_e		TH1520_REG(2, 20, 0x1)
#define h264_dec_outswap32_e		TH1520_REG(2, 19, 0x1)
#define h264_dec_data_disc_e		TH1520_REG(2, 18, 0x1)
#define h264_dec_out_tiled_e		TH1520_REG(2, 17, 0x1)
#define h264_dec_latency		TH1520_REG(2, 11, 0x3f)
#define h264_dec_clk_gate_e		TH1520_REG(2, 10, 0x1)
#define h264_dec_in_endian		TH1520_REG(2, 9, 0x1)
#define h264_dec_out_endian		TH1520_REG(2, 8, 0x1)
#define h264_dec_adv_pre_dis		TH1520_REG(2, 6, 0x1)
#define h264_dec_scmd_dis		TH1520_REG(2, 5, 0x1)
#define h264_dec_max_burst		TH1520_REG(2, 0, 0x1f)

/* swreg3 —— 解码模式与图像结构 */
#define h264_rlc_mode_e			TH1520_REG(3, 24, 0x1)
#define h264_pic_interlace_e		TH1520_REG(3, 23, 0x1)
#define h264_pic_fieldmode_e		TH1520_REG(3, 22, 0x1)
#define h264_pic_topfield_e		TH1520_REG(3, 19, 0x1)
#define h264_dec_out_dis		TH1520_REG(3, 15, 0x1)
#define h264_filtering_dis		TH1520_REG(3, 14, 0x1)
#define h264_mvc_e			TH1520_REG(3, 13, 0x1)
#define h264_write_mvs_e		TH1520_REG(3, 12, 0x1)
#define h264_reftopfirst_e		TH1520_REG(3, 11, 0x1)
#define h264_seq_mbaff_e		TH1520_REG(3, 10, 0x1)
#define h264_picord_count_e		TH1520_REG(3, 9, 0x1)
#define h264_dec_axi_wr_id		TH1520_REG(3, 0, 0xff)

/* swreg4 —— 图像尺寸与参考帧数 */
#define h264_pic_mb_width		TH1520_REG(4, 23, 0x1ff)
#define h264_pic_mb_height_p		TH1520_REG(4, 11, 0xff)
#define h264_ref_frames			TH1520_REG(4, 0, 0x1f)
/* pic_height_in_mbs > 255 时的高位扩展，位于 swreg7[25] */
#define h264_pic_mb_h_ext		TH1520_REG(7, 25, 0x1)

/* swreg5 —— 码流起始位与色度 QP 偏移 */
#define h264_strm_start_bit		TH1520_REG(5, 26, 0x3f)
#define h264_type1_quant_e		TH1520_REG(5, 24, 0x1)
#define h264_ch_qp_offset		TH1520_REG(5, 19, 0x1f)
#define h264_ch_qp_offset2		TH1520_REG(5, 14, 0x1f)
#define h264_fieldpic_flag_e		TH1520_REG(5, 0, 0x1)

/* swreg6 —— 码流长度与初始 QP。注意 STREAM_LEN 只有 24 bit（16 MiB）。 */
#define h264_start_code_e		TH1520_REG(6, 31, 0x1)
#define h264_init_qp			TH1520_REG(6, 25, 0x3f)
#define h264_ch_8pix_ileav_e		TH1520_REG(6, 24, 0x1)
#define h264_stream_len			TH1520_REG(6, 0, 0xffffff)
#define TH1520_H264_MAX_STREAM_LEN	0xffffffU

/* swreg7 —— 熵编码与 frame_num */
#define h264_cabac_e			TH1520_REG(7, 31, 0x1)
#define h264_blackwhite_e		TH1520_REG(7, 30, 0x1)
#define h264_dir_8x8_infer_e		TH1520_REG(7, 29, 0x1)
#define h264_weight_pred_e		TH1520_REG(7, 28, 0x1)
#define h264_weight_bipr_idc		TH1520_REG(7, 26, 0x3)
#define h264_framenum_len		TH1520_REG(7, 16, 0x1f)
#define h264_framenum			TH1520_REG(7, 0, 0xffff)

/* swreg8 —— PPS 标志、参考帧标记语法长度与 IDR */
#define h264_const_intra_e		TH1520_REG(8, 31, 0x1)
#define h264_filt_ctrl_pres		TH1520_REG(8, 30, 0x1)
#define h264_rdpic_cnt_pres		TH1520_REG(8, 29, 0x1)
#define h264_8x8trans_flag_e		TH1520_REG(8, 28, 0x1)
#define h264_refpic_mk_len		TH1520_REG(8, 17, 0x7ff)
#define h264_idr_pic_e			TH1520_REG(8, 16, 0x1)
#define h264_idr_pic_id			TH1520_REG(8, 0, 0xffff)

/* swreg9 —— PPS id、活动参考索引数与 POC 语法长度 */
#define h264_pps_id			TH1520_REG(9, 24, 0xff)
#define h264_refidx1_active		TH1520_REG(9, 19, 0x1f)
#define h264_refidx0_active		TH1520_REG(9, 14, 0x1f)
#define h264_poc_length			TH1520_REG(9, 0, 0xff)

/* swreg48 —— 错误隐藏起始 MB（上游 G1_REG_ERR_CONC）。本驱动恒写 0。 */
#define TH1520_H264_SWREG_ERR_CONC	48

/* swreg49 —— 亚像素预测滤波器抽头 */
#define h264_pred_bc_tap_0_0		TH1520_REG(49, 22, 0x3ff)
#define h264_pred_bc_tap_0_1		TH1520_REG(49, 12, 0x3ff)
#define h264_pred_bc_tap_0_2		TH1520_REG(49, 2, 0x3ff)

/* swreg51 / swreg55 —— 参考帧片上缓冲（refbu）。本驱动关闭 refbu。 */
#define h264_refbu_e			TH1520_REG(51, 31, 0x1)
#define h264_refbu2_buf_e		TH1520_REG(55, 31, 0x1)
#define h264_apf_threshold		TH1520_REG(55, 0, 0x3fff)

/* swreg266 —— 错误容忍与 64bit swap */
#define h264_ignore_slice_error_e	TH1520_REG(266, 31, 0x1)
#define h264_swap_64bit_r		TH1520_REG(266, 1, 0x1)
#define h264_swap_64bit_w		TH1520_REG(266, 0, 0x1)

/* swreg314 —— 输出 stride */
#define h264_dec_out_y_stride		TH1520_REG(314, 16, 0xffff)
#define h264_dec_out_c_stride		TH1520_REG(314, 0, 0xffff)

/* swreg318 / swreg319 —— 两级超时看门狗 */
#define h264_ext_timeout_override_e	TH1520_REG(318, 31, 0x1)
#define h264_ext_timeout_cycles		TH1520_REG(318, 0, 0x7fffffff)
#define h264_timeout_override_e		TH1520_REG(319, 31, 0x1)
#define h264_timeout_cycles		TH1520_REG(319, 0, 0x7fffffff)

/* swreg38 / swreg39 —— DPB 长期参考与有效位图（整字） */
#define TH1520_H264_SWREG_LT_REF	38
#define TH1520_H264_SWREG_VALID_REF	39

/*
 * H.264 地址寄存器。LSB 与 MSB 不相邻，必须成对显式给出。
 * 一律通过 th1520_vdec_write_addr() 写入。
 */
#define TH1520_H264_ADDR_STREAM_LSB	12
#define TH1520_H264_ADDR_STREAM_MSB	122
#define TH1520_H264_ADDR_DST_LSB	13
#define TH1520_H264_ADDR_DST_MSB	123
#define TH1520_H264_ADDR_REF_LSB(i)	(14 + (i))
#define TH1520_H264_ADDR_REF_MSB(i)	(124 + (i))
#define TH1520_H264_ADDR_QTABLE_LSB	40
#define TH1520_H264_ADDR_QTABLE_MSB	140
#define TH1520_H264_ADDR_DIR_MV_LSB	41
#define TH1520_H264_ADDR_DIR_MV_MSB	141
#define TH1520_H264_ADDR_CH8PIX_LSB	59
#define TH1520_H264_ADDR_CH8PIX_MSB	145

/*
 * swreg30..37：每个寄存器放两个参考帧的 frame_num/pic_num。
 * REFER(2n)_NBR 在 [15:0]，REFER(2n+1)_NBR 在 [31:16]。
 */
#define TH1520_H264_SWREG_REF_PIC(i)	(30 + (i))
#define TH1520_H264_REF_NBR_EVEN(x)	(((x) & 0xffff) << 0)
#define TH1520_H264_REF_NBR_ODD(x)	(((x) & 0xffff) << 16)

/*
 * 参考列表寄存器（每项 5 bit）：
 *   swreg42..46：B 帧 L0/L1 的第 0..14 项，每 swreg 三组 F/B
 *   swreg47    ：B 帧 L0/L1 的第 15 项 + P 帧 L0 的第 0..3 项
 *   swreg10/11 ：P 帧 L0 的第 4..9 / 10..15 项
 * 与上游 hantro_g1_regs.h 的 G1_REG_BD_REF_PIC / BD_P_REF_PIC / FWD_PIC 完全一致。
 */
#define TH1520_H264_SWREG_BD_REF_PIC(i)	(42 + (i))
#define TH1520_H264_SWREG_BD_P_REF_PIC	47
#define TH1520_H264_SWREG_FWD_PIC(i)	(10 + (i))

/* ========================================================================
 *                          HEVC (G2, DEC_MODE = 12)
 * ======================================================================== */

/* swreg2 —— 8 个 4bit swap 域 */
#define hevc_strm_swap			TH1520_REG(2, 28, 0xf)
#define hevc_pic_swap			TH1520_REG(2, 24, 0xf)
#define hevc_dirmv_swap			TH1520_REG(2, 20, 0xf)
#define hevc_tab0_swap			TH1520_REG(2, 16, 0xf)
#define hevc_tab1_swap			TH1520_REG(2, 12, 0xf)
#define hevc_tab2_swap			TH1520_REG(2, 8, 0xf)
#define hevc_tab3_swap			TH1520_REG(2, 4, 0xf)
#define hevc_rscan_swap			TH1520_REG(2, 0, 0xf)

/* swreg3 —— 模式与输出控制 */
#define hevc_comp_table_swap		TH1520_REG(3, 20, 0xf)
#define hevc_out_ec_bypass		TH1520_REG(3, 17, 0x1)
#define hevc_out_rs_e			TH1520_REG(3, 16, 0x1)
#define hevc_out_dis			TH1520_REG(3, 15, 0x1)
#define hevc_filtering_dis		TH1520_REG(3, 14, 0x1)
#define hevc_write_mvs_e		TH1520_REG(3, 12, 0x1)
#define hevc_apf_one_pid		TH1520_REG(3, 11, 0x1)
#define hevc_buffer_empty_int_e		TH1520_REG(3, 10, 0x1)
#define hevc_block_buffer_mode_e	TH1520_REG(3, 9, 0x1)
#define hevc_last_buffer_e		TH1520_REG(3, 8, 0x1)

/* swreg4 —— 以最小 CB 为单位的图像尺寸 */
#define hevc_pic_width_in_cbs		TH1520_REG(4, 19, 0x1fff)
#define hevc_pic_height_in_cbs		TH1520_REG(4, 6, 0x1fff)
#define hevc_num_ref_frames		TH1520_REG(4, 0, 0x1f)

/* swreg5 */
#define hevc_strm_start_bit		TH1520_REG(5, 25, 0x7f)
#define hevc_scaling_list_e		TH1520_REG(5, 24, 0x1)
#define hevc_cb_qp_offset		TH1520_REG(5, 19, 0x1f)
#define hevc_cr_qp_offset		TH1520_REG(5, 14, 0x1f)
#define hevc_sign_data_hide		TH1520_REG(5, 12, 0x1)
#define hevc_tempor_mvp_e		TH1520_REG(5, 11, 0x1)
#define hevc_max_cu_qpd_depth		TH1520_REG(5, 5, 0x3f)
#define hevc_cu_qpd_e			TH1520_REG(5, 4, 0x1)

/* swreg6 —— HEVC 的 STREAM_LEN 是完整 32 bit（不像 H.264 只有 24 bit） */
#define hevc_stream_len			TH1520_REG(6, 0, 0xffffffff)

/* swreg7 —— slice / 滤波参数 */
#define hevc_cabac_init_present		TH1520_REG(7, 31, 0x1)
#define hevc_blackwhite_e		TH1520_REG(7, 30, 0x1)
#define hevc_weight_pred_e		TH1520_REG(7, 28, 0x1)
#define hevc_weight_bipr_idc		TH1520_REG(7, 26, 0x3)
#define hevc_filt_slice_border		TH1520_REG(7, 25, 0x1)
#define hevc_filt_tile_border		TH1520_REG(7, 24, 0x1)
#define hevc_asym_pred_e		TH1520_REG(7, 23, 0x1)
#define hevc_sao_e			TH1520_REG(7, 22, 0x1)
#define hevc_pcm_filt_disable		TH1520_REG(7, 21, 0x1)
#define hevc_slice_chqp_flag		TH1520_REG(7, 20, 0x1)
#define hevc_depend_slice_e		TH1520_REG(7, 19, 0x1)
#define hevc_filt_override_e		TH1520_REG(7, 18, 0x1)
#define hevc_strong_smooth_e		TH1520_REG(7, 17, 0x1)
#define hevc_filt_offset_beta		TH1520_REG(7, 12, 0x1f)
#define hevc_filt_offset_tc		TH1520_REG(7, 7, 0x1f)
#define hevc_slice_hdr_ext_e		TH1520_REG(7, 6, 0x1)
#define hevc_slice_hdr_ebits		TH1520_REG(7, 3, 0x7)

/* swreg8 —— 位深与输出格式 */
#define hevc_const_intra_e		TH1520_REG(8, 31, 0x1)
#define hevc_filt_ctrl_pres		TH1520_REG(8, 30, 0x1)
#define hevc_idr_pic_e			TH1520_REG(8, 16, 0x1)
#define hevc_pcm_bitdepth_y		TH1520_REG(8, 12, 0xf)
#define hevc_pcm_bitdepth_c		TH1520_REG(8, 8, 0xf)
#define hevc_bit_depth_y_minus8		TH1520_REG(8, 6, 0x3)
#define hevc_bit_depth_c_minus8		TH1520_REG(8, 4, 0x3)
#define hevc_output_8_bits		TH1520_REG(8, 3, 0x1)
#define hevc_output_format		TH1520_REG(8, 0, 0x7)

/* swreg9 */
#define hevc_refidx1_active		TH1520_REG(9, 19, 0x1f)
#define hevc_refidx0_active		TH1520_REG(9, 14, 0x1f)
#define hevc_hdr_skip_length		TH1520_REG(9, 0, 0x3fff)

/* swreg10 —— 起始码、初始 QP 与 tile 数量。_v0 为旧修订位宽，.so 两套都写。 */
#define hevc_start_code_e		TH1520_REG(10, 31, 0x1)
#define hevc_init_qp_v0			TH1520_REG(10, 25, 0x3f)
#define hevc_init_qp			TH1520_REG(10, 24, 0x7f)
#define hevc_num_tile_cols_v0		TH1520_REG(10, 20, 0x1f)
#define hevc_num_tile_cols		TH1520_REG(10, 19, 0x1f)
#define hevc_num_tile_rows_v0		TH1520_REG(10, 15, 0x1f)
#define hevc_num_tile_rows		TH1520_REG(10, 14, 0x1f)
#define hevc_tile_enable		TH1520_REG(10, 1, 0x1)
#define hevc_entr_code_synch_e		TH1520_REG(10, 0, 0x1)

/* swreg12 —— 长期参考位图与 CB / PCM 尺寸 */
#define hevc_refer_lterm_e		TH1520_REG(12, 16, 0xffff)
#define hevc_min_cb_size		TH1520_REG(12, 13, 0x7)
#define hevc_max_cb_size		TH1520_REG(12, 10, 0x7)
#define hevc_min_pcm_size		TH1520_REG(12, 7, 0x7)
#define hevc_max_pcm_size		TH1520_REG(12, 4, 0x7)
#define hevc_pcm_e			TH1520_REG(12, 3, 0x1)
#define hevc_transform_skip_e		TH1520_REG(12, 2, 0x1)
#define hevc_transq_bypass_e		TH1520_REG(12, 1, 0x1)
#define hevc_refpiclist_mod_e		TH1520_REG(12, 0, 0x1)

/* swreg13 —— 变换树与并行 merge 层级 */
#define hevc_min_trb_size		TH1520_REG(13, 13, 0x7)
#define hevc_max_trb_size		TH1520_REG(13, 10, 0x7)
#define hevc_max_intra_hierdepth	TH1520_REG(13, 7, 0x7)
#define hevc_max_inter_hierdepth	TH1520_REG(13, 4, 0x7)
#define hevc_parallel_merge		TH1520_REG(13, 0, 0xf)

/*
 * swreg14..19 —— 初始参考列表，每项 5 bit。
 * 每个 swreg 存 3 组 (F, B)：F 在 0/10/20，B 在 5/15/25。
 * swreg19 只有第 15 项。
 */
#define TH1520_HEVC_SWREG_RLIST(i)	(14 + (i))

/* swreg20 —— 非整 CTB 边界与 4x4 单位尺寸 */
#define hevc_partial_ctb_x		TH1520_REG(20, 31, 0x1)
#define hevc_partial_ctb_y		TH1520_REG(20, 30, 0x1)
#define hevc_pic_width_4x4		TH1520_REG(20, 16, 0xfff)
#define hevc_pic_height_4x4		TH1520_REG(20, 0, 0xfff)

/* swreg44 / swreg45 —— 总线忙与超时看门狗 */
#define hevc_busbusy_cycles		TH1520_REG(44, 0, 0xffffffff)
#define hevc_timeout_override_e		TH1520_REG(45, 31, 0x1)
#define hevc_timeout_cycles		TH1520_REG(45, 0, 0x7fffffff)

/* swreg46..49 —— 16 个参考帧相对当前帧的 POC 差，每项 8 bit */
#define TH1520_HEVC_SWREG_CUR_POC(i)	(46 + (i))

/* swreg55 —— 自适应预取 */
#define hevc_apf_disable		TH1520_REG(55, 31, 0x1)
#define hevc_apf_threshold		TH1520_REG(55, 0, 0xffff)

/* swreg58 / swreg59 —— 总线参数 */
#define hevc_clk_gate_idle_e		TH1520_REG(58, 17, 0x1)
#define hevc_clk_gate_e			TH1520_REG(58, 16, 0x1)
#define hevc_refer_doublebuffer_e	TH1520_REG(58, 15, 0x1)
#define hevc_axi_rd_id_e		TH1520_REG(58, 14, 0x1)
#define hevc_axi_wd_id_e		TH1520_REG(58, 13, 0x1)
#define hevc_buswidth			TH1520_REG(58, 8, 0x7)
#define hevc_max_burst			TH1520_REG(58, 0, 0xff)
#define hevc_axi_wr_id			TH1520_REG(59, 16, 0xffff)
#define hevc_axi_rd_id			TH1520_REG(59, 0, 0xffff)

/* swreg184 —— 下采样输出。本驱动关闭。 */
#define hevc_down_scale_e		TH1520_REG(184, 7, 0x1)
#define hevc_down_scale_y		TH1520_REG(184, 2, 0x3)
#define hevc_down_scale_x		TH1520_REG(184, 0, 0x3)

/* swreg258 / swreg259 —— 环形码流缓冲 */
#define hevc_strm_buffer_len		TH1520_REG(258, 0, 0xffffffff)
#define hevc_strm_start_offset		TH1520_REG(259, 0, 0xffffffff)

/* swreg314 —— 输出 stride */
#define hevc_dec_out_y_stride		TH1520_REG(314, 16, 0xffff)
#define hevc_dec_out_c_stride		TH1520_REG(314, 0, 0xffff)

/*
 * HEVC 地址寄存器：全部是 (MSB, LSB) 相邻两个 swreg，MSB 在低编号。
 * 这里给出的是 MSB 的 swreg 编号；LSB = MSB + 1。
 */
#define TH1520_HEVC_ADDR_OUT_LUMA	64
#define TH1520_HEVC_ADDR_REF_LUMA(i)	(66 + (i) * 2)
#define TH1520_HEVC_ADDR_OUT_CHROMA	98
#define TH1520_HEVC_ADDR_REF_CHROMA(i)	(100 + (i) * 2)
#define TH1520_HEVC_ADDR_OUT_MV		132
#define TH1520_HEVC_ADDR_REF_MV(i)	(134 + (i) * 2)
#define TH1520_HEVC_ADDR_TILE_SIZES	166
#define TH1520_HEVC_ADDR_STREAM		168
#define TH1520_HEVC_ADDR_SCALING_LIST	170
#define TH1520_HEVC_ADDR_RS_OUT_LUMA	174
#define TH1520_HEVC_ADDR_RS_OUT_CHROMA	176
#define TH1520_HEVC_ADDR_TILE_FILTER	178
#define TH1520_HEVC_ADDR_TILE_SAO	180
#define TH1520_HEVC_ADDR_TILE_BSD	182
#define TH1520_HEVC_ADDR_DS_LUMA	185
#define TH1520_HEVC_ADDR_DS_CHROMA	187
#define TH1520_HEVC_ADDR_OUT_COMP_LUMA	189
#define TH1520_HEVC_ADDR_REF_COMP_LUMA(i) (191 + (i) * 2)
#define TH1520_HEVC_ADDR_OUT_COMP_CHROMA 223
#define TH1520_HEVC_ADDR_REF_COMP_CHROMA(i) (225 + (i) * 2)

#endif /* TH1520_VDEC_REGS_H_ */
