# 解码速度比较

`tools/benchmark-decode.sh` 比较 GStreamer V4L2 stateless 硬件解码和 FFmpeg
软件解码，统计完整应用进程的耗时、输出帧数、帧率及 CPU 用量。计时包括进程
启动、输入探测、解码、排空和退出。两种应用前端存在实现差异，结果用于比较
应用解码吞吐量；VPU 单帧执行时间需要另行测量。

## 2026-09-22 的 1080p 结果

TH1520、Linux `6.6.140-th1520`、GStreamer 1.22.0，CPU governor 为
`performance`，频率为 1.848 GHz。相同输入各 600 帧，预热一次后取三轮
中位数，表中单位为 fps。

| 编码 | V4L2 stateless | FFmpeg 单线程 | FFmpeg 四线程 | FFmpeg 厂商 OMX |
| --- | ---: | ---: | ---: | ---: |
| H.264 | 102.19 | 19.77 | 63.46 | 236.19 |
| HEVC | 6.86 | 21.68 | 53.95 | 296.15 |

厂商 H.264 约为 V4L2 的 2.31 倍，厂商 HEVC 约为 V4L2 的 43.17 倍。
HEVC V4L2 的约 87.47 秒中，用户态 CPU 时间约为 86.57 秒，具体函数
尚待分析。厂商结果每轮耗时约 2 秒，启动与退出开销保留在计时内。

全部正式轮次输出 600 帧，vendor 测量复用经 SHA-256 核对的同一输入。
厂商库日志确认 `h264_omx`、`hevc_omx` 初始化 `libomxil-bellagio.so.0`。
4K benchmark 按用户指示取消。测试结束后恢复 V4L2 临时模块和 runtime PM
`auto`，系统安装与启动配置保持原样。

## 输入与执行

