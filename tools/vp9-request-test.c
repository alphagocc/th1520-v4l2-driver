// SPDX-License-Identifier: GPL-2.0-only
/*
 * TH1520 VP9 control and Request API negative tests.
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * Build: cc -std=c11 -O2 -Wall -Wextra -Werror -o vp9-request-test \
 *           tools/vp9-request-test.c
 * Run:   ./vp9-request-test --device /dev/video0 --media /dev/media0
 *
 * Uses standard Linux 6.6 V4L2 UAPI only. The payload is a placeholder:
 * every submitted frame deliberately fails software parameter validation.
 * Passing this program does not establish hardware decode, pixel accuracy,
 * probability/reference correctness, or hardware timeout recovery.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#define WIDTH 64U
#define HEIGHT 64U
#define PAYLOAD_SIZE 64U
#define DATA_OFFSET 13U
#define HAVE_FRAME 1U
#define HAVE_HEADER 2U
#define HAVE_BOTH (HAVE_FRAME | HAVE_HEADER)

struct mapped_buffer {
	void *addr;
	uint32_t size;
};

struct test_context {
	int video_fd, media_fd, request_fd;
	bool output_streaming, capture_streaming;
	struct mapped_buffer output, capture;
	unsigned int sequence, timeout_ms, checks;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static int checked_ioctl(int fd, unsigned long request, void *arg,
			 const char *name)
{
	if (!xioctl(fd, request, arg))
		return 0;
	perror(name);
	return -1;
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
	int64_t deadline = monotonic_ms() + ctx->timeout_ms;
	struct pollfd pfd = { .fd = ctx->request_fd, .events = POLLPRI };

	for (;;) {
		int64_t remaining = deadline - monotonic_ms();
		int ret;

		if (remaining <= 0) {
			fprintf(stderr, "request completion timed out after %u ms\n",
				ctx->timeout_ms);
			return -1;
		}
		ret = poll(&pfd, 1, (int)remaining);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0) {
			perror("poll request");
			return -1;
		}
		if (!ret)
			continue;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "request poll events=0x%x\n", pfd.revents);
			return -1;
		}
		if (pfd.revents & POLLPRI)
			return 0;
	}
}

static struct v4l2_ctrl_vp9_frame base_frame(void)
{
	struct v4l2_ctrl_vp9_frame frame = {
		.flags = V4L2_VP9_FRAME_FLAG_KEY_FRAME |
			 V4L2_VP9_FRAME_FLAG_SHOW_FRAME |
			 V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING |
			 V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING,
		.uncompressed_header_size = 1,
		.compressed_header_size = 1,
		.frame_width_minus_1 = WIDTH - 1,
		.frame_height_minus_1 = HEIGHT - 1,
		.render_width_minus_1 = WIDTH - 1,
		.render_height_minus_1 = HEIGHT - 1,
		.bit_depth = 8,
	};

	return frame;
}

static int frame_control_ioctl(struct test_context *ctx,
			       unsigned long cmd,
			       struct v4l2_ctrl_vp9_frame *frame)
{
	struct v4l2_ext_control control = {
		.id = V4L2_CID_STATELESS_VP9_FRAME,
		.size = sizeof(*frame), .ptr = frame,
	};
	struct v4l2_ext_controls ext = {
		.which = V4L2_CTRL_WHICH_CUR_VAL,
		.count = 1, .controls = &control,
	};

	return xioctl(ctx->video_fd, cmd, &ext);
}

static int test_controls(struct test_context *ctx)
{
	static const char * const names[] = {
		"profile-1-422", "profile-2-bit-depth-10", "without-x-subsampling",
		"without-y-subsampling", "odd-width-63", "odd-height-63",
	};
	struct v4l2_ctrl_vp9_frame frame = base_frame();
	unsigned int i, stage;

	if (frame_control_ioctl(ctx, VIDIOC_TRY_EXT_CTRLS, &frame) ||
	    frame_control_ioctl(ctx, VIDIOC_S_EXT_CTRLS, &frame)) {
		perror("valid VP9 FRAME controls");
		return -1;
	}
	printf("control-baseline: PASS (TRY and S_EXT_CTRLS)\n");
	ctx->checks++;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		for (stage = 0; stage < 2; stage++) {
			unsigned long cmd = stage ? VIDIOC_S_EXT_CTRLS :
						   VIDIOC_TRY_EXT_CTRLS;
			int ret, error;

			frame = base_frame();
			switch (i) {
			case 0:
				frame.profile = 1;
				frame.flags &= ~V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;
				break;
			case 1: frame.profile = 2; frame.bit_depth = 10; break;
			case 2: frame.flags &= ~V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING; break;
			case 3: frame.flags &= ~V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING; break;
			case 4: frame.frame_width_minus_1 = 62; break;
			case 5: frame.frame_height_minus_1 = 62; break;
			}
			errno = 0;
			ret = frame_control_ioctl(ctx, cmd, &frame);
			error = errno;
			if (ret != -1 || error != EINVAL) {
				fprintf(stderr, "%s %s: FAIL ret=%d errno=%d, expected EINVAL\n",
					names[i], stage ? "S" : "TRY", ret, error);
				return -1;
			}
		}
		printf("control-%s: PASS (TRY and S_EXT_CTRLS: EINVAL)\n", names[i]);
		ctx->checks++;
	}
	return 0;
}

static int set_request_controls(struct test_context *ctx,
				struct v4l2_ctrl_vp9_frame *frame,
				unsigned int presence)
{
	struct v4l2_ctrl_vp9_compressed_hdr header = { 0 };
	struct v4l2_ext_control controls[2];
	struct v4l2_ext_controls ext = {
		.which = V4L2_CTRL_WHICH_REQUEST_VAL,
		.request_fd = ctx->request_fd, .controls = controls,
	};

	memset(controls, 0, sizeof(controls));
	if (presence & HAVE_FRAME)
		controls[ext.count++] = (struct v4l2_ext_control) {
			.id = V4L2_CID_STATELESS_VP9_FRAME,
			.size = sizeof(*frame), .ptr = frame,
		};
	if (presence & HAVE_HEADER)
		controls[ext.count++] = (struct v4l2_ext_control) {
			.id = V4L2_CID_STATELESS_VP9_COMPRESSED_HDR,
			.size = sizeof(header), .ptr = &header,
		};
	if (!ext.count)
		return 0;
	return checked_ioctl(ctx->video_fd, VIDIOC_S_EXT_CTRLS, &ext,
			     "S_EXT_CTRLS request");
}

static int queue_output(struct test_context *ctx)
{
	struct v4l2_plane plane = {
		.length = ctx->output.size,
		.bytesused = DATA_OFFSET + PAYLOAD_SIZE,
		.data_offset = DATA_OFFSET,
	};
	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP, .index = 0,
		.length = 1, .m.planes = &plane,
		.field = V4L2_FIELD_NONE,
		.flags = V4L2_BUF_FLAG_REQUEST_FD,
		.request_fd = ctx->request_fd,
	};

	buf.timestamp.tv_sec = ++ctx->sequence;
	memset(ctx->output.addr, 0xa5, ctx->output.size);
	return checked_ioctl(ctx->video_fd, VIDIOC_QBUF, &buf, "QBUF OUTPUT request");
}

static int reinit_request(struct test_context *ctx)
{
	return checked_ioctl(ctx->request_fd, MEDIA_REQUEST_IOC_REINIT, NULL,
			     "MEDIA_REQUEST_IOC_REINIT");
}

static int test_missing_controls(struct test_context *ctx)
{
	static const struct {
		const char *name;
		unsigned int presence;
	} cases[] = {
		{ "missing-FRAME", HAVE_HEADER },
		{ "missing-COMPRESSED_HDR", HAVE_FRAME },
		{ "missing-both-controls", 0 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct v4l2_ctrl_vp9_frame frame = base_frame();
		int ret, error;

		if (set_request_controls(ctx, &frame, cases[i].presence) || queue_output(ctx))
			return -1;
		errno = 0;
		ret = xioctl(ctx->request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL);
		error = errno;
		if (ret != -1 || error != ENOENT) {
			fprintf(stderr, "%s: FAIL ret=%d errno=%d, expected ENOENT\n",
				cases[i].name, ret, error);
			return -1;
		}
		if (reinit_request(ctx))
			return -1;
		printf("%s: PASS (QUEUE: ENOENT; REINIT succeeded)\n", cases[i].name);
		ctx->checks++;
	}
	return 0;
}

static int stream_queue(struct test_context *ctx, enum v4l2_buf_type type,
			bool start)
{
	bool *state = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ?
		      &ctx->output_streaming : &ctx->capture_streaming;

	if (checked_ioctl(ctx->video_fd, start ? VIDIOC_STREAMON : VIDIOC_STREAMOFF,
			  &type, start ? "STREAMON" : "STREAMOFF"))
		return -1;
	*state = start;
	return 0;
}

static int run_error_frame(struct test_context *ctx,
			   struct v4l2_ctrl_vp9_frame *frame, const char *name)
{
	struct v4l2_plane cap_plane = { .length = ctx->capture.size }, out_plane = { 0 };
	struct v4l2_buffer cap = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP, .index = 0,
		.length = 1, .m.planes = &cap_plane,
	};
	struct v4l2_buffer out = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP, .index = 0,
		.length = 1, .m.planes = &out_plane,
	};
	int64_t start, elapsed;

	if (set_request_controls(ctx, frame, HAVE_BOTH) ||
	    checked_ioctl(ctx->video_fd, VIDIOC_QBUF, &cap, "QBUF CAPTURE") ||
	    queue_output(ctx))
		return -1;
	start = monotonic_ms();
	if (checked_ioctl(ctx->request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL,
			  "MEDIA_REQUEST_IOC_QUEUE") || wait_request(ctx))
		return -1;
	if (checked_ioctl(ctx->video_fd, VIDIOC_DQBUF, &cap, "DQBUF CAPTURE") ||
	    checked_ioctl(ctx->video_fd, VIDIOC_DQBUF, &out, "DQBUF OUTPUT"))
		return -1;
	elapsed = monotonic_ms() - start;
	printf("%s: elapsed=%" PRId64 "ms src_flags=0x%08x cap_flags=0x%08x\n",
	       name, elapsed, out.flags, cap.flags);
	if (!(out.flags & V4L2_BUF_FLAG_ERROR) || !(cap.flags & V4L2_BUF_FLAG_ERROR) ||
	    out.index || cap.index ||
	    cap.timestamp.tv_sec != (long)ctx->sequence || cap.timestamp.tv_usec) {
		fprintf(stderr, "%s: FAIL (expected both ERROR buffers and preserved timestamp)\n",
			name);
		return -1;
	}
	if (reinit_request(ctx))
		return -1;
	printf("%s: PASS (both ERROR; request complete and reinitialized)\n", name);
	ctx->checks++;
	return 0;
}

static int test_error_frames(struct test_context *ctx)
{
	static const char * const names[] = {
		"headers-exceed-payload", "headers-equal-payload",
		"zero-uncompressed-header", "zero-compressed-header",
		"frame-width-exceeds-negotiated", "frame-height-exceeds-negotiated",
	};
	unsigned int i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		struct v4l2_ctrl_vp9_frame frame = base_frame();

		switch (i) {
		case 0: frame.compressed_header_size = PAYLOAD_SIZE; break;
		case 1: frame.compressed_header_size = PAYLOAD_SIZE - 1; break;
		case 2: frame.uncompressed_header_size = 0; break;
		case 3: frame.compressed_header_size = 0; break;
		case 4: frame.frame_width_minus_1 = WIDTH + 1; break;
		case 5: frame.frame_height_minus_1 = HEIGHT + 1; break;
		}
		if (run_error_frame(ctx, &frame, names[i]))
			return -1;
	}
	return 0;
}

static int test_streamoff_pending(struct test_context *ctx)
{
	struct v4l2_ctrl_vp9_frame frame = base_frame();
	struct pollfd pfd = { .fd = ctx->request_fd, .events = POLLPRI };
	int ret;

	/* No CAPTURE buffer: this request must remain pending until STREAMOFF. */
	frame.compressed_header_size = PAYLOAD_SIZE;
	if (set_request_controls(ctx, &frame, HAVE_BOTH) || queue_output(ctx) ||
	    checked_ioctl(ctx->request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL,
			  "QUEUE pending request"))
		return -1;
	do {
		ret = poll(&pfd, 1, 0);
	} while (ret < 0 && errno == EINTR);
	if (ret) {
		fprintf(stderr, "pending request completed or poll failed before STREAMOFF\n");
		return -1;
	}
	if (stream_queue(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, false) ||
	    wait_request(ctx) || reinit_request(ctx) ||
	    stream_queue(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, false))
		return -1;
	printf("streamoff-pending-request: PASS (request completed and reinitialized)\n");
	ctx->checks++;
	if (stream_queue(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, true) ||
	    stream_queue(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true) ||
	    run_error_frame(ctx, &frame, "streamon-after-streamoff"))
		return -1;
	return 0;
}

