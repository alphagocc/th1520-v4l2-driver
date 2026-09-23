// SPDX-License-Identifier: GPL-2.0-only
/*
 * TH1520 VC8000D V4L2 stateless decoder interface.
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * Framework portions adapted from Linux hantro_v4l2.c:
 * Copyright (C) 2018 Collabora, Ltd.
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *     Alpha Lin <Alpha.Lin@rock-chips.com>
 *     Jeffy Chen <jeffy.chen@rock-chips.com>
 * Copyright 2018 Google LLC.
 *     Tomasz Figa <tfiga@chromium.org>
 * Based on the s5p-mfc driver:
 * Copyright (C) 2010-2011 Samsung Electronics Co., Ltd.
 *
 * Linux commit 8ba098e6b6ff0db8edf28528d1552be261af30d4.
 * The stateless ABI and public source links are listed in docs/sources.md.
 */

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/string.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "th1520_vdec.h"

static const struct th1520_vdec_fmt th1520_vdec_formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.codec_mode = TH1520_MODE_NONE,
		.is_bitstream = false,
	},
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.codec_mode = TH1520_MODE_H264_DEC,
		.is_bitstream = true,
	},
	{
		.fourcc = V4L2_PIX_FMT_HEVC_SLICE,
		.codec_mode = TH1520_MODE_HEVC_DEC,
		.is_bitstream = true,
	},
	{
		.fourcc = V4L2_PIX_FMT_VP9_FRAME,
		.codec_mode = TH1520_MODE_VP9_DEC,
		.is_bitstream = true,
	},
};

/*
 * Default bitstream-buffer capacity. Userspace may negotiate a different
 * sizeimage with VIDIOC_S_FMT; each stateless request supplies one frame.
 */
#define TH1520_DEFAULT_BITSTREAM_SIZE	(1024 * 1024)
#define TH1520_DEFAULT_WIDTH		1920
#define TH1520_DEFAULT_HEIGHT		1088

static const struct th1520_vdec_fmt *th1520_vdec_find_fmt(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(th1520_vdec_formats); i++)
		if (th1520_vdec_formats[i].fourcc == fourcc)
			return &th1520_vdec_formats[i];

	return NULL;
}

static size_t
th1520_vdec_nv12_size(const struct v4l2_pix_format_mplane *pix_mp)
{
	return pix_mp->plane_fmt[0].bytesperline *
	       ALIGN(pix_mp->height, TH1520_MB_DIM) * 3 / 2;
}

/*
 * 各 codec 的原生帧及 motion vectors 都使用私有缓冲。
 * CAPTURE 的分配大小和 payload 均为线性 NV12 图像字节数。
 */
static void
th1520_vdec_fill_pixfmt_cap(struct v4l2_pix_format_mplane *pix_mp)
{
	struct v4l2_plane_pix_format *plane = &pix_mp->plane_fmt[0];

	pix_mp->num_planes = 1;
	plane->bytesperline = ALIGN(pix_mp->width, TH1520_MB_DIM);
	plane->sizeimage = th1520_vdec_nv12_size(pix_mp);
}

static void th1520_vdec_fill_pixfmt_out(struct v4l2_pix_format_mplane *pix_mp)
{
	pix_mp->num_planes = 1;
	pix_mp->plane_fmt[0].bytesperline = 0;
	if (!pix_mp->plane_fmt[0].sizeimage)
		pix_mp->plane_fmt[0].sizeimage = TH1520_DEFAULT_BITSTREAM_SIZE;
}

static int th1520_vdec_querycap(struct file *file, void *priv,
				struct v4l2_capability *cap)
{
	strscpy(cap->driver, TH1520_VDEC_NAME, sizeof(cap->driver));
	strscpy(cap->card, TH1520_VDEC_NAME, sizeof(cap->card));
	return 0;
}

static int th1520_vdec_enum_fmt(struct file *file, void *priv,
				struct v4l2_fmtdesc *f, bool capture)
{
	struct th1520_vdec_ctx *ctx = fh_to_ctx(priv);
	unsigned int i, idx = 0;

