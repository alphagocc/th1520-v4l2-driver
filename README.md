# TH1520 VC8000D V4L2 解码驱动

`th1520-vdec.ko` 为 TH1520 的 VC8000D 提供 Linux V4L2 stateless 解码接口，
支持 H.264、HEVC 和 VP9，输出线性 NV12。用户空间通过 Media Request API
提交解析后的帧参数并管理参考帧，驱动负责硬件解码、缓冲区和任务调度。

**日常解码与性能测量优先使用支持 V4L2 Request API 的 FFmpeg。** 本项目
验证过的客户端为 FFmpeg 8.1 的 `v4l2-request-n8.1` 分支。以下示例统一使用
包含 H.264、HEVC、VP9 的构建，GStreamer 和底层接口测试见文末说明。

## 支持范围

| 编码 | Profile | 图像格式 | 板端像素测试覆盖 |
| --- | --- | --- | --- |
| H.264 | Baseline、Main、High | 逐行、8 bit、4:2:0 | 至 3840×2160 |
| HEVC | Main、Main Still Picture | 逐行、8 bit、4:2:0 | 至 3840×2160 |
| VP9 | Profile 0 | 8 bit、4:2:0，宽高为偶数 | 至 1920×1080 |

驱动允许的最大缓冲尺寸为 4096×2304，CAPTURE 格式为 NV12。表中的尺寸表示
实际像素测试范围；VP9 的更大尺寸以及各编码的长期运行仍需单独验证。
分辨率变化由客户端重新协商缓冲格式和容量。

验证环境为 TH1520、RevyOS、Linux `6.6.140-th1520`。平台资源、设备树和
内核兼容要求见 [硬件说明](docs/hardware.md)，VP9 的具体约束见
[VP9 实现说明](docs/vp9.md)。

寄存器地址、位域、各编码模式的参数及 PP 配置见
[VC8000D 寄存器指南](docs/swreg.md)。

## 准备运行环境

以下命令在开发板的驱动仓库目录执行，该目录包含 `Kbuild` 和 `tools/`。
需要 C 编译器、GNU make、curl、pkg-config、Python 3、v4l-utils，以及与
运行内核匹配的头文件。运行 FFmpeg 的账户须有 `/dev/videoN` 和 `/dev/mediaN`
的读写权限，设备编号以驱动加载后的输出为准。

### 构建并加载驱动

```sh
KDIR=/usr/src/linux-headers-6.6-th1520 JOBS=4 sh tools/board-build.sh
sudo sh tools/board-load.sh
```

`board-build.sh` 核对头文件版本与 `uname -r`，从 Linux stable 获取匹配版本的
`v4l2-mem2mem`、`v4l2-h264`、`v4l2-vp9` helper，在项目目录编译这些模块和
驱动，并记录源码地址与校验值。

加载脚本会卸载原解码模块，因此执行前应关闭使用 VPU 的程序。新模块仅在
本次启动期间加载，重启后沿用系统原有配置。加载成功后会列出对应的 video
设备和 media 设备。

### 准备 FFmpeg

