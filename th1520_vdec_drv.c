// SPDX-License-Identifier: GPL-2.0-only
/*
 * TH1520 VC8000D V4L2 stateless decoder platform support.
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * Framework portions adapted from the Linux Hantro driver:
 * Copyright (C) 2018 Collabora, Ltd.
 * Copyright 2018 Google LLC.
 *     Tomasz Figa <tfiga@chromium.org>
 * Request validation also follows the Linux vicodec driver:
 * Copyright 2018 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 * Based on the s5p-mfc driver:
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 *
 * Linux commit 8ba098e6b6ff0db8edf28528d1552be261af30d4.
 * See docs/sources.md for public upstream links and docs/hardware.md for
 * the platform resources verified on the supported TH1520 board.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include <media/media-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "th1520_vdec.h"

/*
 * Clock names used by the supported TH1520 device tree:
 * cclk: decoder core; aclk: AXI bus; pclk: APB register interface.
 */
static const char * const th1520_vdec_clk_names[TH1520_VDEC_NUM_CLOCKS] = {
	"cclk", "aclk", "pclk",
};

void *th1520_vdec_get_ctrl(struct th1520_vdec_ctx *ctx, u32 id)
{
	struct v4l2_ctrl *ctrl;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_handler, id);
	return ctrl ? ctrl->p_cur.p : NULL;
}

dma_addr_t th1520_vdec_get_ref(struct th1520_vdec_ctx *ctx, u64 ts)
{
	struct vb2_queue *q = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	struct th1520_vdec_buffer *capture;
	struct vb2_buffer *buf;

	buf = vb2_find_buffer(q, ts);
	if (!buf)
		return 0;

	capture = th1520_vdec_vbuf_to_buffer(to_vb2_v4l2_buffer(buf));
	return capture->native.cpu ? capture->native.dma : 0;
}

const struct v4l2_event th1520_vdec_eos_event = {
	.type = V4L2_EVENT_EOS,
};

static void th1520_vdec_job_finish_no_pm(struct th1520_vdec_dev *vpu,
					 struct th1520_vdec_ctx *ctx,
					 enum vb2_buffer_state result)
{
	struct vb2_v4l2_buffer *src, *dst;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	if (WARN_ON(!src) || WARN_ON(!dst))
		return;

	if (result == VB2_BUF_STATE_ERROR && ctx->codec_ops->abort)
		ctx->codec_ops->abort(ctx);

	src->sequence = ctx->sequence_out++;
	dst->sequence = ctx->sequence_cap++;

	if (v4l2_m2m_is_last_draining_src_buf(ctx->fh.m2m_ctx, src)) {
		dst->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_event_queue_fh(&ctx->fh, &th1520_vdec_eos_event);
		v4l2_m2m_mark_stopped(ctx->fh.m2m_ctx);
	}

	v4l2_m2m_buf_done_and_job_finish(vpu->m2m_dev, ctx->fh.m2m_ctx, result);
}

static void th1520_vdec_job_finish(struct th1520_vdec_dev *vpu,
				   struct th1520_vdec_ctx *ctx,
				   enum vb2_buffer_state result)
{
	clk_bulk_disable(TH1520_VDEC_NUM_CLOCKS, vpu->clocks);
	pm_runtime_mark_last_busy(vpu->dev);
	pm_runtime_put_autosuspend(vpu->dev);

	th1520_vdec_job_finish_no_pm(vpu, ctx, result);
}

void th1520_vdec_irq_done(struct th1520_vdec_dev *vpu,
			  struct th1520_vdec_ctx *ctx,
			  enum vb2_buffer_state result)
{
	/*
	 * The IRQ already claimed this context under irqlock.  A watchdog
	 * worker may be running, but it cannot claim the same job.  Cancelling
	 * a pending timer is cleanup, not the decision about who completes it.
	 */
	cancel_delayed_work(&vpu->watchdog_work);
	if (result == VB2_BUF_STATE_DONE && ctx->codec_ops->done)
		ctx->codec_ops->done(ctx);
	th1520_vdec_job_finish(vpu, ctx, result);
}