测试需要 Linux、Python 3、PyGObject 的 `gi` 与 `Gst`、FFmpeg，以及
GStreamer 的 `v4l2codecs` 插件。测试脚本仅使用当前设备，模块加载沿用
[临时加载说明](../README.md#构建与临时加载)。

本次 RevyOS 带有 Python GI 和 GStreamer 1.22.0，缺少 `Gst-1.0.typelib`。
可将匹配版本的发行版包解压到构建目录，在本次 shell 中设置搜索目录：

```sh
mkdir -p build/benchmark-runtime
cd build/benchmark-runtime
apt-get download gir1.2-gstreamer-1.0=1.22.0-2
dpkg-deb -x gir1.2-gstreamer-1.0_1.22.0-2_riscv64.deb root
export GI_TYPELIB_PATH="$PWD/root/usr/lib/riscv64-linux-gnu/girepository-1.0"
cd ../..
```

该版本匹配本开发板的 `libgstreamer1.0-0 1.22.0-2`，仅作本次测试依赖。
包内文件保存在项目目录，系统软件安装数据库保持原样；其他系统应先核对
各自运行库和包版本。

在驱动仓库根目录生成连续测试序列，然后执行比较：

```sh
sh tools/make-benchmark-fixtures.sh test-results/benchmark-fixtures
sh tools/benchmark-decode.sh "$PWD" test-results/benchmark-decode \
  --input test-results/benchmark-fixtures/h264-1920x1080.h264 \
  --input test-results/benchmark-fixtures/hevc-1920x1080.h265 \
  --threads 1,4 --target-seconds 5 --rounds 3
```

生成脚本使用 FFmpeg `testsrc2`，默认生成 1080p 每份 600 帧，输入
均为逐行 8 bit 4:2:0。H.264 使用 libx264 High、QP 24、veryfast，HEVC
使用 libx265 Main、CRF 28、ultrafast，GOP 长度均为 60。具体编码参数、
生成器版本、帧数和 SHA-256 保存在输出目录。样本可在另一台计算机生成后
传送到开发板，编码耗时排除在解码测量之外。

`FRAMES_1080=6000` 可生成连续的 6000 帧长输入。4K 默认关闭，仅在设置
`INCLUDE_4K=1` 时生成各 300 帧的额外输入。本次 MMIO 对照使用长输入，
结果见 [relaxed MMIO 记录](mmio-performance.md)。

`--input` 接受带参数集、从随机访问图像开始的完整 Annex-B 文件。默认输入
为现有两个 1080p 矩阵样本。脚本通过串接完整序列增加测量长度，记录实际
重复次数；短序列的频繁 GOP 或序列切换开销属于该输入的测量结果。

## 厂商 FFmpeg 比较

`--vendor-only` 测量 FFmpeg 的 `h264_omx`、`hevc_omx` decoder，执行前
检查 `ffmpeg -decoders`。该模式使用厂商 OpenMAX IL 库和对应内核模块，
软件单线程预检仅用于核对输入帧数，正式测量包含厂商后端。厂商库内部的
线程管理保持默认设置。

```sh
sh tools/benchmark-decode.sh "$PWD" test-results/benchmark-vendor \
  --vendor-only --no-calibration --ffmpeg /usr/bin/ffmpeg \
  --input test-results/benchmark-fixtures/h264-1920x1080.h264 \
  --input test-results/benchmark-fixtures/hevc-1920x1080.h265 \
  --rounds 3 --target-seconds 5 --timeout 300
```

`--no-calibration` 每轮完整读取传入文件一次。与先前的 V4L2 结果比较时，
应核对各份 `input_sha256` 和 `expected_frames` 相同；如果先前校准生成了
重复序列，应传入其实际 `input.h264` 或 `input.h265`。耗时短于目标的
测量仍保留标记。

模块切换应在所有解码进程退出后执行。性能比较使用系统原始厂商模块，
寄存器取证用的调试模块会增加日志开销。各后端分别完成测试后，重新执行
`board-load.sh` 恢复 V4L2 临时模块；系统模块文件和开机配置保持原样。
benchmark 工具本身仅执行解码，模块管理由调用方完成。

## 比较方法

硬件解码明确选择 `h264parse ! v4l2slh264dec` 或
`h265parse ! v4l2slh265dec`，输出送往 `fakesink`，关闭时钟同步与显示。
脚本核对 decoder factory 及 `v4l2codecs` 插件，并在 EOS 时读取
`GstBaseSink.stats.rendered`。输出丢帧计数必须为零。

FFmpeg 在输入前指定 `-hwaccel none -c:v h264` 或 `-c:v hevc`，分别使用
显式线程数。输出采用 `wrapped_avframe` 和 null muxer，保留帧时间信息，
通过最终 `progress=end` 与 `frame` 核对完成状态和帧数。每轮必须输出与
独立软件预检相同的帧数。

各后端使用相同的缓存内码流，原生解码缓冲均在应用内释放，省略颜色转换、
显示和原始图像文件输出。硬件输出为 NV12，软件保留其原生图像格式。像素
正确性由容器测试与解码矩阵单独验证，benchmark 的通过条件为完整输出和
正常结束。

校准阶段使最快后端的单轮耗时尽量达到 `--target-seconds`，随后各执行一轮
预热和至少两轮正式测量。默认正式测量三轮，并轮换后端顺序。汇总包含帧率
中位数、最小值、最大值、变异系数和硬件相对软件的速度比。输入大小上限或
校准次数用尽时保留耗时不足标记，启动开销始终保留在结果中。

CPU 用量以 `RUSAGE_CHILDREN` 的用户态与内核态 CPU 时间之和除以计时器
时间计算，100% 表示一个逻辑 CPU。脚本观察 CPU 频率、governor、温度、
可用 CPU、系统负载和模块状态，保持频率策略与 affinity 设置原样。每次运行
生成独立目录，包含环境记录、输入校验值、逐轮日志和 JSON、TSV 汇总。

合成图案、编码参数、分辨率和软件线程数都会影响结果。该测量用于本开发板
当前配置下的比较，其他内容和硬件版本需要分别测量。
