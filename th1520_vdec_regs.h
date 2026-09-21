/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * TH1520 VC8000D register definitions for ASIC 0x80018000, build 0x1f88.
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * Public Hantro register naming and helper conventions:
 * Copyright 2018 Google LLC.
 *     Tomasz Figa <tfiga@chromium.org>
 * Copyright (c) 2021, Collabora
 *     Benjamin Gaignard <benjamin.gaignard@collabora.com>
 *
 * Linux commit 8ba098e6b6ff0db8edf28528d1552be261af30d4; see docs/sources.md.
 * The TH1520 register coordinates are hardware-interface facts validated
 * for this build. They are not interchangeable with other Hantro cores.
 * Each register index denotes a 32-bit word at decoder-base + 4 * index.
 * The supported configuration and remaining limits are in docs/hardware.md.
 */

#ifndef TH1520_VDEC_REGS_H_
#define TH1520_VDEC_REGS_H_

#include <linux/bits.h>
#include <linux/types.h>

/*
 * The per-job shadow covers all 512 decoder and post-processor words.
 */
#define TH1520_VDEC_REG_COUNT		512

#define TH1520_VDEC_REG_OFF(n)		((n) * 4)

/*
 * Register 0 is the read-only ASIC identifier; the build is in word 309.
 */
#define TH1520_VDEC_SWREG_ID		0

#define TH1520_VDEC_SWREG_IRQ		1

/*
 * Bit-field descriptor: register index, least-significant bit and unshifted mask.
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

/*
 * Register 1: interrupt status and decoder control.
 */
#define TH1520_IRQ_DEC_PIC_INF		BIT(24)
#define TH1520_IRQ_DEC_TILE_INT		BIT(23)
#define TH1520_IRQ_DEC_LINE_CNT_INT	BIT(22)
#define TH1520_IRQ_DEC_EXT_TIMEOUT_INT	BIT(21)
#define TH1520_IRQ_DEC_NO_SLICE_INT	BIT(20)
#define TH1520_IRQ_DEC_LAST_SLICE_INT	BIT(19)
#define TH1520_IRQ_DEC_BUSBUSY		BIT(19)
#define TH1520_IRQ_DEC_TIMEOUT		BIT(18)
#define TH1520_IRQ_DEC_SLICE_INT	BIT(17)
#define TH1520_IRQ_DEC_ERROR_INT	BIT(16)
#define TH1520_IRQ_DEC_ASO_INT		BIT(15)
#define TH1520_IRQ_DEC_BUFFER_INT	BIT(14)
#define TH1520_IRQ_DEC_BUS_INT		BIT(13)
#define TH1520_IRQ_DEC_RDY_INT		BIT(12)
#define TH1520_IRQ_DEC_ABORT_INT	BIT(11)
#define TH1520_IRQ_DEC_IRQ		BIT(8)
#define TH1520_IRQ_DEC_TILE_INT_E	BIT(7)
#define TH1520_IRQ_DEC_SELF_RESET_DIS	BIT(6)
#define TH1520_IRQ_DEC_ABORT_E		BIT(5)
#define TH1520_IRQ_DEC_IRQ_DIS		BIT(4)
#define TH1520_IRQ_DEC_TIMEOUT_SOURCE	BIT(3)
#define TH1520_IRQ_DEC_BUS_INT_DIS	BIT(2)
#define TH1520_IRQ_DEC_STRM_CORRUPTED	BIT(1)
#define TH1520_IRQ_DEC_E		BIT(0)

#define TH1520_IRQ_STAT_MASK		GENMASK(23, 11)

#define TH1520_IRQ_ERROR_MASK \
	(TH1520_IRQ_DEC_TIMEOUT | TH1520_IRQ_DEC_ERROR_INT | \
	 TH1520_IRQ_DEC_BUS_INT | TH1520_IRQ_DEC_BUFFER_INT)

