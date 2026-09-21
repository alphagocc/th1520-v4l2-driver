<!-- SPDX-License-Identifier: GPL-2.0-only -->

# 合法 H.264 测试矩阵

在独立驱动仓库根目录执行：

```sh
sh tools/make-h264-matrix.sh test-results/h264-matrix
```

脚本要求 Python 3、带 libx264 和 `trace_headers` 的 FFmpeg，以及 ffprobe。
省略目录参数时，生成文件保存到脚本所属仓库的 `test-results/h264-matrix`。
默认编码使用 2 个线程，每例 8–16 帧；4K 用例使用 8 帧确定性的 SMPTE bars，
其余主要使用 `testsrc2`。weighted-P 用例增加确定性的亮度渐变。

| 用例组 | 检查内容 |
| --- | --- |
| Baseline、Main、High | SPS profile 与 PPS CAVLC、CABAC 标志 |
| refs1、refs4、refs16 | SPS 中的 `max_num_ref_frames` |
| B、B-pyramid | 实际 B picture 数，以及 `nal_ref_idc` 非零的 B picture 数 |
| slices4 | 每幅图像实际包含四个 slice header |
| weighted-P、weighted-B | 非默认显式 P 权重；启用隐式 B 权重且实际存在 B picture |
| custom-CQM | 从 PPS delta_scale 还原两组自定义 4x4 亮度矩阵并比较系数 |
| 322×242、640×360、1920×1080 | SPS coded size、裁剪参数与 display size |
| 3840×2160 | 8 帧 High、CABAC、refs1 |

refs16 验证 SPS 允许的参考帧上限。16 帧片段最多包含 15 幅先前图像；
清单同时记录 PPS 的默认活动参考数，避免将该用例描述为所有宏块都使用 16 个参考帧。

每例保存 `.h264`、显式指定 `-c:v h264 -hwaccel none` 解码的 `.sw.nv12`、
编码日志、header trace、probe JSON 和 case JSON。软件解码使用 `-xerror`
及 `-err_detect explode`，并核对帧数和 NV12 文件长度。

`matrix.tsv` 只列入通过全部检查的用例，字段为 `case`、`stream`、
`software_nv12`、`width`、`height`、`coded_width`、`coded_height`、`frames`、
`features`；文件名相对于输出目录。`matrix.json` 另含请求参数、完整命令、
工具版本和 SHA-256。全部选择的用例成功时，`complete` 才为 `true`。
编码器参数错误或目标特征缺失会终止生成，失败原因保存在 JSON 清单。

单独生成部分用例时可使用独立输出目录：

```sh
MATRIX_CASES=high-custom-cqm,high-3840x2160 MATRIX_THREADS=2 \
    sh tools/make-h264-matrix.sh test-results/h264-matrix-selected
```

`MATRIX_THREADS` 范围为 1–4，`MATRIX_TIMEOUT` 为单个命令的最长秒数，默认 300。
`FFMPEG` 和 `FFPROBE` 可指定工具名称或可执行文件。每次运行的清单仅描述本次选择。

编码、解码和 header trace 使用 [FFmpeg 官方命令接口](https://ffmpeg.org/ffmpeg.html)
与 [trace_headers 文档](https://ffmpeg.org/ffmpeg-bitstream-filters.html#trace_005fheaders)。
测试图像由 FFmpeg 滤镜生成，自定义缩放矩阵由脚本中的坐标公式构造。
