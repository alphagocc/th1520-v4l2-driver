// SPDX-License-Identifier: GPL-2.0
/*
 * TH1520 VC8000D — 影子寄存器、公共配置、启动与中断处理。
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * 本文件实现的时序完全对应两处已验证的证据：
 *
 *  1. 用户态 DWL 的 DWLEnableHw()（analysis/vc8000d-register-config/README.md §8）：
 *     配置整片影子寄存器 → 写 GO 寄存器（swreg1，DEC_E=1）→ 推送寄存器。
 *
 *  2. 厂商内核 DecFlushRegs()
 *     （reference/vpu-vc8000d-kernel/linux/subsys_driver/hantro_dec.c:860）：
 *     先写 swreg3..N，再写 swreg2，最后写 swreg1（写 swreg1 才真正启动解码）。
 *     swreg0 是只读的 HW build id，从不写入。
 *
 * 中断处理对应 hantrodec_isr()（同文件 :3566）与
 * H264RunAsic/HevcRunAsic 的收尾序列：
 *     读 DEC_IRQ_STAT → 写 0 清状态 → 写 DEC_IRQ=0。
 */

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/moduleparam.h>
#include <linux/string.h>

#include "th1520_vdec.h"

/*
 * flush 策略，见 struct th1520_vdec_dev.reg_dirty 的说明。
 * 默认稀疏模式：只写本帧设置过的寄存器，其余保持硬件当前值
 * （与上游 mainline hantro 行为一致）。
 */
static bool flush_all;
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

/**
 * th1520_vdec_write_addr - 写一个 64 位 DMA 地址到分离的 LSB/MSB 寄存器对
 *
 * H.264 的地址寄存器 LSB 与 MSB 不相邻（例如码流基址 LSB=swreg12、MSB=swreg122），
 * 因此必须显式给出两个编号。
 *
 * MSB 始终显式写入（当前 DMA 掩码为 32 bit，因此恒为 0）；显式写 0 而不是
 * 依赖复位值，是因为复位值属于“待硬件验证”项。
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
 * AXI burst 长度与总线宽度：产品表中两个 codec 共用 swreg58（见 regs.h），
 * 厂商栈（SetCommonConfigRegs）两个 codec 都写 MAX_BURST=16、BUSWIDTH=2。
 * 注意 DWLEnableHw 在启用 L2 cache 时会把 burst 钳到 16；
 * 本驱动不启用 cache/shaper，仍取 16 以与两边一致。
 */
#define TH1520_MAX_BURST		16

/*
 * 硬件超时看门狗（swreg318/319）：厂商栈对两个 codec 都写
 * OVERRIDE_E=1 + 周期 5242880（golden 实测 0x80500000；
 * .so 的 dec_timeout_cycles 全局常量）。
 * 之前本驱动“不发明周期数”是因为没有厂商值可依；现在有了 golden 实测值，
 * 直接采用。软件侧另有 2 秒看门狗兜底（TH1520_VDEC_TIMEOUT_MS）。
 */
#define TH1520_TIMEOUT_OVERRIDE		0x80500000U

static void th1520_vdec_common_config_h264(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;

	/*
	 * swreg2 —— 产品表布局：四组 swap 域全部为 0，只置 CLK_GATE_E[10]。
	 * 整字值 0x00000400，与厂商栈 SetCommonConfigRegs 的实测结果一致
	 * （该函数对两个 codec 写同一套 common config；HEVC 的 golden 抓取
	 * 证实了 0x400，H.264 走的是同一代码路径）。
	 * 之前的 G1 表版本（timeout_e/swap32/endian/latency/max_burst 等位）
	 * 在产品表中不存在，写它们会污染 DIRMV_SWAP 域并把 DRM_E 置 1。
	 */
	th1520_vdec_reg_write_raw(vpu, 2, 0x00000400);

	/* swreg3 —— 解码模式与图像结构（产品表与 G1 表这些位一致）。 */
	th1520_vdec_reg_write(vpu, &th1520_dec_mode, TH1520_DEC_MODE_H264);
	/* VLC 模式；本驱动不使用 RLC 模式。 */
	th1520_vdec_reg_write(vpu, &h264_rlc_mode_e, 0);
	/* DEC_OUT_DIS 必须为 0，否则硬件不写输出帧。 */
	th1520_vdec_reg_write(vpu, &h264_dec_out_dis, 0);
	th1520_vdec_reg_write(vpu, &h264_filtering_dis, 0);
	th1520_vdec_reg_write(vpu, &h264_mvc_e, 0);

	/*
	 * swreg58 —— 总线参数（产品表：MAX_BURST/BUSWIDTH 在这里，不在 swreg2）。
	 * AXI_RD_ID_E=1 与 golden 一致（DWLEnableHw 在 cache 版本 >4 时置位，
	 * TH1520 的 golden 抓取证实为 1）。
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

	/* 自适应预取阈值，取上游/厂商同值。 */
	th1520_vdec_reg_write(vpu, &h264_apf_threshold, 8);

	/* swreg266 —— 忽略 slice 错误：保持关闭。 */
	th1520_vdec_reg_write(vpu, &h264_ignore_slice_error_e, 0);

	/* swreg318/319 —— 两级硬件超时看门狗，周期取厂商实测值。 */
	th1520_vdec_reg_write_raw(vpu, 318, TH1520_TIMEOUT_OVERRIDE);
	th1520_vdec_reg_write_raw(vpu, 319, TH1520_TIMEOUT_OVERRIDE);

	/* 使用中断，而不是轮询。 */
	th1520_vdec_reg_write(vpu, &th1520_dec_irq_dis, 0);
}