void th1520_vdec_watchdog(struct work_struct *work)
{
	struct th1520_vdec_dev *vpu;
	struct th1520_vdec_ctx *ctx;
	unsigned long flags, now;

	vpu = container_of(to_delayed_work(work), struct th1520_vdec_dev,
			   watchdog_work);
	spin_lock_irqsave(&vpu->irqlock, flags);
	ctx = vpu->active_ctx;
	if (!ctx) {
		spin_unlock_irqrestore(&vpu->irqlock, flags);
		return;
	}

	now = jiffies;
	if (time_before(now, vpu->watchdog_deadline)) {
		/* An older worker may enter after the next job was armed. */
		mod_delayed_work(system_wq, &vpu->watchdog_work,
				 vpu->watchdog_deadline - now);
		spin_unlock_irqrestore(&vpu->irqlock, flags);
		return;
	}
	vpu->active_ctx = NULL;
	spin_unlock_irqrestore(&vpu->irqlock, flags);

	dev_err(vpu->dev, "frame processing timed out\n");

	/* 时钟这时还开着，可以安全回读硬件寄存器。 */
	th1520_vdec_dump_regs(vpu, "timeout");

	if (ctx->codec_ops->reset)
		ctx->codec_ops->reset(ctx);

	th1520_vdec_job_finish(vpu, ctx, VB2_BUF_STATE_ERROR);
}

void th1520_vdec_start_prepare_run(struct th1520_vdec_ctx *ctx)
{
	struct vb2_v4l2_buffer *src_buf = th1520_vdec_get_src_buf(ctx);

	v4l2_ctrl_request_setup(src_buf->vb2_buf.req_obj.req,
				&ctx->ctrl_handler);

	/*
	 * 每一帧都从全零的影子寄存器开始重建配置。
	 * 这样每个被写入的位都由本次配置决定，不依赖硬件复位值或上一帧残留
	 * （复位值在 analysis 中属于“待硬件验证”项，不能作为依据）。
	 */
	th1520_vdec_regs_reset(ctx->dev);
}

void th1520_vdec_end_prepare_run(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src_buf = th1520_vdec_get_src_buf(ctx);
	unsigned long timeout = msecs_to_jiffies(TH1520_VDEC_TIMEOUT_MS);
	unsigned long flags;

	v4l2_ctrl_request_complete(src_buf->vb2_buf.req_obj.req,
				   &ctx->ctrl_handler);

	spin_lock_irqsave(&vpu->irqlock, flags);
	vpu->active_ctx = ctx;
	vpu->watchdog_deadline = jiffies + timeout;
	/* Re-arm even when an earlier invocation is still returning. */
	mod_delayed_work(system_wq, &vpu->watchdog_work, timeout);
	spin_unlock_irqrestore(&vpu->irqlock, flags);
}

static void th1520_vdec_device_run(void *priv)
{
	struct th1520_vdec_ctx *ctx = priv;
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src, *dst;
	bool submitted = false;
	int ret;

	mutex_lock(&vpu->run_mutex);
	src = th1520_vdec_get_src_buf(ctx);
	dst = th1520_vdec_get_dst_buf(ctx);
	if (WARN_ON(!src) || WARN_ON(!dst))
		goto err_cancel_job;

	ret = pm_runtime_resume_and_get(vpu->dev);
	if (ret < 0) {
		dev_err(vpu->dev, "failed to resume: %d\n", ret);
		goto err_cancel_job;
	}

	ret = clk_bulk_enable(TH1520_VDEC_NUM_CLOCKS, vpu->clocks);
	if (ret) {
		dev_err(vpu->dev, "failed to enable clocks: %d\n", ret);
		pm_runtime_put_noidle(vpu->dev);
		goto err_cancel_job;
	}

	/*
	 * 6.6 的 v4l2_m2m_buf_copy_metadata() 有第三个参数 copy_frame_flags；
	 * 较新的内核把它去掉了（恒为拷贝）。解码器需要把 KEY/P/B 帧标志
	 * 一并带到 CAPTURE buffer，所以传 true。
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	v4l2_m2m_buf_copy_metadata(src, dst, true);
#else
	v4l2_m2m_buf_copy_metadata(src, dst);
#endif

	submitted = true;
	if (ctx->codec_ops->run(ctx)) {
		clk_bulk_disable(TH1520_VDEC_NUM_CLOCKS, vpu->clocks);
		pm_runtime_mark_last_busy(vpu->dev);
		pm_runtime_put_autosuspend(vpu->dev);
		goto err_cancel_job;
	}

	mutex_unlock(&vpu->run_mutex);
	return;

err_cancel_job:
	/* Codec run() completes its controls; cover failures before entering it. */
	if (!submitted && src)
		v4l2_ctrl_request_complete(src->vb2_buf.req_obj.req,
					   &ctx->ctrl_handler);
	th1520_vdec_job_finish_no_pm(vpu, ctx, VB2_BUF_STATE_ERROR);
	mutex_unlock(&vpu->run_mutex);
}

