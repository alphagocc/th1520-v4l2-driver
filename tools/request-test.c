// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standalone TH1520 stateless Request API test.
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * Build: cc -std=c11 -O2 -Wall -Wextra -Werror -o request-test tools/request-test.c
 * Run:   ./request-test --device /dev/video0 --media /dev/media0
 *        ./request-test --data-offset 13 --malformed
 *        ./request-test --workers 2 --iterations 20 --data-offset 13
 *        ./request-test --expect-timeout-once
 *        ./request-test --invalid-hevc-params
 *
 * --expect-timeout-once requires an independently prepared test module
 * that drops one acknowledged completion. It only checks the outcome;
 * this program does not enable or install fault injection in the driver.
 * --invalid-hevc-params checks seven invalid control cases with a placeholder
 * HEVC NAL. Run this mode after the HEVC parameter validation fixes are
 * installed; valid HEVC decoding requires a separate real-stream test.
 *
 * No codec library is needed. A 64x64 Baseline IDR consists of sixteen
 * I_PCM macroblocks with deblocking disabled. The samples are compared
 * exactly, including chroma order and emulation-prevention test bytes.
 *
 * UAPI source: Linux v6.6.140 include/uapi/linux/{media.h,v4l2-controls.h,
 * videodev2.h}, https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/
 * The test uses standard V4L2 ioctls and the Media Request API only.
 * H.264 syntax: ITU-T H.264 sections 7.3.2.1, 7.3.2.2, 7.3.3, 7.3.5,
 * 7.3.5.1; I-slice mb_type 25 is I_PCM. Only the IDR slice is submitted
 * to V4L2. --dump-stream also emits SPS/PPS for independent software checks.
 * The bit writer, generated sample patterns and request checks are original
 * test code for this repository; they require no hardware-specific library.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#define WIDTH 64U
#define HEIGHT 64U
#define STREAM_CAPACITY 16384U
#define MAX_DATA_OFFSET 65536U
#define DEFAULT_TIMEOUT_MS 6000U
#define MIN_WATCHDOG_ELAPSED_MS 1500U
#define MAX_WORKERS 16U

/* Each concurrent process uses a different, reproducible pixel pattern. */
static unsigned int pattern_seed;

struct bit_writer {
	uint8_t data[STREAM_CAPACITY];
	size_t bits;
};

struct nal {
	uint8_t data[STREAM_CAPACITY];
	size_t size;
	unsigned int prevention_bytes;
};

static uint8_t sample_y(unsigned int x, unsigned int y)
{
	static const uint8_t escape_pattern[] = { 0, 0, 0, 1, 0, 0, 2, 3 };

	if (!y && x < sizeof(escape_pattern))
		return escape_pattern[x];
	return (uint8_t)(17 + 3 * x + 5 * y + pattern_seed);
}

static uint8_t sample_cb(unsigned int x, unsigned int y)
{
	return (uint8_t)(3 + 7 * x + 11 * y + 3 * pattern_seed);
}

static uint8_t sample_cr(unsigned int x, unsigned int y)
{
	return (uint8_t)(97 + 13 * x + 9 * y + 5 * pattern_seed);
}

static void put_bits(struct bit_writer *w, uint32_t value, unsigned int count)
{
	unsigned int i;

	if (count > 32 || w->bits + count > sizeof(w->data) * 8) {
		fprintf(stderr, "internal bitstream capacity exceeded\n");
		exit(EXIT_FAILURE);
	}
	for (i = count; i; i--, w->bits++)
		w->data[w->bits / 8] |= ((value >> (i - 1)) & 1U) <<
					 (7 - w->bits % 8);
}

static void put_ue(struct bit_writer *w, uint32_t value)
{
	uint32_t code = value + 1, tmp = code;
	unsigned int leading_zeroes = 0;

	while (tmp >>= 1)
		leading_zeroes++;
	put_bits(w, 0, leading_zeroes);
	put_bits(w, code, leading_zeroes + 1);
}

static void put_se(struct bit_writer *w, int value)
{
	put_ue(w, value <= 0 ? (uint32_t)(-2 * value) :
			       (uint32_t)(2 * value - 1));
}

static void byte_align_zero(struct bit_writer *w)
{
	while (w->bits % 8)
		put_bits(w, 0, 1);
}

static void rbsp_trailing_bits(struct bit_writer *w)
{
	put_bits(w, 1, 1);
	byte_align_zero(w);
}

static void append_byte(struct nal *nal, uint8_t value)
{
	if (nal->size == sizeof(nal->data)) {
		fprintf(stderr, "internal NAL capacity exceeded\n");
		exit(EXIT_FAILURE);
	}
	nal->data[nal->size++] = value;
}

static void finish_nal(struct nal *nal, uint8_t header,
		       const struct bit_writer *w)
{
	unsigned int zeroes = 0;
	size_t i;

	memset(nal, 0, sizeof(*nal));
	append_byte(nal, 0);
	append_byte(nal, 0);
	append_byte(nal, 0);
	append_byte(nal, 1);
	append_byte(nal, header);
	for (i = 0; i < w->bits / 8; i++) {
		uint8_t value = w->data[i];

		if (zeroes == 2 && value <= 3) {
			append_byte(nal, 3);
			nal->prevention_bytes++;
			zeroes = 0;
		}
		append_byte(nal, value);
		zeroes = value ? 0 : zeroes + 1;
	}
}