FFmpeg 源码压缩包来自
[Kwiboo FFmpeg 的 v4l2-request-n8.1 分支](https://code.ffmpeg.org/Kwiboo/FFmpeg/src/branch/v4l2-request-n8.1)。
从该页面下载 ZIP，按 [源码与依赖准备说明](docs/ffmpeg-request.md)核对压缩包
SHA-256，并在 `build/ffmpeg-request-runtime/sysroot` 准备匹配系统运行库的
libdrm、libudev 开发文件。构建脚本使用本次测试压缩包的固定校验值；分支内容
更新后，新下载的压缩包需要重新核对版本和构建条件。

将压缩包保存为 `build/FFmpeg-v4l2-request-n8.1.zip` 后执行：

```sh
JOBS=2 sh tools/build-ffmpeg-request.sh build/FFmpeg-v4l2-request-n8.1.zip
JOBS=2 sh tools/build-ffmpeg-vp9-request.sh "$PWD"

FFMPEG="$PWD/build/ffmpeg-request-vp9/ffmpeg"
"$FFMPEG" -hide_banner -hwaccels
"$FFMPEG" -hide_banner -decoders
```

第一步生成 H.264、HEVC 构建；第二步复用其源码、配置和依赖，在独立目录加入
VP9。后续统一使用 `build/ffmpeg-request-vp9/ffmpeg`。构建保留在项目目录，
具体配置与来源记录见 [FFmpeg VP9 Request 构建说明](docs/ffmpeg-vp9-request.md)。

使用现有 FFmpeg 时，将 `FFMPEG` 指向对应程序。`-hwaccels` 输出应包含
`v4l2request`，`-decoders` 应包含需要的 `h264`、`hevc` 或 `vp9`。
实际硬件选择还须通过解码日志确认，方法见下一节。

## 使用 FFmpeg 解码

通过 `-hwaccel v4l2request` 选择硬件加速，输入解码器仍使用普通的编码名称：

| 编码 | `-c:v` 参数 | 示例输入 |
| --- | --- | --- |
| H.264 | `h264` | `input.h264` |
| HEVC | `hevc` | `input.h265` |
| VP9 | `vp9` | `input.ivf` |

以下以 VP9 为例。处理其他编码时，修改 `CODEC` 和 `INPUT`。该 FFmpeg 构建
同时支持 MP4、Matroska 和 WebM 容器，输入格式由 FFmpeg 自动探测。

```sh
CODEC=vp9
INPUT=input.ivf

"$FFMPEG" -hide_banner -loglevel verbose \
  -hwaccel v4l2request -hwaccel_output_format drm_prime \
  -c:v "$CODEC" -threads 1 -i "$INPUT" \
  -map 0:v:0 -an -sn -dn -fps_mode passthrough \
  -c:v wrapped_avframe -pix_fmt +drm_prime \
  -f null - -progress pipe:1
```

此命令保留 DRM 硬件帧并送往 null 输出，适合检查硬件解码和测量吞吐量。
`+drm_prime` 要求输出保持硬件帧格式。日志应显示选中的 media driver 为
`th1520-vdec`，CAPTURE 格式为 NV12，例如：

```text
Using V4L2 media driver th1520-vdec (...) for VP9F
Using CAPTURE buffer format NV12 (1920x1088)
```

正常结束时应返回退出码 0，并输出完整帧数和 `progress=end`。
CAPTURE 存储高度可能包含补齐行，例如 1080 行图像使用 1088 行缓冲；
客户端依据帧参数处理可见图像区域。

### 导出 NV12 图像

需要保存图像或进行像素比较时，使用 `hwdownload` 下载硬件帧：

```sh
"$FFMPEG" -hide_banner -loglevel verbose \
  -hwaccel v4l2request -hwaccel_output_format drm_prime \
  -c:v "$CODEC" -threads 1 -i "$INPUT" \
  -map 0:v:0 -an -sn -dn -fps_mode passthrough \
  -vf hwdownload,format=nv12 \
  -c:v rawvideo -pix_fmt nv12 -f rawvideo output.nv12
```

`output.nv12` 为无容器的原始图像序列，读取时需要指定可见宽高。
图像下载和文件写入会增加处理时间，纯解码吞吐量使用前一节的 DRM 帧与 null
输出方式测量。

## 性能测量

下表为板端 FFmpeg V4L2 Request 的 1080p 测量结果。每份输入为连续 600 帧，
预热后执行三轮，表中列出硬件解码帧率的中位数。各编码使用各自的测试输入，
详细参数、驱动版本和测量条件见对应报告。

| 编码 | 硬件解码帧率 | 报告 |
| --- | ---: | --- |
| H.264 | 326.40 fps | [FFmpeg Request 测试](docs/ffmpeg-request.md) |
| HEVC | 415.05 fps | [FFmpeg Request 测试](docs/ffmpeg-request.md) |
| VP9 | 224.01 fps | [VP9 benchmark](docs/vp9-benchmark.md) |

VP9 同一构建的四线程软件解码为 68.94 fps。进程 CPU 用量中位数为硬件
28.5%、软件 357.8%，100% 表示一个逻辑核心。测试构建保留 `--disable-asm`，
软件性能应按这一编译配置解读。

VP9 输入生成及测量入口为：

```sh
sh tools/make-vp9-benchmark.sh test-results/vp9-benchmark
python3 tools/benchmark-vp9-request.py \
  --ffmpeg "$FFMPEG" \
  --input test-results/vp9-benchmark/vp9-1920x1080.ivf \
  --output test-results/vp9-benchmark-results \
  --frames 600 --rounds 3 --software-threads 4
```

生成输入需要带 `libvpx-vp9` 编码器和 `trace_headers` 的 FFmpeg、ffprobe。
可以在另一台计算机生成后传送到开发板。每次测量使用独立的结果目录，工具保存
逐轮命令、日志、帧数、耗时、CPU 用量和 JSON 汇总。

## 验证与其他客户端

`v4l2-compliance 1.28.1` 的 56 项检查通过，失败 0、警告 0。H.264、HEVC 的
像素测试、容器与分辨率变化，以及保留差异的 HEVC 样本见
[验证记录](docs/validation.md)。VP9 的 198 帧像素比较、230 项异常请求、
并发和错误恢复结果见 [VP9 验证记录](docs/vp9-validation.md)。

GStreamer 可用于应用集成和项目测试矩阵，硬件解码元素为
`v4l2slh264dec`、`v4l2slh265dec`、`v4l2slvp9dec`。现有
`tools/decode-matrix.sh` 使用这些元素执行像素比较；GStreamer 1.22 的 HEVC
裁剪处理说明见 [GStreamer 性能记录](docs/gstreamer-performance.md)。

控件、Request 生命周期、截断帧和 watchdog 检查的入口汇总在
[工具说明](tools/README.md)。相关实现采用公开的 V4L2 stateless decoder
和 Media Request API，源码依据、固定上游版本及许可证见
[来源记录](docs/sources.md)。
