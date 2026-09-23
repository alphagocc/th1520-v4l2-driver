// SPDX-License-Identifier: GPL-2.0-only
/*
 * Replay captured standard VP9 Request API controls and a keyframe.
 * Copyright (C) 2026 th1520-v4l2 contributors
 *
 * Build: cc -std=c11 -O2 -Wall -Wextra -Werror -o vp9-decode-test \
 *           tools/vp9-decode-test.c
 *
 * The two control files contain exactly one native-endian Linux UAPI
 * v4l2_ctrl_vp9_frame / v4l2_ctrl_vp9_compressed_hdr structure. The bitstream
 * is one VP9 keyframe without IVF framing. The reference is tightly packed
 * visible 8-bit NV12. Capture padding is excluded from the comparison.
 *
 * Default: valid keyframe -> truncated tile -> valid keyframe recovery.
 * --fault-first: full keyframe must ERROR after >=1500 ms -> valid recovery.
 * The latter needs a separately installed one-shot lost-completion test
 * module; this program does not install or enable fault injection.
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

#define TIMEOUT_MS 10000
#define WATCHDOG_MIN_MS 1500
#define MAX_INPUT_SIZE (64U * 1024U * 1024U)
#define MAX_DATA_OFFSET 65536U

struct blob {
	uint8_t *data;
	size_t size;
};

struct buffer {
	uint8_t *addr;
	uint32_t size;
};

struct context {
	int video_fd, media_fd, request_fd;
	bool output_streaming, capture_streaming;
	struct buffer output, capture;
	struct v4l2_pix_format_mplane capture_format;
	struct v4l2_ctrl_vp9_frame frame;
	struct v4l2_ctrl_vp9_compressed_hdr header;
	struct blob bitstream, reference;
	unsigned int width, height, data_offset, sequence, checked_frames;
};

static int xioctl(int fd, unsigned long cmd, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, cmd, arg);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static int checked_ioctl(int fd, unsigned long cmd, void *arg, const char *name)
{
	if (!xioctl(fd, cmd, arg))
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

static int load_blob(const char *file, struct blob *blob)
{
	FILE *stream = fopen(file, "rb");
	long length;
	int ret = -1;

	if (!stream) {
		perror(file);
		return -1;
	}
	if (fseek(stream, 0, SEEK_END) || (length = ftell(stream)) <= 0 ||
	    (unsigned long)length > MAX_INPUT_SIZE || fseek(stream, 0, SEEK_SET)) {
		fprintf(stderr, "%s: invalid or excessive input length\n", file);
		goto out;
	}
	blob->data = malloc((size_t)length);
	if (!blob->data) {
		perror("malloc input");
		goto out;
	}
	blob->size = (size_t)length;
	if (fread(blob->data, 1, blob->size, stream) != blob->size) {
		fprintf(stderr, "%s: incomplete read\n", file);
		goto out;
	}
	ret = 0;
out:
	if (fclose(stream)) {
		perror("fclose input");
		ret = -1;
	}
	return ret;
}

static int load_control(const char *file, void *control, size_t size)
{
	struct blob blob = { 0 };
	int ret = load_blob(file, &blob);

	if (!ret) {
		if (blob.size != size) {
			fprintf(stderr, "%s: got %zu bytes, expected UAPI structure size %zu\n",
				file, blob.size, size);
			ret = -1;
		} else {
			memcpy(control, blob.data, size);
		}
	}
	free(blob.data);
	return ret;
}

static int validate_inputs(struct context *ctx)
{
	uint32_t subsampling = V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING |
			       V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;
	size_t headers = (size_t)ctx->frame.uncompressed_header_size +
			 ctx->frame.compressed_header_size;

	ctx->width = (unsigned int)ctx->frame.frame_width_minus_1 + 1;
	ctx->height = (unsigned int)ctx->frame.frame_height_minus_1 + 1;
	if (!(ctx->frame.flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) ||
	    ctx->frame.profile || ctx->frame.bit_depth != 8 ||
	    (ctx->frame.flags & subsampling) != subsampling ||
	    ctx->width < 48 || ctx->width > 4096 ||
	    ctx->height < 48 || ctx->height > 2304 ||
	    (ctx->width & 1) || (ctx->height & 1)) {
		fprintf(stderr, "fixture must be an even-sized Profile 0 8-bit 4:2:0 keyframe\n");
		return -1;
	}
	if (!ctx->frame.uncompressed_header_size || !ctx->frame.compressed_header_size ||
	    headers + 1 >= ctx->bitstream.size) {
		fprintf(stderr, "fixture needs complete headers and more than one tile payload byte\n");
		return -1;
	}
	if (ctx->reference.size != (size_t)ctx->width * ctx->height * 3 / 2) {
		fprintf(stderr, "reference must be visible %ux%u NV12: got %zu, expected %zu bytes\n",
			ctx->width, ctx->height, ctx->reference.size,
			(size_t)ctx->width * ctx->height * 3 / 2);
		return -1;
	}
	printf("fixture: %ux%u frame_ctrl=%zu compressed_ctrl=%zu bitstream=%zu "
	       "headers=%zu reference=%zu data_offset=%u\n", ctx->width, ctx->height,
	       sizeof(ctx->frame), sizeof(ctx->header), ctx->bitstream.size, headers,
	       ctx->reference.size, ctx->data_offset);
	return 0;
}

static int set_formats(struct context *ctx)
{
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
	size_t requested = (ctx->data_offset + ctx->bitstream.size + 64 + 4095) & ~(size_t)4095;

	fmt.fmt.pix_mp.width = ctx->width;
	fmt.fmt.pix_mp.height = ctx->height;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_VP9_FRAME;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage = (uint32_t)requested;
	if (checked_ioctl(ctx->video_fd, VIDIOC_S_FMT, &fmt, "S_FMT OUTPUT"))
		return -1;
	if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_VP9_FRAME ||
	    fmt.fmt.pix_mp.width != ctx->width || fmt.fmt.pix_mp.height != ctx->height ||
	    fmt.fmt.pix_mp.num_planes != 1 ||
	    fmt.fmt.pix_mp.plane_fmt[0].sizeimage < ctx->data_offset + ctx->bitstream.size) {
		fprintf(stderr, "unexpected OUTPUT format\n");
		return -1;
	}
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = ctx->width;
	fmt.fmt.pix_mp.height = ctx->height;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (checked_ioctl(ctx->video_fd, VIDIOC_S_FMT, &fmt, "S_FMT CAPTURE"))
		return -1;
	if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
	    fmt.fmt.pix_mp.width < ctx->width || fmt.fmt.pix_mp.height < ctx->height ||
	    fmt.fmt.pix_mp.num_planes != 1 || (fmt.fmt.pix_mp.height & 1) ||
	    fmt.fmt.pix_mp.plane_fmt[0].bytesperline < ctx->width) {
		fprintf(stderr, "unexpected CAPTURE format\n");
		return -1;
	}
	ctx->capture_format = fmt.fmt.pix_mp;
	printf("CAPTURE: storage=%ux%u stride=%u sizeimage=%u\n",
	       fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
	       fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
	       fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
	return 0;
}

static int map_queue(struct context *ctx, enum v4l2_buf_type type, struct buffer *map)
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

static int stream_queue(struct context *ctx, enum v4l2_buf_type type, bool start)
{
	bool *state = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ?
		      &ctx->output_streaming : &ctx->capture_streaming;

	if (checked_ioctl(ctx->video_fd, start ? VIDIOC_STREAMON : VIDIOC_STREAMOFF,
			  &type, start ? "STREAMON" : "STREAMOFF"))
		return -1;
	*state = start;
	return 0;
}

static int wait_request(struct context *ctx, const char *label, int64_t start)
{
	struct pollfd pfd = { .fd = ctx->request_fd, .events = POLLPRI };
	int64_t deadline = start + TIMEOUT_MS;

	for (;;) {
		int64_t remaining = deadline - monotonic_ms();
		int ret;

		if (remaining <= 0) {
			fprintf(stderr, "%s: request poll timeout, elapsed=%" PRId64 "ms\n",
				label, monotonic_ms() - start);
			return -1;
		}
		ret = poll(&pfd, 1, (int)remaining);
		if (ret < 0 && errno == EINTR)
			continue;
		printf("%s: poll ret=%d revents=0x%x elapsed=%" PRId64 "ms\n",
		       label, ret, pfd.revents, monotonic_ms() - start);
		if (ret < 0) {
			perror("poll request");
			return -1;
		}
		if (!ret)
			continue;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			return -1;
		if (pfd.revents & POLLPRI)
			return 0;
	}
}

static int compare_nv12(struct context *ctx, const struct v4l2_plane *plane,
			const char *label)
{
	size_t stride = ctx->capture_format.plane_fmt[0].bytesperline;
	size_t storage_height = ctx->capture_format.height;
	size_t stored_size = stride * storage_height * 3 / 2;
	size_t mismatches = 0, checked = 0;
	uint32_t checksum = 2166136261U;
	unsigned int p, x, y;

	if (plane->data_offset > plane->bytesused || plane->bytesused > ctx->capture.size ||
	    plane->bytesused - plane->data_offset < stored_size) {
		fprintf(stderr, "%s: invalid CAPTURE extent bytesused=%u offset=%u "
			"map=%u required=%zu\n", label, plane->bytesused,
			plane->data_offset, ctx->capture.size, stored_size);
		return -1;
	}
	for (p = 0; p < 2; p++) {
		const uint8_t *actual = ctx->capture.addr + plane->data_offset +
			(p ? stride * storage_height : 0);
		const uint8_t *reference = ctx->reference.data +
			(p ? (size_t)ctx->width * ctx->height : 0);
		unsigned int rows = p ? ctx->height / 2 : ctx->height;

		for (y = 0; y < rows; y++) {
			for (x = 0; x < ctx->width; x++) {
				uint8_t got = actual[y * stride + x];
				uint8_t want = reference[(size_t)y * ctx->width + x];

				if (got != want) {
					if (mismatches < 8)
						fprintf(stderr, "%s: %s[%u,%u] got=%u expected=%u\n",
							label, p ? "UV" : "Y", x, y, got, want);
					mismatches++;
				}
				checksum = (checksum ^ got) * 16777619U;
				checked++;
			}
		}
	}
	printf("%s: pixels checked=%zu mismatches=%zu fnv1a=%08" PRIx32 "\n",
	       label, checked, mismatches, checksum);
	return mismatches ? -1 : 0;
}

static int run_frame(struct context *ctx, const char *label, size_t length,
		     bool expect_error, bool expect_watchdog)
{
	struct v4l2_ext_control controls[] = {
		{ .id = V4L2_CID_STATELESS_VP9_FRAME,
		  .size = sizeof(ctx->frame), .ptr = &ctx->frame },
		{ .id = V4L2_CID_STATELESS_VP9_COMPRESSED_HDR,
		  .size = sizeof(ctx->header), .ptr = &ctx->header },
	};
	struct v4l2_ext_controls ext = {
		.which = V4L2_CTRL_WHICH_REQUEST_VAL,
		.request_fd = ctx->request_fd, .count = 2, .controls = controls,
	};
	struct v4l2_plane out_plane = { 0 }, cap_plane = { .length = ctx->capture.size };
	struct v4l2_buffer out = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP,
		.index = 0, .length = 1, .m.planes = &out_plane, .field = V4L2_FIELD_NONE,
		.flags = V4L2_BUF_FLAG_REQUEST_FD | V4L2_BUF_FLAG_KEYFRAME,
		.request_fd = ctx->request_fd,
	};
	struct v4l2_buffer cap = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
		.index = 0, .length = 1, .m.planes = &cap_plane,
	};
	int64_t start, elapsed;
	int failed = 0;

	if (length > ctx->bitstream.size || ctx->data_offset + length > ctx->output.size) {
		fprintf(stderr, "%s: input length exceeds mapped buffer\n", label);
		return -1;
	}
	memset(ctx->output.addr, 0, ctx->output.size);
	memset(ctx->output.addr, 0xa5, ctx->data_offset);
	memcpy(ctx->output.addr + ctx->data_offset, ctx->bitstream.data, length);
	memset(ctx->capture.addr, 0xa5, ctx->capture.size);
	if (checked_ioctl(ctx->video_fd, VIDIOC_S_EXT_CTRLS, &ext, "S_EXT_CTRLS request") ||
	    checked_ioctl(ctx->video_fd, VIDIOC_QBUF, &cap, "QBUF CAPTURE"))
		return -1;
	out_plane.length = ctx->output.size;
	out_plane.bytesused = ctx->data_offset + (uint32_t)length;
	out_plane.data_offset = ctx->data_offset;
	out.timestamp.tv_sec = ++ctx->sequence;
	if (checked_ioctl(ctx->video_fd, VIDIOC_QBUF, &out, "QBUF OUTPUT request"))
		return -1;
	start = monotonic_ms();
	if (checked_ioctl(ctx->request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL, "REQUEST QUEUE") ||
	    wait_request(ctx, label, start))
		return -1;
	memset(&out_plane, 0, sizeof(out_plane));
	memset(&cap_plane, 0, sizeof(cap_plane));
	out.flags = 0;
	out.request_fd = 0;
	cap.flags = 0;
	if (checked_ioctl(ctx->video_fd, VIDIOC_DQBUF, &cap, "DQBUF CAPTURE") ||
	    checked_ioctl(ctx->video_fd, VIDIOC_DQBUF, &out, "DQBUF OUTPUT"))
		return -1;
	elapsed = monotonic_ms() - start;
	printf("%s: input=%zu elapsed=%" PRId64 "ms src_flags=0x%08x "
	       "cap_flags=0x%08x cap_bytesused=%u\n", label, length, elapsed,
	       out.flags, cap.flags, cap_plane.bytesused);
	if (out.index || cap.index || cap.timestamp.tv_sec != (long)ctx->sequence ||
	    cap.timestamp.tv_usec) {
		fprintf(stderr, "%s: buffer index or timestamp mismatch\n", label);
		failed = 1;
	}
	if (expect_error) {
		if (!(out.flags & V4L2_BUF_FLAG_ERROR) || !(cap.flags & V4L2_BUF_FLAG_ERROR)) {
			fprintf(stderr, "%s: expected ERROR on both queues\n", label);
			failed = 1;
		}
	} else if ((out.flags | cap.flags) & V4L2_BUF_FLAG_ERROR) {
		fprintf(stderr, "%s: valid frame returned ERROR\n", label);
		failed = 1;
	} else if (compare_nv12(ctx, &cap_plane, label)) {
		failed = 1;
	}
	if (expect_watchdog && (elapsed < WATCHDOG_MIN_MS || elapsed >= TIMEOUT_MS)) {
		fprintf(stderr, "%s: expected watchdog completion in [%d,%d) ms\n",
			label, WATCHDOG_MIN_MS, TIMEOUT_MS);
		failed = 1;
	}
	if (checked_ioctl(ctx->request_fd, MEDIA_REQUEST_IOC_REINIT, NULL, "REQUEST REINIT"))
		return -1;
	ctx->checked_frames++;
	printf("%s: %s\n", label, failed ? "FAIL" : "PASS");
	return failed;
}

static int cleanup(struct context *ctx)
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
	free(ctx->bitstream.data);
	free(ctx->reference.data);
	return failed;
}

static void usage(FILE *file, const char *name)
{
	fprintf(file, "Usage: %s --frame-controls FILE --compressed-controls FILE\n"
		"       --bitstream FILE --reference FILE [--device /dev/video0]\n"
		"       [--media /dev/media0] [--data-offset N] [--fault-first]\n"
		"Default: keyframe pixels, truncated tile ERROR, keyframe pixel recovery.\n"
		"--fault-first: full keyframe watchdog ERROR, then keyframe pixel recovery.\n"
		"Poll timeout is 10000 ms; --fault-first requires external one-shot injection.\n",
		name);
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "media", required_argument, NULL, 'm' },
		{ "frame-controls", required_argument, NULL, 'f' },
		{ "compressed-controls", required_argument, NULL, 'c' },
		{ "bitstream", required_argument, NULL, 'b' },
		{ "reference", required_argument, NULL, 'r' },
		{ "data-offset", required_argument, NULL, 'o' },
		{ "fault-first", no_argument, NULL, 'F' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct context ctx = { .video_fd = -1, .media_fd = -1, .request_fd = -1 };
	struct v4l2_capability cap = { 0 };
	const char *video = "/dev/video0", *media = "/dev/media0";
	const char *frame_file = NULL, *header_file = NULL, *stream_file = NULL, *ref_file = NULL;
	bool fault_first = false;
	int opt, ret, failed = 0, result = EXIT_FAILURE;

	while ((opt = getopt_long(argc, argv, "", options, NULL)) != -1) {
		switch (opt) {
		case 'd': video = optarg; break;
		case 'm': media = optarg; break;
		case 'f': frame_file = optarg; break;
		case 'c': header_file = optarg; break;
		case 'b': stream_file = optarg; break;
		case 'r': ref_file = optarg; break;
		case 'F': fault_first = true; break;
		case 'o': {
			char *end;
			unsigned long value;

			errno = 0;
			value = strtoul(optarg, &end, 0);
			if (errno || !*optarg || *optarg == '-' || *end || value > MAX_DATA_OFFSET)
				goto bad_options;
			ctx.data_offset = (unsigned int)value;
			break;
		}
		case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
		default: goto bad_options;
		}
	}
	if (optind != argc || !frame_file || !header_file || !stream_file || !ref_file)
		goto bad_options;
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (load_control(frame_file, &ctx.frame, sizeof(ctx.frame)) ||
	    load_control(header_file, &ctx.header, sizeof(ctx.header)) ||
	    load_blob(stream_file, &ctx.bitstream) || load_blob(ref_file, &ctx.reference) ||
	    validate_inputs(&ctx))
		goto out;
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
	printf("device=%s driver=%s media=%s mode=%s\n", video, cap.driver, media,
	       fault_first ? "watchdog-recovery" : "truncated-tile-recovery");
	if (set_formats(&ctx) ||
	    map_queue(&ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &ctx.output) ||
	    map_queue(&ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &ctx.capture) ||
	    checked_ioctl(ctx.media_fd, MEDIA_IOC_REQUEST_ALLOC, &ctx.request_fd, "REQUEST ALLOC") ||
	    stream_queue(&ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, true) ||
	    stream_queue(&ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true))
		goto out;
	ret = run_frame(&ctx, fault_first ? "fault-first" : "normal", ctx.bitstream.size,
			fault_first, fault_first);
	if (ret < 0)
		goto out;
	failed |= ret;
	if (!fault_first) {
		size_t truncated = (size_t)ctx.frame.uncompressed_header_size +
				   ctx.frame.compressed_header_size + 1;

		ret = run_frame(&ctx, "truncated-tile", truncated, true, false);
		if (ret < 0)
			goto out;
		failed |= ret;
	}
	ret = run_frame(&ctx, "recovery", ctx.bitstream.size, false, false);
	if (ret < 0)
		goto out;
	failed |= ret;
	result = failed ? EXIT_FAILURE : EXIT_SUCCESS;
out:
	if (cleanup(&ctx))
		result = EXIT_FAILURE;
	printf("vp9-decode-test: %s checked_frames=%u\n",
	       result == EXIT_SUCCESS ? "PASS" : "FAIL", ctx.checked_frames);
	return result;
bad_options:
	usage(stderr, argv[0]);
	return EXIT_FAILURE;
}