	for (i = 0; i < ARRAY_SIZE(th1520_vdec_formats); i++) {
		const struct th1520_vdec_fmt *fmt = &th1520_vdec_formats[i];

		if (capture != !fmt->is_bitstream)
			continue;

		/*
		 * CAPTURE 侧的可用格式取决于已经选定的 OUTPUT 码流格式；
		 * 当前各 codec 都只输出 8 bit NV12。
		 */
		if (capture && !ctx->vpu_src_fmt)
			continue;

		if (idx++ == f->index) {
			f->pixelformat = fmt->fourcc;
			return 0;
		}
	}

	return -EINVAL;
}

static int th1520_vdec_enum_fmt_cap(struct file *file, void *priv,
				    struct v4l2_fmtdesc *f)
{
	return th1520_vdec_enum_fmt(file, priv, f, true);
}

static int th1520_vdec_enum_fmt_out(struct file *file, void *priv,
				    struct v4l2_fmtdesc *f)
{
	return th1520_vdec_enum_fmt(file, priv, f, false);
}

static int th1520_vdec_enum_framesizes(struct file *file, void *priv,
				       struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index != 0)
		return -EINVAL;

	if (!th1520_vdec_find_fmt(fsize->pixel_format))
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = TH1520_MIN_WIDTH;
	fsize->stepwise.max_width = TH1520_MAX_WIDTH;
	fsize->stepwise.step_width = fsize->pixel_format == V4L2_PIX_FMT_VP9_FRAME ?
				     2 : TH1520_MB_DIM;
	fsize->stepwise.min_height = TH1520_MIN_HEIGHT;
	fsize->stepwise.max_height = TH1520_MAX_HEIGHT;
	fsize->stepwise.step_height = fsize->stepwise.step_width;

	return 0;
}

static int th1520_vdec_g_fmt_cap(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct th1520_vdec_ctx *ctx = fh_to_ctx(priv);

	f->fmt.pix_mp = ctx->dst_fmt;
	return 0;
}

static int th1520_vdec_g_fmt_out(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct th1520_vdec_ctx *ctx = fh_to_ctx(priv);

	f->fmt.pix_mp = ctx->src_fmt;
	return 0;
}

static int th1520_vdec_try_fmt(struct th1520_vdec_ctx *ctx,
			       struct v4l2_format *f, bool capture)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct th1520_vdec_fmt *fmt;

	fmt = th1520_vdec_find_fmt(pix_mp->pixelformat);
	if (!fmt || capture == fmt->is_bitstream) {
		/* 回落到该队列的默认格式。 */
		pix_mp->pixelformat = capture ? V4L2_PIX_FMT_NV12 :
						V4L2_PIX_FMT_HEVC_SLICE;
		fmt = th1520_vdec_find_fmt(pix_mp->pixelformat);
		if (WARN_ON(!fmt))
			return -EINVAL;
	}

	pix_mp->field = V4L2_FIELD_NONE;
	pix_mp->width = clamp(pix_mp->width, (u32)TH1520_MIN_WIDTH,
			      (u32)TH1520_MAX_WIDTH);
	pix_mp->height = clamp(pix_mp->height, (u32)TH1520_MIN_HEIGHT,
			       (u32)TH1520_MAX_HEIGHT);

	if (capture) {
		/*
		 * CAPTURE 的分辨率由 OUTPUT 的码流分辨率决定，
		 * 用户态不能单独改（stateless API 要求）。
		 */
		pix_mp->width = ctx->src_fmt.width;
		pix_mp->height = ctx->src_fmt.height;
		if (ctx->vpu_src_fmt->codec_mode == TH1520_MODE_VP9_DEC) {
			pix_mp->width = ALIGN(pix_mp->width, TH1520_MB_DIM);
			pix_mp->height = ALIGN(pix_mp->height, TH1520_MB_DIM);
		}
		pix_mp->colorspace = ctx->src_fmt.colorspace;
		pix_mp->xfer_func = ctx->src_fmt.xfer_func;
		pix_mp->ycbcr_enc = ctx->src_fmt.ycbcr_enc;
		pix_mp->quantization = ctx->src_fmt.quantization;
		th1520_vdec_fill_pixfmt_cap(pix_mp);
	} else {
		unsigned int alignment = fmt->codec_mode == TH1520_MODE_VP9_DEC ?
					 2 : TH1520_MB_DIM;

		pix_mp->width = ALIGN(pix_mp->width, alignment);
		pix_mp->height = ALIGN(pix_mp->height, alignment);
		th1520_vdec_fill_pixfmt_out(pix_mp);
	}

	return 0;
}

