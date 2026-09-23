// SPDX-License-Identifier: GPL-2.0-only
/*
 * TH1520 VC8000D register submission, post-processing and interrupts.
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * Each job prepares a complete shadow register block. Configuration
 * registers are submitted before the control register that starts decoding.
 * The read-only ASIC identifier is preserved. Hardware observations and
 * the supported configuration are described in docs/hardware.md.
 */

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <media/videobuf2-dma-contig.h>

#include "th1520_vdec.h"

/*
 * flush 策略，见 struct th1520_vdec_dev.reg_dirty 的说明。
 * 默认提交整个产品寄存器块，覆盖 512 个配置寄存器。
 * 切换codec/context时还必须清除上一帧的PP及参考帧设置。
 */
static bool flush_all = true;
module_param(flush_all, bool, 0644);
MODULE_PARM_DESC(flush_all,
		 "flush the whole shadow register block instead of only registers written this frame");

u32 vdpu_read(struct th1520_vdec_dev *vpu, u32 offset)
{
	return readl(vpu->reg_base + offset);
}

void vdpu_write(struct th1520_vdec_dev *vpu, u32 val, u32 offset)
{
	writel(val, vpu->reg_base + offset);
}

void th1520_vdec_regs_reset(struct th1520_vdec_dev *vpu)
{
	memset(vpu->regs, 0, sizeof(vpu->regs));
	bitmap_zero(vpu->reg_dirty, TH1520_VDEC_REG_COUNT);
}

void th1520_vdec_reg_write(struct th1520_vdec_dev *vpu,
			   const struct th1520_vdec_reg *reg, u32 val)
{
	u32 *slot;

	if (WARN_ON(reg->swreg >= TH1520_VDEC_REG_COUNT))
		return;

	slot = &vpu->regs[reg->swreg];
	*slot &= ~(reg->mask << reg->shift);
	*slot |= (val & reg->mask) << reg->shift;
	__set_bit(reg->swreg, vpu->reg_dirty);
}

void th1520_vdec_reg_write_raw(struct th1520_vdec_dev *vpu, u16 swreg, u32 val)
{
	if (WARN_ON(swreg >= TH1520_VDEC_REG_COUNT))
		return;

	vpu->regs[swreg] = val;
	__set_bit(swreg, vpu->reg_dirty);
}

u32 th1520_vdec_reg_read_raw(struct th1520_vdec_dev *vpu, u16 swreg)
{
	if (WARN_ON(swreg >= TH1520_VDEC_REG_COUNT))
		return 0;

	return vpu->regs[swreg];
}

/*
 * th1520_vdec_write_addr - populate both words of a DMA address
 *
 * The supported configuration uses 32-bit DMA. Explicit upper-word writes
 * clear address state left by previous jobs.
 */
void th1520_vdec_write_addr(struct th1520_vdec_dev *vpu, u16 lsb_swreg,
			    u16 msb_swreg, dma_addr_t addr)
{
	th1520_vdec_reg_write_raw(vpu, lsb_swreg, lower_32_bits(addr));
	th1520_vdec_reg_write_raw(vpu, msb_swreg, upper_32_bits(addr));
}

/**
 * th1520_vdec_write_addr_pair - HEVC 形式的地址写入
 *
 * HEVC 的所有地址寄存器都是相邻的 (MSB, LSB) 对，MSB 在低编号。
 */
void th1520_vdec_write_addr_pair(struct th1520_vdec_dev *vpu, u16 msb_swreg,
				 dma_addr_t addr)
{
	th1520_vdec_write_addr(vpu, msb_swreg + 1, msb_swreg, addr);
}

/*
 * Both codecs use a 128-bit AXI interface and a burst length of 16.
 * The validated field encodings reside in register 58.
 */
#define TH1520_MAX_BURST		16

/*
 * Both hardware timeout counters use 5242880 cycles with their override
 * bits enabled. A separate software watchdog bounds request completion.
 */
#define TH1520_TIMEOUT_OVERRIDE		0x80500000U

