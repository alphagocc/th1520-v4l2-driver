# TH1520 VC8000D V4L2 stateless 解码驱动

`th1520-vdec.ko` 为 TH1520 的 VC8000D 解码核提供 Linux V4L2 memory-to-memory
接口，使用 stateless decoder 与 Media Request API。用户空间负责码流解析和
DPB 管理，驱动执行硬件解码并输出线性 NV12。

当前支持范围为逐行、8 bit、4:2:0 的 H.264 Baseline、Main、High 以及 HEVC
Main、Main Still Picture。OUTPUT 格式为 `V4L2_PIX_FMT_H264_SLICE` 或
`V4L2_PIX_FMT_HEVC_SLICE`，使用 Annex-B、每个请求提交完整帧；CAPTURE 格式为
`V4L2_PIX_FMT_NV12`。当前尺寸上限为 4096×2304，硬件测试覆盖至 3840×2160。

## 硬件验证状态

2026-09-21 至 2026-09-22，在 TH1520、RevyOS、Linux `6.6.140-th1520` 上完成
30 个合法样本、1020 帧的完整输出检查。ASIC ID 为 `0x80018000`，build ID
为 `0x1f88`。

| 样本组 | 样本数 | 帧数 | 比较结果 |
| --- | ---: | ---: | --- |
| H.264 测试矩阵 | 15 | 196 | 全部与软件逐字节相同 |
| 自生成 HEVC 测试矩阵 | 9 | 85 | 8 个软件一致，1 个厂商一致 |
| 官方 HEVC conformance | 6 | 739 | 5 个软件一致，1 个 SDK 尺寸范围外附加测试 |
| 合计 | **30** | **1020** | **28 个软件一致，1 个厂商一致，1 个范围外附加测试** |

`v4l2-compliance 1.28.1` 共 **56/56** 项通过，失败 0、警告 0。混合 H.264
与 HEVC 的两个 context，以及 Request API 的 2 worker、10 轮检查通过。
测试方法、覆盖范围和两个 HEVC 差异样本见 [验证记录](docs/validation.md)。

## 构建与临时加载

在本仓库根目录执行。构建需要 `make`、C 编译器、`curl` 和与运行内核匹配的
内核头文件。经过验证的 RevyOS 头文件目录为：

```sh
KDIR=/usr/src/linux-headers-6.6-th1520 JOBS=4 sh tools/board-build.sh
sudo sh tools/board-load.sh
```

`board-build.sh` 检查头文件的 `kernel.release` 与 `uname -r`。目标 RevyOS
内核缺少 `v4l2-mem2mem` 和 `v4l2-h264` helper，脚本从 kernel.org 获取匹配
版本的 Linux GPL 源文件，在 `build/helpers-<kernel-release>/` 编译这两个
模块，再构建驱动；`SOURCES` 记录下载地址。

加载前应关闭占用解码设备的程序。`board-load.sh` 在当前启动期间卸载原解码
模块，装载媒体依赖、两个 helper 和 `th1520-vdec.ko`，最后显示设备编号。
模块与构建产物保存在工作目录，开机配置保持原样；重启后沿用系统原有的加载
配置。再次测试时重新执行 `board-load.sh`。

目标设备树使用 `xuantie,th1520-vc8000d` 或 `thead,light-vc8000d`，`reg`
描述 VPU 子系统，驱动增加解码核偏移 `0x1000`。经过验证的开发板使用现有
设备树节点。平台资源与硬件配置见 [硬件说明](docs/hardware.md)。

## 解码与像素比较

生成样本需要带 `libx264`、`libx265` 的 FFmpeg 和 Python 3。板端需要
GStreamer 的 `h264parse`、`h265parse`、`videoconvert` 及 `v4l2codecs` 插件，
以及 `cmp`、`timeout`、`sha256sum`。加载驱动后检查硬件元素：

```sh
gst-inspect-1.0 v4l2slh264dec
gst-inspect-1.0 v4l2slh265dec
```

基础样本、混合 context 与 Request API 回归：

```sh
sh tools/make-fixtures.sh test-results/fixtures
sh tools/board-test.sh test-results
```

完整测试矩阵与官方样本：

```sh
sh tools/make-h264-matrix.sh test-results/h264-matrix
sh tools/decode-matrix.sh test-results/h264-matrix test-results/h264-matrix-results --timeout 300

sh tools/make-hevc-matrix.sh test-results/hevc-matrix
sh tools/decode-matrix.sh test-results/hevc-matrix test-results/hevc-matrix-results --timeout 300

sh tools/fetch-hevc-conformance.sh
sh tools/decode-matrix.sh test-results/hevc-conformance test-results/hevc-conformance-results --timeout 300
```

