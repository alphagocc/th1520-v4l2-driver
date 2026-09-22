# GStreamer 1.22 HEVC 裁剪性能修复

2026-09-22 在 TH1520、Linux `6.6.140-th1520` 上，将 1080p HEVC 的应用
解码速度从 6.86 fps 提高到 109.22 fps，约为原来的 15.9 倍。相同输入
每轮 600 帧，三轮中位数从 87.47 秒降至 5.49 秒，输出帧数完整且丢帧为零。
H.264 为 101.67 fps，与此前 102.19 fps 接近。

## 原因与修改

60 帧仅解析约 0.17 秒，完整解码约 9.8 秒。用户态采样的主要热点位于
`libgstvideo`，单帧调试日志确认 HEVC 裁剪将 NV12 展开为 AYUV 后重新打包。

GStreamer 1.22.0 的 `gstv4l2codech265dec.c` 将
`GST_VIDEO_CONVERTER_OPT_DITHER_QUANTIZATION` 设为 `0`，而 converter
仅在该值为 `1` 时选择快速实现。本次将其恢复为默认值 `1`，保留
`GST_VIDEO_DITHER_NONE`、裁剪区域和图像尺寸。日志确认修改后使用现有的
逐平面复制。源码和许可见 [补丁来源](../patches/README.md)。

以同一官方源码和编译选项构建的未修改插件，完整 60 帧仍需 9.61 秒；
修复插件为 1.45 秒。该短片包含进程启动开销，完整 600 帧结果用于吞吐量
比较。内核驱动、寄存器和 DMA 设置保持原样。

## 临时使用

板端构建结果保存在 `driver/build/gstreamer-crop/plugin/`。在驱动仓库根目录
使用以下入口，仅为本次命令选择修复插件：

```sh
sh tools/with-gst-crop.sh gst-inspect-1.0 v4l2slh265dec
sh tools/with-gst-crop.sh gst-launch-1.0 -q \
  filesrc location=/path/to/input.h265 ! h265parse ! v4l2slh265dec ! fakesink sync=false
```

`gst-inspect` 的 `Filename` 应指向项目的 `build/gstreamer-crop/plugin/`。
系统插件文件和开机配置保持原样。模块仍使用 `board-load.sh` 临时加载。
像素比较和 benchmark 脚本也可放在 `with-gst-crop.sh` 后执行。

## 重新构建

构建器限定本板验证过的包组合：core `1.22.0-2`、base `1.22.0-3revyos2`、
bad `1.22.0-4+deb12u5`。`gst/codecs` 属于 unstable API，其他版本应先核对
头文件和运行库，构建器会拒绝版本差异。

开发头文件可解压到项目目录，保持系统软件安装状态原样：

```sh
mkdir -p build/gstreamer-crop/debs
cd build/gstreamer-crop/debs
apt-get download libglib2.0-dev=2.82.0-1 \
  libgstreamer1.0-dev=1.22.0-2 \
  libgstreamer-plugins-base1.0-dev=1.22.0-3revyos2 \
  libgudev-1.0-dev=238-5
for package in ./*.deb; do dpkg-deb -x "$package" ../sysroot; done
cd ../../..
sh tools/build-gstreamer-crop.sh
```

工具下载并核对官方 `gst-plugins-bad-1.22.0.tar.xz`，应用单行补丁，仅编译
`v4l2codecs` 插件并链接系统现有 codec、parser 与 video 库。上游源码的版权
和 LGPL 文本保留在构建目录。原始源码、包、插件与构建信息均保存在 `build/`。
`GST_CROP_BUILD` 可指定其他构建目录，`GST_DEV_ROOT` 可指定头文件目录。
独立目录中设置 `GST_CROP_PATCHED=0` 可构建未修改对照版本。

## 正确性与测量范围

HEVC 1080p 8 帧、322×242 12 帧及左裁剪 4、上裁剪 2 的 12 帧均与软件
逐字节相同。MP4、MKV 和两组分辨率变化的四项测试共 70 帧全部通过。
非零左裁剪需要软件参考启用 `-flags +unaligned`，否则 FFmpeg 可能按照
SIMD 对齐限制舍去部分左裁剪；`decode-matrix.sh` 同步补充此设置。

此前厂商 HEVC 为 296.15 fps，当前应用吞吐量仍有差距。本次修复针对
GStreamer 的多余像素转换，保留现有输出复制和原生图像格式。4K 测量按用户
要求保持取消。
