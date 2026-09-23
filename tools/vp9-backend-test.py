#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual VP9 backend with host DMA, vb2 and register-array stubs.

The register array checks software-generated values only. This test cannot
establish that the hardware interprets those values as intended.
Usage: python3 tools/vp9-backend-test.py --kernel-tree /path/to/linux-6.6
       python3 tools/vp9-backend-test.py --kernel-archive linux-6.6.140.tar.gz
"""

import argparse
import importlib.util
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import tempfile

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "vp9_host", Path(__file__).with_name("vp9-offline-test.py"))
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)

EXTRA = r"""
#include <errno.h>
#include <media/v4l2-vp9.h>
#include "th1520_vdec_regs.h"
typedef uintptr_t dma_addr_t;
typedef u16 __le16;
#define GFP_KERNEL 0
#define TH1520_MIN_WIDTH 48
#define TH1520_MIN_HEIGHT 48
#define TH1520_MAX_WIDTH 4096
#define TH1520_MAX_HEIGHT 2304
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define DIV_ROUND_UP(x, a) (((x) + (a) - 1) / (a))
#define round_down(x, a) ((x) & ~((a) - 1))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define clamp_t(t, x, lo, hi) clamp((t)(x), (t)(lo), (t)(hi))
#define cpu_to_le16(x) ((u16)(x))
#undef static_assert
#define static_assert(x) _Static_assert((x), #x)
#define dma_rmb() ((void)0)
#define dev_err_ratelimited(dev, ...) ((void)(dev))
struct vb2_buffer {
	struct { u32 data_offset; } planes[1];
	size_t payload, size;
	dma_addr_t dma;
	u64 timestamp;
	struct { void *req; } req_obj;
};
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; };
struct th1520_vdec_buffer {
	struct vb2_v4l2_buffer vb;
	struct th1520_vdec_aux_buf native;
	struct { u32 width, height, chroma_offset, mv_offset, stride; bool valid; } vp9;
};
struct vb2_queue { unsigned int num_buffers; struct vb2_buffer *bufs[8]; };
struct th1520_vdec_dev {
	void *dev;
	u32 regs[TH1520_VDEC_REG_COUNT];
	u32 mmio[TH1520_VDEC_REG_COUNT];
	unsigned int mmio_reads;
};
struct th1520_vdec_ctx {
	struct th1520_vdec_vp9_ctx *vp9;
	struct th1520_vdec_dev *dev;
	struct { u32 width, height; } src_fmt;
	struct { void *m2m_ctx; } fh;
	unsigned int ctrl_handler;
	struct vb2_v4l2_buffer *src, *dst;
	struct v4l2_ctrl_vp9_frame *frame_ctrl;
	struct v4l2_ctrl_vp9_compressed_hdr *header_ctrl;
	unsigned int prepares, completes, starts;
};
struct th1520_vdec_codec_ops {
	int (*init)(struct th1520_vdec_ctx *);
	void (*exit)(struct th1520_vdec_ctx *);
	int (*run)(struct th1520_vdec_ctx *);
	int (*check_result)(struct th1520_vdec_ctx *, u32);
	void (*done)(struct th1520_vdec_ctx *);
	void (*abort)(struct th1520_vdec_ctx *);
	void (*reset)(struct th1520_vdec_ctx *);
};
void *kzalloc(size_t size, int flags);
void kfree(void *ptr);
void *dma_alloc_coherent(void *dev, size_t size, dma_addr_t *dma, int flags);
void dma_free_coherent(void *dev, size_t size, void *ptr, dma_addr_t dma);
static inline size_t vb2_get_plane_payload(struct vb2_buffer *b, unsigned int p) { (void)p; return b->payload; }
static inline size_t vb2_plane_size(struct vb2_buffer *b, unsigned int p) { (void)p; return b->size; }
static inline dma_addr_t vb2_dma_contig_plane_dma_addr(struct vb2_buffer *b, unsigned int p) { (void)p; return b->dma; }
static inline struct vb2_v4l2_buffer *to_vb2_v4l2_buffer(struct vb2_buffer *b) { return (void *)b; }
static inline struct th1520_vdec_buffer *th1520_vdec_vbuf_to_buffer(struct vb2_v4l2_buffer *b) { return (void *)b; }
static inline struct vb2_v4l2_buffer *th1520_vdec_get_src_buf(struct th1520_vdec_ctx *c) { return c->src; }
static inline struct vb2_v4l2_buffer *th1520_vdec_get_dst_buf(struct th1520_vdec_ctx *c) { return c->dst; }
static inline struct vb2_queue *v4l2_m2m_get_dst_vq(void *p) { return p; }
static inline struct vb2_buffer *vb2_find_buffer(struct vb2_queue *q, u64 ts) {
	unsigned int i;
	for (i = 0; i < q->num_buffers; i++) if (q->bufs[i]->timestamp == ts) return q->bufs[i];
	return NULL;
}
static inline void th1520_vdec_reg_write_raw(struct th1520_vdec_dev *d, u16 r, u32 v) { assert(r < TH1520_VDEC_REG_COUNT); d->regs[r] = v; }
static inline void th1520_vdec_reg_write(struct th1520_vdec_dev *d, const struct th1520_vdec_reg *r, u32 v) {
	assert(r->swreg < TH1520_VDEC_REG_COUNT);
	d->regs[r->swreg] = (d->regs[r->swreg] & ~(r->mask << r->shift)) | ((v & r->mask) << r->shift);
}
static inline void th1520_vdec_write_addr_pair(struct th1520_vdec_dev *d, u16 r, dma_addr_t a) {
	th1520_vdec_reg_write_raw(d, r, (u64)a >> 32);
	th1520_vdec_reg_write_raw(d, r + 1, a);
}
static inline u32 vdpu_read(struct th1520_vdec_dev *d, u32 offset) {
	assert(!(offset & 3) && offset / 4 < TH1520_VDEC_REG_COUNT);
	++d->mmio_reads;
	return d->mmio[offset / 4];
}
static inline void *th1520_vdec_get_ctrl(struct th1520_vdec_ctx *c, u32 id) {
	return id == V4L2_CID_STATELESS_VP9_FRAME ? (void *)c->frame_ctrl : c->header_ctrl;
}
static inline void v4l2_ctrl_request_complete(void *req, unsigned int *complete) { (void)req; ++*complete; }
static inline void th1520_vdec_start_prepare_run(struct th1520_vdec_ctx *c) { ++c->prepares; memset(c->dev->regs, 0, sizeof(c->dev->regs)); }
static inline void th1520_vdec_end_prepare_run(struct th1520_vdec_ctx *c) { ++c->ctrl_handler; ++c->starts; }
static inline void th1520_vdec_start(struct th1520_vdec_dev *d) { (void)d; }
static inline void th1520_vdec_set_common_config(struct th1520_vdec_ctx *c) { (void)c; }
static inline void th1520_vdec_set_postproc(struct th1520_vdec_ctx *c, u32 w, u32 h) { (void)c; (void)w; (void)h; }
static inline void th1520_vdec_hw_reset(struct th1520_vdec_ctx *c) { (void)c; }
"""

HARNESS = r"""
/* Including the actual source exposes its static validation/config helpers. */
#include "th1520_vdec_vp9.c"