static void th1520_vdec_common_config_h264(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;

	/*
	 * H.264 uses table byte-order value 3 and tiled native references.
	 * The remaining byte-order fields are zero; register clock gating is on.
	 */
	th1520_vdec_reg_write_raw(vpu, 2, 0x00000400);
	th1520_vdec_reg_write(vpu, &h264_table_byte_order, 3);
	/*
	 * Enable the native tiled reference layout.
	 */
	th1520_vdec_reg_write_raw(vpu, 2, vpu->regs[2] | BIT(7));

	/* swreg3 —— 解码模式与图像结构（产品表与 G1 表这些位一致）。 */
	/*
	 * Baseline selects mode 0; the other supported profiles select mode 15.
	 */
	th1520_vdec_reg_write(vpu, &th1520_dec_mode,
		ctx->h264.high10p_mode ? TH1520_DEC_MODE_H264_HIGH10 : TH1520_DEC_MODE_H264);
	th1520_vdec_reg_write(vpu, &hevc_out_ec_bypass, ctx->h264.high10p_mode);
	/* VLC 模式；本驱动不使用 RLC 模式。 */
	th1520_vdec_reg_write(vpu, &h264_rlc_mode_e, 0);
	/* DEC_OUT_DIS 必须为 0，否则硬件不写输出帧。 */
	th1520_vdec_reg_write(vpu, &h264_dec_out_dis, 0);
	th1520_vdec_reg_write(vpu, &h264_filtering_dis, 0);
	th1520_vdec_reg_write(vpu, &h264_mvc_e, 0);
	/*
	 * Configure complete-frame VLC input and the input-exhausted interrupt.
	 */
	th1520_vdec_reg_write(vpu, &h264_input_exhausted_irq_enable, 1);
	th1520_vdec_reg_write(vpu, &h264_input_block_mode, 0);

	/*
	 * Register 58 configures the AXI burst length, bus width and ID controls.
	 */
	th1520_vdec_reg_write(vpu, &h264_max_burst_sw58, TH1520_MAX_BURST);
	th1520_vdec_reg_write(vpu, &h264_buswidth_sw58, TH1520_BUS_WIDTH_128);
	th1520_vdec_reg_write(vpu, &h264_axi_rd_id_e_sw58, 1);
	th1520_vdec_reg_write(vpu, &h264_axi_wd_id_e_sw58, 0);

	/* 错误隐藏：关闭（产品表 swreg48[13:12] ERROR_CONC_MODE=0）。 */
	th1520_vdec_reg_write_raw(vpu, TH1520_H264_SWREG_ERR_CONC, 0);

	/*
	 * swreg49 —— 亚像素预测滤波器抽头 (1, -5, 20)。
	 * 这是 H.264 6-tap 滤波器的固定系数，上游 hantro_g1_h264_dec.c 同值。
	 */
	th1520_vdec_reg_write(vpu, &h264_pred_bc_tap_0_0, 1);
	th1520_vdec_reg_write(vpu, &h264_pred_bc_tap_0_1, (u32)(-5) & 0x3ff);
	th1520_vdec_reg_write(vpu, &h264_pred_bc_tap_0_2, 20);

	/* 自适应预取阈值，使用硬件验证值。 */
	th1520_vdec_reg_write(vpu, &h264_apf_threshold, 8);

	/* swreg266 —— 忽略 slice 错误：保持关闭。 */
	th1520_vdec_reg_write(vpu, &h264_error_control_bit31, 0);

	/* swreg318/319 —— 两级硬件超时看门狗，周期采用硬件验证值。 */
	th1520_vdec_reg_write_raw(vpu, 318, TH1520_TIMEOUT_OVERRIDE);
	th1520_vdec_reg_write_raw(vpu, 319, TH1520_TIMEOUT_OVERRIDE);

	/* 使用中断，而不是轮询。 */
	th1520_vdec_reg_write(vpu, &th1520_dec_irq_dis, 0);
}

