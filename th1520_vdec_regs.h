/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TH1520 VC8000D decoder register definitions.
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * 证据来源（全部为“已验证事实”，除非另行标注）：
 *
 *   analysis/vc8000d-register-config/swreg-map-vc8000d.md   ← 本文件位域的权威来源
 *     （VC8000D 产品表 @0x4CBB80，product id 0x8001）
 *   analysis/vc8000d-product-table.md   （发现过程、golden 字段级解码、驱动差异清单）
 *   analysis/vc8000d-register-config/swreg-map-h264.md / swreg-map-hevc.md
 *     （G1 @0x4D40A0 / G2 @0x4DC5C0 codec 表，仅对旧 product 0x6731/0x6732 有效）
 *
 * ⚠ 2026-08-13 重要更正：早期分析只 dump 了 G1/G2 两张 codec 表，并把产品表
 * 0x4CBB80 误标为“JPEG 表”。实际运行时 .so 的 SetDecRegister 按
 * HIWORD(container->regs[0]) 选表（0x6731→G1、0x6732→G2、0x8001→产品表），
 * 而 container->regs[0] 来自 DWLReadAsicID() —— TH1520 的 HW build id 实测为
 * 0x80018000（product 0x8001），因此 H.264/HEVC 解码用的都是产品表。
 * G1/G2 表的字段**位置**对 TH1520 无效；本文件所有位域位置已按产品表重定义。
 * 关键错位实例（G1/G2 表位置 → 产品表位置）：
 *   START_CODE_E: H.264 swreg6[31] / HEVC swreg10[31] → 统一 swreg13[31]
 *   INIT_QP:      H.264 swreg6[25]  / HEVC swreg10[30:24] → 统一 swreg13[30:24]
 *   LAST_BUFFER_E: swreg3[8] → swreg3[0]；OUT_EC_BYPASS: swreg3[17] → swreg3[8]
 *   HEVC tile 计数 → 8K 位域 swreg10[23:17]/[16:12]
 *   CLK_GATE_E（HEVC）: swreg58[16] → swreg2[10]；超时看门狗 → swreg318/319
 *   H.264 全部地址寄存器 → HEVC 风格统一位置（STREAM@168/169、OUT@64/65、
 *     REF@66+2i、DIRMV@132/133、QTABLE@174/175）
 *
 * 三张表均由 analysis/build_swreg_map.py / build_product_map.py 直接从真实
 * 交付二进制 libOMX.hantro.VC8000D.video.decoder.so 的寄存器规格表 dump 得到：
 * 每表 2129 条 × 16 字节 = (swreg, 位宽, shift, used)，数值均为二进制实测值。
 * 寄存器名来自 omx_il_g-master/.../8170enum.h，经多重独立验证
 * （g1/g2 规格表 LCS 匹配、SetCommonConfigRegs 调用序列、.rodata id 数组、
 * 上游 mainline hantro 交叉对照），golden 寄存器抓取（analysis/golden-registers.txt）
 * 提供了 TH1520 上实际值的最终锚点。
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

/*
 * ========================================================================
 *  ⚠ 2026-08-13 产品表更正：本文件所有位域位置已按 VC8000D 产品表
 * （0x4CBB80，product id 0x8001）重定义，不再是 G1/G2 codec 表位置。
 * TH1520 的 HW build id = 0x80018000，.so 运行时 SetDecRegister 按
 * HIWORD(regs[0])=0x8001 选中产品表；G1/G2 表只对旧 product 有效。
 * 证据链与 golden 字段级解码见 analysis/vc8000d-product-table.md，
 * 完整位域图见 analysis/vc8000d-register-config/swreg-map-vc8000d.md。
 * ========================================================================
 */

/* ========================================================================
 *                       H.264 (DEC_MODE = 0)
 * ======================================================================== */

