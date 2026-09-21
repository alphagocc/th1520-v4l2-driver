#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build on RevyOS using headers matching the running kernel.
set -eu

TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
if [ -z "${KDIR:-}" ]; then
    KDIR="/lib/modules/$(uname -r)/build"
    if [ ! -r "$KDIR/include/config/kernel.release" ] &&
       [ -r /usr/src/linux-headers-6.6-th1520/include/config/kernel.release ]; then
        KDIR=/usr/src/linux-headers-6.6-th1520
    fi
fi
JOBS=${JOBS:-4}
RELEASE=$(cat "$KDIR/include/config/kernel.release")
if [ "$RELEASE" != "$(uname -r)" ]; then
    printf 'Kernel headers %s differ from running kernel %s\n' "$RELEASE" "$(uname -r)" >&2
    exit 1
fi

# RevyOS 6.6.140 omits these helper modules because no in-tree M2M decoder
# selects them. Keep the upstream GPL sources and their provenance in build/.
HELPERS="$DRIVER_DIR/build/helpers-$RELEASE"
mkdir -p "$HELPERS"
TAG="v${RELEASE%-th1520}"
BASE="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/drivers/media/v4l2-core"
for NAME in v4l2-mem2mem v4l2-h264; do
    if [ ! -s "$HELPERS/$NAME.c" ]; then
        curl --fail --silent --show-error --location --retry 2 \
            --connect-timeout 15 --max-time 60 \
            "$BASE/$NAME.c?h=$TAG" -o "$HELPERS/$NAME.c"
    fi
done
printf 'obj-m += v4l2-mem2mem.o\nobj-m += v4l2-h264.o\n' > "$HELPERS/Kbuild"
printf '%s\n' "$BASE/v4l2-mem2mem.c?h=$TAG" "$BASE/v4l2-h264.c?h=$TAG" > "$HELPERS/SOURCES"
make -C "$KDIR" M="$HELPERS" -j"$JOBS" modules
make -C "$DRIVER_DIR" KDIR="$KDIR" \
    KBUILD_EXTRA_SYMBOLS="$HELPERS/Module.symvers" -j"$JOBS" modules
sha256sum "$DRIVER_DIR/th1520-vdec.ko" \
    "$HELPERS/v4l2-mem2mem.c" "$HELPERS/v4l2-h264.c"
