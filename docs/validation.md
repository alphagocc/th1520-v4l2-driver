# TH1520 硬件验证记录

测试时间为 2026-09-21 至 2026-09-22，平台为 TH1520、RevyOS、Linux
`6.6.140-th1520`，ASIC ID `0x80018000`，build ID `0x1f88`。
本文汇总本项目的硬件观测和像素比较结果，构建与执行命令见 [README](../README.md)。

## 测试方法

软件基准使用 FFmpeg，显式指定 `-hwaccel none -c:v h264` 或
`-hwaccel none -c:v hevc`，decoder 参数置于 `-i` 之前，输出 NV12。
硬件测试通过 GStreamer 的 `v4l2slh264dec` 或 `v4l2slh265dec` 提交 stateless
请求，使用格式打包处理移除行距填充后比较完整 NV12 文件。记录包含帧数、
有效字节数、SHA-256 及 `cmp` 结果。

测试通过 `make-h264-matrix.sh`、`make-hevc-matrix.sh` 和
`fetch-hevc-conformance.sh` 准备码流，再由 `decode-matrix.sh` 执行比较。
每次运行生成独立的 `summary.json`、`summary.tsv` 及逐例日志。

## 合法码流

30 个样本、1020 帧全部输出。28 个样本与软件逐字节相同，1 个样本与厂商
输出逐字节相同，另有 1 个样本属于厂商 SDK 尺寸范围外的附加测试。

| 样本组 | 样本数 | 帧数 | 与软件逐字节相同的样本数 |
| --- | ---: | ---: | ---: |
| H.264 测试矩阵 | 15 | 196 | 15 |
| 自生成 HEVC 测试矩阵 | 9 | 85 | 8 |
| 官方 HEVC conformance | 6 | 739 | 5 |
| 合计 | **30** | **1020** | **28** |

H.264 覆盖 Baseline、Main、High、CAVLC、CABAC、B 帧、B-pyramid、参考帧上限
1、4、16、多 slice、weighted prediction、自定义 scaling matrix、显示尺寸
裁剪、1080p 和 3840×2160。生成脚本核查实际码流头部，参考帧上限与实际引用
数量分别记录。逐例参数见 [H.264 矩阵说明](../tools/h264-matrix.md)。

自生成 HEVC 覆盖 CTU16、CTU32、WPP、SAO 开关、无损编码、参考帧配置、
scaling list、多 slice、尺寸裁剪、1080p 和 3840×2160。640×360 的
`hevc-ctu16-wpp` 共 12 帧，软件比较有少量色度差异，V4L2 输出与厂商 OMX
逐字节相同；其他八个样本与软件一致。

官方样本的 LTR 500 帧、SLICES 9 帧、SLIST 65 帧、TILES 100 帧、TMVP
17 帧均通过完整序列的软件比较。64×240 的 WPP_D 共 48 帧，43 帧软件一致，
5 帧存在极少量边缘 Y ±1 差异。测试所用厂商 SDK 要求宽度、高度分别至少
144，该样本保留为范围外附加测试，像素差异成因仍待确认。

上述两个差异样本的严格软件比较仍为 `cmp=different`；对应矩阵脚本退出码
为 1。厂商一致与尺寸范围外的分类只描述本次验证范围。详细比较与输出校验值
见 [HEVC 输出差异](hevc-vendor-comparison.md)。

## API、并发与错误处理

| 检查 | 结果 |
| --- | --- |
| `v4l2-compliance 1.28.1` | 视频设备 48/48，总计 56/56；失败 0，警告 0 |
| H.264 与 HEVC 两个 context 同时解码 | 完整输出及像素比较通过 |
| Request API：2 worker、10 轮 | 全部通过 |
| H.264 `data_offset=13` | 输出像素检查通过 |
| 正常帧、截断帧、恢复帧 | 截断请求返回 ERROR，恢复帧像素通过 |
| 七种异常 HEVC 控件 | 请求被拒绝并返回 ERROR，测试按时完成 |

七种异常 HEVC 控件覆盖 CB 尺寸范围、tile 数量、非均匀 tile 宽高、POC 计数
及 SPS 与缓冲尺寸的匹配检查。这些结果补充合法码流验证。

## 尚待验证的范围

H.264 MP4、HEVC Matroska 及两组分辨率切换由 `tools/test-containers.sh`
提供。本地脚本检查完成，四项板端测试尚未执行，也未计入上述样本与帧数。

watchdog 故障注入专项尚未运行。IRQ 与 watchdog 的作业完成权通过代码审查；
截断码流恢复属于单独的错误处理测试，无法替代丢失中断或硬件停滞测试。

4K 样本为有界短序列，持续吞吐量、长时间运行及更多硬件版本仍需单独验证。
当前支持范围为逐行、8 bit、4:2:0，其他位深、色度采样与 H.264 场编码尚未
纳入本项目的硬件支持范围。

## 测试期间的模块加载

全部测试采用当前启动期间的临时模块加载。模块与构建产物保存在工作目录，
系统模块安装目录、开机加载与黑名单配置保持原样，重启后沿用原系统配置。
收尾检查确认 runtime PM 为 `auto`、`suspended`。
