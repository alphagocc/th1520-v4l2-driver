/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * TH1520 VC8000D V4L2 stateless decoder driver.
 *
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * V4L2 framework references and licensing are listed in docs/sources.md.
 * The supported hardware interface is documented in docs/hardware.md.
 */

#ifndef TH1520_VDEC_H_
#define TH1520_VDEC_H_

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <media/media-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-h264.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "th1520_vdec_regs.h"

#define TH1520_VDEC_NAME		"th1520-vdec"

#define TH1520_VDEC_NUM_CLOCKS		3

/*
 * The device-tree resource starts at the VPU subsystem base.
 * The decoder occupies 1023 32-bit registers at offset 0x1000.
 * See docs/hardware.md for the supported device-tree contract.
 */
#define TH1520_VDEC_CORE_OFFSET		0x1000
#define TH1520_VDEC_CORE_IOSIZE		(1023 * 4)

/* H.264 macroblock edge length, columns, and rows */
#define TH1520_MB_DIM			16
#define TH1520_MB_WIDTH(w)		DIV_ROUND_UP(w, TH1520_MB_DIM)
#define TH1520_MB_HEIGHT(h)		DIV_ROUND_UP(h, TH1520_MB_DIM)

#define TH1520_MIN_WIDTH		48
#define TH1520_MIN_HEIGHT		48
/*
 * H.264 的 PIC_MB_WIDTH 只有 9 bit（swreg4[31:23]），即最大 511 MB = 8176 像素；
 * PIC_MB_HEIGHT_P 8 bit + swreg7[25] 扩展位。本驱动先限制到 4096x2304，
 * 更大的尺寸需要在目标板上验证扩展位行为后再放开。
 */
#define TH1520_MAX_WIDTH		4096
#define TH1520_MAX_HEIGHT		2304

/* 一次解码提交的软件超时（硬件另有 swreg318/319 看门狗） */
#define TH1520_VDEC_TIMEOUT_MS		2000

/* H.264 与 HEVC 的 DPB 槽位数都是 16 */
#define TH1520_DPB_SIZE			16

struct th1520_vdec_ctx;

/**
 * struct th1520_vdec_aux_buf - 驱动私有的 DMA 辅助缓冲
 */
struct th1520_vdec_aux_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

/**
 * struct th1520_vdec_buffer - 队列缓冲及其私有原生参考帧
 * @m2m:      V4L2 M2M 队列缓冲
 * @native:   HEVC 原生帧及 direct-MV，随 CAPTURE 缓冲分配和释放
 */
struct th1520_vdec_buffer {
	struct v4l2_m2m_buffer m2m;
	struct th1520_vdec_aux_buf native;
};

enum th1520_vdec_codec_mode {
	TH1520_MODE_NONE = -1,
	TH1520_MODE_H264_DEC,
	TH1520_MODE_HEVC_DEC,
};

/**
 * struct th1520_vdec_fmt - 支持的像素格式
 * @fourcc:	  V4L2 fourcc
 * @codec_mode:	  对应的 codec 后端；TH1520_MODE_NONE 表示这是原始帧格式
 * @is_bitstream: 是否为压缩码流格式（OUTPUT 队列）
 */
struct th1520_vdec_fmt {
	u32 fourcc;
	enum th1520_vdec_codec_mode codec_mode;
	bool is_bitstream;
};

/**
 * struct th1520_vdec_codec_ops - 每个 codec 后端需要实现的回调
 * @init:      分配 codec 私有资源（每个 ctx 一次）
 * @exit:      释放 codec 私有资源
 * @run:       把当前 request 的控件翻译成寄存器并启动硬件
 * @done:      一次解码成功完成后的收尾（可为 NULL）
 * @reset:     超时后复位硬件
 */
struct th1520_vdec_codec_ops {
	int (*init)(struct th1520_vdec_ctx *ctx);
	void (*exit)(struct th1520_vdec_ctx *ctx);
	int (*run)(struct th1520_vdec_ctx *ctx);
	void (*done)(struct th1520_vdec_ctx *ctx);
	void (*reset)(struct th1520_vdec_ctx *ctx);
};