/*
 * swreg2 —— 产品表布局：
 *   STRM_SWAP[31:28] / PIC_SWAP[27:24] / DIRMV_SWAP[23:20] / TAB_SWAP[15:12]
 *   TILED_MODE_MSB[17] / TILED_MODE_LSB[7] / DRM_E[4] / CLK_GATE_E[10]
 * 厂商栈（SetCommonConfigRegs）四个 swap 域全部写 0，只置 CLK_GATE_E=1，
 * 整字值 0x00000400 —— 与 HEVC 共用同一个 common config 结果。
 * G1 表的 STRSWAP32_E / STRENDIAN_E / INSWAP32_E / OUTSWAP32_E /
 * DATA_DISC_E / DEC_TIMEOUT_E / DEC_LATENCY / DEC_IN_ENDIAN / DEC_OUT_ENDIAN /
 * DEC_ADV_PRE_DIS / DEC_SCMD_DIS / DEC_MAX_BURST[4:0] 在产品表中均不存在。
 */
#define h264_dec_clk_gate_e		TH1520_REG(2, 10, 0x1)

/* swreg3 —— 图像结构（产品表与 G1 表在这些位上一致）。 */
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
/*
 * G1 表的 DEC_AXI_WR_ID（swreg3[7:0]）在产品表中不存在：产品表 swreg3[7:0]
 * 是 APF_ONE_PID / REF_READ_DIS / L2_SHAPER_E / BUFFER_EMPTY_INT_E /
 * BLOCK_BUFFER_MODE_E / LAST_BUFFER_E。AXI ID 在产品表位于 swreg60。
 */

/*
 * swreg58 —— 产品表中 H.264 与 HEVC 共用（G1 表的 MAX_BURST 在 swreg2[4:0]，
 * 在 VC8000D 上无效）。
 */
#define h264_max_burst_sw58		TH1520_REG(58, 0, 0xff)
#define h264_buswidth_sw58		TH1520_REG(58, 8, 0x7)
#define h264_axi_rd_id_e_sw58		TH1520_REG(58, 14, 0x1)
#define h264_axi_wd_id_e_sw58		TH1520_REG(58, 13, 0x1)

/* swreg4 —— 图像尺寸（单位：宏块）与参考帧数。产品表与 G1 表一致。 */
#define h264_pic_mb_width		TH1520_REG(4, 23, 0x1ff)
#define h264_pic_mb_height_p		TH1520_REG(4, 11, 0xff)
#define h264_ref_frames			TH1520_REG(4, 0, 0x1f)
/* pic_height_in_mbs > 255 时的高位扩展，位于 swreg7[25]。 */
#define h264_pic_mb_h_ext		TH1520_REG(7, 25, 0x1)

/* swreg5 —— 码流起始位与色度 QP 偏移。STRM_START_BIT 产品表为 7 bit。 */
#define h264_strm_start_bit		TH1520_REG(5, 25, 0x7f)
#define h264_type1_quant_e		TH1520_REG(5, 24, 0x1)
#define h264_ch_qp_offset		TH1520_REG(5, 19, 0x1f)
#define h264_ch_qp_offset2		TH1520_REG(5, 14, 0x1f)
#define h264_fieldpic_flag_e		TH1520_REG(5, 0, 0x1)

/*
 * swreg6 —— 码流长度。产品表（id 161）为完整 32 bit，H.264 与 HEVC 相同
 * （G1 表的 24 bit 截断在 VC8000D 上不适用）。
 * G1 表的 START_CODE_E[31] / INIT_QP[25] 在产品表中位于 swreg13（见下）。
 */
#define h264_stream_len			TH1520_REG(6, 0, 0xffffffff)

/* swreg7 —— 熵编码与 frame_num。产品表与 G1 表一致。 */
#define h264_cabac_e			TH1520_REG(7, 31, 0x1)
#define h264_blackwhite_e		TH1520_REG(7, 30, 0x1)
#define h264_dir_8x8_infer_e		TH1520_REG(7, 29, 0x1)
#define h264_weight_pred_e		TH1520_REG(7, 28, 0x1)
#define h264_weight_bipr_idc		TH1520_REG(7, 26, 0x3)
#define h264_framenum_len		TH1520_REG(7, 16, 0x1f)
#define h264_framenum			TH1520_REG(7, 0, 0xffff)