static int th1520_vdec_try_fmt_cap(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	return th1520_vdec_try_fmt(fh_to_ctx(priv), f, true);
}

static int th1520_vdec_try_fmt_out(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	return th1520_vdec_try_fmt(fh_to_ctx(priv), f, false);
}

static int th1520_vdec_s_fmt_out(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct th1520_vdec_ctx *ctx = fh_to_ctx(priv);
	struct vb2_queue *vq, *peer_vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	/*
	 * 改变码流格式会重新选择 codec 后端，因此 CAPTURE 队列必须还没有
	 * 分配缓冲区。
	 */
	peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
				  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(peer_vq))
		return -EBUSY;

	ret = th1520_vdec_try_fmt(ctx, f, false);
	if (ret)
		return ret;

	ctx->vpu_src_fmt = th1520_vdec_find_fmt(f->fmt.pix_mp.pixelformat);
	ctx->src_fmt = f->fmt.pix_mp;

	switch (ctx->vpu_src_fmt->codec_mode) {
	case TH1520_MODE_H264_DEC:
		ctx->codec_ops = &th1520_vdec_h264_ops;
		break;
	case TH1520_MODE_HEVC_DEC:
		ctx->codec_ops = &th1520_vdec_hevc_ops;
		break;
	case TH1520_MODE_VP9_DEC:
		ctx->codec_ops = &th1520_vdec_vp9_ops;
		break;
	default:
		return -EINVAL;
	}

	/* 同步 CAPTURE 侧的分辨率与缓冲大小。 */
	ctx->dst_fmt.width = ctx->src_fmt.width;
	ctx->dst_fmt.height = ctx->src_fmt.height;
	if (ctx->vpu_src_fmt->codec_mode == TH1520_MODE_VP9_DEC) {
		ctx->dst_fmt.width = ALIGN(ctx->dst_fmt.width, TH1520_MB_DIM);
		ctx->dst_fmt.height = ALIGN(ctx->dst_fmt.height, TH1520_MB_DIM);
	}
	ctx->dst_fmt.colorspace = ctx->src_fmt.colorspace;
	ctx->dst_fmt.xfer_func = ctx->src_fmt.xfer_func;
	ctx->dst_fmt.ycbcr_enc = ctx->src_fmt.ycbcr_enc;
	ctx->dst_fmt.quantization = ctx->src_fmt.quantization;
	th1520_vdec_fill_pixfmt_cap(&ctx->dst_fmt);

	return 0;
}

static int th1520_vdec_s_fmt_cap(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct th1520_vdec_ctx *ctx = fh_to_ctx(priv);
	struct vb2_queue *vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ret = th1520_vdec_try_fmt(ctx, f, true);
	if (ret)
		return ret;

	ctx->vpu_dst_fmt = th1520_vdec_find_fmt(f->fmt.pix_mp.pixelformat);
	ctx->dst_fmt = f->fmt.pix_mp;

	return 0;
}

static int th1520_vdec_g_selection(struct file *file, void *priv,
				   struct v4l2_selection *sel)
{
	struct th1520_vdec_ctx *ctx = fh_to_ctx(priv);

	if (sel->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = ctx->src_fmt.width;
		sel->r.height = ctx->src_fmt.height;
		return 0;
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_PADDED:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = ctx->dst_fmt.width;
		sel->r.height = ctx->dst_fmt.height;
		return 0;
	default:
		return -EINVAL;
	}
}

const struct v4l2_ioctl_ops th1520_vdec_ioctl_ops = {
	.vidioc_querycap = th1520_vdec_querycap,
	.vidioc_enum_framesizes = th1520_vdec_enum_framesizes,

	.vidioc_enum_fmt_vid_cap = th1520_vdec_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out = th1520_vdec_enum_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = th1520_vdec_g_fmt_cap,
	.vidioc_g_fmt_vid_out_mplane = th1520_vdec_g_fmt_out,
	.vidioc_try_fmt_vid_cap_mplane = th1520_vdec_try_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = th1520_vdec_try_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = th1520_vdec_s_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane = th1520_vdec_s_fmt_out,

	.vidioc_g_selection = th1520_vdec_g_selection,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_stateless_try_decoder_cmd,
	.vidioc_decoder_cmd = v4l2_m2m_ioctl_stateless_decoder_cmd,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ---------------------------- vb2 队列 -------------------------------- */

static int th1520_vdec_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
				   unsigned int *num_planes, unsigned int sizes[],
				   struct device *alloc_devs[])
{
	struct th1520_vdec_ctx *ctx = vb2_get_drv_priv(vq);
	const struct v4l2_pix_format_mplane *pix_mp;