#define th1520_dec_irq_stat		TH1520_REG(1, 11, 0x1fff)
#define th1520_dec_irq			TH1520_REG(1, 8, 0x1)
#define th1520_dec_tile_int_e		TH1520_REG(1, 7, 0x1)
#define th1520_dec_abort_e		TH1520_REG(1, 5, 0x1)
#define th1520_dec_irq_dis		TH1520_REG(1, 4, 0x1)
#define th1520_dec_e			TH1520_REG(1, 0, 0x1)

/*
 * Register 3 bits 31:27 select the hardware decoder engine.
 */
#define TH1520_DEC_MODE_H264		0
#define TH1520_DEC_MODE_JPEG		3
#define TH1520_DEC_MODE_HEVC		12
#define TH1520_DEC_MODE_VP9		13
#define TH1520_DEC_MODE_H264_HIGH10	15

#define th1520_dec_mode			TH1520_REG(3, 27, 0x1f)

#define TH1520_BUS_WIDTH_32		0
#define TH1520_BUS_WIDTH_64		1
#define TH1520_BUS_WIDTH_128		2
#define TH1520_BUS_WIDTH_256		3

/*
 * H.264 controls. The build-specific coding-block dimensions are in the backend.
 */
#define h264_dec_clk_gate_e		TH1520_REG(2, 10, 0x1)
#define h264_table_byte_order		TH1520_REG(2, 12, 0xf)
#define h264_input_exhausted_irq_enable		TH1520_REG(3, 2, 0x1)
#define h264_input_block_mode		TH1520_REG(3, 1, 0x1)

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

#define h264_max_burst_sw58		TH1520_REG(58, 0, 0xff)
#define h264_buswidth_sw58		TH1520_REG(58, 8, 0x7)
#define h264_axi_rd_id_e_sw58		TH1520_REG(58, 14, 0x1)
#define h264_axi_wd_id_e_sw58		TH1520_REG(58, 13, 0x1)

#define h264_pic_mb_width		TH1520_REG(4, 23, 0x1ff)
#define h264_pic_mb_height_p		TH1520_REG(4, 11, 0xff)
#define h264_ref_frames			TH1520_REG(4, 0, 0x1f)

#define h264_pic_mb_h_ext		TH1520_REG(7, 25, 0x1)

#define h264_strm_start_bit		TH1520_REG(5, 25, 0x7f)
#define h264_type1_quant_e		TH1520_REG(5, 24, 0x1)
#define h264_ch_qp_offset		TH1520_REG(5, 19, 0x1f)
#define h264_ch_qp_offset2		TH1520_REG(5, 14, 0x1f)
#define h264_fieldpic_flag_e		TH1520_REG(5, 0, 0x1)

/*
 * Register 6 contains the full 32-bit stream length.
 */
#define h264_stream_len			TH1520_REG(6, 0, 0xffffffff)

#define h264_cabac_e			TH1520_REG(7, 31, 0x1)
#define h264_blackwhite_e		TH1520_REG(7, 30, 0x1)
#define h264_dir_8x8_infer_e		TH1520_REG(7, 29, 0x1)
#define h264_weight_pred_e		TH1520_REG(7, 28, 0x1)
#define h264_weight_bipr_idc		TH1520_REG(7, 26, 0x3)
#define h264_framenum_len		TH1520_REG(7, 16, 0x1f)
#define h264_framenum			TH1520_REG(7, 0, 0xffff)

#define h264_const_intra_e		TH1520_REG(8, 31, 0x1)
#define h264_filt_ctrl_pres		TH1520_REG(8, 30, 0x1)
#define h264_rdpic_cnt_pres		TH1520_REG(8, 29, 0x1)
#define h264_8x8trans_flag_e		TH1520_REG(8, 28, 0x1)
#define h264_refpic_mk_len		TH1520_REG(8, 17, 0x7ff)
#define h264_idr_pic_e			TH1520_REG(8, 16, 0x1)
#define h264_idr_pic_id			TH1520_REG(8, 0, 0xffff)