static int set_format(struct test_context *ctx, enum v4l2_buf_type type)
{
	bool output = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	uint32_t fourcc = output ? V4L2_PIX_FMT_VP9_FRAME : V4L2_PIX_FMT_NV12;
	struct v4l2_format fmt = { .type = type };

	fmt.fmt.pix_mp.width = WIDTH;
	fmt.fmt.pix_mp.height = HEIGHT;
	fmt.fmt.pix_mp.pixelformat = fourcc;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (output)
		fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 4096;
	if (checked_ioctl(ctx->video_fd, VIDIOC_S_FMT, &fmt, "S_FMT"))
		return -1;
	if (fmt.fmt.pix_mp.pixelformat != fourcc || fmt.fmt.pix_mp.width != WIDTH ||
	    fmt.fmt.pix_mp.height != HEIGHT || fmt.fmt.pix_mp.num_planes != 1) {
		fprintf(stderr, "unexpected negotiated format\n");
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

	if (checked_ioctl(ctx->video_fd, VIDIOC_REQBUFS, &req, "REQBUFS"))
		return -1;
	if (!req.count || (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE &&
	    !(req.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS))) {
		fprintf(stderr, "queue has no buffers or OUTPUT lacks Request API\n");
		return -1;
	}
	if (checked_ioctl(ctx->video_fd, VIDIOC_QUERYBUF, &buf, "QUERYBUF"))
		return -1;
	if (plane.length < DATA_OFFSET + PAYLOAD_SIZE) {
		fprintf(stderr, "mapped buffer too small\n");
		return -1;
	}
	addr = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED,
		    ctx->video_fd, plane.m.mem_offset);
	if (addr == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	map->addr = addr;
	map->size = plane.length;
	return 0;
}

static int cleanup(struct test_context *ctx)
{
	int failed = 0;

	if (ctx->output_streaming)
		failed |= stream_queue(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, false);
	if (ctx->capture_streaming)
		failed |= stream_queue(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, false);
	if (ctx->output.addr && munmap(ctx->output.addr, ctx->output.size))
		failed = -1;
	if (ctx->capture.addr && munmap(ctx->capture.addr, ctx->capture.size))
		failed = -1;
	if (ctx->request_fd >= 0 && close(ctx->request_fd))
		failed = -1;
	if (ctx->video_fd >= 0 && close(ctx->video_fd))
		failed = -1;
	if (ctx->media_fd >= 0 && close(ctx->media_fd))
		failed = -1;
	return failed;
}

static void usage(FILE *file, const char *name)
{
	fprintf(file, "Usage: %s [--device /dev/video0] [--media /dev/media0]\n"
		"       [--iterations N] [--timeout-ms N]\n"
		"VP9 control rejection, missing request controls, software ERROR,\n"
		"and pending-request STREAMOFF/STREAMON checks; no pixel validation.\n",
		name);
}

static bool parse_uint(const char *text, unsigned int min, unsigned int max,
		       unsigned int *value)
{
	char *end;
	unsigned long number;

	errno = 0;
	number = strtoul(text, &end, 0);
	if (errno || !*text || *text == '-' || *end || number < min || number > max)
		return false;
	*value = (unsigned int)number;
	return true;
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "media", required_argument, NULL, 'm' },
		{ "iterations", required_argument, NULL, 'i' },
		{ "timeout-ms", required_argument, NULL, 't' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct test_context ctx = { .video_fd = -1, .media_fd = -1,
		.request_fd = -1, .timeout_ms = 3000 };
	struct v4l2_capability cap = { 0 };
	const char *video = "/dev/video0", *media = "/dev/media0";
	unsigned int iterations = 1, round;
	int opt, result = EXIT_FAILURE;

	while ((opt = getopt_long(argc, argv, "", options, NULL)) != -1) {
		switch (opt) {
		case 'd': video = optarg; break;
		case 'm': media = optarg; break;
		case 'i':
			if (!parse_uint(optarg, 1, 10000, &iterations))
				goto bad_options;
			break;
		case 't':
			if (!parse_uint(optarg, 50, 60000, &ctx.timeout_ms))
				goto bad_options;
			break;
		case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
		default: goto bad_options;
		}
	}
	if (optind != argc)
		goto bad_options;
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("VP9 negative Request API tests: placeholder payload=%u data_offset=%u\n",
	       PAYLOAD_SIZE, DATA_OFFSET);
	ctx.video_fd = open(video, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (ctx.video_fd < 0) {
		perror(video);
		goto out;
	}
	ctx.media_fd = open(media, O_RDWR | O_CLOEXEC);
	if (ctx.media_fd < 0) {
		perror(media);
		goto out;
	}
	if (checked_ioctl(ctx.video_fd, VIDIOC_QUERYCAP, &cap, "QUERYCAP"))
		goto out;
	printf("device=%s driver=%s media=%s\n", video, cap.driver, media);
	if (set_format(&ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) ||
	    set_format(&ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) || test_controls(&ctx) ||
	    map_queue(&ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &ctx.output) ||
	    map_queue(&ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &ctx.capture) ||
	    checked_ioctl(ctx.media_fd, MEDIA_IOC_REQUEST_ALLOC, &ctx.request_fd,
			  "MEDIA_IOC_REQUEST_ALLOC") || test_missing_controls(&ctx) ||
	    stream_queue(&ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, true) ||
	    stream_queue(&ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true))
		goto out;
	for (round = 0; round < iterations; round++) {
		printf("round=%u/%u\n", round + 1, iterations);
		if (test_error_frames(&ctx) || test_streamoff_pending(&ctx) ||
		    test_missing_controls(&ctx))
			goto out;
	}
	result = EXIT_SUCCESS;
out:
	if (cleanup(&ctx))
		result = EXIT_FAILURE;
	printf("vp9-request-test: %s checks=%u (software rejection and queue lifecycle only)\n",
	       result == EXIT_SUCCESS ? "PASS" : "FAIL", ctx.checks);
	return result;
bad_options:
	usage(stderr, argv[0]);
	return EXIT_FAILURE;
}