H.264 脚本核查实际码流特征，参数见 [H.264 矩阵说明](tools/h264-matrix.md)。
HEVC 脚本保存编码参数、日志、header trace 和软件 NV12；官方样本按固定大小
与 SHA-256 下载，来源见 [HEVC conformance 样本](docs/hevc-conformance-samples.md)。

软件基准显式选择 FFmpeg 的 `h264` 或 `hevc` decoder。硬件使用 GStreamer
stateless 元素，处理输出行距后比较全部 NV12 字节。`decode-matrix.sh` 保存
`summary.json`、`summary.tsv` 和逐例日志；现有 `.sw.nv12` 可作为软件参考。
两个保留差异的 HEVC 样本仍得到 `cmp=different` 和退出码 1，详细解释见
[HEVC 输出差异](docs/hevc-vendor-comparison.md)。

## Request API 与合规检查

`request-test.c` 生成 64×64 的 H.264 Baseline I_PCM 图像，提交 Media Request
并检查输出像素。设备编号以加载脚本的输出为准：

```sh
mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror -o build/request-test tools/request-test.c
./build/request-test --device /dev/video0 --media /dev/media0
./build/request-test --workers 2 --iterations 10 --data-offset 13 --malformed
./build/request-test --invalid-hevc-params

v4l2-compliance --version
v4l2-compliance -d /dev/video0 -m /dev/media0
```

`--malformed` 依次提交正常帧、截断帧与恢复帧，检查完成状态及恢复后的像素。
`--invalid-hevc-params` 检查七种异常 HEVC 控件。合规结果的比较基准为
`v4l2-compliance 1.28.1`，执行前应确认工具版本。

## 当前边界与后续验证

10 bit、4:2:2、4:4:4、H.264 场解码及 MBAFF 属于当前支持范围之外。SPS 尺寸
需要与协商的缓冲尺寸一致；分辨率变化需要用户空间重新协商和分配缓冲。
现有 4K 测试确认短序列输出正确，4K 吞吐量和长时间运行仍需单独测量。
1080p 的解码速度比较见 [benchmark 记录](docs/benchmark.md)。

`test-containers.sh` 提供 H.264 MP4、HEVC Matroska 以及两组分辨率变化检查。
2026-09-22 四项板端测试全部通过，共 70 帧与软件参考逐字节相同。
H.264 的三段尺寸为 320×240、640×360、1920×1080，HEVC 为
320×240、1280×720、1920×1080。生成前述 fixtures 和两个矩阵后，入口为：

```sh
sh tools/test-containers.sh "$PWD" test-results/containers
```

解码速度比较由 `tools/benchmark-decode.sh` 提供，使用相同输入比较 GStreamer
V4L2 stateless 硬件解码与 FFmpeg 软件解码，记录多轮帧率及 CPU 用量。
输入生成、计时范围与执行命令见 [benchmark 说明](docs/benchmark.md)。

GStreamer 1.22.0 的 HEVC 裁剪性能修复使 1080p 从 6.86 fps 提高到
109.22 fps，像素和分辨率变化回归通过。修复插件仅在项目目录构建，使用
`tools/with-gst-crop.sh` 为单次命令启用；详见
[性能修复说明](docs/gstreamer-performance.md)。

用户提供的 FFmpeg 8.1 `v4l2-request-n8.1` 客户端也通过板端硬解检查。
1080p 每份 600 帧，三轮中位数为 H.264 326.40 fps、HEVC 415.05 fps；
三项短样本共 28 帧与软件逐字节相同。临时构建与 Request 选择方式见
[FFmpeg Request 说明](docs/ffmpeg-request.md)。

watchdog 故障注入专项尚未运行。IRQ 与 watchdog 的作业完成权通过代码审查，
该项状态与截断码流恢复测试分别记录在 [验证记录](docs/validation.md)。

## 文档与上游接口

本仓库包含构建、加载和验证所需脚本，硬件参数见
[硬件说明](docs/hardware.md)，上游来源与许可证说明见
[来源记录](docs/sources.md)。测试样本与构建产物由脚本生成或下载。

Linux 接口采用公开的
[V4L2 stateless decoder 规范](https://docs.kernel.org/6.6/userspace-api/media/v4l/dev-stateless-decoder.html)
和 [Media Request API](https://docs.kernel.org/6.6/userspace-api/media/mediactl/request-api.html)。
