// SPDX-License-Identifier: GPL-2.0
/*
 * TH1520 VC8000D V4L2 stateless decoder driver — platform glue.
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * 平台资源（compatible / clock / IRQ / reg）来自厂商内核侧资料
 * reference/vpu-vc8000d-kernel/linux/subsys_driver/{hantro_dec.c,subsys.c}，
 * 详见 driver/README.md 的“平台资源证据”一节。
 *
 * 驱动结构参考上游 drivers/media/platform/verisilicon/hantro_drv.c
 * （Linux commit 8ba098e6b6ff0db8edf28528d1552be261af30d4，GPL-2.0）。
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
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
 * 时钟名与使能顺序取自 hantro_dec.c 的 devm_clk_get()/decoder_runtime_resume()：
 *   cclk = 解码核心时钟（同时也是 devfreq 调频对象）
 *   aclk = AXI 总线时钟
 *   pclk = APB 寄存器时钟
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
	struct vb2_buffer *buf;

	buf = vb2_find_buffer(q, ts);
	if (!buf)
		return 0;

	return vb2_dma_contig_plane_dma_addr(buf, 0);
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
	pm_runtime_mark_last_busy(vpu->dev);
	pm_runtime_put_autosuspend(vpu->dev);

	clk_bulk_disable(TH1520_VDEC_NUM_CLOCKS, vpu->clocks);

	th1520_vdec_job_finish_no_pm(vpu, ctx, result);
}

void th1520_vdec_irq_done(struct th1520_vdec_dev *vpu,
			  enum vb2_buffer_state result)
{
	struct th1520_vdec_ctx *ctx = v4l2_m2m_get_curr_priv(vpu->m2m_dev);

	if (!ctx)
		return;

	/*
	 * cancel_delayed_work() 返回 false 表示看门狗已经开跑，
	 * 由看门狗负责结束这个 job，中断路径不能重复结束。
	 */
	if (cancel_delayed_work(&vpu->watchdog_work)) {
		if (result == VB2_BUF_STATE_DONE && ctx->codec_ops->done)
			ctx->codec_ops->done(ctx);
		th1520_vdec_job_finish(vpu, ctx, result);
	}
}

void th1520_vdec_watchdog(struct work_struct *work)
{
	struct th1520_vdec_dev *vpu;
	struct th1520_vdec_ctx *ctx;

	vpu = container_of(to_delayed_work(work), struct th1520_vdec_dev,
			   watchdog_work);
	ctx = v4l2_m2m_get_curr_priv(vpu->m2m_dev);
	if (!ctx)
		return;

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
	struct vb2_v4l2_buffer *src_buf = th1520_vdec_get_src_buf(ctx);

	v4l2_ctrl_request_complete(src_buf->vb2_buf.req_obj.req,
				   &ctx->ctrl_handler);

	schedule_delayed_work(&ctx->dev->watchdog_work,
			      msecs_to_jiffies(TH1520_VDEC_TIMEOUT_MS));
}

static void th1520_vdec_device_run(void *priv)
{
	struct th1520_vdec_ctx *ctx = priv;
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src, *dst;
	int ret;

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

	if (ctx->codec_ops->run(ctx)) {
		clk_bulk_disable(TH1520_VDEC_NUM_CLOCKS, vpu->clocks);
		pm_runtime_mark_last_busy(vpu->dev);
		pm_runtime_put_autosuspend(vpu->dev);
		goto err_cancel_job;
	}

	return;

err_cancel_job:
	th1520_vdec_job_finish_no_pm(vpu, ctx, VB2_BUF_STATE_ERROR);
}