static void th1520_vdec_job_abort(void *priv)
{
	struct th1520_vdec_ctx *ctx = priv;
	struct th1520_vdec_dev *vpu = ctx->dev;
	unsigned long flags;
	bool claimed;

	mutex_lock(&vpu->run_mutex);
	spin_lock_irqsave(&vpu->irqlock, flags);
	claimed = vpu->active_ctx == ctx;
	if (claimed)
		vpu->active_ctx = NULL;
	spin_unlock_irqrestore(&vpu->irqlock, flags);

	if (claimed) {
		cancel_delayed_work(&vpu->watchdog_work);
		if (ctx->codec_ops->reset)
			ctx->codec_ops->reset(ctx);
		th1520_vdec_job_finish(vpu, ctx, VB2_BUF_STATE_ERROR);
	}
	/* If IRQ or watchdog claimed the job, the M2M core waits for it. */
	mutex_unlock(&vpu->run_mutex);
}

static const struct v4l2_m2m_ops th1520_vdec_m2m_ops = {
	.device_run = th1520_vdec_device_run,
	.job_abort = th1520_vdec_job_abort,
};

/* ---------------------------------------------------------------------- */

static int th1520_vdec_open(struct file *file)
{
	struct th1520_vdec_dev *vpu = video_drvdata(file);
	struct th1520_vdec_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = vpu;
	ctx->bit_depth = 8;

	if (mutex_lock_interruptible(&vpu->vpu_mutex)) {
		ret = -ERESTARTSYS;
		goto err_free;
	}

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(vpu->m2m_dev, ctx,
					    th1520_vdec_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_fh_exit;
	}

	th1520_vdec_reset_fmts(ctx);

	ret = th1520_vdec_ctrls_setup(ctx);
	if (ret)
		goto err_m2m_release;

	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	v4l2_fh_add(&ctx->fh);

	mutex_unlock(&vpu->vpu_mutex);
	return 0;

err_m2m_release:
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
err_fh_exit:
	v4l2_fh_exit(&ctx->fh);
	mutex_unlock(&vpu->vpu_mutex);
err_free:
	kfree(ctx);
	return ret;
}

static int th1520_vdec_release(struct file *file)
{
	struct th1520_vdec_ctx *ctx = fh_to_ctx(file->private_data);
	struct th1520_vdec_dev *vpu = ctx->dev;

	mutex_lock(&vpu->vpu_mutex);
	v4l2_fh_del(&ctx->fh);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_exit(&ctx->fh);
	mutex_unlock(&vpu->vpu_mutex);

	kfree(ctx);
	return 0;
}

const struct v4l2_file_operations th1520_vdec_fops = {
	.owner = THIS_MODULE,
	.open = th1520_vdec_open,
	.release = th1520_vdec_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

/* ---------------------------------------------------------------------- */

static int th1520_vdec_request_validate(struct media_request *req)
{
	struct media_request_object *obj;
	struct th1520_vdec_ctx *ctx = NULL;
	struct v4l2_ctrl_handler *hdl;
	unsigned int count = vb2_request_buffer_cnt(req);
	bool has_frame, has_compressed;

	if (!count)
		return -ENOENT;
	if (count != 1)
		return -EINVAL;

	list_for_each_entry(obj, &req->objects, list) {
		struct vb2_buffer *vb;

		if (!vb2_request_object_is_buffer(obj))
			continue;
		vb = container_of(obj, struct vb2_buffer, req_obj);
		if (!V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type))
			return -EINVAL;
		ctx = vb2_get_drv_priv(vb->vb2_queue);
		break;
	}
	if (!ctx)
		return -ENOENT;

	if (ctx->vpu_src_fmt->codec_mode == TH1520_MODE_VP9_DEC) {
		/* Presence is per request; a previous frame's controls cannot carry. */
		hdl = v4l2_ctrl_request_hdl_find(req, &ctx->ctrl_handler);
		if (!hdl)
			return -ENOENT;
		has_frame = v4l2_ctrl_request_hdl_ctrl_find(hdl,
					 V4L2_CID_STATELESS_VP9_FRAME) != NULL;
		has_compressed = v4l2_ctrl_request_hdl_ctrl_find(hdl,
					 V4L2_CID_STATELESS_VP9_COMPRESSED_HDR) != NULL;
		v4l2_ctrl_request_hdl_put(hdl);
		if (!has_frame || !has_compressed)
			return -ENOENT;
	}

	return vb2_request_validate(req);
}