#define h264_pps_id			TH1520_REG(9, 24, 0xff)
#define h264_refidx1_active		TH1520_REG(9, 19, 0x1f)
#define h264_refidx0_active		TH1520_REG(9, 14, 0x1f)
#define h264_poc_length			TH1520_REG(9, 0, 0xff)

/*
 * Register 13 contains start-code parsing and initial QP controls.
 */
#define h264_start_code_e		TH1520_REG(13, 31, 0x1)
#define h264_init_qp			TH1520_REG(13, 24, 0x7f)

#define TH1520_H264_SWREG_ERR_CONC	48

#define h264_pred_bc_tap_0_0		TH1520_REG(49, 22, 0x3ff)
#define h264_pred_bc_tap_0_1		TH1520_REG(49, 12, 0x3ff)
#define h264_pred_bc_tap_0_2		TH1520_REG(49, 2, 0x3ff)

#define h264_apf_threshold		TH1520_REG(55, 0, 0xffff)

#define h264_error_control_bit31	TH1520_REG(266, 31, 0x1)

#define h264_native_luma_stride		TH1520_REG(314, 16, 0xffff)
#define h264_native_chroma_stride		TH1520_REG(314, 0, 0xffff)

#define h264_ext_timeout_override_e	TH1520_REG(318, 31, 0x1)
#define h264_ext_timeout_cycles		TH1520_REG(318, 0, 0x7fffffff)
#define h264_timeout_override_e		TH1520_REG(319, 31, 0x1)
#define h264_timeout_cycles		TH1520_REG(319, 0, 0x7fffffff)

/*
 * Long-term and valid-reference bitmaps use complete 32-bit words.
 */
#define TH1520_H264_SWREG_LT_REF	38
#define TH1520_H264_SWREG_VALID_REF	39

/*
 * Address pairs are ordered MSB then LSB. Native references use these addresses.
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
 * Each reference-picture word packs two 16-bit frame numbers.
 */
#define TH1520_H264_SWREG_REF_PIC(i)	(30 + (i))
#define TH1520_H264_REF_NBR_EVEN(x)	(((x) & 0xffff) << 0)
#define TH1520_H264_REF_NBR_ODD(x)	(((x) & 0xffff) << 16)

/*
 * Mode 0 reference lists pack six 5-bit entries per word.
 */
#define TH1520_H264_SWREG_BD_REF_PIC(i)	(42 + (i))
#define TH1520_H264_SWREG_BD_P_REF_PIC	47
#define TH1520_H264_SWREG_FWD_PIC(i)	(10 + (i))

/*
 * HEVC controls for hardware mode 12.
 */
#define hevc_clk_gate_e			TH1520_REG(2, 10, 0x1)

#define hevc_out_ec_bypass		TH1520_REG(3, 8, 0x1)
#define hevc_control_bit7		TH1520_REG(3, 7, 0x1)
#define hevc_skip_reference_reads		TH1520_REG(3, 6, 0x1)
#define hevc_control_bit5		TH1520_REG(3, 5, 0x1)
#define hevc_input_exhausted_irq_enable		TH1520_REG(3, 2, 0x1)
#define hevc_input_block_mode	TH1520_REG(3, 1, 0x1)
#define hevc_final_input_buffer		TH1520_REG(3, 0, 0x1)

#define hevc_out_dis			TH1520_REG(3, 15, 0x1)
#define hevc_filtering_dis		TH1520_REG(3, 14, 0x1)
#define hevc_write_mvs_e		TH1520_REG(3, 12, 0x1)

#define hevc_pic_width_in_cbs		TH1520_REG(4, 19, 0x1fff)
#define hevc_pic_height_in_cbs		TH1520_REG(4, 6, 0x1fff)
#define hevc_num_ref_frames		TH1520_REG(4, 0, 0x1f)