static unsigned int alloc_call, fail_dma, live_dma, live_ctx;
static bool fail_ctx;
static struct { void *ptr; size_t size; } allocated[8];
void *kzalloc(size_t size, int flags) {
	void *ptr; (void)flags;
	if (fail_ctx) return NULL;
	ptr = calloc(1, size);
	assert(ptr); ++live_ctx; return ptr;
}
void kfree(void *ptr) { assert(ptr && live_ctx); --live_ctx; free(ptr); }
void *dma_alloc_coherent(void *dev, size_t size, dma_addr_t *dma, int flags) {
	void *ptr; unsigned int i; (void)dev; (void)flags;
	if (++alloc_call == fail_dma) return NULL;
	ptr = malloc(size); assert(ptr);
	for (i = 0; i < ARRAY_SIZE(allocated); i++) if (!allocated[i].ptr) break;
	assert(i < ARRAY_SIZE(allocated));
	allocated[i].ptr = ptr; allocated[i].size = size;
	++live_dma; *dma = (uintptr_t)ptr; return ptr;
}
void dma_free_coherent(void *dev, size_t size, void *ptr, dma_addr_t dma) {
	unsigned int i; (void)dev; assert((uintptr_t)ptr == dma);
	for (i = 0; i < ARRAY_SIZE(allocated); i++) if (allocated[i].ptr == ptr) break;
	assert(i < ARRAY_SIZE(allocated) && allocated[i].size == size && live_dma);
	allocated[i].ptr = NULL; --live_dma; free(ptr);
}
static u32 field(struct th1520_vdec_dev *dev, struct th1520_vdec_reg reg) {
	return (dev->regs[reg.swreg] >> reg.shift) & reg.mask;
}
static void good_frame(struct v4l2_ctrl_vp9_frame *f, unsigned int w, unsigned int h) {
	memset(f, 0, sizeof(*f));
	f->flags = V4L2_VP9_FRAME_FLAG_KEY_FRAME | V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING | V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;
	f->bit_depth = 8; f->frame_width_minus_1 = w - 1; f->frame_height_minus_1 = h - 1;
	f->uncompressed_header_size = 12; f->compressed_header_size = 7; f->quant.base_q_idx = 100;
}
static void test_init_failures(struct th1520_vdec_ctx *ctx) {
	unsigned int i;
	fail_ctx = true;
	assert(th1520_vp9_init(ctx) == -ENOMEM && !ctx->vp9);
	fail_ctx = false;
	for (i = 1; i <= 7; i++) {
		alloc_call = 0; fail_dma = i;
		assert(th1520_vp9_init(ctx) == -ENOMEM);
		assert(!ctx->vp9 && !live_ctx && !live_dma);
		th1520_vp9_exit(ctx);
	}
	fail_dma = 0; alloc_call = 0;
	assert(!th1520_vp9_init(ctx));
	assert(alloc_call == 7 && live_dma == 7 && live_ctx == 1);
	assert(ctx->vp9->need_keyframe);
}
static void test_native_sizes(struct th1520_vdec_ctx *ctx) {
	const unsigned int widths[] = { 48, 50, 2048, 2050, 4096 };
	const unsigned int heights[] = { 48, 50, 1080, 1088, 2304 };
	unsigned int i, j;
	for (i = 0; i < ARRAY_SIZE(widths); i++) for (j = 0; j < ARRAY_SIZE(heights); j++) {
		unsigned int w = widths[i], h = heights[j];
		size_t y = ((w + 15) / 16 * 16) * ((h + 7) / 8 * 8);
		size_t uv = ((y / 2 + 63) / 64) * 64;
		size_t mv = ((w + 63) / 64) * ((h + 63) / 64) * 1024;
		ctx->src_fmt.width = w; ctx->src_fmt.height = h;
		assert(vp9_chroma_offset(w, h) == y);
		assert(vp9_mv_offset(w, h) == y + uv + 64);
		assert(th1520_vdec_vp9_native_size(ctx) == y + uv + 64 + mv);
	}
	ctx->src_fmt.width = 4096; ctx->src_fmt.height = 2304;
	assert(th1520_vdec_vp9_native_size(ctx) == 16515136);
}
static void test_stream(struct th1520_vdec_ctx *ctx) {
	struct v4l2_ctrl_vp9_frame frame, bad;
	struct v4l2_ctrl_vp9_compressed_hdr hdr = { .tx_mode = V4L2_VP9_TX_MODE_SELECT };
	struct vb2_buffer *src = &ctx->src->vb2_buf;
	unsigned int offset;
	good_frame(&frame, 128, 128);
	src->size = 4096; src->payload = 200; src->dma = 0x12340000;
	for (offset = 0; offset < 32; offset++) {
		src->planes[0].data_offset = offset;
		assert(!vp9_validate_frame(ctx, &frame, &hdr));
		ctx->vp9->frame = frame;
		vp9_config_stream(ctx);
		assert(field(ctx->dev, hevc_strm_start_offset) == (offset + 19) / 16 * 16);
		assert(field(ctx->dev, hevc_strm_start_bit) == (offset + 19) % 16 * 8);
		assert(field(ctx->dev, hevc_stream_len) == 200 - (offset + 19) / 16 * 16);
		assert(field(ctx->dev, hevc_strm_buffer_len) == 4096);
		assert(ctx->dev->regs[TH1520_HEVC_ADDR_STREAM + 1] == 0x12340000);
	}
	src->planes[0].data_offset = 19;
	src->payload = 38; assert(vp9_validate_frame(ctx, &frame, &hdr) == -EINVAL);
	src->payload = 18; assert(vp9_validate_frame(ctx, &frame, &hdr) == -EINVAL);
	src->payload = 4097; assert(vp9_validate_frame(ctx, &frame, &hdr) == -EINVAL);
	src->payload = 200;
	src->dma++; assert(vp9_validate_frame(ctx, &frame, &hdr) == -EINVAL); src->dma--;
	bad = frame; bad.compressed_header_size = 0; assert(vp9_validate_frame(ctx, &bad, &hdr) == -EINVAL);
	bad = frame; bad.frame_width_minus_1 = 128; assert(vp9_validate_frame(ctx, &bad, &hdr) == -EINVAL);
	bad = frame; bad.tile_cols_log2 = 1; assert(vp9_validate_frame(ctx, &bad, &hdr) == -EINVAL);
	bad = frame; bad.tile_rows_log2 = 3; assert(vp9_validate_frame(ctx, &bad, &hdr) == -EINVAL);
	bad = frame; bad.flags &= ~V4L2_VP9_FRAME_FLAG_KEY_FRAME; assert(vp9_validate_frame(ctx, &bad, &hdr) == -EINVAL);
}
static void test_segmentation(struct th1520_vdec_ctx *ctx) {
	struct th1520_vdec_vp9_ctx *v = ctx->vp9;
	struct v4l2_ctrl_vp9_frame *f = &v->frame;
	unsigned int i;
	good_frame(f, 128, 128); f->flags &= ~V4L2_VP9_FRAME_FLAG_KEY_FRAME;
	f->lf.level = 32;
	f->seg.flags = V4L2_VP9_SEGMENTATION_FLAG_ENABLED | V4L2_VP9_SEGMENTATION_FLAG_UPDATE_DATA | V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP;
	for (i = 0; i < 8; i++) {
		f->seg.feature_enabled[i] = 3;
		f->seg.feature_data[i][0] = i & 1 ? 255 : -255;
		f->seg.feature_data[i][1] = i & 1 ? 63 : -63;
	}
	v->last.valid = true; v->last.width = 256; v->last.height = 128;
	v->cur.width = 128; v->cur.height = 128;
	memset(v->segment_map.cpu, 0xa5, v->segment_map.size);
	ctx->dev->regs[14] = 0xabc00000;
	vp9_config_segmentation(ctx);
	for (i = 0; i < v->segment_map.size; i++) assert(((u8 *)v->segment_map.cpu)[i] == 0);
	for (i = 0; i < 8; i++) {
		u32 word = ctx->dev->regs[i < 6 ? 14 + i : 31 + i - 6];
		assert((word & 255) == (i & 1 ? 255 : 0));
		assert(((word >> 8) & 63) == (i & 1 ? 63 : 0));
	}
	assert((ctx->dev->regs[14] & 0xfffc0000) == 0xabc00000);
	assert(v->next_segment == 1 - v->active_segment);
	v->last.width = 128;
	memset(v->segment_map.cpu, 0x5a, v->segment_map.size);
	f->seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE;
	f->seg.feature_data[0][0] = 42; f->seg.feature_data[0][1] = 11;
	vp9_config_segmentation(ctx);
	assert((ctx->dev->regs[14] & 255) == 42);
	assert(((ctx->dev->regs[14] >> 8) & 63) == 11);
	assert(((u8 *)v->segment_map.cpu)[0] == 0x5a);
}
static void test_tiles(struct th1520_vdec_ctx *ctx) {
	struct th1520_vdec_vp9_ctx *v = ctx->vp9;
	const unsigned int heights[] = { 48, 64, 66, 128, 130, 192, 194, 2304 };
	unsigned int h, rows_log, col, row;
	for (h = 0; h < ARRAY_SIZE(heights); h++) for (rows_log = 0; rows_log <= 2; rows_log++) {
		u16 *table = v->tiles.cpu;
		unsigned int sum = 0;
		v->cur.width = 2048; v->cur.height = heights[h];
		v->frame.tile_cols_log2 = 3; v->frame.tile_rows_log2 = rows_log;
		vp9_config_tiles(ctx);
		for (row = 0; row < (1U << rows_log); row++) {
			unsigned int width_sum = 0;
			for (col = 0; col < 8; col++) width_sum += table[(row * 8 + col) * 2];
			assert(width_sum == 0 || width_sum == 32);
			sum += table[row * 16 + 1];
		}
		assert(sum == (heights[h] + 63) / 64);
		assert(field(ctx->dev, hevc_num_tile_cols_8k) == 8);
		for (row = (1U << rows_log) * 32; row < v->tiles.size; row++) assert(((u8 *)table)[row] == 0);
	}
}
static void test_refs(struct th1520_vdec_ctx *ctx) {
	struct th1520_vdec_buffer refs[3] = {0};
	struct th1520_vdec_vp9_ctx *v = ctx->vp9;
	struct vb2_queue *q = ctx->fh.m2m_ctx;
	unsigned int i;
	good_frame(&v->frame, 128, 128); v->frame.flags &= ~V4L2_VP9_FRAME_FLAG_KEY_FRAME;
	v->cur.width = v->cur.height = 128;
	v->frame.last_frame_ts = 10; v->frame.golden_frame_ts = 11; v->frame.alt_frame_ts = 12;
	q->num_buffers = 4; q->bufs[0] = &ctx->dst->vb2_buf;
	for (i = 0; i < 3; i++) {
		refs[i].vb.vb2_buf.timestamp = 10 + i;
		refs[i].native.cpu = refs; refs[i].native.dma = 0x100000 * (i + 1);
		refs[i].vp9.valid = true; refs[i].vp9.width = refs[i].vp9.height = 128;
		refs[i].vp9.stride = 512; refs[i].vp9.chroma_offset = 16384; refs[i].vp9.mv_offset = 24640;
		q->bufs[i + 1] = &refs[i].vb.vb2_buf;
	}
	assert(!vp9_config_refs(ctx));
	v->frame.last_frame_ts = ctx->dst->vb2_buf.timestamp; assert(vp9_config_refs(ctx) == -EINVAL);
	v->frame.last_frame_ts = 10; refs[0].vp9.valid = false; assert(vp9_config_refs(ctx) == -EINVAL);
	refs[0].vp9.valid = true; refs[0].vp9.width = 258; assert(vp9_config_refs(ctx) == -EINVAL);
	refs[0].vp9.width = 128; v->frame.alt_frame_ts = 999; assert(vp9_config_refs(ctx) == -EINVAL);
	q->num_buffers = 1;
}
static void set_hw_stream_position(struct th1520_vdec_dev *dev, u64 position) {
	/* Hardware readback is separate from the programmed shadow registers. */
	dev->mmio[TH1520_HEVC_ADDR_STREAM] = position >> 32;
	dev->mmio[TH1520_HEVC_ADDR_STREAM + 1] = position;
}
static void test_result_bounds(struct th1520_vdec_ctx *ctx) {
	const u64 bases[] = { 0x12340000ULL, 0x112340000ULL, 0x1fffffff0ULL };
	const unsigned int offsets[] = { 0, 13, 31 };
	struct th1520_vdec_vp9_ctx before;
	struct th1520_vdec_buffer *dst = th1520_vdec_vbuf_to_buffer(ctx->dst);
	struct vb2_buffer *src = &ctx->src->vb2_buf;
	dma_addr_t saved_dma = src->dma;
	u32 saved_offset = src->planes[0].data_offset;
	u8 *saved_mv = malloc(ctx->vp9->previous_mv.size);
	u8 *saved_segment_map = malloc(ctx->vp9->segment_map.size);
	unsigned int b, o, i, cases = 0;

	assert(saved_mv && saved_segment_map);
	assert(th1520_vdec_vp9_ops.check_result == th1520_vp9_check_result);
	memcpy(&before, ctx->vp9, sizeof(before));
	memcpy(saved_mv, ctx->vp9->previous_mv.cpu, ctx->vp9->previous_mv.size);
	memcpy(saved_segment_map, ctx->vp9->segment_map.cpu, ctx->vp9->segment_map.size);
	assert(!dst->vp9.valid && ctx->vp9->need_keyframe && !ctx->vp9->last.valid);
	for (b = 0; b < ARRAY_SIZE(bases); b++) for (o = 0; o < ARRAY_SIZE(offsets); o++) {
		u64 data_start = bases[b] + offsets[o] +
			ctx->vp9->frame.uncompressed_header_size + ctx->vp9->frame.compressed_header_size;
		u64 data_end = bases[b] + src->payload;
		const struct { u64 position; u32 status; int result; } checks[] = {
			{ data_start - 1, TH1520_IRQ_DEC_RDY_INT, -EIO },
			{ data_start, TH1520_IRQ_DEC_RDY_INT, 0 },
			{ data_start + 1, TH1520_IRQ_DEC_RDY_INT, 0 },
			{ data_end - 1, TH1520_IRQ_DEC_RDY_INT, 0 },
			{ data_end, TH1520_IRQ_DEC_RDY_INT, 0 },
			{ data_end + 1, TH1520_IRQ_DEC_RDY_INT, -EIO },
			{ data_end + 32, TH1520_IRQ_DEC_RDY_INT, -EIO },
			{ bases[b] + src->size - 1, TH1520_IRQ_DEC_RDY_INT, -EIO },
			{ data_end, TH1520_IRQ_DEC_RDY_INT | TH1520_IRQ_DEC_ASO_INT, -EIO },
		};

		src->dma = bases[b];
		src->planes[0].data_offset = offsets[o];
		for (i = 0; i < ARRAY_SIZE(checks); i++) {
			unsigned int reads = ctx->dev->mmio_reads;
			set_hw_stream_position(ctx->dev, checks[i].position);
			assert(th1520_vdec_vp9_ops.check_result(ctx, checks[i].status) == checks[i].result);
			assert(ctx->dev->mmio_reads - reads ==
			       (checks[i].status & TH1520_IRQ_DEC_ASO_INT ? 0 : 2));
			/* check_result observes only; done has not published any frame state. */
			assert(!memcmp(&before, ctx->vp9, sizeof(before)));
			assert(!memcmp(saved_mv, ctx->vp9->previous_mv.cpu, ctx->vp9->previous_mv.size));
			assert(!memcmp(saved_segment_map, ctx->vp9->segment_map.cpu, ctx->vp9->segment_map.size));
			assert(!dst->vp9.valid);
			cases++;
		}
	}
	src->dma = saved_dma;
	src->planes[0].data_offset = saved_offset;
	set_hw_stream_position(ctx->dev, saved_dma + src->payload);
	assert(!th1520_vdec_vp9_ops.check_result(ctx, TH1520_IRQ_DEC_RDY_INT));
	free(saved_segment_map);
	free(saved_mv);
	printf("PASS: VP9 result checks: %u bounds/ASO cases, 64-bit readback, state unpublished before done\n", cases);
}
static void test_job(struct th1520_vdec_ctx *ctx) {
	struct v4l2_ctrl_vp9_frame f;
	struct v4l2_ctrl_vp9_compressed_hdr h = {0};
	struct th1520_vdec_buffer *dst = th1520_vdec_vbuf_to_buffer(ctx->dst);
	struct v4l2_vp9_frame_context saved_contexts[4];
	struct v4l2_vp9_segmentation saved_segmentation;
	u8 saved_active_segment;
	unsigned int count;
	good_frame(&f, 128, 128);
	ctx->frame_ctrl = &f; ctx->header_ctrl = &h;
	th1520_vp9_abort(ctx);
	memcpy(saved_contexts, ctx->vp9->frame_context, sizeof(saved_contexts));
	memcpy(&saved_segmentation, &ctx->vp9->segmentation, sizeof(saved_segmentation));
	saved_active_segment = ctx->vp9->active_segment;
	memset(ctx->vp9->previous_mv.cpu, 0x3c, ctx->vp9->previous_mv.size);
	ctx->src->vb2_buf.payload = 200;
	ctx->src->vb2_buf.planes[0].data_offset = 19;
	dst->native.size = vp9_mv_offset(128, 128) + vp9_mv_size(128, 128);
	dst->native.cpu = calloc(1, dst->native.size); assert(dst->native.cpu);
	dst->native.dma = (uintptr_t)dst->native.cpu;
	/* A reused native buffer carries the previous job's sync bytes. */
	memset(dst->native.cpu, 0xa5, dst->native.size);
	count = ctx->ctrl_handler;
	assert(!th1520_vp9_run(ctx));
	assert(ctx->ctrl_handler == count + 1 && ctx->starts == 1 && !dst->vp9.valid);
	assert(ctx->vp9->need_keyframe && !ctx->vp9->last.valid);
	assert(!memcmp(saved_contexts, ctx->vp9->frame_context, sizeof(saved_contexts)));
	assert(!memcmp(&saved_segmentation, &ctx->vp9->segmentation, sizeof(saved_segmentation)));
	assert(saved_active_segment == ctx->vp9->active_segment);
	for (unsigned int i = 0; i < ctx->vp9->previous_mv.size; i++)
		assert(((u8 *)ctx->vp9->previous_mv.cpu)[i] == 0x3c);
	for (unsigned int i = dst->vp9.mv_offset - 32; i < dst->vp9.mv_offset; i++)
		assert(((u8 *)dst->native.cpu)[i] == 0);
	assert(((u8 *)dst->native.cpu)[dst->vp9.mv_offset - 33] == 0xa5);
	memset(dst->native.cpu + dst->vp9.mv_offset, 0x5a, vp9_mv_size(128, 128));
	test_result_bounds(ctx);
	th1520_vp9_done(ctx);
	assert(dst->vp9.valid && !ctx->vp9->need_keyframe && ctx->vp9->last.valid);
	assert(((u8 *)ctx->vp9->previous_mv.cpu)[0] == 0x5a);
	th1520_vp9_abort(ctx);
	assert(ctx->vp9->need_keyframe && !ctx->vp9->last.valid);
	ctx->frame_ctrl = NULL; count = ctx->ctrl_handler;
	assert(th1520_vp9_run(ctx) == -EINVAL && ctx->ctrl_handler == count + 1 && ctx->starts == 1);
	free(dst->native.cpu); memset(&dst->native, 0, sizeof(dst->native));
}
int main(void) {
	struct th1520_vdec_dev dev = {0};
	struct th1520_vdec_buffer src = {0}, dst = {0};
	struct vb2_queue q = {0};
	struct th1520_vdec_ctx ctx = { .dev = &dev, .src = &src.vb, .dst = &dst.vb, .fh.m2m_ctx = &q };
	dst.vb.vb2_buf.timestamp = 1;
	test_native_sizes(&ctx);
	test_init_failures(&ctx);
	test_stream(&ctx);
	test_segmentation(&ctx);
	test_tiles(&ctx);
	test_refs(&ctx);
	test_job(&ctx);
	th1520_vp9_exit(&ctx); th1520_vp9_exit(&ctx); th1520_vp9_abort(&ctx);
	assert(!ctx.vp9 && !live_dma && !live_ctx);
	puts("PASS: VP9 backend allocation failures, native sizes, payload bounds, result bounds, segmentation, tiles, references, and jobs");
	return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--kernel-tree", type=Path)
    source.add_argument("--kernel-archive", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--no-sanitize", action="store_true")
    args = parser.parse_args()
    driver = Path(__file__).resolve().parent.parent
    wanted = {"linux/v4l2-controls.h": "include/uapi/linux/v4l2-controls.h",
              "media/v4l2-vp9.h": "include/media/v4l2-vp9.h",
              "v4l2-vp9.c": "drivers/media/v4l2-core/v4l2-vp9.c"}
    if args.kernel_tree:
        upstream = {out: (args.kernel_tree / name).read_bytes() for out, name in wanted.items()}
    else:
        with tarfile.open(args.kernel_archive) as archive:
            members = archive.getmembers()
            upstream = {}
            for out, name in wanted.items():
                matches = [m for m in members if m.isfile() and m.name.endswith("/" + name)]
                if len(matches) != 1:
                    raise ValueError(f"Expected one upstream {name}, found {len(matches)}")
                upstream[out] = archive.extractfile(matches[0]).read()
    common = host.COMMON.replace("struct th1520_vdec_ctx { struct th1520_vdec_vp9_ctx *vp9; };", "")
    common = "#ifndef TEST_BACKEND_H\n#define TEST_BACKEND_H\n" + common + EXTRA + "\n#endif\n"
    (driver / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="vp9-backend-", dir=driver / "build") as name:
        build = Path(name)
        files = {
            "host.h": common.encode(), "linux/types.h": host.TYPES.encode(),
            "linux/const.h": b'#define _BITUL(x) (1UL << (x))\n#define _BITULL(x) (1ULL << (x))\n',
            "linux/bits.h": b'#define BIT(x) (1U << (x))\n#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))\n',
            "media/v4l2-ctrls.h": b'#include "host.h"\n#include <linux/v4l2-controls.h>\n',
            "linux/module.h": b'#include "host.h"\n',
            "linux/dma-mapping.h": b'#include "host.h"\n',
            "linux/slab.h": b'#include "host.h"\n',
            "media/videobuf2-dma-contig.h": b'#include "host.h"\n',
            "test.c": HARNESS.encode(), **upstream,
        }
        for relative, data in files.items():
            dest = build / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        binary = build / "vp9-backend-test"
        command = shlex.split(args.cc) + [
            "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-sign-compare",
            "-Wno-unused-parameter", "-Werror=implicit-function-declaration",
            "-fno-omit-frame-pointer", "-I", str(build), "-I", str(driver),
            "-include", str(build / "host.h"),
        ]
        if not args.no_sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        command += [str(build / "test.c"), str(driver / "th1520_vdec_vp9_probs.c"),
                    str(build / "v4l2-vp9.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