static void make_sps(struct nal *nal)
{
	struct bit_writer w = { 0 };

	put_bits(&w, 66, 8);       /* profile_idc: Baseline */
	put_bits(&w, 0x80, 8);     /* constraint_set0_flag */
	put_bits(&w, 10, 8);       /* level_idc: level 1 */
	put_ue(&w, 0);            /* seq_parameter_set_id */
	put_ue(&w, 0);            /* log2_max_frame_num_minus4 */
	put_ue(&w, 2);            /* pic_order_cnt_type */
	put_ue(&w, 1);            /* max_num_ref_frames */
	put_bits(&w, 0, 1);       /* gaps_in_frame_num_value_allowed_flag */
	put_ue(&w, WIDTH / 16 - 1);
	put_ue(&w, HEIGHT / 16 - 1);
	put_bits(&w, 1, 1);       /* frame_mbs_only_flag */
	put_bits(&w, 1, 1);       /* direct_8x8_inference_flag */
	put_bits(&w, 0, 1);       /* frame_cropping_flag */
	put_bits(&w, 0, 1);       /* vui_parameters_present_flag */
	rbsp_trailing_bits(&w);
	finish_nal(nal, 0x67, &w);
}

static void make_pps(struct nal *nal)
{
	struct bit_writer w = { 0 };

	put_ue(&w, 0);            /* pic_parameter_set_id */
	put_ue(&w, 0);            /* seq_parameter_set_id */
	put_bits(&w, 0, 1);       /* entropy_coding_mode_flag: CAVLC */
	put_bits(&w, 0, 1);       /* bottom_field_pic_order_in_frame_present_flag */
	put_ue(&w, 0);            /* num_slice_groups_minus1 */
	put_ue(&w, 0);            /* num_ref_idx_l0_default_active_minus1 */
	put_ue(&w, 0);            /* num_ref_idx_l1_default_active_minus1 */
	put_bits(&w, 0, 1);       /* weighted_pred_flag */
	put_bits(&w, 0, 2);       /* weighted_bipred_idc */
	put_se(&w, 0);            /* pic_init_qp_minus26 */
	put_se(&w, 0);            /* pic_init_qs_minus26 */
	put_se(&w, 0);            /* chroma_qp_index_offset */
	put_bits(&w, 1, 1);       /* deblocking_filter_control_present_flag */
	put_bits(&w, 0, 1);       /* constrained_intra_pred_flag */
	put_bits(&w, 0, 1);       /* redundant_pic_cnt_present_flag */
	rbsp_trailing_bits(&w);
	finish_nal(nal, 0x68, &w);
}

static void make_idr(struct nal *nal)
{
	struct bit_writer w = { 0 };
	unsigned int mb_x, mb_y, x, y;

	put_ue(&w, 0);            /* first_mb_in_slice */
	put_ue(&w, 2);            /* slice_type: I */
	put_ue(&w, 0);            /* pic_parameter_set_id */
	put_bits(&w, 0, 4);       /* frame_num */
	put_ue(&w, 0);            /* idr_pic_id */
	/* pic_order_cnt_type=2 has no POC fields in the slice header. */
	put_bits(&w, 0, 1);       /* no_output_of_prior_pics_flag */
	put_bits(&w, 0, 1);       /* long_term_reference_flag */
	put_se(&w, 0);            /* slice_qp_delta */
	put_ue(&w, 1);            /* disable_deblocking_filter_idc */

	for (mb_y = 0; mb_y < HEIGHT / 16; mb_y++) {
		for (mb_x = 0; mb_x < WIDTH / 16; mb_x++) {
			put_ue(&w, 25); /* I_PCM */
			byte_align_zero(&w);
			for (y = 0; y < 16; y++)
				for (x = 0; x < 16; x++)
					put_bits(&w, sample_y(mb_x * 16 + x,
							      mb_y * 16 + y), 8);
			for (y = 0; y < 8; y++)
				for (x = 0; x < 8; x++)
					put_bits(&w, sample_cb(mb_x * 8 + x,
							       mb_y * 8 + y), 8);
			for (y = 0; y < 8; y++)
				for (x = 0; x < 8; x++)
					put_bits(&w, sample_cr(mb_x * 8 + x,
							       mb_y * 8 + y), 8);
		}
	}
	rbsp_trailing_bits(&w);
	finish_nal(nal, 0x65, &w);
}

static int dump_stream(const char *name, const struct nal *idr)
{
	struct nal sps, pps;
	FILE *f;
	bool ok;

	make_sps(&sps);
	make_pps(&pps);
	f = fopen(name, "wb");
	if (!f) {
		perror(name);
		return -1;
	}
	ok = fwrite(sps.data, 1, sps.size, f) == sps.size &&
	     fwrite(pps.data, 1, pps.size, f) == pps.size &&
	     fwrite(idr->data, 1, idr->size, f) == idr->size;
	if (fclose(f))
		ok = false;
	if (!ok) {
		fprintf(stderr, "failed to write %s\n", name);
		return -1;
	}
	printf("fixture: %s (%zu bytes, SPS/PPS/IDR)\n", name,
	       sps.size + pps.size + idr->size);
	return 0;
}

/* V4L2 device setup and request execution. */

enum hevc_invalid_case {
	HEVC_INVALID_MIN_CB,
	HEVC_INVALID_CB_DIFF,
	HEVC_INVALID_TILE_COUNTS,
	HEVC_INVALID_COLUMN_SUM,
	HEVC_INVALID_ROW_SUM,
	HEVC_INVALID_POC_COUNTS,
	HEVC_INVALID_SPS_SIZE,
	HEVC_INVALID_CASE_COUNT,
};