	pix_mp = V4L2_TYPE_IS_OUTPUT(vq->type) ? &ctx->src_fmt : &ctx->dst_fmt;

	if (*num_planes) {
		if (*num_planes != 1)
			return -EINVAL;
		if (sizes[0] < pix_mp->plane_fmt[0].sizeimage)
			return -EINVAL;
		return 0;
	}

	*num_planes = 1;
	sizes[0] = pix_mp->plane_fmt[0].sizeimage;
	return 0;
}

static int th1520_vdec_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct th1520_vdec_ctx *ctx = vb2_get_drv_priv(vq);
	struct th1520_vdec_buffer *buf =
		th1520_vdec_vbuf_to_buffer(to_vb2_v4l2_buffer(vb));
	const struct v4l2_pix_format_mplane *pix_mp;

	pix_mp = V4L2_TYPE_IS_OUTPUT(vq->type) ? &ctx->src_fmt : &ctx->dst_fmt;

	if (vb2_plane_size(vb, 0) < pix_mp->plane_fmt[0].sizeimage)
		return -EINVAL;

	if (!V4L2_TYPE_IS_OUTPUT(vq->type)) {
		if (!buf->native.cpu) {
			size_t size;

			switch (ctx->vpu_src_fmt->codec_mode) {
			case TH1520_MODE_H264_DEC:
				size = th1520_vdec_h264_native_size(ctx);
				break;
			case TH1520_MODE_HEVC_DEC:
				size = th1520_vdec_hevc_native_size(ctx);
				break;
			case TH1520_MODE_VP9_DEC:
				size = th1520_vdec_vp9_native_size(ctx);
				break;
			default:
				return -EINVAL;
			}

			buf->native.cpu = dma_alloc_coherent(ctx->dev->dev, size,
						     &buf->native.dma, GFP_KERNEL);
			if (!buf->native.cpu)
				return -ENOMEM;
			buf->native.size = size;
		}

		buf->vp9.valid = false;
		vb2_set_plane_payload(vb, 0, th1520_vdec_nv12_size(pix_mp));
	}

	return 0;
}

static void th1520_vdec_buf_cleanup(struct vb2_buffer *vb)
{
	struct th1520_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct th1520_vdec_buffer *buf =
		th1520_vdec_vbuf_to_buffer(to_vb2_v4l2_buffer(vb));

	if (buf->native.cpu) {
		dma_free_coherent(ctx->dev->dev, buf->native.size,
				  buf->native.cpu, buf->native.dma);
		memset(&buf->native, 0, sizeof(buf->native));
	}
	memset(&buf->vp9, 0, sizeof(buf->vp9));
}

static void th1520_vdec_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct th1520_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void th1520_vdec_buf_request_complete(struct vb2_buffer *vb)
{
	struct th1520_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->ctrl_handler);
}

static int th1520_vdec_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}

static int th1520_vdec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct th1520_vdec_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *buf;
	int ret = 0;

	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, q);

	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		ctx->sequence_out = 0;

		/*
		 * codec 私有资源绑定在承载码流的 OUTPUT 队列上：
		 * 此时 OUTPUT 格式（决定用哪个 codec 后端）与 CAPTURE 分辨率
		 * 都已经由 S_FMT 确定。
		 */
		if (ctx->codec_ops && ctx->codec_ops->init) {
			ret = ctx->codec_ops->init(ctx);
			if (ret)
				goto err_return_bufs;
		}
	} else {
		ctx->sequence_cap = 0;
	}

	return 0;

