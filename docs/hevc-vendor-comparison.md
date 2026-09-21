# HEVC 输出差异与厂商比较

本页记录 TH1520、Linux `6.6.140-th1520` 上的输出比较，测试时间为
2026-09-21 至 2026-09-22，ASIC ID 为 `0x80018000`，build ID 为 `0x1f88`。
软件基准采用显式 FFmpeg HEVC decoder，硬件输出整理为紧密排列的 NV12。

## CTU16 与 WPP 样本

自生成的 `hevc-ctu16-wpp.h265` 为逐行、8 bit、4:2:0、640×360，共 12 帧。
V4L2 与厂商 OMX decoder 的输出逐字节相同，共享 SHA-256 为：

```text
40e4bc3d017539af809ea7c6fb3946683b0c150610dc16602307c743d4db8d85
```

显式软件解码的 SHA-256 为：

```text
21ee86b7469675949614972abd1017ad7848500b93223de3daf32762d5e15a63
```

两个硬件接口相对软件基准均有少量色度差异，首个差异位于字节偏移 260734。
该样本按厂商输出一致计入本项目的验证记录。差异的成因仍待研究，严格软件
比较继续将其记录为 `cmp=different`。

## 64×240 的 WPP 附加测试

官方 `WPP_D_ericsson_MAIN_2` 为合法 HEVC Main、8 bit、4:2:0 码流，尺寸
64×240，CTB 为 64×64，包含 WPP 和 dependent slice。

测试所用厂商 SDK 的软件接受范围要求宽度、高度分别至少为 144。该样本超出
这一范围，厂商接口未提供可用于像素比较的输出。这一限制描述 SDK 的接受
条件，ASIC 对该尺寸的处理仍以硬件测试结果为依据。

V4L2 输出全部 48 帧，其中 43 帧与软件逐字节相同，5 帧存在极少量边缘亮度
±1 差异。该项保留为 SDK 尺寸范围外的附加测试，当前证据无法确认像素差异
的硬件成因。`decode-matrix.sh` 对该样本仍报告 `cmp=different`。

## 其余官方样本

TILES 100 帧、SLICES 9 帧、TMVP 17 帧、SLIST 65 帧和 LTR 500 帧均与软件
NV12 逐字节相同。样本来源和参数见 [conformance 样本](hevc-conformance-samples.md)，
汇总范围见 [验证记录](validation.md)。
