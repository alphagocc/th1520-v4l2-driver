#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build only the LGPL GStreamer 1.22.0 v4l2codecs plugin; install nothing.
# GST_DEV_ROOT may point to an extracted development-package tree.
set -eu
TOOLS_DIR=${0%/*}
[ "$TOOLS_DIR" != "$0" ] || TOOLS_DIR=.
DRIVER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)
WORK=${GST_CROP_BUILD:-"$DRIVER_DIR/build/gstreamer-crop"}
DEV=${GST_DEV_ROOT:-"$WORK/sysroot"}
TRIPLET=$(cc -dumpmachine)
SRC="$WORK/gst-plugins-bad-1.22.0"
OUT="$WORK/plugin"
PATCH="$DRIVER_DIR/patches/gstreamer-1.22-hevc-crop-fastpath.patch"
PATCHED=${GST_CROP_PATCHED:-1}
case "$PATCHED" in 0|1) ;; *) printf 'GST_CROP_PATCHED must be 0 or 1.\n' >&2; exit 1;; esac
# gst/codecs is an unstable API: this standalone build is for the tested board.
require_package()
{
    actual=$(dpkg-query -W -f='${Version}' "$1")
    [ "$actual" = "$2" ] || {
        printf 'Expected %s=%s; found %s. Check the unstable codecs ABI before rebuilding.\n' "$1" "$2" "$actual" >&2
        exit 1
    }
}
require_package libgstreamer1.0-0 1.22.0-2
require_package libgstreamer-plugins-bad1.0-0 1.22.0-4+deb12u5
require_package libgstreamer-plugins-base1.0-0 1.22.0-3revyos2
mkdir -p "$WORK" "$OUT"
ARCHIVE="$WORK/gst-plugins-bad-1.22.0.tar.xz"
ARCHIVE_SHA=3c9d9300f5f4fb3e3d36009379d1fb6d9ecd79c1a135df742b8a68417dd663a1
if ! printf '%s  %s\n' "$ARCHIVE_SHA" "$ARCHIVE" | sha256sum -c - >/dev/null 2>&1; then
    curl -fL --max-time 120 -o "$ARCHIVE.part" \
        https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-1.22.0.tar.xz
    printf '%s  %s\n' "$ARCHIVE_SHA" "$ARCHIVE.part" | sha256sum -c -
    mv "$ARCHIVE.part" "$ARCHIVE"
fi
printf '%s  %s\n' "$ARCHIVE_SHA" "$ARCHIVE" | sha256sum -c -
[ -d "$SRC" ] || tar -xf "$ARCHIVE" -C "$WORK"
if [ "$PATCHED" = 1 ]; then
    if patch --dry-run --batch --forward -d "$SRC" -p3 < "$PATCH" >/dev/null 2>&1; then
        patch --batch --forward -d "$SRC" -p3 < "$PATCH"
    else
        patch --dry-run --batch --reverse -d "$SRC" -p3 < "$PATCH" >/dev/null
    fi
else
    patch --dry-run --batch --forward -d "$SRC" -p3 < "$PATCH" >/dev/null
fi
# Source list matches sys/v4l2codecs/meson.build in this pinned release.
# Reuse the installed codecs/parser libraries, including distribution fixes.
cat > "$OUT/config.h" <<'EOF'
#define PACKAGE "gst-plugins-bad"
#define VERSION "1.22.0"
#define GST_PACKAGE_NAME "TH1520 local v4l2codecs build"
#define GST_PACKAGE_ORIGIN "https://gstreamer.freedesktop.org/"
#define HAVE_MAKEDEV_IN_SYSMACROS 1
EOF
[ -f "$DEV/usr/include/gstreamer-1.0/gst/gst.h" ] && [ -f "$DEV/usr/include/glib-2.0/glib.h" ] || {
    printf 'Development headers missing under %s. See docs/gstreamer-performance.md.\n' "$DEV" >&2
    exit 1
}
grep -Eq '^#define GST_VERSION_MAJOR +\(1\)' "$DEV/usr/include/gstreamer-1.0/gst/gstversion.h"
grep -Eq '^#define GST_VERSION_MINOR +\(22\)' "$DEV/usr/include/gstreamer-1.0/gst/gstversion.h"
grep -Eq '^#define GST_VERSION_MICRO +\(0\)' "$DEV/usr/include/gstreamer-1.0/gst/gstversion.h"
{
    printf 'GStreamer 1.22.0; patched=%s\n' "$PATCHED"
    cc --version
    dpkg-query -W libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 libgstreamer-plugins-bad1.0-0
    sha256sum "$PATCH" "$ARCHIVE"
} > "$OUT/build-info.txt"
set --
for file in plugin gstv4l2codecallocator gstv4l2codecdevice gstv4l2codech264dec \
    gstv4l2codech265dec gstv4l2codecmpeg2dec gstv4l2codecpool gstv4l2codecvp8dec \
    gstv4l2codecvp9dec gstv4l2decoder gstv4l2format gstv4l2codecalphadecodebin; do
    set -- "$@" "$SRC/sys/v4l2codecs/$file.c"
done
cc -shared -fPIC -O2 -g -Wall -Wextra -Wno-unused-parameter \
    -DHAVE_CONFIG_H -DGST_USE_UNSTABLE_API \
    -I"$OUT" -I"$SRC/gst-libs" -I"$SRC/sys/v4l2codecs" \
    -I"$DEV/usr/include/gstreamer-1.0" -I"$DEV/usr/include/glib-2.0" \
    -I"$DEV/usr/lib/$TRIPLET/glib-2.0/include" -I"$DEV/usr/include/gudev-1.0" \
    "$@" -Wl,-z,defs \
    -l:libgstcodecs-1.0.so.0 -l:libgstcodecparsers-1.0.so.0 \
    -l:libgstbase-1.0.so.0 -l:libgstreamer-1.0.so.0 -l:libgstvideo-1.0.so.0 \
    -l:libgstallocators-1.0.so.0 -l:libgstpbutils-1.0.so.0 \
    -l:libgobject-2.0.so.0 -l:libglib-2.0.so.0 -l:libgudev-1.0.so.0 \
    -o "$OUT/libgstv4l2codecs.so.part"
mv "$OUT/libgstv4l2codecs.so.part" "$OUT/libgstv4l2codecs.so"
sha256sum "$OUT/libgstv4l2codecs.so"
printf 'Temporary plugin: %s\n' "$OUT"
printf 'Use GST_PLUGIN_PATH_1_0=%s and a separate GST_REGISTRY.\n' "$OUT"