static const char * const hevc_invalid_case_names[] = {
	"hevc-min-cb-shift-255",
	"hevc-cb-diff-shift-255",
	"hevc-tiles-32x32",
	"hevc-nonuniform-column-5-of-4",
	"hevc-nonuniform-row-5-of-4",
	"hevc-poc-count-1-active-0",
	"hevc-sps-width-80-capture-64",
};

struct mapped_buffer {
	uint8_t *addr;
	size_t size;
};

struct test_context {
	int video_fd;
	int media_fd;
	int request_fd;
	bool output_streaming;
	bool capture_streaming;
	struct mapped_buffer output;
	struct mapped_buffer capture;
	struct v4l2_pix_format_mplane capture_format;
	uint32_t data_offset;
	unsigned int timeout_ms;
	unsigned int sequence;
	unsigned int worker;
	bool invalid_hevc_params;
	enum hevc_invalid_case hevc_case;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static int64_t monotonic_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_request(struct test_context *ctx)
{
	struct pollfd pfd = { .fd = ctx->request_fd, .events = POLLPRI };
	int64_t deadline = monotonic_ms() + ctx->timeout_ms;

	for (;;) {
		int64_t remaining = deadline - monotonic_ms();
		int ret;

		if (remaining <= 0) {
			fprintf(stderr, "request timeout after %u ms\n", ctx->timeout_ms);
			return -1;
		}
		ret = poll(&pfd, 1, (int)remaining);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0) {
			perror("poll request");
			return -1;
		}
		if (ret == 0)
			continue;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "request poll revents=0x%x\n", pfd.revents);
			return -1;
		}
		if (pfd.revents & POLLPRI)
			return 0;
	}
}

static int set_formats(struct test_context *ctx, size_t stream_size)
{
	bool hevc = ctx->invalid_hevc_params;
	uint32_t coded_format = hevc ? V4L2_PIX_FMT_HEVC_SLICE : V4L2_PIX_FMT_H264_SLICE;
	const char *coded_name = hevc ? "HEVC_SLICE" : "H264_SLICE";
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
	struct v4l2_ext_control controls[] = {
		{ .id = hevc ? V4L2_CID_STATELESS_HEVC_DECODE_MODE :
			       V4L2_CID_STATELESS_H264_DECODE_MODE,
		  .value = hevc ? V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED :
				  V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED },
		{ .id = hevc ? V4L2_CID_STATELESS_HEVC_START_CODE :
			       V4L2_CID_STATELESS_H264_START_CODE,
		  .value = hevc ? V4L2_STATELESS_HEVC_START_CODE_ANNEX_B :
				  V4L2_STATELESS_H264_START_CODE_ANNEX_B },
	};
	struct v4l2_ext_controls ext = {
		.which = V4L2_CTRL_WHICH_CUR_VAL,
		.count = sizeof(controls) / sizeof(controls[0]),
		.controls = controls,
	};

	fmt.fmt.pix_mp.width = WIDTH;
	fmt.fmt.pix_mp.height = HEIGHT;
	fmt.fmt.pix_mp.pixelformat = coded_format;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	fmt.fmt.pix_mp.num_planes = 1;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage =
		(uint32_t)((ctx->data_offset + stream_size + 64 + 4095) & ~4095U);
	if (xioctl(ctx->video_fd, VIDIOC_S_FMT, &fmt)) {
		perror("VIDIOC_S_FMT OUTPUT");
		return -1;
	}
	if (fmt.fmt.pix_mp.pixelformat != coded_format ||
	    fmt.fmt.pix_mp.width != WIDTH || fmt.fmt.pix_mp.height != HEIGHT ||
	    fmt.fmt.pix_mp.num_planes != 1 ||
	    fmt.fmt.pix_mp.plane_fmt[0].sizeimage < ctx->data_offset + stream_size) {
		fprintf(stderr, "driver did not accept the 64x64 %s format\n", coded_name);
		return -1;
	}
	printf("OUTPUT: %s %ux%u sizeimage=%u\n", coded_name, WIDTH, HEIGHT,
	       fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = WIDTH;
	fmt.fmt.pix_mp.height = HEIGHT;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (xioctl(ctx->video_fd, VIDIOC_S_FMT, &fmt)) {
		perror("VIDIOC_S_FMT CAPTURE");
		return -1;
	}
	if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
	    fmt.fmt.pix_mp.width != WIDTH || fmt.fmt.pix_mp.height != HEIGHT ||
	    fmt.fmt.pix_mp.num_planes != 1 ||
	    fmt.fmt.pix_mp.plane_fmt[0].bytesperline < WIDTH) {
		fprintf(stderr, "driver did not accept single-plane 64x64 NV12\n");
		return -1;
	}
	ctx->capture_format = fmt.fmt.pix_mp;
	printf("CAPTURE: %ux%u stride=%u sizeimage=%u\n", WIDTH, HEIGHT,
	       ctx->capture_format.plane_fmt[0].bytesperline,
	       ctx->capture_format.plane_fmt[0].sizeimage);

	if (xioctl(ctx->video_fd, VIDIOC_S_EXT_CTRLS, &ext)) {
		perror("VIDIOC_S_EXT_CTRLS decode mode/start code");
		return -1;
	}
	return 0;
}