/**
 * struct th1520_vdec_dev - 每个 VC8000D 解码核一个
 * @v4l2_dev:	 V4L2 设备
 * @m2m_dev:	 mem2mem 设备
 * @mdev:	 media 设备（Request API 必需）
 * @vfd:	 /dev/videoX
 * @pdev:	 平台设备
 * @dev:	 struct device
 * @clocks:	 cclk / aclk / pclk
 * @reg_base:	 VC8000D 寄存器块的 ioremap 结果
 * @irq:	 解码中断
 * @vpu_mutex:	 保护 V4L2 设备级操作
 * @irqlock:	 保护活动作业、超时时间与中断状态
 * @watchdog_work: 软件超时处理
 * @active_ctx:   当前硬件作业；完成处理取得该指针后清零
 * @watchdog_deadline: 当前作业的 jiffies 截止时间，由 irqlock 保护
 * @regs:	 影子寄存器数组
 *
 * The shadow block is rebuilt for every job so register values do not
 * depend on the previous context or hardware reset defaults.
 *
 * 由于 v4l2-m2m 保证同一时刻只有一个 job 在跑，@regs 放在 dev 上即可。
 */
struct th1520_vdec_dev {
	struct v4l2_device v4l2_dev;
	struct v4l2_m2m_dev *m2m_dev;
	struct media_device mdev;
	struct video_device *vfd;
	struct platform_device *pdev;
	struct device *dev;

	struct clk_bulk_data clocks[TH1520_VDEC_NUM_CLOCKS];
	void __iomem *reg_base;
	int irq;

	struct mutex vpu_mutex;	/* 串行化 V4L2 设备级操作 */
	spinlock_t irqlock;	/* Active job, deadline and IRQ status. */

	struct delayed_work watchdog_work;
	struct th1520_vdec_ctx *active_ctx;
	unsigned long watchdog_deadline;

	u32 regs[TH1520_VDEC_REG_COUNT];
	/*
	 * Dirty-register bitmap for optional diagnostic sparse submissions.
	 * The default flush_all=1 writes all configuration registers to prevent
	 * state from leaking between codecs or contexts.
	 */
	DECLARE_BITMAP(reg_dirty, TH1520_VDEC_REG_COUNT);
};

/* --- H.264 每 ctx 状态 ------------------------------------------------- */

struct th1520_vdec_h264_ctrls {
	const struct v4l2_ctrl_h264_decode_params *decode;
	const struct v4l2_ctrl_h264_sps *sps;
	const struct v4l2_ctrl_h264_pps *pps;
	const struct v4l2_ctrl_h264_scaling_matrix *scaling;
};