/* swreg8 —— PPS 标志、参考帧标记语法长度与 IDR。产品表与 G1 表一致。 */
#define h264_const_intra_e		TH1520_REG(8, 31, 0x1)
#define h264_filt_ctrl_pres		TH1520_REG(8, 30, 0x1)
#define h264_rdpic_cnt_pres		TH1520_REG(8, 29, 0x1)
#define h264_8x8trans_flag_e		TH1520_REG(8, 28, 0x1)
#define h264_refpic_mk_len		TH1520_REG(8, 17, 0x7ff)
#define h264_idr_pic_e			TH1520_REG(8, 16, 0x1)
#define h264_idr_pic_id			TH1520_REG(8, 0, 0xffff)

/* swreg9 —— PPS id、活动参考索引数与 POC 语法长度。产品表与 G1 表一致。 */
#define h264_pps_id			TH1520_REG(9, 24, 0xff)
#define h264_refidx1_active		TH1520_REG(9, 19, 0x1f)
#define h264_refidx0_active		TH1520_REG(9, 14, 0x1f)
#define h264_poc_length			TH1520_REG(9, 0, 0xff)

/*
 * swreg13 —— 产品表把 START_CODE_E 与 INIT_QP 统一放在这里
 * （与 HEVC 同位置；G1 表的位置 swreg6[31]/[25] 在 VC8000D 上无效）。
 */
#define h264_start_code_e		TH1520_REG(13, 31, 0x1)
#define h264_init_qp			TH1520_REG(13, 24, 0x7f)

/* swreg48 —— 错误隐藏。产品表 [13:12] ERROR_CONC_MODE，驱动恒写 0（关闭）。 */
#define TH1520_H264_SWREG_ERR_CONC	48

/* swreg49 —— 亚像素预测滤波器抽头。产品表与 G1 表一致。 */
#define h264_pred_bc_tap_0_0		TH1520_REG(49, 22, 0x3ff)
#define h264_pred_bc_tap_0_1		TH1520_REG(49, 12, 0x3ff)
#define h264_pred_bc_tap_0_2		TH1520_REG(49, 2, 0x3ff)

/*
 * swreg55 —— 自适应预取。产品表 APF_THRESHOLD 为 16 bit
 * （G1 表为 14 bit；取值 8 两者相同）。
 * G1 表的 REFBU_E（swreg51）/ REFBU2_BUF_E（swreg55[31]）在产品表中不存在。
 */
#define h264_apf_threshold		TH1520_REG(55, 0, 0xffff)

/*
 * swreg266 —— G1 表的 IGNORE_SLICE_ERROR_E[31] 产品表同位置；
 * SWAP_64BIT_R/W 在产品表中不存在。
 */
#define h264_ignore_slice_error_e	TH1520_REG(266, 31, 0x1)

/* swreg314 —— 输出 stride。产品表与 G1 表一致。 */
#define h264_dec_out_y_stride		TH1520_REG(314, 16, 0xffff)
#define h264_dec_out_c_stride		TH1520_REG(314, 0, 0xffff)

/* swreg318 / swreg319 —— 两级超时看门狗（产品表与 G1 表同位置）。 */
#define h264_ext_timeout_override_e	TH1520_REG(318, 31, 0x1)
#define h264_ext_timeout_cycles		TH1520_REG(318, 0, 0x7fffffff)
#define h264_timeout_override_e		TH1520_REG(319, 31, 0x1)
#define h264_timeout_cycles		TH1520_REG(319, 0, 0x7fffffff)

/* swreg38 / swreg39 —— DPB 长期参考与有效位图（整字）。产品表同位置。 */
#define TH1520_H264_SWREG_LT_REF	38
#define TH1520_H264_SWREG_VALID_REF	39

/*
 * H.264 地址寄存器 —— 产品表把全部地址 id 统一到 HEVC 风格位置
 * （G1 表位置 STREAM@12/122、DST@13/123、REF@14+i、QTABLE@40、DIRMV@41
 * 在 VC8000D 上无效）：
 *   STREAM_BASE   swreg168/169
 *   DEC_OUT_BASE  swreg64/65
 *   REFERi_BASE   swreg66+2i / 67+2i（低 2 bit 复用为 FIELD_E/TOPC_E）
 *   DIR_MV_BASE   swreg132/133
 *   QTABLE_BASE   swreg174/175
 * 一律通过 th1520_vdec_write_addr() 写入。
 */