static const struct media_device_ops th1520_vdec_media_ops = {
	.req_validate = th1520_vdec_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static int th1520_vdec_v4l2_init(struct th1520_vdec_dev *vpu)
{
	struct video_device *vfd;
	int ret;

	ret = v4l2_device_register(vpu->dev, &vpu->v4l2_dev);
	if (ret)
		return ret;

	vpu->m2m_dev = v4l2_m2m_init(&th1520_vdec_m2m_ops);
	if (IS_ERR(vpu->m2m_dev)) {
		ret = PTR_ERR(vpu->m2m_dev);
		dev_err(vpu->dev, "failed to init mem2mem device: %d\n", ret);
		goto err_v4l2_unreg;
	}

	vpu->mdev.dev = vpu->dev;
	strscpy(vpu->mdev.model, TH1520_VDEC_NAME, sizeof(vpu->mdev.model));
	snprintf(vpu->mdev.bus_info, sizeof(vpu->mdev.bus_info),
		 "platform:%s", dev_name(vpu->dev));
	media_device_init(&vpu->mdev);
	vpu->mdev.ops = &th1520_vdec_media_ops;
	vpu->v4l2_dev.mdev = &vpu->mdev;

	vfd = video_device_alloc();
	if (!vfd) {
		ret = -ENOMEM;
		goto err_m2m_release;
	}

	vfd->fops = &th1520_vdec_fops;
	vfd->release = video_device_release;
	vfd->lock = &vpu->vpu_mutex;
	vfd->v4l2_dev = &vpu->v4l2_dev;
	vfd->vfl_dir = VFL_DIR_M2M;
	vfd->device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M_MPLANE;
	vfd->ioctl_ops = &th1520_vdec_ioctl_ops;
	strscpy(vfd->name, TH1520_VDEC_NAME "-dec", sizeof(vfd->name));

	vpu->vfd = vfd;
	video_set_drvdata(vfd, vpu);

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(vpu->dev, "failed to register video device: %d\n", ret);
		goto err_vfd_release;
	}

	ret = v4l2_m2m_register_media_controller(vpu->m2m_dev, vfd,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret) {
		dev_err(vpu->dev, "failed to init media controller: %d\n", ret);
		goto err_vfd_unreg;
	}

	ret = media_device_register(&vpu->mdev);
	if (ret) {
		dev_err(vpu->dev, "failed to register media device: %d\n", ret);
		goto err_m2m_mc_unreg;
	}

	dev_info(vpu->dev, "registered %s as /dev/video%d\n", vfd->name,
		 vfd->num);
	return 0;

err_m2m_mc_unreg:
	v4l2_m2m_unregister_media_controller(vpu->m2m_dev);
err_vfd_unreg:
	video_unregister_device(vfd);
	vpu->vfd = NULL;
	goto err_m2m_release;
err_vfd_release:
	video_device_release(vfd);
	vpu->vfd = NULL;
err_m2m_release:
	media_device_cleanup(&vpu->mdev);
	v4l2_m2m_release(vpu->m2m_dev);
err_v4l2_unreg:
	v4l2_device_unregister(&vpu->v4l2_dev);
	return ret;
}

static void th1520_vdec_v4l2_cleanup(struct th1520_vdec_dev *vpu)
{
	media_device_unregister(&vpu->mdev);
	v4l2_m2m_unregister_media_controller(vpu->m2m_dev);
	if (vpu->vfd)
		video_unregister_device(vpu->vfd);
	media_device_cleanup(&vpu->mdev);
	v4l2_m2m_release(vpu->m2m_dev);
	v4l2_device_unregister(&vpu->v4l2_dev);
}

/* ---------------------------------------------------------------------- */

/*
 * The upper half of the read-only ASIC identifier selects product 0x8001.
 * The separate build identifier is read from register 309.
 */
#define TH1520_VDEC_HW_ID_VC8000D	0x8001

/*
 * The subsystem MMU starts at offset 0x3000. Its identifier is at word 6
 * and its enable bit is at word 226. This driver uses physical DMA
 * addresses, so the MMU must remain disabled while decoding.
 */
#define TH1520_VDEC_MMU_OFFSET		0x3000
#define TH1520_VDEC_MMU_IOSIZE		(229 * 4)
#define TH1520_MMU_REG_HW_ID		(6 * 4)
#define TH1520_MMU_REG_CONTROL		(226 * 4)