static int map_queue(struct test_context *ctx, enum v4l2_buf_type type,
		     struct mapped_buffer *map)
{
	struct v4l2_requestbuffers req = {
		.count = 1, .type = type, .memory = V4L2_MEMORY_MMAP,
	};
	struct v4l2_plane plane = { 0 };
	struct v4l2_buffer buf = {
		.type = type, .memory = V4L2_MEMORY_MMAP,
		.index = 0, .length = 1, .m.planes = &plane,
	};
	void *addr;

	if (xioctl(ctx->video_fd, VIDIOC_REQBUFS, &req)) {
		perror("VIDIOC_REQBUFS");
		return -1;
	}
	if (!req.count) {
		fprintf(stderr, "VIDIOC_REQBUFS returned zero buffers\n");
		return -1;
	}
	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE &&
	    !(req.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS)) {
		fprintf(stderr, "OUTPUT queue does not advertise Request API support\n");
		return -1;
	}
	if (xioctl(ctx->video_fd, VIDIOC_QUERYBUF, &buf)) {
		perror("VIDIOC_QUERYBUF");
		return -1;
	}
	addr = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED,
		    ctx->video_fd, plane.m.mem_offset);
	if (addr == MAP_FAILED) {
		perror("mmap buffer");
		return -1;
	}
	map->addr = addr;
	map->size = plane.length;
	return 0;
}

static int set_h264_request_controls(struct test_context *ctx)
{
	struct v4l2_ctrl_h264_sps sps = {
		.profile_idc = 66,
		.constraint_set_flags = V4L2_H264_SPS_CONSTRAINT_SET0_FLAG,
		.level_idc = 10,
		.chroma_format_idc = 1,
		.pic_order_cnt_type = 2,
		.max_num_ref_frames = 1,
		.pic_width_in_mbs_minus1 = WIDTH / 16 - 1,
		.pic_height_in_map_units_minus1 = HEIGHT / 16 - 1,
		.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY |
			 V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE,
	};
	struct v4l2_ctrl_h264_pps pps = {
		.flags = V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT,
	};
	struct v4l2_ctrl_h264_scaling_matrix scaling;
	struct v4l2_ctrl_h264_decode_params decode = {
		.nal_ref_idc = 3,
		.dec_ref_pic_marking_bit_size = 2,
		.flags = V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC,
	};
	struct v4l2_ext_control controls[] = {
		{ .id = V4L2_CID_STATELESS_H264_SPS,
		  .size = sizeof(sps), .ptr = &sps },
		{ .id = V4L2_CID_STATELESS_H264_PPS,
		  .size = sizeof(pps), .ptr = &pps },
		{ .id = V4L2_CID_STATELESS_H264_SCALING_MATRIX,
		  .size = sizeof(scaling), .ptr = &scaling },
		{ .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
		  .size = sizeof(decode), .ptr = &decode },
	};
	struct v4l2_ext_controls ext = {
		.which = V4L2_CTRL_WHICH_REQUEST_VAL,
		.request_fd = ctx->request_fd,
		.count = sizeof(controls) / sizeof(controls[0]),
		.controls = controls,
	};

	memset(&scaling, 16, sizeof(scaling));
	if (xioctl(ctx->video_fd, VIDIOC_S_EXT_CTRLS, &ext)) {
		fprintf(stderr, "request controls: error_idx=%u: %s\n",
			ext.error_idx, strerror(errno));
		return -1;
	}
	return 0;
}

static int set_hevc_invalid_controls(struct test_context *ctx)
{
	/* The otherwise valid template has 16x16 CTBs and a 4x4 CTB grid. */
	struct v4l2_ctrl_hevc_sps sps = {
		.pic_width_in_luma_samples = WIDTH,
		.pic_height_in_luma_samples = HEIGHT,
		.chroma_format_idc = 1,
		.log2_max_pic_order_cnt_lsb_minus4 = 4,
		.log2_diff_max_min_luma_coding_block_size = 1,
		.log2_diff_max_min_luma_transform_block_size = 2,
	};
	struct v4l2_ctrl_hevc_pps pps = { 0 };
	struct v4l2_ctrl_hevc_scaling_matrix scaling;
	struct v4l2_ctrl_hevc_decode_params decode = {
		.flags = V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC |
			 V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC,
	};
	struct v4l2_ext_control controls[] = {
		{ .id = V4L2_CID_STATELESS_HEVC_SPS,
		  .size = sizeof(sps), .ptr = &sps },
		{ .id = V4L2_CID_STATELESS_HEVC_PPS,
		  .size = sizeof(pps), .ptr = &pps },
		{ .id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX,
		  .size = sizeof(scaling), .ptr = &scaling },
		{ .id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS,
		  .size = sizeof(decode), .ptr = &decode },
	};
	struct v4l2_ext_controls ext = {
		.which = V4L2_CTRL_WHICH_REQUEST_VAL,
		.request_fd = ctx->request_fd,
		.count = sizeof(controls) / sizeof(controls[0]),
		.controls = controls,
	};

	switch (ctx->hevc_case) {
	case HEVC_INVALID_MIN_CB:
		sps.log2_min_luma_coding_block_size_minus3 = 255;
		break;
	case HEVC_INVALID_CB_DIFF:
		sps.log2_diff_max_min_luma_coding_block_size = 255;
		break;
	case HEVC_INVALID_TILE_COUNTS:
		pps.flags = V4L2_HEVC_PPS_FLAG_TILES_ENABLED |
			    V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING;
		pps.num_tile_columns_minus1 = 31;
		pps.num_tile_rows_minus1 = 31;
		break;
	case HEVC_INVALID_COLUMN_SUM:
		pps.flags = V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
		pps.num_tile_columns_minus1 = 1;
		pps.column_width_minus1[0] = 4;
		break;
	case HEVC_INVALID_ROW_SUM:
		pps.flags = V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
		pps.num_tile_rows_minus1 = 1;
		pps.row_height_minus1[0] = 4;
		break;
	case HEVC_INVALID_POC_COUNTS:
		decode.num_poc_st_curr_before = 1;
		break;
	case HEVC_INVALID_SPS_SIZE:
		sps.pic_width_in_luma_samples = WIDTH + 16;
		break;
	default:
		fprintf(stderr, "unknown HEVC invalid-parameter case\n");
		return -1;
	}
	memset(&scaling, 16, sizeof(scaling));
	if (xioctl(ctx->video_fd, VIDIOC_S_EXT_CTRLS, &ext)) {
		fprintf(stderr, "HEVC request controls: error_idx=%u: %s\n",
			ext.error_idx, strerror(errno));
		return -1;
	}
	return 0;
}