static const struct v4l2_m2m_ops th1520_vdec_m2m_ops = {
	.device_run = th1520_vdec_device_run,
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
	/*
	 * Stateless 解码器要求每个 request 至少携带一个 OUTPUT buffer。
	 * 具体的控件完整性检查由各 codec 后端在 run() 里做。
	 */
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
	strscpy(vpu->mdev.bus_info, "platform:" TH1520_VDEC_NAME,
		sizeof(vpu->mdev.bus_info));
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
 * swreg0 是只读的 HW build id。厂商内核 hantro_dec.c 的 CheckHwId() 用
 *   hw_id = readl(base) >> 16
 * 判定核类型：
 *   IS_G1(0x6731) / IS_G2(0x6732) / IS_VC8000D(0x8001)
 * TH1520 上的解码核应当读出 0x8001。
 *
 * 这是一次纯读操作，不会启动硬件；它同时验证了 reg + 0x1000 的映射是否正确。
 */
#define TH1520_VDEC_HW_ID_VC8000D	0x8001

/*
 * VPU 子系统里的 Hantro MMU 块（子系统基址 + 0x3000）。
 * 寄存器编号取自 reference/vpu-vc8000d-kernel/linux/subsys_driver/hantro_mmu.c：
 *   MMU_REG_HW_ID   = 6*4
 *   MMU_REG_CONTROL = 226*4   （写 1 使能，写 0 关闭）
 *
 * 厂商 hantrodec 的 probe 会调用 MMUInit() + MMUEnable()，也就是说开机后
 * MMU 可能处于**已使能**状态。本驱动用的是 CMA 物理地址、不建页表，
 * 因此 MMU 必须处于旁路（关闭）状态，否则硬件会把物理地址当虚拟地址翻译，
 * 取到的码流和写出的帧全是错的。
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
	u32 hw_id;
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
	th1520_vdec_bypass_mmu(vpu, res);

	clk_bulk_disable(TH1520_VDEC_NUM_CLOCKS, vpu->clocks);
	pm_runtime_mark_last_busy(vpu->dev);
	pm_runtime_put_autosuspend(vpu->dev);

	dev_info(vpu->dev, "HW build id 0x%08x (product 0x%04x)\n",
		 hw_id, hw_id >> 16);

	if ((hw_id >> 16) != TH1520_VDEC_HW_ID_VC8000D) {
		dev_err(vpu->dev,
			"unexpected product id 0x%04x, expected 0x%04x — check reg mapping\n",
			hw_id >> 16, TH1520_VDEC_HW_ID_VC8000D);
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
	spin_lock_init(&vpu->irqlock);
	INIT_DELAYED_WORK(&vpu->watchdog_work, th1520_vdec_watchdog);
	platform_set_drvdata(pdev, vpu);

	/*
	 * 地址位宽：H.264 表里 RLC_VLC_BASE_MSB(swreg122) 存在，
	 * 但二进制中 H264RunAsic 有断言“非 High10 模式下 regs[122] 必须为 0”，
	 * 即 8bit H.264 路径要求码流地址落在 4 GiB 以内。
	 * 在目标板上确认 MSB 寄存器行为之前，这里保守地把 DMA 掩码限制成 32 bit，
	 * 同时驱动仍然显式写 MSB 寄存器（值为 0）。
	 * 放开到 64 bit 属于 README 中的“待硬件验证”项。
	 */
	ret = dma_set_mask_and_coherent(vpu->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(vpu->dev, "failed to set DMA mask: %d\n", ret);
		return ret;
	}

	/*
	 * DT 的 reg 是 **VPU 子系统基址**（板上实测：vdec@ffecc00000，
	 * reg = <0xff 0xecc00000 0x0 0x800000>，8 MiB 窗口），
	 * VC8000D 解码核在子系统基址 + 0x1000，窗口 1023*4 字节。
	 * 该布局来自 reference/vpu-vc8000d-kernel/linux/subsys_driver/subsys.c
	 * 的 core_array[]，并已在 RevyOS 6.6.119-th1520 的实际 DT 上确认。
	 *
	 * 这里只映射解码核本身，不 request 整个 8 MiB 子系统窗口：
	 * 子系统里还有 VCMD / L2CACHE / MMU / DEC400 等其它块，
	 * 本驱动一概不碰。
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
 * compatible 取自 hantro_dec.c 的 isp_of_match[]，并在目标板上确认：
 * RevyOS 6.6.119-th1520 的 DT 节点为
 *   vdec@ffecc00000 { compatible = "xuantie,th1520-vc8000d";
 *                     reg = <0xff 0xecc00000 0x0 0x800000>;
 *                     interrupts = <131 4>;
 *                     clock-names = "aclk", "cclk", "pclk"; ... };
 *
 * 本驱动直接绑定该节点，因此**不需要修改设备树**，但必须先阻止厂商模块
 * 抢占同一个设备（板上默认加载了 hantrodec.ko 与 vc8000.ko）：
 *
 *   echo 'blacklist hantrodec' | sudo tee /etc/modprobe.d/th1520-vdec.conf
 *   echo 'blacklist vc8000'   | sudo tee -a /etc/modprobe.d/th1520-vdec.conf
 *
 * 或调试期间先 rmmod 再手动 bind。
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
	},
};
module_platform_driver(th1520_vdec_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TH1520 VC8000D V4L2 stateless decoder driver");