static void th1520_vdec_common_config_hevc(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;

	/*
	 * HEVC uses zero byte-order fields and enables register clock gating.
	 */
	th1520_vdec_reg_write_raw(vpu, 2, 0x00000400);

	/* swreg3 —— 产品表布局（G2 表位置在 VC8000D 上无效，见 regs.h）。 */
	th1520_vdec_reg_write(vpu, &th1520_dec_mode, TH1520_DEC_MODE_HEVC);
	/*
	 * Bypass native-reference compression. The decoder writes tiled native
	 * images; the post-processor converts them to linear CAPTURE output.
	 */
	th1520_vdec_reg_write(vpu, &hevc_out_ec_bypass, 1);
	/*
	 * Keep reference reads enabled and leave unused control bits clear.
	 */
	th1520_vdec_reg_write(vpu, &hevc_control_bit7, 0);
	th1520_vdec_reg_write(vpu, &hevc_skip_reference_reads, 0);
	th1520_vdec_reg_write(vpu, &hevc_control_bit5, 0);
	/*
	 * Report exhausted input through an interrupt for each complete-frame job.
	 */
	th1520_vdec_reg_write(vpu, &hevc_input_exhausted_irq_enable, 1);
	th1520_vdec_reg_write(vpu, &hevc_input_block_mode, 0);
	th1520_vdec_reg_write(vpu, &hevc_final_input_buffer, 0);
	/* DEC_OUT_DIS 必须为 0，否则硬件不写输出帧。 */
	th1520_vdec_reg_write(vpu, &hevc_out_dis, 0);

	/*
	 * Configure the validated 128-bit AXI bus, burst length and ID controls.
	 */
	th1520_vdec_reg_write(vpu, &hevc_refer_doublebuffer_e, 0);
	th1520_vdec_reg_write(vpu, &hevc_axi_rd_id_e, 1);
	th1520_vdec_reg_write(vpu, &hevc_axi_wd_id_e, 0);
	th1520_vdec_reg_write(vpu, &hevc_buswidth, TH1520_BUS_WIDTH_128);
	th1520_vdec_reg_write(vpu, &hevc_max_burst, TH1520_MAX_BURST);

	/*
	 * Use zero AXI read and write identifiers.
	 */
	th1520_vdec_reg_write(vpu, &hevc_axi_rd_id, 0);
	th1520_vdec_reg_write(vpu, &hevc_axi_wr_id, 0);

	/* 自适应预取阈值，使用硬件验证值。 */
	th1520_vdec_reg_write(vpu, &hevc_apf_disable, 0);
	th1520_vdec_reg_write(vpu, &hevc_apf_threshold, 8);

	/* 不使用下采样输出。 */
	th1520_vdec_reg_write(vpu, &hevc_down_scale_e, 0);

	/* swreg318/319 —— 两级硬件超时看门狗，周期采用硬件验证值。 */
	th1520_vdec_reg_write_raw(vpu, 318, TH1520_TIMEOUT_OVERRIDE);
	th1520_vdec_reg_write_raw(vpu, 319, TH1520_TIMEOUT_OVERRIDE);

	/* Register 265 fields 27:18 and 17:8 use the validated value 64.
	 * Keep master bit 31 clear while using uncompressed native buffers.
	 */
	th1520_vdec_reg_write_raw(vpu, 265, 0x01004000);

	/* Each request submits a complete frame; intermediate tile IRQs stay off. */
	th1520_vdec_reg_write(vpu, &th1520_dec_tile_int_e, 0);

	th1520_vdec_reg_write(vpu, &th1520_dec_irq_dis, 0);
}

void th1520_vdec_set_common_config(struct th1520_vdec_ctx *ctx)
{
	switch (ctx->vpu_src_fmt->codec_mode) {
	case TH1520_MODE_H264_DEC:
		th1520_vdec_common_config_h264(ctx);
		break;
	case TH1520_MODE_HEVC_DEC:
	case TH1520_MODE_VP9_DEC:
		th1520_vdec_common_config_hevc(ctx);
		/* SDK Vp9AsicInit uses the same SetCommonConfigRegs branch. */
		if (ctx->vpu_src_fmt->codec_mode == TH1520_MODE_VP9_DEC)
			th1520_vdec_reg_write(ctx->dev, &th1520_dec_mode,
					       TH1520_DEC_MODE_VP9);
		break;
	default:
		break;
	}
}

/* PPSetRegs @0x163fc4, verified against successful TH1520 HEVC and
 * H.264 command buffers. The PP consumes the native tiled decode stream.
 */