err_return_bufs:
	/* 保留 request，缓冲区以 QUEUED 状态等待再次 STREAMON。 */
	while ((buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
		v4l2_m2m_buf_done(buf, VB2_BUF_STATE_QUEUED);

	return ret;
}

static void th1520_vdec_stop_streaming(struct vb2_queue *q)
{
	struct th1520_vdec_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *buf;
	struct vb2_queue *cap_q = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	unsigned int i;

	/* STREAMOFF invalidates both VP9 probabilities and reference contents. */
	if (ctx->codec_ops && ctx->codec_ops->abort)
		ctx->codec_ops->abort(ctx);
	for (i = 0; i < cap_q->num_buffers; i++) {
		struct vb2_buffer *vb = vb2_get_buffer(cap_q, i);

		if (vb)
			th1520_vdec_vbuf_to_buffer(to_vb2_v4l2_buffer(vb))->vp9.valid = false;
	}

	if (V4L2_TYPE_IS_OUTPUT(q->type) && ctx->codec_ops &&
	    ctx->codec_ops->exit)
		ctx->codec_ops->exit(ctx);

	/*
	 * m2m 框架在调用 .stop_streaming 之前已经 v4l2_m2m_cancel_job()，
	 * 此时没有 job 在跑，可以安全地把缓冲全部还回去。
	 */
	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(q->type))
			buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			buf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!buf)
			break;

		v4l2_ctrl_request_complete(buf->vb2_buf.req_obj.req,
					   &ctx->ctrl_handler);
		v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
	}

	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, q);

	if (V4L2_TYPE_IS_OUTPUT(q->type) &&
	    v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		v4l2_event_queue_fh(&ctx->fh, &th1520_vdec_eos_event);
}