static void th1520_vdec_common_config_hevc(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_dev *vpu = ctx->dev;

	/*
	 * swreg2 —— 产品表布局：四组 swap 域全部为 0，只置 CLK_GATE_E[10]，
	 * 整字 0x00000400。与厂商栈 golden 抓取逐位一致
	 * （analysis/golden-registers.txt；分析见 analysis/vc8000d-product-table.md）。
	 */
	th1520_vdec_reg_write_raw(vpu, 2, 0x00000400);

	/* swreg3 —— 产品表布局（G2 表位置在 VC8000D 上无效，见 regs.h）。 */
	th1520_vdec_reg_write(vpu, &th1520_dec_mode, TH1520_DEC_MODE_HEVC);
	/*
	 * OUT_EC_BYPASS=1：旁路参考帧压缩。golden 厂商栈为 0（压缩开 +
	 * L2CACHE/DEC400 全套配置 + PP 输出）；旁路路径在本驱动早期调试中
	 * 已证明能写出线性输出（见 README §6.0.1 的实验），而压缩路径需要
	 * 一整块未验证的 L2CACHE/DEC400 配置。先走旁路，待码流解析跑通后
	 * 再决定是否切到压缩路径。与 golden 的差异记录在 README §6.0.3。
	 */
	th1520_vdec_reg_write(vpu, &hevc_out_ec_bypass, 1);
	/* APF_ONE_PID / REF_READ_DIS / L2_SHAPER_E：golden=0/1/0。
	 * REF_READ_DIS=1 与 golden 的压缩+L2 路径绑定（DWL 层静态配置）；
	 * 旁路路径保持 0，I 帧无参考读取不受影响。待硬件验证。 */
	th1520_vdec_reg_write(vpu, &hevc_apf_one_pid, 0);
	th1520_vdec_reg_write(vpu, &hevc_ref_read_dis, 0);
	th1520_vdec_reg_write(vpu, &hevc_l2_shaper_e, 0);
	/* BUFFER_EMPTY_INT_E=1 与 golden 一致；LAST_BUFFER_E 恒 0（golden 实测）。 */
	th1520_vdec_reg_write(vpu, &hevc_buffer_empty_int_e, 1);
	th1520_vdec_reg_write(vpu, &hevc_block_buffer_mode_e, 0);
	th1520_vdec_reg_write(vpu, &hevc_last_buffer_e, 0);
	/* DEC_OUT_DIS 必须为 0，否则硬件不写输出帧。 */
	th1520_vdec_reg_write(vpu, &hevc_out_dis, 0);

	/*
	 * swreg58 —— 产品表布局：CLK_GATE 不在这里（真实位置 swreg2[10]）。
	 * AXI_RD_ID_E=1 与 golden 一致；BUSWIDTH=2 / MAX_BURST=16 同厂商。
	 */
	th1520_vdec_reg_write(vpu, &hevc_refer_doublebuffer_e, 0);
	th1520_vdec_reg_write(vpu, &hevc_axi_rd_id_e, 1);
	th1520_vdec_reg_write(vpu, &hevc_axi_wd_id_e, 0);
	th1520_vdec_reg_write(vpu, &hevc_buswidth, TH1520_BUS_WIDTH_128);
	th1520_vdec_reg_write(vpu, &hevc_max_burst, TH1520_MAX_BURST);

	/* swreg60 —— AXI ID（产品表位置）。golden 不写（=0）。 */
	th1520_vdec_reg_write(vpu, &hevc_axi_rd_id, 0);
	th1520_vdec_reg_write(vpu, &hevc_axi_wr_id, 0);

	/* 自适应预取阈值，取上游/厂商同值。 */
	th1520_vdec_reg_write(vpu, &hevc_apf_disable, 0);
	th1520_vdec_reg_write(vpu, &hevc_apf_threshold, 8);

	/* 不使用下采样输出。 */
	th1520_vdec_reg_write(vpu, &hevc_down_scale_e, 0);

	/* swreg318/319 —— 两级硬件超时看门狗，周期取厂商实测值。 */
	th1520_vdec_reg_write_raw(vpu, 318, TH1520_TIMEOUT_OVERRIDE);
	th1520_vdec_reg_write_raw(vpu, 319, TH1520_TIMEOUT_OVERRIDE);

	/*
	 * swreg265（cache/shaper 主使能）不写：旁路路径不配置 cache/shaper。
	 * 与 golden（0x81004000）的差异记录在 README §6.0.3。
	 */

	/*
	 * tile 中断：关闭。本驱动一次提交整帧，只期待一个 DEC_RDY_INT。
	 * 若将来要走逐 tile 中断循环，需要在 IRQ 里实现
	 * “清状态 → 重新置 DEC_E”的循环（见 README §6.8）。
	 */
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
		th1520_vdec_common_config_hevc(ctx);
		break;
	default:
		break;
	}
}