struct th1520_vdec_h264_reflists {
	struct v4l2_h264_reference p[V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reference b0[V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reference b1[V4L2_H264_REF_LIST_LEN];
};

struct th1520_vdec_h264_ctx {
	bool high10p_mode;
	struct th1520_vdec_aux_buf priv;
	struct v4l2_h264_dpb_entry dpb[TH1520_DPB_SIZE];
	struct th1520_vdec_h264_reflists reflists;
	struct th1520_vdec_h264_ctrls ctrls;
	u32 dpb_valid;
	u32 dpb_longterm;
	s32 cur_poc;
};

/* --- HEVC 每 ctx 状态 -------------------------------------------------- */

struct th1520_vdec_hevc_ctrls {
	const struct v4l2_ctrl_hevc_sps *sps;
	const struct v4l2_ctrl_hevc_pps *pps;
	const struct v4l2_ctrl_hevc_decode_params *decode_params;
	const struct v4l2_ctrl_hevc_scaling_matrix *scaling;
};

struct th1520_vdec_hevc_ctx {
	struct th1520_vdec_aux_buf tile_sizes;
	struct th1520_vdec_aux_buf scaling_lists;
	struct th1520_vdec_aux_buf tile_filter;
	struct th1520_vdec_aux_buf tile_sao;
	struct th1520_vdec_aux_buf tile_bsd;
	struct th1520_vdec_hevc_ctrls ctrls;
	unsigned int num_tile_cols_allocated;
	dma_addr_t ref_bufs[V4L2_HEVC_DPB_ENTRIES_NUM_MAX];
	s32 ref_bufs_poc[V4L2_HEVC_DPB_ENTRIES_NUM_MAX];
	u32 ref_bufs_used;
};

/**
 * struct th1520_vdec_ctx - 每个打开的 fd 一个解码上下文
 */
struct th1520_vdec_ctx {
	struct v4l2_fh fh;
	struct th1520_vdec_dev *dev;

	struct v4l2_ctrl_handler ctrl_handler;

	struct v4l2_pix_format_mplane src_fmt;
	struct v4l2_pix_format_mplane dst_fmt;
	const struct th1520_vdec_fmt *vpu_src_fmt;
	const struct th1520_vdec_fmt *vpu_dst_fmt;

	const struct th1520_vdec_codec_ops *codec_ops;

	u32 sequence_out;
	u32 sequence_cap;

	unsigned int bit_depth;

	union {
		struct th1520_vdec_h264_ctx h264;
		struct th1520_vdec_hevc_ctx hevc;
	};
};

static inline struct th1520_vdec_ctx *fh_to_ctx(struct v4l2_fh *fh)
{
	return container_of(fh, struct th1520_vdec_ctx, fh);
}

/* --- 寄存器访问（th1520_vdec_hw.c） ----------------------------------- */

void th1520_vdec_regs_reset(struct th1520_vdec_dev *vpu);
void th1520_vdec_reg_write(struct th1520_vdec_dev *vpu,
			   const struct th1520_vdec_reg *reg, u32 val);
void th1520_vdec_reg_write_raw(struct th1520_vdec_dev *vpu, u16 swreg, u32 val);
u32 th1520_vdec_reg_read_raw(struct th1520_vdec_dev *vpu, u16 swreg);
void th1520_vdec_write_addr(struct th1520_vdec_dev *vpu, u16 lsb_swreg,
			    u16 msb_swreg, dma_addr_t addr);
void th1520_vdec_write_addr_pair(struct th1520_vdec_dev *vpu, u16 msb_swreg,
				 dma_addr_t addr);
void th1520_vdec_set_common_config(struct th1520_vdec_ctx *ctx);
void th1520_vdec_set_postproc(struct th1520_vdec_ctx *ctx, u32 width, u32 height);
void th1520_vdec_start(struct th1520_vdec_dev *vpu);
void th1520_vdec_dump_regs(struct th1520_vdec_dev *vpu, const char *why);
void th1520_vdec_hw_reset(struct th1520_vdec_ctx *ctx);
irqreturn_t th1520_vdec_irq(int irq, void *dev_id);

u32 vdpu_read(struct th1520_vdec_dev *vpu, u32 offset);
void vdpu_write(struct th1520_vdec_dev *vpu, u32 val, u32 offset);

/* --- 核心（th1520_vdec_drv.c） ---------------------------------------- */

extern const struct v4l2_event th1520_vdec_eos_event;

void th1520_vdec_irq_done(struct th1520_vdec_dev *vpu,
			  struct th1520_vdec_ctx *ctx,
			  enum vb2_buffer_state result);
void th1520_vdec_watchdog(struct work_struct *work);
void th1520_vdec_start_prepare_run(struct th1520_vdec_ctx *ctx);
void th1520_vdec_end_prepare_run(struct th1520_vdec_ctx *ctx);
void *th1520_vdec_get_ctrl(struct th1520_vdec_ctx *ctx, u32 id);
dma_addr_t th1520_vdec_get_ref(struct th1520_vdec_ctx *ctx, u64 ts);

/* --- V4L2（th1520_vdec_v4l2.c） --------------------------------------- */

extern const struct v4l2_ioctl_ops th1520_vdec_ioctl_ops;
extern const struct v4l2_file_operations th1520_vdec_fops;

int th1520_vdec_queue_init(void *priv, struct vb2_queue *src_vq,
			   struct vb2_queue *dst_vq);
int th1520_vdec_ctrls_setup(struct th1520_vdec_ctx *ctx);
void th1520_vdec_reset_fmts(struct th1520_vdec_ctx *ctx);

/* --- codec 后端 -------------------------------------------------------- */

extern const struct th1520_vdec_codec_ops th1520_vdec_h264_ops;
extern const struct th1520_vdec_codec_ops th1520_vdec_hevc_ops;

/* H.264 CABAC 初始化表，单位是 u32（th1520_vdec_h264_cabac.c） */
#define TH1520_H264_CABAC_TABLE_LEN	(460 * 2)
extern const u32 th1520_vdec_h264_cabac_table[TH1520_H264_CABAC_TABLE_LEN];

/* --- 缓冲区尺寸与布局 -------------------------------------------------- */

static inline struct th1520_vdec_buffer *
th1520_vdec_vbuf_to_buffer(struct vb2_v4l2_buffer *vbuf)
{
	return container_of(vbuf, struct th1520_vdec_buffer, m2m.vb);
}

static inline struct vb2_v4l2_buffer *
th1520_vdec_get_src_buf(struct th1520_vdec_ctx *ctx)
{
	return v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
}

static inline struct vb2_v4l2_buffer *
th1520_vdec_get_dst_buf(struct th1520_vdec_ctx *ctx)
{
	return v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
}

/*
 * H.264 的 direct-MV（colocated）缓冲追加在解码帧之后。
 * 每个宏块 384 字节（4:2:0）或 256 字节（单色），另加多核变体的 32 字节余量。
 * 与上游 hantro_h264_mv_size() 一致。
 */
static inline size_t th1520_vdec_h264_mv_size(unsigned int width,
					      unsigned int height)
{
	return 64 * TH1520_MB_WIDTH(width) * TH1520_MB_HEIGHT(height) + 32;
}

/*
 * HEVC 原生参考帧采用 4x4 tile，后处理器向 CAPTURE 输出线性 NV12。
 * 原生 Y stride = ALIGN(coded_width * 4, 64)，Y 区域每四行占一条 tile 行。
 * 色度区域按 64 字节补齐，随后预留 64 字节，再保存每个 64x64 CTB
 * 对应的 256 字节 direct-MV 数据。尺寸取格式协商确定的 coded size，
 * CAPTURE 缓冲存在期间该尺寸保持固定。
 */
static inline size_t
th1520_vdec_hevc_native_chroma_offset(const struct th1520_vdec_ctx *ctx)
{
	size_t stride = ALIGN(ctx->src_fmt.width * 4, 64);

	return stride * ALIGN(ctx->src_fmt.height, TH1520_MB_DIM) / 4;
}

static inline size_t
th1520_vdec_hevc_native_mv_offset(const struct th1520_vdec_ctx *ctx)
{
	size_t luma_size = th1520_vdec_hevc_native_chroma_offset(ctx);

	return luma_size + ALIGN(luma_size / 2, 64) + 64;
}

static inline size_t
th1520_vdec_hevc_native_size(const struct th1520_vdec_ctx *ctx)
{
	size_t mv_size = DIV_ROUND_UP(ctx->src_fmt.width, 64) *
			 DIV_ROUND_UP(ctx->src_fmt.height, 64) * 256;

	return th1520_vdec_hevc_native_mv_offset(ctx) + mv_size;
}

/* H264 mode15 uses the same Y/C/sync layout, with 80 bytes per MB for MV.
 * h264bsdInitDpb @0x8595a..0x85a4e, uncompressed 8-bit tiled output.
 */
static inline size_t
th1520_vdec_h264_native_chroma_offset(const struct th1520_vdec_ctx *ctx)
{
	return th1520_vdec_hevc_native_chroma_offset(ctx);
}

static inline size_t
th1520_vdec_h264_native_mv_offset(const struct th1520_vdec_ctx *ctx)
{
	if (ctx->h264.high10p_mode)
		return th1520_vdec_hevc_native_mv_offset(ctx);
	return th1520_vdec_h264_native_chroma_offset(ctx) * 3 / 2;
}

static inline size_t
th1520_vdec_h264_native_size(const struct th1520_vdec_ctx *ctx)
{
	size_t mbs = TH1520_MB_WIDTH(ctx->src_fmt.width) *
		     TH1520_MB_HEIGHT(ctx->src_fmt.height);

	/* Allocate before per-request SPS determines mode0/mode15. */
	return th1520_vdec_hevc_native_mv_offset(ctx) + ALIGN(80 * ALIGN(mbs, 4), 64);
}

#endif /* TH1520_VDEC_H_ */