static const struct vb2_ops th1520_vdec_queue_ops = {
	.queue_setup = th1520_vdec_queue_setup,
	.buf_prepare = th1520_vdec_buf_prepare,
	.buf_cleanup = th1520_vdec_buf_cleanup,
	.buf_queue = th1520_vdec_buf_queue,
	.buf_out_validate = th1520_vdec_buf_out_validate,
	.buf_request_complete = th1520_vdec_buf_request_complete,
	.start_streaming = th1520_vdec_start_streaming,
	.stop_streaming = th1520_vdec_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

int th1520_vdec_queue_init(void *priv, struct vb2_queue *src_vq,
			   struct vb2_queue *dst_vq)
{
	struct th1520_vdec_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &th1520_vdec_queue_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->buf_struct_size = sizeof(struct th1520_vdec_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->vpu_mutex;
	src_vq->dev = ctx->dev->v4l2_dev.dev;
	src_vq->supports_requests = true;
	src_vq->requires_requests = true;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	/* CAPTURE 帧还会作为 DPB 参考帧被硬件读取。 */
	dst_vq->bidirectional = true;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &th1520_vdec_queue_ops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->buf_struct_size = sizeof(struct th1520_vdec_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->vpu_mutex;
	dst_vq->dev = ctx->dev->v4l2_dev.dev;

	return vb2_queue_init(dst_vq);
}

/* ------------------------------ 控件 ---------------------------------- */

static int th1520_vdec_try_ctrl(struct v4l2_ctrl *ctrl)
{
	switch (ctrl->id) {
	case V4L2_CID_STATELESS_H264_SPS: {
		const struct v4l2_ctrl_h264_sps *sps = ctrl->p_new.p_h264_sps;

		/* 本驱动只实现 8 bit 4:2:0（DEC_MODE = 0）。 */
		if (sps->chroma_format_idc != 1)
			return -EINVAL;
		if (sps->bit_depth_luma_minus8 != 0 ||
		    sps->bit_depth_chroma_minus8 != 0)
			return -EINVAL;
		if (sps->flags & V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS)
			return -EINVAL;
		break;
	}
	case V4L2_CID_STATELESS_HEVC_SPS: {
		const struct v4l2_ctrl_hevc_sps *sps = ctrl->p_new.p_hevc_sps;

		if (sps->chroma_format_idc != 1)
			return -EINVAL;
		/*
		 * BIT_DEPTH_Y/C_MINUS8 只有 2 bit（swreg8[7:6]/[5:4]），
		 * 硬件本身支持到 10/12 bit，但本驱动的 CAPTURE 只导出 8 bit NV12。
		 */
		if (sps->bit_depth_luma_minus8 != 0 ||
		    sps->bit_depth_chroma_minus8 != 0)
			return -EINVAL;
		break;
	}
	case V4L2_CID_STATELESS_VP9_FRAME: {
		const struct v4l2_ctrl_vp9_frame *frame = ctrl->p_new.p_vp9_frame;
		u32 width = frame->frame_width_minus_1 + 1;
		u32 height = frame->frame_height_minus_1 + 1;
		u32 subsampling = V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING |
				  V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;

		if (frame->profile != 0 || frame->bit_depth != 8 ||
		    (frame->flags & subsampling) != subsampling)
			return -EINVAL;
		if (width < TH1520_MIN_WIDTH || width > TH1520_MAX_WIDTH ||
		    height < TH1520_MIN_HEIGHT || height > TH1520_MAX_HEIGHT ||
		    (width & 1) || (height & 1))
			return -EINVAL;
		break;
	}
	default:
		break;
	}

	return 0;
}

static const struct v4l2_ctrl_ops th1520_vdec_ctrl_ops = {
	.try_ctrl = th1520_vdec_try_ctrl,
};

struct th1520_vdec_ctrl_desc {
	u32 codec;
	struct v4l2_ctrl_config cfg;
};

/*
 * Linux 6.6 的 v4l2_ctrl_config.p_def 支持复合控件默认值。
 * 默认 SPS 与初始格式一致，并满足本驱动的 8 bit 4:2:0 限制，
 * G_EXT_CTRLS 返回的默认值因此可以再次通过 TRY_EXT_CTRLS。
 */
static const struct v4l2_ctrl_h264_sps th1520_vdec_h264_default_sps = {
	.profile_idc = 66,
	.constraint_set_flags = V4L2_H264_SPS_CONSTRAINT_SET0_FLAG,
	.level_idc = 40,
	.chroma_format_idc = 1,
	.pic_order_cnt_type = 2,
	.max_num_ref_frames = 1,
	.pic_width_in_mbs_minus1 = TH1520_DEFAULT_WIDTH / TH1520_MB_DIM - 1,
	.pic_height_in_map_units_minus1 = TH1520_DEFAULT_HEIGHT / TH1520_MB_DIM - 1,
	.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY |
		 V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE,
};

static const struct v4l2_ctrl_hevc_sps th1520_vdec_hevc_default_sps = {
	.pic_width_in_luma_samples = TH1520_DEFAULT_WIDTH,
	.pic_height_in_luma_samples = TH1520_DEFAULT_HEIGHT,
	.chroma_format_idc = 1,
	.log2_max_pic_order_cnt_lsb_minus4 = 4,
	.log2_diff_max_min_luma_coding_block_size = 3,
	.log2_diff_max_min_luma_transform_block_size = 3,
};

static const struct v4l2_ctrl_vp9_frame th1520_vdec_vp9_default_frame = {
	.flags = V4L2_VP9_FRAME_FLAG_KEY_FRAME |
		 V4L2_VP9_FRAME_FLAG_SHOW_FRAME |
		 V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING |
		 V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING,
	.frame_width_minus_1 = TH1520_DEFAULT_WIDTH - 1,
	.frame_height_minus_1 = TH1520_DEFAULT_HEIGHT - 1,
	.render_width_minus_1 = TH1520_DEFAULT_WIDTH - 1,
	.render_height_minus_1 = TH1520_DEFAULT_HEIGHT - 1,
	.bit_depth = 8,
};

/*
 * stateless 解码器要求的控件集合，见
 * Documentation/userspace-api/media/v4l/ext-ctrls-codec-stateless.rst。
 */
static const struct th1520_vdec_ctrl_desc th1520_vdec_ctrls[] = {
	/* --- H.264 --- */
	{
		.codec = TH1520_MODE_H264_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS },
	}, {
		.codec = TH1520_MODE_H264_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_H264_SPS,
			 .ops = &th1520_vdec_ctrl_ops,
			 .p_def = { .p_const = &th1520_vdec_h264_default_sps } },
	}, {
		.codec = TH1520_MODE_H264_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_H264_PPS },
	}, {
		.codec = TH1520_MODE_H264_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_H264_SCALING_MATRIX },
	}, {
		.codec = TH1520_MODE_H264_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_H264_DECODE_MODE,
			 .min = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
			 .max = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
			 .def = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED },
	}, {
		.codec = TH1520_MODE_H264_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_H264_START_CODE,
			 .min = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
			 .max = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
			 .def = V4L2_STATELESS_H264_START_CODE_ANNEX_B },
	}, {
		.codec = TH1520_MODE_H264_DEC,
		.cfg = { .id = V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			 .min = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE,
			 .max = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
			 .def = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN,
			 .menu_skip_mask =
				BIT(V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED) },
	}, {
		.codec = TH1520_MODE_H264_DEC,
		.cfg = { .id = V4L2_CID_MPEG_VIDEO_H264_LEVEL,
			 .min = V4L2_MPEG_VIDEO_H264_LEVEL_1_0,
			 .max = V4L2_MPEG_VIDEO_H264_LEVEL_5_1,
			 .def = V4L2_MPEG_VIDEO_H264_LEVEL_4_0 },
	},
	/* --- HEVC --- */
	{
		.codec = TH1520_MODE_HEVC_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_HEVC_SPS,
			 .ops = &th1520_vdec_ctrl_ops,
			 .p_def = { .p_const = &th1520_vdec_hevc_default_sps } },
	}, {
		.codec = TH1520_MODE_HEVC_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_HEVC_PPS },
	}, {
		.codec = TH1520_MODE_HEVC_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS },
	}, {
		.codec = TH1520_MODE_HEVC_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX },
	}, {
		.codec = TH1520_MODE_HEVC_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_HEVC_DECODE_MODE,
			 .min = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
			 .max = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
			 .def = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED },
	}, {
		.codec = TH1520_MODE_HEVC_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_HEVC_START_CODE,
			 .min = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
			 .max = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
			 .def = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B },
	}, {
		.codec = TH1520_MODE_HEVC_DEC,
		.cfg = { .id = V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
			 .min = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN,
			 .max = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE,
			 .def = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN },
	}, {
		.codec = TH1520_MODE_HEVC_DEC,
		.cfg = { .id = V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
			 .min = V4L2_MPEG_VIDEO_HEVC_LEVEL_1,
			 .max = V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1,
			 .def = V4L2_MPEG_VIDEO_HEVC_LEVEL_4 },
	},
	/* --- VP9: parsed frame and compressed header, one frame per request. --- */
	{
		.codec = TH1520_MODE_VP9_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_VP9_FRAME,
			 .ops = &th1520_vdec_ctrl_ops,
			 .p_def = { .p_const = &th1520_vdec_vp9_default_frame } },
	}, {
		.codec = TH1520_MODE_VP9_DEC,
		.cfg = { .id = V4L2_CID_STATELESS_VP9_COMPRESSED_HDR },
	}, {
		.codec = TH1520_MODE_VP9_DEC,
		.cfg = { .id = V4L2_CID_MPEG_VIDEO_VP9_PROFILE,
			 .min = V4L2_MPEG_VIDEO_VP9_PROFILE_0,
			 .max = V4L2_MPEG_VIDEO_VP9_PROFILE_0,
			 .def = V4L2_MPEG_VIDEO_VP9_PROFILE_0 },
	},
};