static void th1520_vdec_bypass_mmu(struct th1520_vdec_dev *vpu,
				   struct resource *res)
{
	void __iomem *mmu;
	u32 hw_id, ctrl;

	if (resource_size(res) < TH1520_VDEC_MMU_OFFSET + TH1520_VDEC_MMU_IOSIZE)
		return;

	mmu = devm_ioremap(vpu->dev, res->start + TH1520_VDEC_MMU_OFFSET,
			   TH1520_VDEC_MMU_IOSIZE);
	if (!mmu) {
		dev_warn(vpu->dev, "cannot map MMU block, skipping bypass\n");
		return;
	}

	hw_id = readl(mmu + TH1520_MMU_REG_HW_ID);
	ctrl = readl(mmu + TH1520_MMU_REG_CONTROL);
	dev_info(vpu->dev, "Hantro MMU hw_id=0x%08x control=0x%08x\n",
		 hw_id, ctrl);

	if (ctrl & 1) {
		dev_warn(vpu->dev,
			 "MMU was enabled (likely left on by hantrodec); disabling for physical addressing\n");
		writel(0, mmu + TH1520_MMU_REG_CONTROL);
		dev_info(vpu->dev, "MMU control now 0x%08x\n",
			 readl(mmu + TH1520_MMU_REG_CONTROL));
	}

	devm_iounmap(vpu->dev, mmu);
}

static int th1520_vdec_probe_hw(struct th1520_vdec_dev *vpu,
				struct resource *res)
{
	u32 hw_id, build_id;
	int ret;

	ret = pm_runtime_resume_and_get(vpu->dev);
	if (ret < 0)
		return ret;

	ret = clk_bulk_enable(TH1520_VDEC_NUM_CLOCKS, vpu->clocks);
	if (ret) {
		pm_runtime_put_noidle(vpu->dev);
		return ret;
	}

	hw_id = vdpu_read(vpu, TH1520_VDEC_REG_OFF(TH1520_VDEC_SWREG_ID));
	/*
	 * Register 309 contains the hardware build identifier.
	 */
	build_id = vdpu_read(vpu, TH1520_VDEC_REG_OFF(309));
	th1520_vdec_bypass_mmu(vpu, res);

	clk_bulk_disable(TH1520_VDEC_NUM_CLOCKS, vpu->clocks);
	pm_runtime_mark_last_busy(vpu->dev);
	pm_runtime_put_autosuspend(vpu->dev);

	dev_info(vpu->dev, "ASIC id 0x%08x (product 0x%04x), build id 0x%08x\n",
		 hw_id, hw_id >> 16, build_id);

	if ((hw_id >> 16) != TH1520_VDEC_HW_ID_VC8000D) {
		dev_err(vpu->dev,
			"unexpected product id 0x%04x, expected 0x%04x — check reg mapping\n",
			hw_id >> 16, TH1520_VDEC_HW_ID_VC8000D);
		return -ENODEV;
	}
	if (build_id != 0x1f88) {
		dev_err(vpu->dev, "unsupported VC8000D build id 0x%08x\n", build_id);
		return -ENODEV;
	}

	return 0;
}