void th1520_vdec_set_postproc(struct th1520_vdec_ctx *ctx, u32 width, u32 height)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *dst = th1520_vdec_get_dst_buf(ctx);
	dma_addr_t dma = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	u32 stride = ctx->dst_fmt.plane_fmt[0].bytesperline;
	u32 chroma_offset = stride * ctx->dst_fmt.height;
	unsigned int i;

	for (i = 320; i < TH1520_VDEC_REG_COUNT; i++)
		th1520_vdec_reg_write_raw(vpu, i, 0);

	th1520_vdec_reg_write_raw(vpu, 320, 1);
	th1520_vdec_reg_write_raw(vpu, 322, 1U << 27);
	th1520_vdec_write_addr_pair(vpu, 325, dma);
	th1520_vdec_write_addr_pair(vpu, 327, dma + chroma_offset);
	th1520_vdec_reg_write_raw(vpu, 329, (stride << 16) | stride);
	th1520_vdec_reg_write_raw(vpu, 331, ((width / 2) << 16) | (height / 2));
	th1520_vdec_reg_write_raw(vpu, 332, (width << 16) | height);
	th1520_vdec_reg_write_raw(vpu, 394, 0x01010000);
}

/*
 * Submit registers 3 through 511, then register 2, and finally register 1.
 * The decoder start bit is written last. Register 0 is read-only.
 */
/**
 * th1520_vdec_dump_regs - 把影子寄存器与硬件回读值并排打印
 *
 * 只在出错路径调用。必须在时钟仍然使能时调用（看门狗里成立，
 * 因为 job_finish() 还没跑到 clk_bulk_disable）。
 *
 * "shadow -> hw" 不一致说明写入没有生效（flush 顺序、地址映射或位域定义有问题）；
 * 一致但硬件不动，说明是配置语义的问题。
 */
void th1520_vdec_dump_regs(struct th1520_vdec_dev *vpu, const char *why)
{
	static const u16 dump_list[] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 17, 18, 19,
		20, 44, 45, 46, 47, 48, 49, 55, 58, 60,
		64, 65, 66, 67, 98, 99, 100, 101, 132, 133, 134, 135,
		166, 167, 168, 169, 170, 171, 178, 179, 180, 181, 182, 183,
		258, 259, 265, 314, 318, 319, 320, 322, 326, 328, 329, 331,
		332, 394,
	};
	unsigned int i;

	dev_err(vpu->dev, "register dump (%s), shadow -> hw:\n", why);
	for (i = 0; i < ARRAY_SIZE(dump_list); i++) {
		u16 n = dump_list[i];
		u32 shadow = vpu->regs[n];
		u32 hw = vdpu_read(vpu, TH1520_VDEC_REG_OFF(n));

		if (!shadow && !hw)
			continue;

		dev_err(vpu->dev, "  swreg%-3u %08x -> %08x%s\n",
			n, shadow, hw,
			shadow != hw ? "   *MISMATCH*" : "");
	}
}

void th1520_vdec_start(struct th1520_vdec_dev *vpu)
{
	unsigned int i;

	th1520_vdec_reg_write(vpu, &th1520_dec_e, 1);

	if (flush_all) {
		for (i = 3; i < TH1520_VDEC_REG_COUNT; i++)
			vdpu_write(vpu, vpu->regs[i], TH1520_VDEC_REG_OFF(i));
	} else {
		for_each_set_bit(i, vpu->reg_dirty, TH1520_VDEC_REG_COUNT) {
			if (i < 3)
				continue;
			vdpu_write(vpu, vpu->regs[i], TH1520_VDEC_REG_OFF(i));
		}
	}

	vdpu_write(vpu, vpu->regs[2], TH1520_VDEC_REG_OFF(2));

	/* Publish DMA data and configuration before the ordered start write. */
	wmb();
	vdpu_write(vpu, vpu->regs[TH1520_VDEC_SWREG_IRQ],
		   TH1520_VDEC_REG_OFF(TH1520_VDEC_SWREG_IRQ));
}

/*
 * th1520_vdec_hw_reset - abort a running job and clear decoder state
 *
 * Request abort with interrupts disabled, wait for the decoder enable bit
 * to clear, then clear the configuration block before the next request.
 */
void th1520_vdec_hw_reset(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;
	unsigned int i;
	u32 status;
	int ret;

	status = vdpu_read(vpu, TH1520_VDEC_REG_OFF(TH1520_VDEC_SWREG_IRQ));
	if (status & TH1520_IRQ_DEC_E) {
		dev_warn(vpu->dev, "device still running, aborting\n");
		vdpu_write(vpu,
			   TH1520_IRQ_DEC_ABORT_E | TH1520_IRQ_DEC_IRQ_DIS,
			   TH1520_VDEC_REG_OFF(TH1520_VDEC_SWREG_IRQ));

		ret = readl_poll_timeout(vpu->reg_base +
					 TH1520_VDEC_REG_OFF(TH1520_VDEC_SWREG_IRQ),
					 status, !(status & TH1520_IRQ_DEC_E),
					 1000, 100000);
		if (ret)
			dev_err(vpu->dev, "abort did not complete\n");
	}

	for (i = TH1520_VDEC_SWREG_IRQ; i < TH1520_VDEC_REG_COUNT; i++)
		vdpu_write(vpu, 0, TH1520_VDEC_REG_OFF(i));

	th1520_vdec_regs_reset(vpu);
}

