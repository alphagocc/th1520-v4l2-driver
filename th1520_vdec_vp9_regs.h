/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 th1520-v4l2 contributors
 * TH1520 0x8001 product-table coordinates and SDK VP9 call sites.
 * Public field terminology: Copyright (C) 2021 Collabora Ltd.
 * Linux commit 8ba098e6b6ff0db8edf28528d1552be261af30d4.
 * See docs/vp9.md. These are not Hantro G2 register coordinates.
 */
#ifndef TH1520_VDEC_VP9_REGS_H_
#define TH1520_VDEC_VP9_REGS_H_

#include "th1520_vdec_regs.h"

#define vp9_transform_mode             TH1520_REG(11, 27, 0x7)
#define vp9_filt_sharpness             TH1520_REG(30, 28, 0x7)
#define vp9_mcomp_filt_type            TH1520_REG(11, 8, 0x7)
#define vp9_high_prec_mv_e             TH1520_REG(11, 7, 0x1)
#define vp9_comp_pred_mode             TH1520_REG(11, 4, 0x3)
#define vp9_gref_sign_bias             TH1520_REG(59, 29, 0x1)
#define vp9_aref_sign_bias             TH1520_REG(59, 28, 0x1)
#define vp9_qp_delta_y_dc              TH1520_REG(13, 23, 0x3f)
#define vp9_qp_delta_ch_dc             TH1520_REG(13, 17, 0x3f)
#define vp9_qp_delta_ch_ac             TH1520_REG(13, 11, 0x3f)
#define vp9_last_sign_bias             TH1520_REG(13, 10, 0x1)
#define vp9_lossless_e                 TH1520_REG(13, 9, 0x1)
#define vp9_comp_pred_var_ref1         TH1520_REG(13, 7, 0x3)
#define vp9_comp_pred_var_ref0         TH1520_REG(13, 5, 0x3)
#define vp9_comp_pred_fixed_ref        TH1520_REG(13, 3, 0x3)
#define vp9_segment_temp_upd_e         TH1520_REG(13, 2, 0x1)
#define vp9_segment_upd_e              TH1520_REG(13, 1, 0x1)
#define vp9_segment_e                  TH1520_REG(13, 0, 0x1)
#define vp9_filt_level                 TH1520_REG(14, 18, 0x3f)
#define vp9_refpic_seg0                TH1520_REG(14, 15, 0x7)
#define vp9_skip_seg0                  TH1520_REG(14, 14, 0x1)
#define vp9_filt_level_seg0            TH1520_REG(14, 8, 0x3f)
#define vp9_quant_seg0                 TH1520_REG(14, 0, 0xff)
#define vp9_refpic_seg1                TH1520_REG(15, 15, 0x7)
#define vp9_skip_seg1                  TH1520_REG(15, 14, 0x1)
#define vp9_filt_level_seg1            TH1520_REG(15, 8, 0x3f)
#define vp9_quant_seg1                 TH1520_REG(15, 0, 0xff)
#define vp9_refpic_seg2                TH1520_REG(16, 15, 0x7)
#define vp9_skip_seg2                  TH1520_REG(16, 14, 0x1)
#define vp9_filt_level_seg2            TH1520_REG(16, 8, 0x3f)
#define vp9_quant_seg2                 TH1520_REG(16, 0, 0xff)
#define vp9_refpic_seg3                TH1520_REG(17, 15, 0x7)
#define vp9_skip_seg3                  TH1520_REG(17, 14, 0x1)
#define vp9_filt_level_seg3            TH1520_REG(17, 8, 0x3f)
#define vp9_quant_seg3                 TH1520_REG(17, 0, 0xff)
#define vp9_refpic_seg4                TH1520_REG(18, 15, 0x7)
#define vp9_skip_seg4                  TH1520_REG(18, 14, 0x1)
#define vp9_filt_level_seg4            TH1520_REG(18, 8, 0x3f)
#define vp9_quant_seg4                 TH1520_REG(18, 0, 0xff)
#define vp9_refpic_seg5                TH1520_REG(19, 15, 0x7)
#define vp9_skip_seg5                  TH1520_REG(19, 14, 0x1)
#define vp9_filt_level_seg5            TH1520_REG(19, 8, 0x3f)
#define vp9_quant_seg5                 TH1520_REG(19, 0, 0xff)
#define vp9_refpic_seg6                TH1520_REG(31, 15, 0x7)
#define vp9_skip_seg6                  TH1520_REG(31, 14, 0x1)
#define vp9_filt_level_seg6            TH1520_REG(31, 8, 0x3f)
#define vp9_quant_seg6                 TH1520_REG(31, 0, 0xff)
#define vp9_refpic_seg7                TH1520_REG(32, 15, 0x7)
#define vp9_skip_seg7                  TH1520_REG(32, 14, 0x1)
#define vp9_filt_level_seg7            TH1520_REG(32, 8, 0x3f)
#define vp9_quant_seg7                 TH1520_REG(32, 0, 0xff)
#define vp9_lref_width                 TH1520_REG(33, 16, 0xffff)
#define vp9_lref_height                TH1520_REG(33, 0, 0xffff)
#define vp9_gref_width                 TH1520_REG(34, 16, 0xffff)
#define vp9_gref_height                TH1520_REG(34, 0, 0xffff)
#define vp9_aref_width                 TH1520_REG(35, 16, 0xffff)
#define vp9_aref_height                TH1520_REG(35, 0, 0xffff)
#define vp9_lref_hor_scale             TH1520_REG(36, 16, 0xffff)
#define vp9_lref_ver_scale             TH1520_REG(36, 0, 0xffff)
#define vp9_gref_hor_scale             TH1520_REG(37, 16, 0xffff)
#define vp9_gref_ver_scale             TH1520_REG(37, 0, 0xffff)
#define vp9_aref_hor_scale             TH1520_REG(38, 16, 0xffff)
#define vp9_aref_ver_scale             TH1520_REG(38, 0, 0xffff)
#define vp9_filt_ref_adj_0             TH1520_REG(59, 21, 0x7f)
#define vp9_filt_ref_adj_1             TH1520_REG(59, 14, 0x7f)
#define vp9_filt_ref_adj_2             TH1520_REG(59, 7, 0x7f)
#define vp9_filt_ref_adj_3             TH1520_REG(59, 0, 0x7f)
#define vp9_filt_mb_adj_0              TH1520_REG(30, 21, 0x7f)
#define vp9_filt_mb_adj_1              TH1520_REG(30, 14, 0x7f)

#define TH1520_VP9_ADDR_SEG_WRITE 78
#define TH1520_VP9_ADDR_SEG_READ  80
#define TH1520_VP9_ADDR_COUNTS    170
#define TH1520_VP9_ADDR_PROBS     172

#endif