static unsigned int compare_sample(const char *plane, unsigned int x,
				   unsigned int y, uint8_t got, uint8_t want,
				   unsigned int mismatches)
{
	if (got == want)
		return 0;
	if (mismatches < 8)
		fprintf(stderr, "%s[%u,%u]: got=%u expected=%u\n",
			plane, x, y, got, want);
	return 1;
}

static int check_nv12(struct test_context *ctx, const struct v4l2_plane *plane)
{
	size_t stride = ctx->capture_format.plane_fmt[0].bytesperline;
	size_t image_size = stride * HEIGHT * 3 / 2;
	const uint8_t *data;
	unsigned int x, y, mismatches = 0;
	uint32_t checksum = 2166136261U;

	if (plane->data_offset > plane->bytesused ||
	    plane->bytesused > ctx->capture.size ||
	    plane->bytesused - plane->data_offset != image_size) {
		fprintf(stderr, "NV12 payload: bytesused=%u offset=%u expected=%zu map=%zu\n",
			plane->bytesused, plane->data_offset, image_size, ctx->capture.size);
		return -1;
	}
	data = ctx->capture.addr + plane->data_offset;
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH; x++) {
			uint8_t got = data[y * stride + x];

			mismatches += compare_sample("Y", x, y, got,
						     sample_y(x, y), mismatches);
			checksum = (checksum ^ got) * 16777619U;
		}
	}
	data += stride * HEIGHT;
	for (y = 0; y < HEIGHT / 2; y++) {
		for (x = 0; x < WIDTH / 2; x++) {
			uint8_t cb = data[y * stride + x * 2];
			uint8_t cr = data[y * stride + x * 2 + 1];

			mismatches += compare_sample("Cb", x, y, cb,
						     sample_cb(x, y), mismatches);
			mismatches += compare_sample("Cr", x, y, cr,
						     sample_cr(x, y), mismatches);
			checksum = (checksum ^ cb) * 16777619U;
			checksum = (checksum ^ cr) * 16777619U;
		}
	}
	printf("pixels: worker=%u seed=%u checked=%u mismatches=%u fnv1a=%08"
	       PRIx32 "\n", ctx->worker, pattern_seed,
	       WIDTH * HEIGHT * 3 / 2, mismatches, checksum);
	return mismatches ? -1 : 0;
}

enum frame_test {
	FRAME_VALID,
	FRAME_TRUNCATED,
	FRAME_TIMEOUT,
	FRAME_INVALID_HEVC,
};