irqreturn_t th1520_vdec_irq(int irq, void *dev_id)
{
	struct th1520_vdec_dev *vpu = dev_id;
	struct th1520_vdec_ctx *ctx;
	enum vb2_buffer_state state;
	unsigned long flags;
	u32 status;

	spin_lock_irqsave(&vpu->irqlock, flags);

	status = vdpu_read(vpu, TH1520_VDEC_REG_OFF(TH1520_VDEC_SWREG_IRQ));
	if (!(status & TH1520_IRQ_DEC_IRQ)) {
		spin_unlock_irqrestore(&vpu->irqlock, flags);
		dev_dbg(vpu->dev, "IRQ without DEC_IRQ, swreg1=0x%08x\n",
			status);
		return IRQ_NONE;
	}

	/*
	 * Acknowledge all pending status bits and clear the decoder enable bit.
	 * Each request submits a complete frame; no input-refill loop is needed.
	 */
	vdpu_write(vpu,
		   status & ~(TH1520_IRQ_STAT_MASK | TH1520_IRQ_DEC_IRQ |
			      TH1520_IRQ_DEC_E),
		   TH1520_VDEC_REG_OFF(TH1520_VDEC_SWREG_IRQ));

	/* Claim the job while the acknowledged hardware status is protected. */
	ctx = vpu->active_ctx;
	vpu->active_ctx = NULL;
	spin_unlock_irqrestore(&vpu->irqlock, flags);
	if (!ctx)
		return IRQ_HANDLED;

	if ((status & TH1520_IRQ_DEC_RDY_INT) &&
	    !(status & (TH1520_IRQ_ERROR_MASK | TH1520_IRQ_DEC_ABORT_INT |
			TH1520_IRQ_DEC_STRM_CORRUPTED))) {
		state = VB2_BUF_STATE_DONE;
		if (ctx->codec_ops->check_result &&
		    ctx->codec_ops->check_result(ctx, status))
			state = VB2_BUF_STATE_ERROR;
	} else {
		state = VB2_BUF_STATE_ERROR;

		dev_err_ratelimited(vpu->dev, "decode failed, swreg1=0x%08x\n",
				    status);

		if (status & TH1520_IRQ_DEC_TIMEOUT)
			dev_warn_ratelimited(vpu->dev, "hw decode timeout\n");
		if (status & TH1520_IRQ_DEC_ERROR_INT)
			dev_warn_ratelimited(vpu->dev, "stream decode error\n");
		if (status & TH1520_IRQ_DEC_BUS_INT)
			dev_warn_ratelimited(vpu->dev, "bus error\n");
		if (status & TH1520_IRQ_DEC_BUFFER_INT)
			dev_warn_ratelimited(vpu->dev,
					     "stream buffer exhausted\n");
		if (status & TH1520_IRQ_DEC_ABORT_INT)
			dev_warn_ratelimited(vpu->dev, "decode aborted\n");
		if (!(status & TH1520_IRQ_STAT_MASK))
			dev_warn_ratelimited(vpu->dev,
					     "IRQ with no status bit set\n");

		/*
		 * 时钟此时仍然使能（job_finish() 还没跑），可以安全回读。
		 * 产品表中的错误定位字段位于 swreg261：
		 *   [31:22] ERROR_ADDR_X / [21:12] ERROR_ADDR_Y
		 *   [0] ERROR_SLICE_HEADER / [5:3] ERROR_SLICE_DATA
		 * 旧注释把 G2 的 swreg260 沿用到此处；此处更正为产品表坐标。
		 */
		dev_err(vpu->dev, "error info swreg261=0x%08x\n",
			vdpu_read(vpu, TH1520_VDEC_REG_OFF(261)));
		th1520_vdec_dump_regs(vpu, "decode error");
	}

	th1520_vdec_irq_done(vpu, ctx, state);
	return IRQ_HANDLED;
}
