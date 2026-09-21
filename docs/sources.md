# 来源与许可

本仓库的驱动和工具采用 GNU General Public License version 2 only，全文
见 [COPYING](../COPYING)。源码中的 `GPL-2.0` 与 `GPL-2.0-only` 标识在此
均指 GPL 第 2 版。各文件保留对应版权声明。

## Linux 上游代码

驱动使用 Linux V4L2、videobuf2 和 Media Request API。框架和 codec 辅助
实现参考及改编自 Linux commit
[`8ba098e6b6ff0db8edf28528d1552be261af30d4`](https://github.com/torvalds/linux/commit/8ba098e6b6ff0db8edf28528d1552be261af30d4)。

| 本仓库内容 | 公开 GPL 源码 | 使用范围 |
| --- | --- | --- |
| `th1520_vdec_drv.c` | [hantro_drv.c](https://github.com/torvalds/linux/blob/8ba098e6b6ff0db8edf28528d1552be261af30d4/drivers/media/platform/verisilicon/hantro_drv.c) | V4L2 M2M 设备和请求生命周期 |
| `th1520_vdec_v4l2.c` | [hantro_v4l2.c](https://github.com/torvalds/linux/blob/8ba098e6b6ff0db8edf28528d1552be261af30d4/drivers/media/platform/verisilicon/hantro_v4l2.c) | 格式、队列和控件组织 |
| `th1520_vdec_h264.c` | [hantro_h264.c](https://github.com/torvalds/linux/blob/8ba098e6b6ff0db8edf28528d1552be261af30d4/drivers/media/platform/verisilicon/hantro_h264.c)、[hantro_g1_h264_dec.c](https://github.com/torvalds/linux/blob/8ba098e6b6ff0db8edf28528d1552be261af30d4/drivers/media/platform/verisilicon/hantro_g1_h264_dec.c) | DPB、参考列表及辅助表处理 |
| `th1520_vdec_h264_cabac.c` | [hantro_h264.c](https://github.com/torvalds/linux/blob/8ba098e6b6ff0db8edf28528d1552be261af30d4/drivers/media/platform/verisilicon/hantro_h264.c) | 完整的 920 字 CABAC 初始化表 |
| `th1520_vdec_hevc.c` | [hantro_hevc.c](https://github.com/torvalds/linux/blob/8ba098e6b6ff0db8edf28528d1552be261af30d4/drivers/media/platform/verisilicon/hantro_hevc.c)、[hantro_g2_hevc_dec.c](https://github.com/torvalds/linux/blob/8ba098e6b6ff0db8edf28528d1552be261af30d4/drivers/media/platform/verisilicon/hantro_g2_hevc_dec.c) | Tile、缩放矩阵和参考帧辅助处理 |
| `th1520_vdec_regs.h` | [hantro_g1_regs.h](https://github.com/torvalds/linux/blob/8ba098e6b6ff0db8edf28528d1552be261af30d4/drivers/media/platform/verisilicon/hantro_g1_regs.h)、[hantro_g2_regs.h](https://github.com/torvalds/linux/blob/8ba098e6b6ff0db8edf28528d1552be261af30d4/drivers/media/platform/verisilicon/hantro_g2_regs.h) | 公共寄存器术语与位域描述辅助形式 |

这些源文件的版权方包括 Rockchip Electronics、Google、Collabora、Samsung
Electronics 和 Safran Passenger Innovations。适用的声明保留在相应文件中。
CABAC 表完整保留公开 Linux 数组的数值与顺序。

TH1520 的寄存器坐标、模式选择、MMIO 次序和原生缓冲参数属于本项目针对
指定硬件的接口实现，使用设备行为调查与目标板测试核实。公开 Hantro 驱动
用于 Linux ABI 和 codec 辅助实现的参考，其其他硬件配置需要分别验证。
本仓库使用的具体接口见 [硬件说明](hardware.md)，实测范围见
[验证记录](validation.md)。

## 构建时获取的 GPL helper

`tools/board-build.sh` 根据当前内核 release 从 Linux stable 仓库获取：

1. [v4l2-mem2mem.c](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/drivers/media/v4l2-core/v4l2-mem2mem.c?h=v6.6.140)
2. [v4l2-h264.c](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/drivers/media/v4l2-core/v4l2-h264.c?h=v6.6.140)

上面的链接对应本次验证的 v6.6.140。下载文件保存在 Git 忽略的 `build/`
目录并保留原始许可头，`SOURCES` 记录实际下载版本。正常构建还需要匹配
目标内核的头文件及编译工具链。

## 公开接口与测试数据

Linux 接口以 [V4L2 stateless decoder](https://docs.kernel.org/6.6/userspace-api/media/v4l/dev-stateless-decoder.html)
和 [Media Request API](https://docs.kernel.org/6.6/userspace-api/media/mediactl/request-api.html)
文档为依据。

生成样本采用 FFmpeg 的合成视频源及 x264、x265 encoder。官方 HEVC 样本
由工具从 FFmpeg FATE 站点单独下载，来源及固定版本记录于
[样本说明](hevc-conformance-samples.md)。码流、像素文件、下载源码、构建
产物和原始厂商交付物均属于 Git 忽略的运行数据，提交中仅包含本项目源码、
工具与文档。下载样本保留其各自的来源和适用许可。

后续贡献应提供可再分发源码的明确许可，保留适用的 SPDX 与版权声明。
专有实现、反编译代码和受限厂商内容不得提交。