#define TH1520_H264_ADDR_STREAM_LSB	169
#define TH1520_H264_ADDR_STREAM_MSB	168
#define TH1520_H264_ADDR_DST_LSB	65
#define TH1520_H264_ADDR_DST_MSB	64
#define TH1520_H264_ADDR_REF_LSB(i)	(67 + (i) * 2)
#define TH1520_H264_ADDR_REF_MSB(i)	(66 + (i) * 2)
#define TH1520_H264_ADDR_QTABLE_LSB	175
#define TH1520_H264_ADDR_QTABLE_MSB	174
#define TH1520_H264_ADDR_DIR_MV_LSB	133
#define TH1520_H264_ADDR_DIR_MV_MSB	132

/*
 * swreg30..37：每个寄存器放两个参考帧的 frame_num/pic_num。
 * REFER(2n)_NBR 在 [15:0]，REFER(2n+1)_NBR 在 [31:16]。产品表同位置。
 */
#define TH1520_H264_SWREG_REF_PIC(i)	(30 + (i))
#define TH1520_H264_REF_NBR_EVEN(x)	(((x) & 0xffff) << 0)
#define TH1520_H264_REF_NBR_ODD(x)	(((x) & 0xffff) << 16)

/*
 * 参考列表寄存器（每项 5 bit）。产品表同位置：
 *   swreg42..46：B 帧 L0/L1 的第 0..14 项（BINIT_RLIST），每 swreg 三组 F/B
 *   swreg47    ：B 帧 L0/L1 的第 15 项 + P 帧 L0 的第 0..3 项
 *   swreg10/11 ：P 帧 L0 的第 4..9 / 10..15 项（PINIT_RLIST_F4..15）
 */
#define TH1520_H264_SWREG_BD_REF_PIC(i)	(42 + (i))
#define TH1520_H264_SWREG_BD_P_REF_PIC	47
#define TH1520_H264_SWREG_FWD_PIC(i)	(10 + (i))

/* ========================================================================
 *                          HEVC (DEC_MODE = 12)
 * ======================================================================== */

/*
 * swreg2 —— 产品表布局与 G2 表不同（见上面产品表更正说明）：
 * 只有 STRM_SWAP[31:28] / PIC_SWAP[27:24] / DIRMV_SWAP[23:20] /
 * TAB_SWAP[15:12] 四组 swap 域 + TILED_MODE + DRM_E + CLK_GATE_E[10]。
 * G2 表的 DEC_TAB0..3_SWAP / DEC_RSCAN_SWAP（id 2102-2106）在产品表中
 * 是 no-op；厂商栈四个 swap 域全写 0、只置 CLK_GATE_E → 整字 0x00000400。
 */
#define hevc_clk_gate_e			TH1520_REG(2, 10, 0x1)

/* swreg3 —— 产品表布局（G2 表位置在 VC8000D 上无效，见分析文档 §3）：
 *   OUT_EC_BYPASS[8] / APF_ONE_PID[7] / REF_READ_DIS[6] / L2_SHAPER_E[5]
 *   UNNAMED[3]（DWL 的 L2 cache 通道使能）/ BUFFER_EMPTY_INT_E[2]
 *   BLOCK_BUFFER_MODE_E[1] / LAST_BUFFER_E[0]
 * G2 表的 COMP_TABLE_SWAP（id 2107）在产品表中是 no-op。
 */
#define hevc_out_ec_bypass		TH1520_REG(3, 8, 0x1)
#define hevc_apf_one_pid		TH1520_REG(3, 7, 0x1)
#define hevc_ref_read_dis		TH1520_REG(3, 6, 0x1)
#define hevc_l2_shaper_e		TH1520_REG(3, 5, 0x1)
#define hevc_buffer_empty_int_e		TH1520_REG(3, 2, 0x1)
#define hevc_block_buffer_mode_e	TH1520_REG(3, 1, 0x1)
#define hevc_last_buffer_e		TH1520_REG(3, 0, 0x1)