/* Return 1 for a completed but failed check, -1 for a transport failure. */
static int run_frame(struct test_context *ctx, const struct nal *idr,
		     const char *label, enum frame_test test)
{
	static const uint8_t bad_prefix[] = { 0, 0, 1, 0x65, 0, 0, 0, 0x80 };
	struct v4l2_plane out_plane = { 0 }, cap_plane = { 0 };
	struct v4l2_buffer out_buf = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP, .index = 0,
		.length = 1, .m.planes = &out_plane,
		.field = V4L2_FIELD_NONE,
		.flags = V4L2_BUF_FLAG_REQUEST_FD | V4L2_BUF_FLAG_KEYFRAME,
		.request_fd = ctx->request_fd,
	};
	struct v4l2_buffer cap_buf = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP, .index = 0,
		.length = 1, .m.planes = &cap_plane,
	};
	size_t length = test == FRAME_TRUNCATED ? 17 : idr->size;
	int64_t start, elapsed;
	int failed = 0;

	if (ctx->data_offset + length > ctx->output.size) {
		fprintf(stderr, "OUTPUT buffer is too small\n");
		return -1;
	}
	memset(ctx->output.addr, 0, ctx->output.size);
	memset(ctx->output.addr, 0xa5, ctx->data_offset);
	if (ctx->data_offset >= sizeof(bad_prefix))
		memcpy(ctx->output.addr, bad_prefix, sizeof(bad_prefix));
	memcpy(ctx->output.addr + ctx->data_offset, idr->data, length);
	/* A previous correct frame must not hide an unwritten output plane. */
	memset(ctx->capture.addr, 0xa5, ctx->capture.size);

	if (test == FRAME_INVALID_HEVC ? set_hevc_invalid_controls(ctx) :
					 set_h264_request_controls(ctx))
		return -1;
	cap_plane.length = (uint32_t)ctx->capture.size;
	if (xioctl(ctx->video_fd, VIDIOC_QBUF, &cap_buf)) {
		perror("VIDIOC_QBUF CAPTURE");
		return -1;
	}
	out_plane.length = (uint32_t)ctx->output.size;
	out_plane.bytesused = ctx->data_offset + (uint32_t)length;
	out_plane.data_offset = ctx->data_offset;
	out_buf.timestamp.tv_sec = ++ctx->sequence;
	if (xioctl(ctx->video_fd, VIDIOC_QBUF, &out_buf)) {
		perror("VIDIOC_QBUF OUTPUT request");
		return -1;
	}
	start = monotonic_ms();
	if (xioctl(ctx->request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL)) {
		perror("MEDIA_REQUEST_IOC_QUEUE");
		return -1;
	}
	if (wait_request(ctx))
		return -1;

	/* Request completion must follow CAPTURE completion for this frame. */
	memset(&cap_plane, 0, sizeof(cap_plane));
	memset(&out_plane, 0, sizeof(out_plane));
	cap_buf.flags = 0;
	out_buf.flags = 0;
	out_buf.request_fd = 0;
	if (xioctl(ctx->video_fd, VIDIOC_DQBUF, &cap_buf)) {
		perror("VIDIOC_DQBUF CAPTURE after request completion");
		return -1;
	}
	if (xioctl(ctx->video_fd, VIDIOC_DQBUF, &out_buf)) {
		perror("VIDIOC_DQBUF OUTPUT after request completion");
		return -1;
	}
	elapsed = monotonic_ms() - start;
	printf("%s: input=%zu data_offset=%u elapsed=%" PRId64
	       "ms src_flags=0x%08x cap_flags=0x%08x cap_bytesused=%u\n",
	       label, length, ctx->data_offset, elapsed,
	       out_buf.flags, cap_buf.flags, cap_plane.bytesused);
	if (out_buf.index || cap_buf.index ||
	    cap_buf.timestamp.tv_sec != (long)ctx->sequence ||
	    cap_buf.timestamp.tv_usec != 0) {
		fprintf(stderr, "%s: buffer index/timestamp mismatch\n", label);
		failed = 1;
	}
	if (test != FRAME_VALID) {
		if (!(cap_buf.flags & V4L2_BUF_FLAG_ERROR)) {
			fprintf(stderr, "%s: expected V4L2_BUF_FLAG_ERROR on CAPTURE\n", label);
			failed = 1;
		}
	} else if ((cap_buf.flags | out_buf.flags) & V4L2_BUF_FLAG_ERROR) {
		fprintf(stderr, "%s: valid frame returned V4L2_BUF_FLAG_ERROR\n", label);
		failed = 1;
	} else if (check_nv12(ctx, &cap_plane)) {
		failed = 1;
	}
	if (test == FRAME_TIMEOUT || test == FRAME_INVALID_HEVC) {
		if (!(out_buf.flags & V4L2_BUF_FLAG_ERROR)) {
			fprintf(stderr, "%s: expected V4L2_BUF_FLAG_ERROR on OUTPUT\n", label);
			failed = 1;
		}
	}
	if (test == FRAME_TIMEOUT) {
		if (elapsed < MIN_WATCHDOG_ELAPSED_MS ||
		    elapsed >= ctx->timeout_ms) {
			fprintf(stderr, "%s: expected watchdog completion in [%u,%u) ms, "
				"observed=%" PRId64 " ms\n", label,
				MIN_WATCHDOG_ELAPSED_MS, ctx->timeout_ms, elapsed);
			failed = 1;
		}
	}
	if (xioctl(ctx->request_fd, MEDIA_REQUEST_IOC_REINIT, NULL)) {
		perror("MEDIA_REQUEST_IOC_REINIT");
		return -1;
	}
	printf("%s: %s\n", label, failed ? "FAIL" : "PASS");
	return failed;
}

static void cleanup(struct test_context *ctx)
{
	enum v4l2_buf_type type;

	if (ctx->output_streaming) {
		type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		if (xioctl(ctx->video_fd, VIDIOC_STREAMOFF, &type))
			perror("VIDIOC_STREAMOFF OUTPUT");
	}
	if (ctx->capture_streaming) {
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		if (xioctl(ctx->video_fd, VIDIOC_STREAMOFF, &type))
			perror("VIDIOC_STREAMOFF CAPTURE");
	}
	if (ctx->output.addr)
		munmap(ctx->output.addr, ctx->output.size);
	if (ctx->capture.addr)
		munmap(ctx->capture.addr, ctx->capture.size);
	if (ctx->request_fd >= 0)
		close(ctx->request_fd);
	if (ctx->video_fd >= 0)
		close(ctx->video_fd);
	if (ctx->media_fd >= 0)
		close(ctx->media_fd);
}

