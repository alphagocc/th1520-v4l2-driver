#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Fault injection in a separate build copy, followed by release-module restore.
# Run as root after board-build.sh and after compiling tools/request-test.
set -eu
[ "$(id -u)" -eq 0 ] || { printf 'Run with sudo.\n' >&2; exit 1; }
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
TEST_DIR="$DRIVER_DIR/build/watchdog-test"
HELPERS="$DRIVER_DIR/build/helpers-$(uname -r)"
mkdir -p "$TEST_DIR"
cp "$DRIVER_DIR/"*.c "$DRIVER_DIR/"*.h "$DRIVER_DIR/Kbuild" "$TEST_DIR/"

# Drop exactly one software completion after acknowledging a real ready IRQ.
# The hardware finishes normally; the 2-second watchdog owns request recovery.
python3 - "$TEST_DIR/th1520_vdec_hw.c" <<'PY'
import pathlib
import sys
p = pathlib.Path(sys.argv[1])
source = p.read_text()
needle = '\tctx = vpu->active_ctx;\n\tvpu->active_ctx = NULL;'
assert source.count(needle) == 1, 'IRQ ownership site changed'
injection = '''\t{
\t\tstatic bool drop_once = true;
\t\tif (drop_once && vpu->active_ctx &&
\t\t    (status & TH1520_IRQ_DEC_RDY_INT)) {
\t\t\tdrop_once = false;
\t\t\tctx = NULL;
\t\t\tdev_info(vpu->dev, "watchdog test: suppressing one completion\\n");
\t\t} else {
\t\t\tctx = vpu->active_ctx;
\t\t\tvpu->active_ctx = NULL;
\t\t}
\t}
'''
p.write_text(source.replace(needle, injection))
PY
make -C "$KDIR" M="$TEST_DIR" \
    KBUILD_EXTRA_SYMBOLS="$HELPERS/Module.symvers" -j4 modules
sha256sum "$TEST_DIR/th1520-vdec.ko" "$DRIVER_DIR/th1520-vdec.ko"
restore_release()
{
    sh "$DRIVER_DIR/tools/board-load.sh"
}
trap restore_release EXIT
rmmod th1520_vdec
insmod "$TEST_DIR/th1520-vdec.ko"
"$DRIVER_DIR/tools/request-test" --expect-timeout-once --data-offset 13
