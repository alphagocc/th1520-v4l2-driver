/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * TH1520 VC8000D VP9 probability helpers.
 * Adapted from Linux hantro_vp9.c, hantro_vp9.h and hantro_g2_vp9_dec.c.
 * Copyright (C) 2021 Collabora Ltd.
 * Copyright (C) 2026 th1520-v4l2 contributors
 * Linux commit 8ba098e6b6ff0db8edf28528d1552be261af30d4.
 * See docs/vp9.md for the independent TH1520 SDK layout evidence.
 */

#include "th1520_vdec.h"
#include "th1520_vdec_vp9.h"

static void *get_coeffs_arr(struct th1520_vp9_counts *cnts, int i, int j, int k, int l, int m)
{
	if (i == 0)
		return &cnts->count_coeffs[j][k][l][m];

	if (i == 1)
		return &cnts->count_coeffs8x8[j][k][l][m];

	if (i == 2)
		return &cnts->count_coeffs16x16[j][k][l][m];

	if (i == 3)
		return &cnts->count_coeffs32x32[j][k][l][m];

	return NULL;
}

static void *get_eobs1(struct th1520_vp9_counts *cnts, int i, int j, int k, int l, int m)
{
	if (i == 0)
		return &cnts->count_coeffs[j][k][l][m][3];

	if (i == 1)
		return &cnts->count_coeffs8x8[j][k][l][m][3];

	if (i == 2)
		return &cnts->count_coeffs16x16[j][k][l][m][3];

	if (i == 3)
		return &cnts->count_coeffs32x32[j][k][l][m][3];

	return NULL;
}

#define INNER_LOOP \
	do {										\
		for (m = 0; m < ARRAY_SIZE(vp9_ctx->cnts.coeff[i][0][0][0]); ++m) {	\
			vp9_ctx->cnts.coeff[i][j][k][l][m] =				\
				get_coeffs_arr(cnts, i, j, k, l, m);			\
			vp9_ctx->cnts.eob[i][j][k][l][m][0] =				\
				&cnts->count_eobs[i][j][k][l][m];			\
			vp9_ctx->cnts.eob[i][j][k][l][m][1] =				\
				get_eobs1(cnts, i, j, k, l, m);				\
		}									\
	} while (0)

void th1520_vp9_init_counts(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_vp9_ctx *vp9_ctx = ctx->vp9;
	struct th1520_vp9_counts *cnts = vp9_ctx->counts.cpu;
	int i, j, k, l, m;

	vp9_ctx->cnts.partition = &cnts->partition_counts;
	vp9_ctx->cnts.skip = &cnts->mbskip_count;
	vp9_ctx->cnts.intra_inter = &cnts->intra_inter_count;
	vp9_ctx->cnts.tx32p = &cnts->tx32x32_count;
	/*
	 * The SDK counter buffer uses tx16x16_count[2][3], while the API
	 * expects tx16p[2][4], so this must be explicitly copied
	 * into vp9_ctx->cnts.tx16p when passing the data to the
	 * vp9 library function
	 */
	vp9_ctx->cnts.tx8p = &cnts->tx8x8_count;

	vp9_ctx->cnts.y_mode = &cnts->sb_ymode_counts;
	vp9_ctx->cnts.uv_mode = &cnts->uv_mode_counts;
	vp9_ctx->cnts.comp = &cnts->comp_inter_count;
	vp9_ctx->cnts.comp_ref = &cnts->comp_ref_count;
	vp9_ctx->cnts.single_ref = &cnts->single_ref_count;
	vp9_ctx->cnts.filter = &cnts->switchable_interp_counts;
	vp9_ctx->cnts.mv_joint = &cnts->mv_counts.joints;
	vp9_ctx->cnts.sign = &cnts->mv_counts.sign;
	vp9_ctx->cnts.classes = &cnts->mv_counts.classes;
	vp9_ctx->cnts.class0 = &cnts->mv_counts.class0;
	vp9_ctx->cnts.bits = &cnts->mv_counts.bits;
	vp9_ctx->cnts.class0_fp = &cnts->mv_counts.class0_fp;
	vp9_ctx->cnts.fp = &cnts->mv_counts.fp;
	vp9_ctx->cnts.class0_hp = &cnts->mv_counts.class0_hp;
	vp9_ctx->cnts.hp = &cnts->mv_counts.hp;

	for (i = 0; i < ARRAY_SIZE(vp9_ctx->cnts.coeff); ++i)
		for (j = 0; j < ARRAY_SIZE(vp9_ctx->cnts.coeff[i]); ++j)
			for (k = 0; k < ARRAY_SIZE(vp9_ctx->cnts.coeff[i][0]); ++k)
				for (l = 0; l < ARRAY_SIZE(vp9_ctx->cnts.coeff[i][0][0]); ++l)
					INNER_LOOP;
}


#undef INNER_LOOP