static int run_device_test(struct test_context *ctx, const char *video,
			   const char *media, const struct nal *idr,
			   unsigned int iterations, bool malformed,
			   bool expect_timeout_once)
{
	static const char * const phases[] = { "normal", "truncated", "recovery" };
	static const char * const timeout_phases[] = { "timeout-normal", "timeout-recovery" };
	struct v4l2_capability cap = { 0 };
	enum v4l2_buf_type type;
	uint32_t capabilities;
	unsigned int round, phase;
	int ret, failed = 0, result = EXIT_FAILURE;

	printf("worker=%u pid=%ld seed=%u iterations=%u\n",
	       ctx->worker, (long)getpid(), pattern_seed, iterations);
	ctx->video_fd = open(video, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (ctx->video_fd < 0) {
		perror(video);
		goto out;
	}
	ctx->media_fd = open(media, O_RDWR | O_CLOEXEC);
	if (ctx->media_fd < 0) {
		perror(media);
		goto out;
	}
	if (xioctl(ctx->video_fd, VIDIOC_QUERYCAP, &cap)) {
		perror("VIDIOC_QUERYCAP");
		goto out;
	}
	capabilities = cap.capabilities & V4L2_CAP_DEVICE_CAPS ?
		       cap.device_caps : cap.capabilities;
	if ((capabilities & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING)) !=
	    (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING)) {
		fprintf(stderr, "device lacks streaming M2M MPLANE capabilities\n");
		goto out;
	}
	printf("device: %s driver=%s media=%s worker=%u\n", video,
	       (const char *)cap.driver, media, ctx->worker);
	if (set_formats(ctx, idr->size) ||
	    map_queue(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &ctx->output) ||
	    map_queue(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &ctx->capture))
		goto out;
	if (xioctl(ctx->media_fd, MEDIA_IOC_REQUEST_ALLOC, &ctx->request_fd)) {
		perror("MEDIA_IOC_REQUEST_ALLOC");
		goto out;
	}
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (xioctl(ctx->video_fd, VIDIOC_STREAMON, &type)) {
		perror("VIDIOC_STREAMON OUTPUT");
		goto out;
	}
	ctx->output_streaming = true;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(ctx->video_fd, VIDIOC_STREAMON, &type)) {
		perror("VIDIOC_STREAMON CAPTURE");
		goto out;
	}
	ctx->capture_streaming = true;

	if (expect_timeout_once)
		printf("watchdog acceptance: valid first frame must return ERROR in "
		       "[%u,%u) ms; next valid frame must match every NV12 sample "
		       "in the same context\n", MIN_WATCHDOG_ELAPSED_MS, ctx->timeout_ms);
	if (ctx->invalid_hevc_params)
		printf("HEVC invalid-parameter acceptance: %u cases, "
		       "each request must return ERROR on both queues within %u ms\n",
		       (unsigned int)HEVC_INVALID_CASE_COUNT, ctx->timeout_ms);
	for (round = 1; round <= iterations; round++) {
		bool timeout_round = expect_timeout_once && round == 1;
		unsigned int phase_count = timeout_round ? 2 : (malformed ? 3 : 1);

		if (ctx->invalid_hevc_params)
			phase_count = HEVC_INVALID_CASE_COUNT;
		for (phase = 0; phase < phase_count; phase++) {
			char label[96];
			enum frame_test test = FRAME_VALID;
			const char *name;

			if (ctx->invalid_hevc_params) {
				ctx->hevc_case = (enum hevc_invalid_case)phase;
				name = hevc_invalid_case_names[phase];
				test = FRAME_INVALID_HEVC;
			} else {
				name = timeout_round ? timeout_phases[phase] : phases[phase];
			}
			if (timeout_round && phase == 0)
				test = FRAME_TIMEOUT;
			else if (!ctx->invalid_hevc_params && !timeout_round && phase == 1)
				test = FRAME_TRUNCATED;

			snprintf(label, sizeof(label), "worker=%u round=%u %s",
				 ctx->worker, round, name);
			ret = run_frame(ctx, idr, label, test);
			if (ret < 0)
				goto out;
			failed |= ret;
		}
	}
	result = failed ? EXIT_FAILURE : EXIT_SUCCESS;
out:
	cleanup(ctx);
	printf("request-test worker=%u: %s\n", ctx->worker,
	       result == EXIT_SUCCESS ? "PASS" : "FAIL");
	return result;
}

static int run_workers(struct test_context *ctx, const char *video,
		       const char *media, unsigned int iterations,
		       unsigned int workers, bool malformed)
{
	pid_t pids[MAX_WORKERS];
	unsigned int i, count = 0;
	int gate[2], failed = 0;

	if (pipe(gate)) {
		perror("pipe worker start gate");
		return EXIT_FAILURE;
	}
	fflush(NULL);
	for (i = 0; i < workers; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork worker");
			failed = 1;
			break;
		}
		if (!pid) {
			struct nal idr;
			char token;
			ssize_t ready;
			int result;

			close(gate[1]);
			do {
				ready = read(gate[0], &token, 1);
			} while (ready < 0 && errno == EINTR);
			close(gate[0]);
			if (ready < 0) {
				perror("read worker start gate");
				_exit(EXIT_FAILURE);
			}
			ctx->worker = i;
			pattern_seed = (pattern_seed + i) & 255;
			make_idr(&idr);
			result = run_device_test(ctx, video, media, &idr,
						 iterations, malformed, false);
			fflush(NULL);
			_exit(result);
		}
		pids[count++] = pid;
	}
	close(gate[0]);
	/* EOF releases all children only after the last process was created. */
	close(gate[1]);
	for (i = 0; i < count; i++) {
		int status;
		pid_t ret;

		do {
			ret = waitpid(pids[i], &status, 0);
		} while (ret < 0 && errno == EINTR);
		if (ret < 0) {
			perror("waitpid worker");
			failed = 1;
		} else if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			fprintf(stderr, "worker=%u pid=%ld failed, wait_status=0x%x\n",
				i, (long)pids[i], status);
			failed = 1;
		}
	}
	printf("request-test workers=%u: %s\n", count, failed ? "FAIL" : "PASS");
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void usage(FILE *f, const char *program)
{
	fprintf(f,
		"Usage: %s [options]\n"
		"  --device FILE       V4L2 decoder (default /dev/video0)\n"
		"  --media FILE        associated media device (default /dev/media0)\n"
		"  --data-offset N     OUTPUT plane offset, 0..65536 (default 0)\n"
		"  --malformed         valid frame, truncated frame, then recovery frame\n"
		"  --invalid-hevc-params check seven rejected HEVC control requests\n"
		"                      use after parameter-validation fixes; one worker\n"
		"  --expect-timeout-once first valid frame must reach watchdog, then recover\n"
		"                      single worker; excludes --malformed/--generate-only\n"
		"  --timeout-ms N      request poll timeout, 50..60000 (default 6000)\n"
		"  --iterations N      repeat each worker's checks, 1..10000 (default 1)\n"
		"  --workers N         concurrent decoder contexts, 1..16 (default 1)\n"
		"  --pattern-seed N    pixel pattern seed, 0..255 (default 0)\n"
		"  --dump-stream FILE  save standalone SPS/PPS/IDR Annex-B fixture\n"
		"  --generate-only     save fixture without opening devices\n"
		"  --help              show this text\n",
		program);
}