#define hevc_strm_start_bit		TH1520_REG(5, 25, 0x7f)
#define hevc_scaling_list_e		TH1520_REG(5, 24, 0x1)
#define hevc_cb_qp_offset		TH1520_REG(5, 19, 0x1f)
#define hevc_cr_qp_offset		TH1520_REG(5, 14, 0x1f)
#define hevc_sign_data_hide		TH1520_REG(5, 12, 0x1)
#define hevc_tempor_mvp_e		TH1520_REG(5, 11, 0x1)
#define hevc_max_cu_qpd_depth		TH1520_REG(5, 5, 0x3f)
#define hevc_cu_qpd_e			TH1520_REG(5, 4, 0x1)

#define hevc_stream_len			TH1520_REG(6, 0, 0xffffffff)

#define hevc_cabac_init_present		TH1520_REG(7, 31, 0x1)
#define hevc_blackwhite_e		TH1520_REG(7, 30, 0x1)
#define hevc_weight_pred_e		TH1520_REG(7, 28, 0x1)
#define hevc_weight_bipr_idc		TH1520_REG(7, 26, 0x3)
#define hevc_loop_filter_across_slices		TH1520_REG(7, 25, 0x1)
#define hevc_loop_filter_across_tiles		TH1520_REG(7, 24, 0x1)
#define hevc_asym_pred_e		TH1520_REG(7, 23, 0x1)
#define hevc_sao_e			TH1520_REG(7, 22, 0x1)
#define hevc_pcm_loop_filter_disabled		TH1520_REG(7, 21, 0x1)
#define hevc_slice_chroma_qp_offsets_present		TH1520_REG(7, 20, 0x1)
#define hevc_dependent_segments_enabled		TH1520_REG(7, 19, 0x1)
#define hevc_deblocking_override_enabled		TH1520_REG(7, 18, 0x1)
#define hevc_strong_smooth_e		TH1520_REG(7, 17, 0x1)
#define hevc_filt_offset_beta		TH1520_REG(7, 12, 0x1f)
#define hevc_filt_offset_tc		TH1520_REG(7, 7, 0x1f)
#define hevc_slice_hdr_ext_e		TH1520_REG(7, 6, 0x1)
#define hevc_slice_header_extra_bits		TH1520_REG(7, 3, 0x7)

#define hevc_const_intra_e		TH1520_REG(8, 31, 0x1)
#define hevc_filt_ctrl_pres		TH1520_REG(8, 30, 0x1)
#define hevc_idr_pic_e			TH1520_REG(8, 16, 0x1)
#define hevc_pcm_luma_sample_depth		TH1520_REG(8, 12, 0xf)
#define hevc_pcm_chroma_sample_depth		TH1520_REG(8, 8, 0xf)
#define hevc_bit_depth_y_minus8		TH1520_REG(8, 6, 0x3)
#define hevc_bit_depth_c_minus8		TH1520_REG(8, 4, 0x3)
#define hevc_output_8_bits		TH1520_REG(8, 3, 0x1)
#define hevc_output_format		TH1520_REG(8, 0, 0x7)

#define hevc_refidx1_active		TH1520_REG(9, 19, 0x1f)
#define hevc_refidx0_active		TH1520_REG(9, 14, 0x1f)
#define hevc_hdr_skip_length		TH1520_REG(9, 0, 0x3fff)

/*
 * Register 10 stores the tile-column and tile-row counts.
 */
#define hevc_num_tile_cols_8k		TH1520_REG(10, 17, 0x7f)
#define hevc_num_tile_rows_8k		TH1520_REG(10, 12, 0x1f)
#define hevc_tile_enable		TH1520_REG(10, 1, 0x1)
#define hevc_entropy_row_sync_enabled		TH1520_REG(10, 0, 0x1)

/*
 * The HEVC long-term-reference bitmap shares register 38 with H.264.
 */