static int th1520_vdec_probe(struct platform_device *pdev)
{
	struct th1520_vdec_dev *vpu;
	struct resource *res;
	int i, ret;

	vpu = devm_kzalloc(&pdev->dev, sizeof(*vpu), GFP_KERNEL);
	if (!vpu)
		return -ENOMEM;

	vpu->dev = &pdev->dev;
	vpu->pdev = pdev;
	mutex_init(&vpu->vpu_mutex);
	mutex_init(&vpu->run_mutex);
	spin_lock_init(&vpu->irqlock);
	INIT_DELAYED_WORK(&vpu->watchdog_work, th1520_vdec_watchdog);
	platform_set_drvdata(pdev, vpu);

	/*
	 * The validated configuration uses 32-bit DMA addresses. Address-pair
	 * upper words are still written explicitly. Wider DMA addressing requires
	 * separate hardware validation.
	 */
	ret = dma_set_mask_and_coherent(vpu->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(vpu->dev, "failed to set DMA mask: %d\n", ret);
		return ret;
	}

	/*
	 * The device-tree resource describes the VPU subsystem. Map the decoder
	 * window at offset 0x1000; the MMU is mapped separately for bypass control.
	 * The supported resource layout is documented in docs/hardware.md.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	if (resource_size(res) < TH1520_VDEC_CORE_OFFSET + TH1520_VDEC_CORE_IOSIZE) {
		dev_err(vpu->dev, "reg window too small (%pa)\n",
			&res->end);
		return -EINVAL;
	}

	vpu->reg_base = devm_ioremap(vpu->dev,
				     res->start + TH1520_VDEC_CORE_OFFSET,
				     TH1520_VDEC_CORE_IOSIZE);
	if (!vpu->reg_base)
		return -ENOMEM;

	for (i = 0; i < TH1520_VDEC_NUM_CLOCKS; i++)
		vpu->clocks[i].id = th1520_vdec_clk_names[i];

	ret = devm_clk_bulk_get(vpu->dev, TH1520_VDEC_NUM_CLOCKS, vpu->clocks);
	if (ret)
		return dev_err_probe(vpu->dev, ret, "failed to get clocks\n");

	vpu->irq = platform_get_irq(pdev, 0);
	if (vpu->irq < 0)
		return vpu->irq;

	ret = devm_request_irq(vpu->dev, vpu->irq, th1520_vdec_irq, 0,
			       dev_name(vpu->dev), vpu);
	if (ret)
		return dev_err_probe(vpu->dev, ret, "failed to request IRQ\n");

	pm_runtime_set_autosuspend_delay(vpu->dev, 100);
	pm_runtime_use_autosuspend(vpu->dev);
	pm_runtime_enable(vpu->dev);

	ret = clk_bulk_prepare(TH1520_VDEC_NUM_CLOCKS, vpu->clocks);
	if (ret)
		goto err_pm_disable;

	ret = th1520_vdec_probe_hw(vpu, res);
	if (ret)
		goto err_clk_unprepare;

	ret = th1520_vdec_v4l2_init(vpu);
	if (ret)
		goto err_clk_unprepare;

	return 0;

err_clk_unprepare:
	clk_bulk_unprepare(TH1520_VDEC_NUM_CLOCKS, vpu->clocks);
err_pm_disable:
	pm_runtime_dont_use_autosuspend(vpu->dev);
	pm_runtime_disable(vpu->dev);
	return ret;
}

static void th1520_vdec_remove(struct platform_device *pdev)
{
	struct th1520_vdec_dev *vpu = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&vpu->watchdog_work);
	th1520_vdec_v4l2_cleanup(vpu);
	clk_bulk_unprepare(TH1520_VDEC_NUM_CLOCKS, vpu->clocks);
	pm_runtime_dont_use_autosuspend(vpu->dev);
	pm_runtime_disable(vpu->dev);
}

static int __maybe_unused th1520_vdec_runtime_suspend(struct device *dev)
{
	/*
	 * 时钟在 device_run()/job_finish() 里成对 enable/disable，
	 * 这里只负责 prepare 之外的电源域，由 genpd 处理。
	 */
	return 0;
}

static int __maybe_unused th1520_vdec_runtime_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops th1520_vdec_pm_ops = {
	SET_RUNTIME_PM_OPS(th1520_vdec_runtime_suspend,
			   th1520_vdec_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

/*
 * Bind the existing TH1520 decoder node. Only one decoder driver may own
 * the device at a time; tools/board-load.sh performs temporary switching.
 * The encoder is a separate device.
 */
static const struct of_device_id th1520_vdec_of_match[] = {
	{ .compatible = "xuantie,th1520-vc8000d" },
	{ .compatible = "thead,light-vc8000d" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, th1520_vdec_of_match);

static struct platform_driver th1520_vdec_driver = {
	.probe = th1520_vdec_probe,
	/*
	 * platform_driver 的移除回调签名随内核版本变化：
	 *   < 6.5   ：只有 .remove，返回 int
	 *   6.5–6.10：.remove_new 返回 void（.remove 已废弃）
	 *   >= 6.11 ：.remove 返回 void，.remove_new 被删除
	 * 目标内核是 6.6，因此默认走 .remove_new。
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	.remove = th1520_vdec_remove,
#else
	.remove_new = th1520_vdec_remove,
#endif
	.driver = {
		.name = TH1520_VDEC_NAME,
		.of_match_table = th1520_vdec_of_match,
		.pm = &th1520_vdec_pm_ops,
		/* An open video fd pins this module; omit unsafe manual unbind. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(th1520_vdec_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TH1520 VC8000D V4L2 stateless decoder driver");
