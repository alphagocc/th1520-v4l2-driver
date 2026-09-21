# HEVC conformance 样本

本项目从 [FFmpeg 官方样本库](https://fate-suite.ffmpeg.org/hevc-conformance/)
选择六个合法 HEVC 码流，压缩文件合计 **1425179 字节**、**739 帧**。
选择依据为 [FATE hevc.mak](https://github.com/FFmpeg/FFmpeg/blob/045c8006d1c10d7196e2e1c33d4de651a401960d/tests/fate/hevc.mak)，
固定 commit 为 `045c8006d1c10d7196e2e1c33d4de651a401960d`。

六个样本均位于 `HEVC_SAMPLES_8BIT`。本项目使用 `ffprobe` 和 `trace_headers`
核查：profile 为 Main，像素格式为 `yuv420p`，`chroma_format_idc=1`，亮度和
色度 `bit_depth_minus8=0`。

| 样本 | 字节数 | 分辨率 | 帧数 | 码流特征 |
| --- | ---: | --- | ---: | --- |
| [TILES_A_Cisco_2](https://fate-suite.ffmpeg.org/hevc-conformance/TILES_A_Cisco_2.bit) | 484767 | 1920×1080 | 100 | 5×5 tiles，非均匀 tile 间距 |
| [SLICES_A_Rovi_3](https://fate-suite.ffmpeg.org/hevc-conformance/SLICES_A_Rovi_3.bit) | 65943 | 640×480 | 9 | 每帧 20 个独立 slice |
| [WPP_D_ericsson_MAIN_2](https://fate-suite.ffmpeg.org/hevc-conformance/WPP_D_ericsson_MAIN_2.bit) | 22474 | 64×240 | 48 | WPP、dependent slice、SAO、temporal MVP |
| [TMVP_A_MS_3](https://fate-suite.ffmpeg.org/hevc-conformance/TMVP_A_MS_3.bit) | 17238 | 416×240 | 17 | temporal MVP 在不同 slice 中切换，包含 SAO |
| [SLIST_B_Sony_8](https://fate-suite.ffmpeg.org/hevc-conformance/SLIST_B_Sony_8.bit) | 344203 | 832×480 | 65 | SPS、PPS 携带显式 scaling list |
| [LTRPSPS_A_Qualcomm_1](https://fate-suite.ffmpeg.org/hevc-conformance/LTRPSPS_A_Qualcomm_1.bit) | 490554 | 416×240 | 500 | SPS 提供 8 个长期参考项，slice 使用长期参考 |

## 获取和检查

在仓库根目录执行：

```sh
sh tools/fetch-hevc-conformance.sh
sh tools/decode-matrix.sh test-results/hevc-conformance test-results/hevc-conformance-results --timeout 300
```

下载脚本将文件保存在 `test-results/hevc-conformance/`，检查每个码流的固定
字节数和 SHA-256。扩展名改为 `.h265`，内容与官方 `.bit` 保持一致。
`SHA256SUMS`、`SOURCES.tsv`、`SELECTION.txt`、固定版本的 `hevc.mak` 和六份
FATE 参考结果保存在同一目录，单次 HTTP 下载上限为 524288 字节。

测试采用完整码流；LTR 样本的 500 帧覆盖序列后段的长期参考。下载文件与测试
输出位于 Git 忽略目录，由脚本取得和生成。

## 软件基准与板端结果

固定 commit 的 `tests/ref/fate/hevc-conformance-<NAME>` 提供逐帧参考。
本项目将 FFmpeg 显式 `-c:v hevc` 软件解码与这些参考比较，739 帧的有效字节数
和像素 CRC 全部相同；时间戳未纳入该项比较。

2026-09-21 至 2026-09-22 的 TH1520 测试中，TILES、SLICES、TMVP、SLIST 和
LTR 共 691 帧的硬件 NV12 与软件逐字节相同。WPP_D 输出全部 48 帧，其中
43 帧软件一致、5 帧存在极少量边缘亮度 ±1 差异。该样本宽度小于测试所用
厂商 SDK 的软件接受下限，差异成因仍待确认。

完整验证范围见 [验证记录](validation.md)，WPP_D 与自生成 CTU16 样本的
区别见 [HEVC 输出差异](hevc-vendor-comparison.md)。