#define INNER_LOOP \
do {									\
	for (m = 0; m < ARRAY_SIZE(adaptive->coef[0][0][0][0]); ++m) {	\
		memcpy(adaptive->coef[i][j][k][l][m],			\
		       probs->coef[i][j][k][l][m],			\
		       sizeof(probs->coef[i][j][k][l][m]));		\
									\
		adaptive->coef[i][j][k][l][m][3] = 0;			\
	}								\
} while (0)

void th1520_vp9_pack_probs(struct th1520_vdec_ctx *ctx, const struct v4l2_ctrl_vp9_frame *dec_params)
{
	struct th1520_vdec_vp9_ctx *vp9_ctx = ctx->vp9;
	struct th1520_vdec_aux_buf *misc = &vp9_ctx->probs;
	struct th1520_vp9_all_probs *all_probs = misc->cpu;
	struct th1520_vp9_probs *adaptive;
	struct th1520_vp9_mv_probs *mv;
	const struct v4l2_vp9_segmentation *seg = &dec_params->seg;
	const struct v4l2_vp9_frame_context *probs = &vp9_ctx->probability_tables;
	int i, j, k, l, m;

	for (i = 0; i < ARRAY_SIZE(all_probs->kf_y_mode_prob); ++i)
		for (j = 0; j < ARRAY_SIZE(all_probs->kf_y_mode_prob[0]); ++j) {
			memcpy(all_probs->kf_y_mode_prob[i][j],
			       v4l2_vp9_kf_y_mode_prob[i][j],
			       ARRAY_SIZE(all_probs->kf_y_mode_prob[i][j]));

			all_probs->kf_y_mode_prob_tail[i][j][0] =
				v4l2_vp9_kf_y_mode_prob[i][j][8];
		}

	memcpy(all_probs->mb_segment_tree_probs, seg->tree_probs,
	       sizeof(all_probs->mb_segment_tree_probs));

	memcpy(all_probs->segment_pred_probs, seg->pred_probs,
	       sizeof(all_probs->segment_pred_probs));

	for (i = 0; i < ARRAY_SIZE(all_probs->kf_uv_mode_prob); ++i) {
		memcpy(all_probs->kf_uv_mode_prob[i], v4l2_vp9_kf_uv_mode_prob[i],
		       ARRAY_SIZE(all_probs->kf_uv_mode_prob[i]));

		all_probs->kf_uv_mode_prob_tail[i][0] = v4l2_vp9_kf_uv_mode_prob[i][8];
	}

	adaptive = &all_probs->probs;

	for (i = 0; i < ARRAY_SIZE(adaptive->inter_mode); ++i) {
		memcpy(adaptive->inter_mode[i], probs->inter_mode[i],
		       ARRAY_SIZE(probs->inter_mode[i]));

		adaptive->inter_mode[i][3] = 0;
	}

	memcpy(adaptive->is_inter, probs->is_inter, sizeof(adaptive->is_inter));

	for (i = 0; i < ARRAY_SIZE(adaptive->uv_mode); ++i) {
		memcpy(adaptive->uv_mode[i], probs->uv_mode[i],
		       sizeof(adaptive->uv_mode[i]));
		adaptive->uv_mode_tail[i][0] = probs->uv_mode[i][8];
	}

	memcpy(adaptive->tx8, probs->tx8, sizeof(adaptive->tx8));
	memcpy(adaptive->tx16, probs->tx16, sizeof(adaptive->tx16));
	memcpy(adaptive->tx32, probs->tx32, sizeof(adaptive->tx32));

	for (i = 0; i < ARRAY_SIZE(adaptive->y_mode); ++i) {
		memcpy(adaptive->y_mode[i], probs->y_mode[i],
		       ARRAY_SIZE(adaptive->y_mode[i]));

		adaptive->y_mode_tail[i][0] = probs->y_mode[i][8];
	}

	for (i = 0; i < ARRAY_SIZE(adaptive->partition[0]); ++i) {
		memcpy(adaptive->partition[0][i], v4l2_vp9_kf_partition_probs[i],
		       sizeof(v4l2_vp9_kf_partition_probs[i]));

		adaptive->partition[0][i][3] = 0;
	}

	for (i = 0; i < ARRAY_SIZE(adaptive->partition[1]); ++i) {
		memcpy(adaptive->partition[1][i], probs->partition[i],
		       sizeof(probs->partition[i]));

		adaptive->partition[1][i][3] = 0;
	}

	memcpy(adaptive->interp_filter, probs->interp_filter,
	       sizeof(adaptive->interp_filter));

	memcpy(adaptive->comp_mode, probs->comp_mode, sizeof(adaptive->comp_mode));

	memcpy(adaptive->skip, probs->skip, sizeof(adaptive->skip));

	mv = &adaptive->mv;

	memcpy(mv->joint, probs->mv.joint, sizeof(mv->joint));
	memcpy(mv->sign, probs->mv.sign, sizeof(mv->sign));
	memcpy(mv->class0_bit, probs->mv.class0_bit, sizeof(mv->class0_bit));
	memcpy(mv->fr, probs->mv.fr, sizeof(mv->fr));
	memcpy(mv->class0_hp, probs->mv.class0_hp, sizeof(mv->class0_hp));
	memcpy(mv->hp, probs->mv.hp, sizeof(mv->hp));
	memcpy(mv->classes, probs->mv.classes, sizeof(mv->classes));
	memcpy(mv->class0_fr, probs->mv.class0_fr, sizeof(mv->class0_fr));
	memcpy(mv->bits, probs->mv.bits, sizeof(mv->bits));

	memcpy(adaptive->single_ref, probs->single_ref, sizeof(adaptive->single_ref));

	memcpy(adaptive->comp_ref, probs->comp_ref, sizeof(adaptive->comp_ref));

	for (i = 0; i < ARRAY_SIZE(adaptive->coef); ++i)
		for (j = 0; j < ARRAY_SIZE(adaptive->coef[0]); ++j)
			for (k = 0; k < ARRAY_SIZE(adaptive->coef[0][0]); ++k)
				for (l = 0; l < ARRAY_SIZE(adaptive->coef[0][0][0]); ++l)
					INNER_LOOP;


}