int th1520_vdec_ctrls_setup(struct th1520_vdec_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->ctrl_handler;
	unsigned int i;
	int ret;

	ret = v4l2_ctrl_handler_init(hdl, ARRAY_SIZE(th1520_vdec_ctrls));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(th1520_vdec_ctrls); i++) {
		/*
		 * v4l2_ctrl_new_custom() 在 cfg->type 为 0 时会用
		 * v4l2_ctrl_fill() 补齐控件类型/名称，再用 cfg 里给出的
		 * min/max/def 覆盖，因此复合控件和标准菜单控件可以统一走这里
		 * （与上游 hantro_ctrls_setup() 相同）。
		 */
		v4l2_ctrl_new_custom(hdl, &th1520_vdec_ctrls[i].cfg, NULL);
		if (hdl->error) {
			ret = hdl->error;
			goto err_free;
		}
	}

	ret = v4l2_ctrl_handler_setup(hdl);
	if (ret)
		goto err_free;

	return 0;

err_free:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

void th1520_vdec_reset_fmts(struct th1520_vdec_ctx *ctx)
{
	struct v4l2_format f;

	/* 默认 OUTPUT 格式：HEVC slice，1920x1088。 */
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC_SLICE;
	f.fmt.pix_mp.width = TH1520_DEFAULT_WIDTH;
	f.fmt.pix_mp.height = TH1520_DEFAULT_HEIGHT;
	f.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	th1520_vdec_s_fmt_out(NULL, &ctx->fh, &f);

	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	th1520_vdec_s_fmt_cap(NULL, &ctx->fh, &f);
}