/* 以下位域产品表与 G2 表同位置 */
#define hevc_out_dis			TH1520_REG(3, 15, 0x1)
#define hevc_filtering_dis		TH1520_REG(3, 14, 0x1)
#define hevc_write_mvs_e		TH1520_REG(3, 12, 0x1)

/* swreg4 —— 以最小 CB 为单位的图像尺寸。产品表与 G2 表一致。 */
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

/* swreg6 —— 码流长度（32 bit）。产品表与 G2 表一致。 */
#define hevc_stream_len			TH1520_REG(6, 0, 0xffffffff)

/* swreg7 —— slice / 滤波参数。产品表与 G2 表一致。 */
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

/* swreg8 —— 位深与输出格式。产品表与 G2 表一致
 * （G2 表的 OUTPUT_8_BITS[3]/OUTPUT_FORMAT[2:0] 在产品表为未命名域，
 * 本驱动写 0，与 golden 相同）。 */
#define hevc_const_intra_e		TH1520_REG(8, 31, 0x1)
#define hevc_filt_ctrl_pres		TH1520_REG(8, 30, 0x1)
#define hevc_idr_pic_e			TH1520_REG(8, 16, 0x1)
#define hevc_pcm_bitdepth_y		TH1520_REG(8, 12, 0xf)
#define hevc_pcm_bitdepth_c		TH1520_REG(8, 8, 0xf)
#define hevc_bit_depth_y_minus8		TH1520_REG(8, 6, 0x3)
#define hevc_bit_depth_c_minus8		TH1520_REG(8, 4, 0x3)
#define hevc_output_8_bits		TH1520_REG(8, 3, 0x1)
#define hevc_output_format		TH1520_REG(8, 0, 0x7)

/* swreg9。产品表与 G2 表一致。 */
#define hevc_refidx1_active		TH1520_REG(9, 19, 0x1f)
#define hevc_refidx0_active		TH1520_REG(9, 14, 0x1f)
#define hevc_hdr_skip_length		TH1520_REG(9, 0, 0x3fff)

/*
 * swreg10 —— 产品表布局：tile 数量只写在 8K 位域
 *   NUM_TILE_COLS_8K[23:17] / NUM_TILE_ROWS_8K[16:12]。
 * G2 表的 START_CODE_E[31] / INIT_QP[30:24] 在产品表中位于 swreg13；
 * 旧 tile 位域 [23:19]/[18:14] 与 8K 位域重叠，.so 先写旧后写新、
 * 新值胜出，驱动只写 8K 位域（golden 实测值 0x21000 即由此而来）。
 */
#define hevc_num_tile_cols_8k		TH1520_REG(10, 17, 0x7f)
#define hevc_num_tile_rows_8k		TH1520_REG(10, 12, 0x1f)
#define hevc_tile_enable		TH1520_REG(10, 1, 0x1)
#define hevc_entr_code_synch_e		TH1520_REG(10, 0, 0x1)

/* swreg12 —— 长期参考位图与 CB / PCM 尺寸。产品表与 G2 表一致。 */
#define hevc_refer_lterm_e		TH1520_REG(12, 16, 0xffff)
#define hevc_min_cb_size		TH1520_REG(12, 13, 0x7)
#define hevc_max_cb_size		TH1520_REG(12, 10, 0x7)
#define hevc_min_pcm_size		TH1520_REG(12, 7, 0x7)
#define hevc_max_pcm_size		TH1520_REG(12, 4, 0x7)
#define hevc_pcm_e			TH1520_REG(12, 3, 0x1)
#define hevc_transform_skip_e		TH1520_REG(12, 2, 0x1)
#define hevc_transq_bypass_e		TH1520_REG(12, 1, 0x1)
#define hevc_refpiclist_mod_e		TH1520_REG(12, 0, 0x1)

/*
 * swreg13 —— 产品表布局：START_CODE_E[31] 与 INIT_QP[30:24] 在这里
 * （G2 表把它们放在 swreg10，在 VC8000D 上无效；golden 实测
 * START_CODE_E=1 + INIT_QP=26 出现在 swreg13 高位）。
 * MIN/MAX_TRB_SIZE、hierdepth、PARALLEL_MERGE 与 G2 表同位置。
 */
