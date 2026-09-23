#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile and test the actual VP9 probability helpers on the host CPU.

Usage: python3 tools/vp9-offline-test.py --kernel-tree /path/to/linux-6.6
       python3 tools/vp9-offline-test.py --kernel-archive linux-6.6.140.tar.gz

This uses the supplied official Linux UAPI and v4l2-vp9.c without modifying
them. Small host stubs replace kernel types, module declarations and the
driver's unrelated device structures. ASan/UBSan check table boundaries.
No device node is opened and no hardware decoding is performed.
"""

import argparse
from pathlib import Path
import shlex
import subprocess
import tarfile
import tempfile


COMMON = r"""
#ifndef TH1520_VDEC_H_
#define TH1520_VDEC_H_
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define clamp(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define WARN_ON(x) ((x) ? (abort(), 1) : 0)
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_AUTHOR(x)
struct th1520_vdec_aux_buf { void *cpu; uintptr_t dma; size_t size; };
struct th1520_vdec_vp9_ctx;
struct th1520_vdec_ctx { struct th1520_vdec_vp9_ctx *vp9; };
#endif
"""

TYPES = r"""
#ifndef TEST_LINUX_TYPES_H
#define TEST_LINUX_TYPES_H
#include <stdint.h>
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t __s8;
typedef int16_t __s16;
typedef int32_t __s32;
typedef int64_t __s64;
#endif
"""

HARNESS = r"""
#include "th1520_vdec_vp9.h"

struct guarded_probs {
	uint64_t before;
	struct th1520_vp9_all_probs data;
	uint64_t after;
};
struct guarded_counts {
	uint64_t before;
	struct th1520_vp9_counts data;
	uint64_t after;
};
static const uint64_t canary = UINT64_C(0xa51ef39408d72bc6);

static void test_pack(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_vp9_ctx *v = ctx->vp9;
	struct th1520_vp9_all_probs *out = v->probs.cpu;
	struct v4l2_ctrl_vp9_frame f = {0};
	struct v4l2_vp9_frame_context *p = &v->probability_tables;
	u8 *bytes = (u8 *)p;
	unsigned int i, j, k, l, m, n;

	/* Distinct neighbouring values expose transposition and padding errors. */
	for (i = 0; i < sizeof(*p); i++)
		bytes[i] = 1 + (i * 37U) % 255;
	for (i = 0; i < sizeof(f.seg.tree_probs); i++)
		f.seg.tree_probs[i] = 20 + i;
	for (i = 0; i < sizeof(f.seg.pred_probs); i++)
		f.seg.pred_probs[i] = 40 + i;
	memset(out, 0, sizeof(*out));
	th1520_vp9_pack_probs(ctx, &f);
	assert(sizeof(*out) == 3744);
	assert(sizeof(struct th1520_vp9_counts) == 13264);
	assert(!memcmp(out->mb_segment_tree_probs, f.seg.tree_probs, 7));
	assert(!memcmp(out->segment_pred_probs, f.seg.pred_probs, 3));
	for (i = 0; i < 10; i++) {
		assert(!memcmp(out->kf_uv_mode_prob[i], v4l2_vp9_kf_uv_mode_prob[i], 8));
		assert(out->kf_uv_mode_prob_tail[i][0] == v4l2_vp9_kf_uv_mode_prob[i][8]);
		assert(!memcmp(out->probs.uv_mode[i], p->uv_mode[i], 8));
		assert(out->probs.uv_mode_tail[i][0] == p->uv_mode[i][8]);
		for (j = 0; j < 10; j++) {
			assert(!memcmp(out->kf_y_mode_prob[i][j], v4l2_vp9_kf_y_mode_prob[i][j], 8));
			assert(out->kf_y_mode_prob_tail[i][j][0] == v4l2_vp9_kf_y_mode_prob[i][j][8]);
		}
	}
	for (i = 0; i < 4; i++) {
		assert(!memcmp(out->probs.y_mode[i], p->y_mode[i], 8));
		assert(out->probs.y_mode_tail[i][0] == p->y_mode[i][8]);
	}
	for (i = 0; i < 7; i++) {
		assert(!memcmp(out->probs.inter_mode[i], p->inter_mode[i], 3));
		assert(out->probs.inter_mode[i][3] == 0);
	}
	for (i = 0; i < 16; i++) {
		assert(!memcmp(out->probs.partition[0][i], v4l2_vp9_kf_partition_probs[i], 3));
		assert(!memcmp(out->probs.partition[1][i], p->partition[i], 3));
		assert(out->probs.partition[0][i][3] == 0);
		assert(out->probs.partition[1][i][3] == 0);
	}
	for (i = 0; i < 4; i++)
		for (j = 0; j < 2; j++)
			for (k = 0; k < 2; k++)
				for (l = 0; l < 6; l++)
					for (m = 0; m < 6; m++) {
						for (n = 0; n < 3; n++)
							assert(out->probs.coef[i][j][k][l][m][n] == p->coef[i][j][k][l][m][n]);
						assert(out->probs.coef[i][j][k][l][m][3] == 0);
					}
	assert(!memcmp(out->probs.mv.joint, p->mv.joint, sizeof(p->mv.joint)));
	assert(!memcmp(out->probs.mv.classes, p->mv.classes, sizeof(p->mv.classes)));
	assert(!memcmp(out->probs.mv.bits, p->mv.bits, sizeof(p->mv.bits)));
	assert(!memcmp(out->probs.single_ref, p->single_ref, sizeof(p->single_ref)));
	assert(!memcmp(out->probs.comp_ref, p->comp_ref, sizeof(p->comp_ref)));
}

static void test_count_mapping(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_vp9_ctx *v = ctx->vp9;
	struct th1520_vp9_counts *hw = v->counts.cpu;
	u32 *coeffs[] = { &hw->count_coeffs[0][0][0][0][0],
		&hw->count_coeffs8x8[0][0][0][0][0],
		&hw->count_coeffs16x16[0][0][0][0][0],
		&hw->count_coeffs32x32[0][0][0][0][0] };
	unsigned int i, j, k, l, m;

	th1520_vp9_init_counts(ctx);
	assert(v->cnts.skip == &hw->mbskip_count);
	assert(v->cnts.partition == &hw->partition_counts);
	assert(v->cnts.tx8p == &hw->tx8x8_count);
	assert(v->cnts.tx32p == &hw->tx32x32_count);
	for (i = 0; i < 4; i++)
		for (j = 0; j < 2; j++)
			for (k = 0; k < 2; k++)
				for (l = 0; l < 6; l++)
					for (m = 0; m < 6; m++) {
						u32 *expected = coeffs[i] + (((j * 2 + k) * 6 + l) * 6 + m) * 4;
						assert((u32 *)v->cnts.coeff[i][j][k][l][m] == expected);
						assert(v->cnts.eob[i][j][k][l][m][1] == expected + 3);
						assert(v->cnts.eob[i][j][k][l][m][0] == &hw->count_eobs[i][j][k][l][m]);
					}
}

static void test_adaptation(struct th1520_vdec_ctx *ctx)
{
	struct th1520_vdec_vp9_ctx *v = ctx->vp9;
	struct th1520_vp9_counts *hw = v->counts.cpu;
	struct v4l2_vp9_frame_context unchanged;
	unsigned int i, index;

	for (index = 0; index < 4; index++) {
		for (i = 0; i < 4; i++)
			v->working_context[i] = v4l2_vp9_default_probs;
		v->probability_tables = v4l2_vp9_default_probs;
		v->cur.frame_context_idx = index;
		v->cur.flags = 0;
		unchanged = v->working_context[index];
		v->probability_tables.skip[0] = 1;
		th1520_vp9_update_probs(ctx);
		assert(!memcmp(&v->working_context[index], &unchanged, sizeof(unchanged)));

		v->cur.flags = V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX |
			V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE;
		th1520_vp9_update_probs(ctx);
		assert(v->working_context[index].skip[0] == 1);
		for (i = 0; i < 4; i++)
			if (i != index)
				assert(!memcmp(&v->working_context[i], &unchanged, sizeof(unchanged)));

		v->working_context[index] = v4l2_vp9_default_probs;
		v->probability_tables = v4l2_vp9_default_probs;
		v->cur.flags = V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX;
		v->cur.tx_mode = V4L2_VP9_TX_MODE_SELECT;
		v->cur.interpolation_filter = V4L2_VP9_INTERP_FILTER_SWITCHABLE;
		v->cur.reference_mode = V4L2_VP9_REFERENCE_MODE_SELECT;
		memset(hw, 0, sizeof(*hw));
		for (i = 0; i < 3; i++)
			hw->mbskip_count[i][0] = 100;
		th1520_vp9_update_probs(ctx);
		for (i = 0; i < 3; i++) {
			unsigned int expected = (unchanged.skip[i] * 128 + 255 * 128 + 128) >> 8;
			assert(v->working_context[index].skip[i] == expected);
		}
		assert(!memcmp(v->working_context[index].coef, unchanged.coef, sizeof(unchanged.coef)));
	}
}

int main(void)
{
	struct th1520_vdec_vp9_ctx vp9 = {0};
	struct th1520_vdec_ctx ctx = { .vp9 = &vp9 };
	struct guarded_probs probs = { .before = canary, .after = canary };
	struct guarded_counts counts = { .before = canary, .after = canary };

	vp9.probs.cpu = &probs.data;
	vp9.probs.size = sizeof(probs.data);
	vp9.counts.cpu = &counts.data;
	vp9.counts.size = sizeof(counts.data);
	test_pack(&ctx);
	test_count_mapping(&ctx);
	test_adaptation(&ctx);
	assert(probs.before == canary && probs.after == canary);
	assert(counts.before == canary && counts.after == canary);
	puts("PASS: VP9 probability layout, table packing, count mapping, and context adaptation");
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

    def read_kernel(name):
        if args.kernel_tree:
            return (args.kernel_tree / name).read_bytes()
        with tarfile.open(args.kernel_archive) as archive:
            matches = [m for m in archive.getmembers() if m.isfile() and m.name.endswith("/" + name)]
            if len(matches) != 1:
                raise ValueError(f"Expected one upstream {name}, found {len(matches)}")
            return archive.extractfile(matches[0]).read()

    (driver / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="vp9-host-", dir=driver / "build") as name:
        build = Path(name)
        for subdir in ("linux", "media"):
            (build / subdir).mkdir()
        files = {
            "host.h": COMMON.encode(),
            "linux/types.h": TYPES.encode(),
            "linux/module.h": b'#include "host.h"\n',
            "linux/const.h": b'#define _BITUL(x) (1UL << (x))\n#define _BITULL(x) (1ULL << (x))\n',
            "media/v4l2-ctrls.h": b'#include "host.h"\n#include <linux/v4l2-controls.h>\n',
            "linux/v4l2-controls.h": read_kernel("include/uapi/linux/v4l2-controls.h"),
            "media/v4l2-vp9.h": read_kernel("include/media/v4l2-vp9.h"),
            "v4l2-vp9.c": read_kernel("drivers/media/v4l2-core/v4l2-vp9.c"),
            "test.c": HARNESS.encode(),
        }
        for relative, data in files.items():
            (build / relative).write_bytes(data)
        binary = build / "vp9-probs-test"
        command = shlex.split(args.cc) + [
            "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-sign-compare",
            "-Wno-unused-parameter",
            "-Werror=implicit-function-declaration", "-fno-omit-frame-pointer",
            "-I", str(build), "-I", str(driver), "-include", str(build / "host.h"),
        ]
        if not args.no_sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        command += [str(driver / "th1520_vdec_vp9_probs.c"), str(build / "v4l2-vp9.c"),
                    str(build / "test.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