static bool parse_uint(const char *text, unsigned int min, unsigned int max,
		       unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 0);
	if (errno || !*text || *text == '-' || *end || parsed < min || parsed > max)
		return false;
	*value = (unsigned int)parsed;
	return true;
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "media", required_argument, NULL, 'm' },
		{ "data-offset", required_argument, NULL, 'o' },
		{ "malformed", no_argument, NULL, 'e' },
		{ "invalid-hevc-params", no_argument, NULL, 'H' },
		{ "expect-timeout-once", no_argument, NULL, 'T' },
		{ "timeout-ms", required_argument, NULL, 't' },
		{ "iterations", required_argument, NULL, 'i' },
		{ "workers", required_argument, NULL, 'w' },
		{ "pattern-seed", required_argument, NULL, 'p' },
		{ "dump-stream", required_argument, NULL, 's' },
		{ "generate-only", no_argument, NULL, 'g' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct test_context ctx = {
		.video_fd = -1, .media_fd = -1, .request_fd = -1,
		.timeout_ms = DEFAULT_TIMEOUT_MS,
	};
	const char *video = "/dev/video0", *media = "/dev/media0", *dump = NULL;
	bool malformed = false, generate_only = false;
	bool expect_timeout_once = false;
	struct nal idr;
	unsigned int value;
	unsigned int iterations = 1, workers = 1;
	int opt;

	while ((opt = getopt_long(argc, argv, "", options, NULL)) != -1) {
		switch (opt) {
		case 'd': video = optarg; break;
		case 'm': media = optarg; break;
		case 'o':
			if (!parse_uint(optarg, 0, MAX_DATA_OFFSET, &value))
				goto bad_option;
			ctx.data_offset = value;
			break;
		case 'e': malformed = true; break;
		case 'H': ctx.invalid_hevc_params = true; break;
		case 'T': expect_timeout_once = true; break;
		case 't':
			if (!parse_uint(optarg, 50, 60000, &value))
				goto bad_option;
			ctx.timeout_ms = value;
			break;
		case 'i':
			if (!parse_uint(optarg, 1, 10000, &iterations))
				goto bad_option;
			break;
		case 'w':
			if (!parse_uint(optarg, 1, MAX_WORKERS, &workers))
				goto bad_option;
			break;
		case 'p':
			if (!parse_uint(optarg, 0, 255, &pattern_seed))
				goto bad_option;
			break;
		case 's': dump = optarg; break;
		case 'g': generate_only = true; break;
		case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
		default: goto bad_option;
		}
	}
	if (optind != argc || (generate_only && !dump))
		goto bad_option;
	if (ctx.invalid_hevc_params &&
	    (malformed || expect_timeout_once || workers != 1 || generate_only || dump)) {
		fprintf(stderr, "--invalid-hevc-params requires one worker and excludes "
			"--malformed, --expect-timeout-once, --dump-stream and --generate-only\n");
		goto bad_option;
	}
	if (expect_timeout_once &&
	    (malformed || workers != 1 || generate_only ||
	     ctx.timeout_ms <= MIN_WATCHDOG_ELAPSED_MS)) {
		fprintf(stderr, "--expect-timeout-once requires one worker, "
			"--timeout-ms greater than %u, and excludes --malformed "
			"and --generate-only\n", MIN_WATCHDOG_ELAPSED_MS);
		goto bad_option;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (ctx.invalid_hevc_params) {
		static const uint8_t placeholder[] = { 0, 0, 0, 1, 0x26, 1, 0x80 };

		memset(&idr, 0, sizeof(idr));
		memcpy(idr.data, placeholder, sizeof(placeholder));
		idr.size = sizeof(placeholder);
		printf("HEVC invalid-parameter probes: %ux%u, placeholder NAL=%zu bytes\n",
		       WIDTH, HEIGHT, idr.size);
	} else {
		make_idr(&idr);
		printf("H.264 fixture: %ux%u Baseline IDR, 16 I_PCM macroblocks, "
		       "slice=%zu bytes, emulation_prevention=%u, seed=%u\n",
		       WIDTH, HEIGHT, idr.size, idr.prevention_bytes, pattern_seed);
	}
	if (dump && dump_stream(dump, &idr))
		return EXIT_FAILURE;
	if (generate_only)
		return EXIT_SUCCESS;

	if (workers > 1)
		return run_workers(&ctx, video, media, iterations, workers, malformed);
	return run_device_test(&ctx, video, media, &idr, iterations, malformed,
			       expect_timeout_once);
bad_option:
	usage(stderr, argv[0]);
	return EXIT_FAILURE;
}