#define hevc_start_code_e		TH1520_REG(13, 31, 0x1)
#define hevc_init_qp			TH1520_REG(13, 24, 0x7f)
#define hevc_min_trb_size		TH1520_REG(13, 13, 0x7)
#define hevc_max_trb_size		TH1520_REG(13, 10, 0x7)
#define hevc_max_intra_hierdepth	TH1520_REG(13, 7, 0x7)
#define hevc_max_inter_hierdepth	TH1520_REG(13, 4, 0x7)
#define hevc_parallel_merge		TH1520_REG(13, 0, 0xf)

/*
 * swreg14..19 —— 初始参考列表，每项 5 bit。
 * 每个 swreg 存 3 组 (F, B)：F 在 0/10/20，B 在 5/15/25。
 * swreg19 只有第 15 项。产品表与 G2 表同位置。
 */
#define TH1520_HEVC_SWREG_RLIST(i)	(14 + (i))

/* swreg20 —— 非整 CTB 边界与 4x4 单位尺寸。产品表与 G2 表一致。 */
#define hevc_partial_ctb_x		TH1520_REG(20, 31, 0x1)
#define hevc_partial_ctb_y		TH1520_REG(20, 30, 0x1)
#define hevc_pic_width_4x4		TH1520_REG(20, 16, 0xfff)
#define hevc_pic_height_4x4		TH1520_REG(20, 0, 0xfff)

/*
 * swreg44/45 —— G2 表的 BUSBUSY_CYCLES / TIMEOUT 在产品表中不存在
 * （id 2114 no-op；超时看门狗在 swreg318/319，与 H.264 共用）。
 */
#define hevc_ext_timeout_override_e	TH1520_REG(318, 31, 0x1)
#define hevc_ext_timeout_cycles		TH1520_REG(318, 0, 0x7fffffff)
#define hevc_timeout_override_e		TH1520_REG(319, 31, 0x1)
#define hevc_timeout_cycles		TH1520_REG(319, 0, 0x7fffffff)

/* swreg46..49 —— 16 个参考帧相对当前帧的 POC 差，每项 8 bit。
 * 产品表与 G2 表同位置。 */
#define TH1520_HEVC_SWREG_CUR_POC(i)	(46 + (i))

/* swreg55 —— 自适应预取。产品表与 G2 表一致。 */
#define hevc_apf_disable		TH1520_REG(55, 31, 0x1)
#define hevc_apf_threshold		TH1520_REG(55, 0, 0xffff)

/*
 * swreg58 —— 产品表布局：CLK_GATE_E/CLK_GATE_IDLE_E 不在这里
 * （G2 表位置在产品表中是 MC_POLLTIME 位，真实 CLK_GATE_E = swreg2[10]）。
 */
#define hevc_refer_doublebuffer_e	TH1520_REG(58, 15, 0x1)
#define hevc_axi_rd_id_e		TH1520_REG(58, 14, 0x1)
#define hevc_axi_wd_id_e		TH1520_REG(58, 13, 0x1)
#define hevc_buswidth			TH1520_REG(58, 8, 0x7)
#define hevc_max_burst			TH1520_REG(58, 0, 0xff)

/* swreg60 —— 产品表的 AXI ID 寄存器（G2 表在 swreg59，位置无效）。 */
#define hevc_axi_wr_id			TH1520_REG(60, 16, 0xffff)
#define hevc_axi_rd_id			TH1520_REG(60, 0, 0xffff)

/*
 * swreg265 —— cache/shaper 控制。产品表字段：
 *   [31]    cache/shaper 主使能（DWLEnableHw 置位）
 *   [27:18] / [17:8]  各 10 bit，DWL 缓存配置写入（golden=64/64，语义未明）
 * 本驱动走 EC_BYPASS 旁路路径，不配置 cache/shaper，该寄存器保持 0。
 * 若将来启用参考帧压缩（对齐 golden 的 EC_BYPASS=0 + L2CACHE/DEC400），
 * 需要写 0x81004000 并配置 L2CACHE/DEC400 寄存器块。
 */
#define TH1520_SWREG_CACHE_SHAPER_CTRL	265

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