/**
 * th1520_vdec_start - 把影子寄存器推给硬件并启动解码
 *
 * 顺序与厂商内核 DecFlushRegs() 完全一致：
 *   swreg3..N → swreg2 → swreg1
 * 写 swreg1 时其中的 DEC_E=1 会真正启动硬件，所以它必须最后写。
 * swreg0 是只读 HW build id，不写。
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

	/* 确保前面的寄存器都已落地再拉 GO。 */
	wmb();
	vdpu_write(vpu, vpu->regs[TH1520_VDEC_SWREG_IRQ],
		   TH1520_VDEC_REG_OFF(TH1520_VDEC_SWREG_IRQ));
}

/**
 * th1520_vdec_hw_reset - 中止正在跑的解码并把寄存器清零
 *
 * 与厂商内核 ResetAsic()
 * （reference/vpu-vc8000d-kernel/linux/subsys_driver/hantro_dec.c:3619）一致：
 * 若 DEC_E 仍为 1，则写 DEC_ABORT_E | DEC_IRQ_DIS 请求中止，
 * 然后把 swreg1 起的整片寄存器清零。
 *
 * VC8000D 没有独立的软复位寄存器，abort 是唯一可用的“复位”手段。
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
	 * 应答顺序取自 .so 的 H264RunAsic / HevcRunAsic 收尾：
	 *   写 DEC_IRQ_STAT = 0 清全部状态位，写 DEC_IRQ = 0 清挂起标志。
	 * 同时清 DEC_E 结束本次解码 —— 本驱动一次提交整帧，
	 * 既不做 tile 循环也不做码流续传，硬件不应继续跑。
	 */
	vdpu_write(vpu,
		   status & ~(TH1520_IRQ_STAT_MASK | TH1520_IRQ_DEC_IRQ |
			      TH1520_IRQ_DEC_E),
		   TH1520_VDEC_REG_OFF(TH1520_VDEC_SWREG_IRQ));

	spin_unlock_irqrestore(&vpu->irqlock, flags);

	if (status & TH1520_IRQ_DEC_RDY_INT) {
		state = VB2_BUF_STATE_DONE;
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
		 * swreg260 是 HEVC 的错误定位寄存器：
		 *   [31:24] ERROR_ADDR_X / [23:16] ERROR_ADDR_Y
		 *   [3] ERROR_SLICE_HEADER / [2:0] ERROR_SLICE_DATA
		 */
		dev_err(vpu->dev, "error info swreg260=0x%08x\n",
			vdpu_read(vpu, TH1520_VDEC_REG_OFF(260)));
		th1520_vdec_dump_regs(vpu, "decode error");
	}

	th1520_vdec_irq_done(vpu, state);
	return IRQ_HANDLED;
}