#define hevc_refer_lterm_e		TH1520_REG(38, 0, 0xffffffff)
#define hevc_min_cb_size		TH1520_REG(12, 13, 0x7)
#define hevc_max_cb_size		TH1520_REG(12, 10, 0x7)
#define hevc_min_pcm_size		TH1520_REG(12, 7, 0x7)
#define hevc_max_pcm_size		TH1520_REG(12, 4, 0x7)
#define hevc_pcm_e			TH1520_REG(12, 3, 0x1)
#define hevc_transform_skip_enabled		TH1520_REG(12, 2, 0x1)
#define hevc_transquant_bypass_enabled		TH1520_REG(12, 1, 0x1)
#define hevc_reference_list_modification_enabled		TH1520_REG(12, 0, 0x1)

#define hevc_start_code_e		TH1520_REG(13, 31, 0x1)
#define hevc_init_qp			TH1520_REG(13, 24, 0x7f)
#define hevc_min_trb_size		TH1520_REG(13, 13, 0x7)
#define hevc_max_trb_size		TH1520_REG(13, 10, 0x7)
#define hevc_max_intra_hierdepth	TH1520_REG(13, 7, 0x7)
#define hevc_max_inter_hierdepth	TH1520_REG(13, 4, 0x7)
#define hevc_parallel_merge		TH1520_REG(13, 0, 0xf)

/*
 * Each reference-list word packs three pairs of 5-bit L0 and L1 indices.
 */
#define TH1520_HEVC_SWREG_RLIST(i)	(14 + (i))

#define hevc_partial_ctb_x		TH1520_REG(20, 31, 0x1)
#define hevc_partial_ctb_y		TH1520_REG(20, 30, 0x1)
#define hevc_pic_width_4x4		TH1520_REG(20, 16, 0xfff)
#define hevc_pic_height_4x4		TH1520_REG(20, 0, 0xfff)

#define hevc_ext_timeout_override_e	TH1520_REG(318, 31, 0x1)
#define hevc_ext_timeout_cycles		TH1520_REG(318, 0, 0x7fffffff)
#define hevc_timeout_override_e		TH1520_REG(319, 31, 0x1)
#define hevc_timeout_cycles		TH1520_REG(319, 0, 0x7fffffff)

/*
 * Each POC word packs four signed 8-bit differences; saturate before encoding.
 */
#define TH1520_HEVC_SWREG_CUR_POC(i)	(46 + (i))

#define hevc_apf_disable		TH1520_REG(55, 31, 0x1)
#define hevc_apf_threshold		TH1520_REG(55, 0, 0xffff)

#define hevc_refer_doublebuffer_e	TH1520_REG(58, 15, 0x1)
#define hevc_axi_rd_id_e		TH1520_REG(58, 14, 0x1)
#define hevc_axi_wd_id_e		TH1520_REG(58, 13, 0x1)
#define hevc_buswidth			TH1520_REG(58, 8, 0x7)
#define hevc_max_burst			TH1520_REG(58, 0, 0xff)

#define hevc_axi_wr_id			TH1520_REG(60, 16, 0xffff)
#define hevc_axi_rd_id			TH1520_REG(60, 0, 0xffff)

/*
 * Register 265: master bit stays clear; fields 27:18 and 17:8 are set to 64.
 * The individual field meanings remain unverified.
 */
#define TH1520_SWREG_CACHE_SHAPER_CTRL	265

#define hevc_down_scale_e		TH1520_REG(184, 7, 0x1)
#define hevc_down_scale_y		TH1520_REG(184, 2, 0x3)
#define hevc_down_scale_x		TH1520_REG(184, 0, 0x3)

#define hevc_strm_buffer_len		TH1520_REG(258, 0, 0xffffffff)
#define hevc_strm_start_offset		TH1520_REG(259, 0, 0xffffffff)

#define hevc_native_luma_stride		TH1520_REG(314, 16, 0xffff)
#define hevc_native_chroma_stride		TH1520_REG(314, 0, 0xffff)

/*
 * HEVC address pairs: the listed index is the MSB word; the LSB follows.
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

#endif