#undef INNER_LOOP

#define copy_tx_and_skip(p1, p2)				\
do {								\
	memcpy((p1)->tx8, (p2)->tx8, sizeof((p1)->tx8));	\
	memcpy((p1)->tx16, (p2)->tx16, sizeof((p1)->tx16));	\
	memcpy((p1)->tx32, (p2)->tx32, sizeof((p1)->tx32));	\
	memcpy((p1)->skip, (p2)->skip, sizeof((p1)->skip));	\
} while (0)

void th1520_vp9_update_probs(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_vp9_ctx *vp9_ctx = ctx->vp9;
	unsigned int fctx_idx;

	if (!(vp9_ctx->cur.flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX))
		return;

	fctx_idx = vp9_ctx->cur.frame_context_idx;

	if (!(vp9_ctx->cur.flags & (V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE |
				    V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT))) {
		/* error_resilient_mode == 0 && frame_parallel_decoding_mode == 0 */
		struct v4l2_vp9_frame_context *probs = &vp9_ctx->probability_tables;
		bool frame_is_intra = vp9_ctx->cur.flags &
		    (V4L2_VP9_FRAME_FLAG_KEY_FRAME | V4L2_VP9_FRAME_FLAG_INTRA_ONLY);
		struct tx_and_skip {
			u8 tx8[2][1];
			u8 tx16[2][2];
			u8 tx32[2][3];
			u8 skip[3];
		} _tx_skip, *tx_skip = &_tx_skip;
		struct v4l2_vp9_frame_symbol_counts *counts;
		struct th1520_vp9_counts *hw_counts;
		u32 tx16p[2][4];
		int i;

		/* buffer the forward-updated TX and skip probs */
		if (frame_is_intra)
			copy_tx_and_skip(tx_skip, probs);

		/* 6.1.2 refresh_probs(): load_probs() and load_probs2() */
		*probs = vp9_ctx->working_context[fctx_idx];

		/* if FrameIsIntra then undo the effect of load_probs2() */
		if (frame_is_intra)
			copy_tx_and_skip(probs, tx_skip);

		counts = &vp9_ctx->cnts;
		hw_counts = vp9_ctx->counts.cpu;
		for (i = 0; i < ARRAY_SIZE(tx16p); ++i) {
			memcpy(tx16p[i],
			       hw_counts->tx16x16_count[i],
			       sizeof(hw_counts->tx16x16_count[0]));
			tx16p[i][3] = 0;
		}
		counts->tx16p = &tx16p;

		v4l2_vp9_adapt_coef_probs(probs, counts,
					  !vp9_ctx->last.valid ||
					  vp9_ctx->last.flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME,
					  frame_is_intra);

		if (!frame_is_intra) {
			/* load_probs2() already done */
			u32 mv_mode[7][4];

			for (i = 0; i < ARRAY_SIZE(mv_mode); ++i) {
				mv_mode[i][0] = hw_counts->inter_mode_counts[i][1][0];
				mv_mode[i][1] = hw_counts->inter_mode_counts[i][2][0];
				mv_mode[i][2] = hw_counts->inter_mode_counts[i][0][0];
				mv_mode[i][3] = hw_counts->inter_mode_counts[i][2][1];
			}
			counts->mv_mode = &mv_mode;
			v4l2_vp9_adapt_noncoef_probs(&vp9_ctx->probability_tables, counts,
						     vp9_ctx->cur.reference_mode,
						     vp9_ctx->cur.interpolation_filter,
						     vp9_ctx->cur.tx_mode, vp9_ctx->cur.flags);
		}
	}

	vp9_ctx->working_context[fctx_idx] = vp9_ctx->probability_tables;

}
