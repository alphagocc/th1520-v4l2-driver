# FFmpeg 8.1 VP9 Request 独立构建

本构建的同源八帧硬件像素比较通过；1080p 600 帧三轮测量见
[VP9 benchmark](vp9-benchmark.md)，硬件中位数 224.01 fps，四线程软件
68.94 fps。

`tools/build-ffmpeg-vp9-request.sh` 复用现有 FFmpeg 8.1 源码和临时依赖，
将 VP9 功能加入另一处构建目录。脚本保留既有 FFmpeg、ffprobe 和源码文件，
构建完成后再次核对原二进制及源码清单中的 SHA-256。

本轮原构建位于
`/home/debian/th1520-v4l2/driver/build/ffmpeg-request-build`，源码位于
`/home/debian/th1520-v4l2/driver/build/ffmpeg-request-source/ffmpeg`。
原源码来源、依赖与配置记录见 [FFmpeg Request 说明](ffmpeg-request.md)。

新构建目录为
`/home/debian/th1520-vp9-20260923/driver/build/ffmpeg-request-vp9`。
构建命令为：

```sh
JOBS=2 sh /home/debian/th1520-vp9-20260923/driver/tools/build-ffmpeg-vp9-request.sh \
  /home/debian/th1520-v4l2/driver \
  /home/debian/th1520-vp9-20260923/driver/build/ffmpeg-request-vp9
```

脚本读取原 `configure-arguments.txt`，仅增补四个组件：

| 原组件集合 | 增补组件 |
| --- | --- |
| decoder `h264,hevc` | `vp9` |
| HWAccel `h264_v4l2request,hevc_v4l2request` | `vp9_v4l2request` |
| parser `h264,hevc` | `vp9` |
| demuxer `h264,hevc,mov,matroska` | `ivf` |

`--disable-asm` 及其他原配置保持原值，BSF 依赖由 configure 选择。
构建后要求 `CONFIG_VP9_SUPERFRAME_SPLIT_BSF` 为 1，并同时核查
H.264、HEVC、VP9 decoder、parser、V4L2 Request HWAccel 和 IVF demuxer。
`make` 并行任务数上限为 2。

源码清单 `source-SHA256SUMS` 记录当前源码文件的摘要；
`source-provenance.json` 保存该清单摘要、原来源记录及四项配置差异。
原压缩包摘要只代表原始输入，当前源码内容以本轮文件清单为依据。
精确 commit 身份仍沿用原来源记录中的未确认状态。

本轮将板端源码清单取回本机，逐文件对照原
`FFmpeg-v4l2-request-n8.1.zip`，10,142 个文件的 SHA-256 全部相同，
文件集合也相同。压缩包摘要为
`d98b03c25d755015aa0e6a943069241b2c54e5d3ba30369aea76176540f5c352`。
核查记录位于
`test-results/vp9-benchmark-20260923/ffmpeg-build/archive-comparison.json`。

`configure.log`、`make.log` 保存配置与编译输出，
`version.txt`、`hwaccels.txt`、`decoders.txt`、`demuxers.txt` 保存产物功能信息，
`SHA256SUMS` 保存新二进制摘要，`verification.json` 保存检查结果。
此脚本只执行构建和功能列表查询。

VP9 的 Request 功能属于 HWAccel，运行时使用 `-c:v vp9`、
`-hwaccel v4l2request`、`-hwaccel_output_format drm_prime` 和 IVF 输入。
性能测试继续保留 `--disable-asm` 这一软件 decoder 限制，并单独记录
图像下载验证与 DRM 帧 null 输出吞吐量。

2026-09-23 板端编译及自动核查通过。产物报告 FFmpeg 8.1，编译器为
GCC 14 `14.3.0-14revyos3~0old1`，`hwaccels` 列出 `drm` 和 `v4l2request`。
原 `ffmpeg`、`ffprobe` 及源码清单中 10,142 个文件的摘要在构建后保持原值。
新二进制 `ffmpeg` 的 SHA-256 为：

```text
cdcbb03becf348c2b4afc456f62cc7002f861bfb4af33881397c3018586ac98d
```

新二进制 `ffprobe` 的 SHA-256 为：

```text
6d2ab0f777c11f406cbe31068a05a5d42119f8da42b4061aa550250beea2a106
```

全部配置、编译日志、功能列表、来源清单和核查结果保存在
`test-results/vp9-benchmark-20260923/ffmpeg-build`。
