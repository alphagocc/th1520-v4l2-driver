# VP9 FFmpeg Request benchmark

2026-09-23 在 TH1520、RevyOS、Linux `6.6.140-th1520` 上测量。CPU 为四核，
governor 为 `performance`，测量前后读回频率均为 1.848 GHz。驱动使用标准
`readl()`、`writel()`，`flush_all=1`，普通模块保持临时加载。

原 FFmpeg 8.1 构建只启用 H.264、HEVC，本轮在同一份源码和配置基础上，独立
增加 VP9 decoder、Request HWAccel、parser 和 IVF demuxer。原二进制和源码
保持原值；新构建及来源核查见 [构建说明](ffmpeg-vp9-request.md)。

## 测量结果

同一份 1920×1080、Profile 0、8 bit、4:2:0 输入包含连续 600 帧，标称 30 fps。
每种方式先预热一次，再交替测量三轮。以下为三轮中位数，计时包含进程启动、
输入探测和退出，CPU 用量为 FFmpeg 子进程的 user 与 system 时间之和。

| 方式 | 解码线程参数 | 帧率 | 进程 CPU 用量 |
| --- | ---: | ---: | ---: |
| V4L2 Request 硬件解码 | 1 | 224.01 fps | 28.5% |
| 同一 FFmpeg 的软件解码 | 4 | 68.94 fps | 357.8% |

硬件帧率为软件的 3.25 倍。CPU 的 100% 表示一个逻辑核心；此数据不包含
整个系统的 CPU 用量。两种方式均使用 `wrapped_avframe` 和 null 输出，硬件
保留 `drm_prime`，软件保留 `yuv420p`。硬件测量省去图像下载；软件构建沿用
`--disable-asm`，这些数值表示上述配置下的应用吞吐量。

| 轮次 | 硬件 fps | 软件 fps |
| --- | ---: | ---: |
| 1 | 223.93 | 69.29 |
| 2 | 224.34 | 68.94 |
| 3 | 224.01 | 68.46 |

八次运行包含两次预热和六次正式测量，每次均以退出码 0、600 帧、
`progress=end` 完成，共 4800 帧。硬件日志明确选择 `th1520-vdec` 的 `VP9F`，
CAPTURE 为 1920×1088 NV12，视频可见尺寸为 1920×1080。

## 输入与像素核对

`tools/make-vp9-benchmark.sh` 生成连续 `testsrc2` 内容，libvpx-vp9 的参数为
CRF 32、`cpu-used=4`、4 列单行 tile、`frame-parallel=0`、`lag=0`、
`auto-alt-ref=0`、每 60 帧一个关键帧。实际 header 与软件逐帧 MD5 分别确认
上述特征和 600 幅图像互异。完整输入为 18,310,339 字节，SHA-256：

```text
1362bc511a1076dc8bfad93a50e99991c5ef641736e43b505f30ba17fd99caf9
```

速度测量前从同一码流原样截取前八包，用本次 FFmpeg 执行硬件解码与
`hwdownload,format=nv12`。输出 24,883,200 字节与软件参考完全相同，SHA-256：

```text
a4f6163f9ea863fd291c0c68e7661185d555510a84186cc2575e9b379af057a5
```

## 执行入口与产物

板端目录为 `/home/debian/th1520-vp9-20260923`：

```sh
python3 driver/tools/benchmark-vp9-request.py \
  --ffmpeg driver/build/ffmpeg-request-vp9/ffmpeg \
  --input test-results/vp9-benchmark-20260923/vp9-1920x1080.ivf \
  --output test-results/vp9-benchmark-results \
  --frames 600 --rounds 3 --software-threads 4
```

该输出目录包含逐次命令、FFmpeg 日志、progress、`runs.json`、`summary.json`
和环境记录。工具拒绝覆盖已有 `summary.json`，再次测量应另用输出目录。
本机副本位于父仓库 `analysis/board-vp9-2026-09-23/raw/test-results/`。

FFmpeg SHA-256 为
`cdcbb03becf348c2b4afc456f62cc7002f861bfb4af33881397c3018586ac98d`；
驱动模块 SHA-256 为
`140756c8457bf3b16063c79a41993b316c6f0f7647e1d036aac7a2030fc76255`。
测量前后 CPU 温度约为 58.8℃ 和 68.0℃。本次采用 1080p 输入，4K 性能保持
先前取消状态。
