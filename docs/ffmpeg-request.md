# FFmpeg V4L2 Request 临时构建与测试

此客户端使用用户提供的 `FFmpeg-v4l2-request-n8.1.zip`，来源分支为
[Kwiboo FFmpeg v4l2-request-n8.1](https://code.ffmpeg.org/Kwiboo/FFmpeg/src/branch/v4l2-request-n8.1)。
源码内 `RELEASE` 为 `8.1`，压缩包 SHA-256 为：

```text
d98b03c25d755015aa0e6a943069241b2c54e5d3ba30369aea76176540f5c352
```

该压缩包的精确 commit 尚未确认。核对分支接口时，服务器返回防机器人
验证页面，因此版本依据为压缩包校验值与内部源码。既有源码目录中的修改
会保留，压缩包校验值仅证明原始输入，无法证明经过修改的源码目录内容。

## 临时依赖

目标环境为 RevyOS riscv64、Linux `6.6.140-th1520`。构建需要 C 编译器、
GNU make、pkg-config、Python 3 和 Linux UAPI 头文件。使用以下两个匹配
系统运行库的开发包，下载与解压均在项目目录进行。

| 软件包 | 版本 |
| --- | --- |
| `libdrm-dev` | `1:2.4.124-2revyos1` |
| `libudev-dev` | `257.9-1~deb13u1` |

以下命令用于准备尚未配置的临时依赖目录。`ROOT` 指向项目根目录，既有
临时依赖可以复用；构建脚本会检查所需文件。

```sh
ROOT=/home/debian/th1520-v4l2
RUNTIME="$ROOT/driver/build/ffmpeg-request-runtime"
SYSROOT="$RUNTIME/sysroot"
mkdir -p "$RUNTIME/packages" "$SYSROOT"
cd "$RUNTIME/packages"
apt-get download 'libdrm-dev=1:2.4.124-2revyos1' \
  'libudev-dev=257.9-1~deb13u1'
for package in ./*.deb; do dpkg-deb -x "$package" "$SYSROOT"; done
```

开发包中的链接器入口需要指向本机匹配的共享库。以下两处链接位于临时
sysroot 内，系统共享库文件保持原样。

```sh
ln -sfn /usr/lib/riscv64-linux-gnu/libdrm.so.2 \
  "$SYSROOT/usr/lib/riscv64-linux-gnu/libdrm.so"
ln -sfn /usr/lib/riscv64-linux-gnu/libudev.so.1 \
  "$SYSROOT/usr/lib/riscv64-linux-gnu/libudev.so"
```

脚本在自身进程中设置 `PKG_CONFIG_SYSROOT_DIR`，并将
`PKG_CONFIG_LIBDIR` 限定为临时 sysroot 的
`usr/lib/riscv64-linux-gnu/pkgconfig` 和 `usr/share/pkgconfig`，使用匹配的
开发头文件及链接器入口。其他发行版应分别核对软件包版本与实际共享库。

## 构建

`tools/build-ffmpeg-request.sh` 默认读取
`driver/build/FFmpeg-v4l2-request-n8.1.zip`，也接受压缩包的显式位置：

```sh
sh "$ROOT/driver/tools/build-ffmpeg-request.sh"
# 压缩包保存在项目根目录时：
sh "$ROOT/driver/tools/build-ffmpeg-request.sh" \
  "$ROOT/FFmpeg-v4l2-request-n8.1.zip"
```

脚本先核对 SHA-256，检查解压目的地属于项目构建目录，并拒绝越界文件及
压缩包符号链接。源码解压到 `driver/build/ffmpeg-request-source/ffmpeg`；
存在完整源码时保留全部内容，缺少 `configure` 的非空目录会报告错误。
构建目录为 `driver/build/ffmpeg-request-build`，默认执行 `make -j4`，
可以通过 `JOBS` 设置并行任务数。

本轮配置启用 H.264、HEVC parser 与 decoder，以及对应的 V4L2 Request
HWAccel，支持 elementary stream、MP4、MKV 输入和 NV12 图像下载。
配置使用 `--disable-asm`，用于排除 C910 旧 RVV 及汇编兼容因素；此产物
用于验证 Request API 和应用吞吐量，软件 decoder 速度应按该编译限制解读。
完整参数记录在构建目录的 `configure-arguments.txt`。

产物为同目录的 `ffmpeg` 和 `ffprobe`。`source-provenance.json` 记录源码
来源与复用状态，`dependency-versions.txt`、`dependency-flags.txt`、
`version.txt`、`hwaccels.txt` 和 `SHA256SUMS` 保存构建证据。脚本仅执行
编译，模块管理沿用项目临时加载说明，系统安装目录及开机配置保持原样。

## 硬件解码选择

此分支的 `h264_v4l2request`、`hevc_v4l2request` 属于 HWAccel。
命令使用普通 `-c:v h264` 或 `-c:v hevc`，同时指定
`-hwaccel v4l2request -hwaccel_output_format drm_prime`。
吞吐量测试使用 DRM 帧及 null 输出，避免每帧下载图像。

```sh
FFMPEG="$ROOT/driver/build/ffmpeg-request-build/ffmpeg"
INPUT="$ROOT/test-results/benchmark-fixtures/hevc-1920x1080.h265"
"$FFMPEG" -hide_banner -loglevel verbose \
  -hwaccel v4l2request -hwaccel_output_format drm_prime \
  -c:v hevc -threads 1 -f hevc -i "$INPUT" \
  -map 0:v:0 -an -sn -dn -fps_mode passthrough \
  -c:v wrapped_avframe -pix_fmt +drm_prime \
  -f null - -progress pipe:1
```

H.264 测试将输入 codec 和 demuxer 两处 `hevc` 改为 `h264`，并选择对应
1080p 文件。输出使用 `+drm_prime` 强制保留硬件格式，验证日志应确认
选中的 media 设备属于 `th1520_vdec`，CAPTURE 格式为 NV12，并核对完整
帧数和最终 `progress=end`。测试使用 1080p 输入。

该分支的设备选择仍自动枚举 media 设备；源码
`libavutil/hwcontext_v4l2request.c:208` 对指定设备的处理标为 TODO。
因此设备身份以实际日志为依据。

像素验证单独执行图像下载，将前述输出参数替换为：

```sh
-vf hwdownload,format=nv12 \
-c:v rawvideo -pix_fmt nv12 -f rawvideo "$OUTPUT"
```

软件参考通过系统 FFmpeg 的 `-hwaccel none -c:v hevc` 或
`-hwaccel none -c:v h264` 生成，同样输出 NV12，再比较完整图像。
此前确认与厂商一致的输出差异继续按既有测试记录处理。

## ABI 检查依据

`libavcodec/v4l2_request_h264.c:351` 在 FRAME_BASED 模式提交 SPS、PPS、
SCALING_MATRIX 和 DECODE_PARAMS。`libavcodec/v4l2_request_hevc.c:656`
支持 FRAME_BASED 与 ANNEX_B，并在 `:677` 探测可选的 SLICE_PARAMS 和
ENTRY_POINT_OFFSETS，符合当前驱动的控件集合。

`libavutil/hwcontext_v4l2request.c:68` 支持线性 NV12，`:154` 使用
`bytesperline × negotiated height` 计算 UV 偏移，与驱动输出布局相符。
该客户端通过 `VIDIOC_CREATE_BUFS` 逐个扩充缓冲池。本轮板端测试通过该
分配方式完成初始化和连续解码，结果记录于下一节。

## 2026-09-22 板端结果

开发板为 TH1520、Linux `6.6.140-th1520`。FFmpeg 8.1 运行日志确认选择
`th1520` media driver，CAPTURE 格式为 NV12。三份图像测试的完整输出均
与系统 FFmpeg 软件参考相同：

| 输入 | 帧数 | 完整 NV12 输出比较 |
| --- | ---: | --- |
| H.264 High 1920×1080 | 8 | 相同 |
| HEVC 1920×1080，每帧四个 slice | 8 | 相同 |
| HEVC 裁剪至 322×242 | 12 | 相同 |

速度测量使用此前相同的两份 1080p 输入，每份 600 帧，各预热一次并正式
执行三轮。FFmpeg 输出强制为 `+drm_prime`，送往 `wrapped_avframe` 和
null muxer，测量期间省略图像下载。表格列出三轮中位数，单位为 fps。

| 编码 | FFmpeg V4L2 Request | GStreamer 裁剪修复后 | FFmpeg 厂商 OMX |
| --- | ---: | ---: | ---: |
| H.264 | 326.40 | 101.67 | 236.19 |
| HEVC | 415.05 | 109.22 | 296.15 |

H.264 三轮分别为 324.81、329.22、326.40 fps，HEVC 分别为
414.70、415.05、415.58 fps。FFmpeg Request 的 CPU 用量约为 H.264
45%、HEVC 48%，100% 代表一个逻辑 CPU。GStreamer 使用其现有图像
输出处理，FFmpeg 保留 DRM 帧；这些结果比较的是各前端的应用吞吐量。
像素正确性由独立图像测试核对。

本轮使用的临时驱动模块 SHA-256 保持为
`de46e6b4851a19af202e72520f8d6c7f4bb3f762e2713060d5c73b12cd4457a3`。
性能提高来自用户态解码客户端与输出处理的变化，驱动模块保持原样。
