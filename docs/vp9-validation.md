# VP9 离线与板端验证记录

2026-09-23 早期的 VP9 检查在本机执行。当时开发板连接、模块加载、硬件解码和
`v4l2-compliance` 均未执行。本文的样本与软件解码结果只证明测试输入和参考
图像有效。随后依据用户授权完成板端检查，结果见本文末尾的板端记录。

实现的证据来源、Profile 0 限制和待确认事项见 [VP9 说明](vp9.md)。既有
[H.264 与 HEVC 硬件记录](validation.md) 的结果保持其原有 codec 范围。

## 本机编译

使用 Linux stable 官方镜像的
[v6.6.140 源码](https://github.com/gregkh/linux/tree/v6.6.140)，在
openSUSE-Tumbleweed WSL 中使用 `riscv64-suse-linux-gcc 15.3.0` 交叉编译。
下载归档 SHA-256 为
`fee04e378f1cae4eae3f5889342e3bd5fbde25066b5d9d63cb52e89656477ce2`。

内核树位于本机 WSL `/tmp/th1520-vp9-offline/linux-6.6.140`；驱动副本位于
`/tmp/th1520-vp9-offline/module`。使用副本避免把编译产物混入源码目录。
构建脚本、配置、日志和模块保存在 `driver/build/vp9-offline/`。

```sh
make -C "$KDIR" M="$MODULE" ARCH=riscv \
    CROSS_COMPILE=riscv64-suse-linux- W=1 -j8 modules
```

内核配置启用 Media Controller、Request API、V4L2、videobuf2 DMA-contig、
V4L2 M2M、H.264 和 VP9 helper。构建检查通过 `COMPILE_TEST=y` 与
`VIDEO_HANTRO=m` 选入隐藏的 helper 配置；这一配置仅用于生成 Linux 的
依赖模块和符号表。

完整内核与配置模块构建成功，生成 `Module.symvers` 后，驱动的八个源文件
编译、严格 `MODPOST` 和模块链接全部通过。最终检查采用默认符号验证，
未使用 `KBUILD_MODPOST_WARN`。新增 VP9 源码无编译警告；`W=1` 报告既有
`th1520_vdec_hw.c` 两处函数注释缺少参数说明，宿主 Perl 另有 Linux
`kernel-doc` 脚本语法提示。

输出模块为 ELF64 RISC-V，`vermagic` 为 `6.6.140 SMP mod_unload riscv`，
依赖项包含 `v4l2-vp9`。模块 SHA-256 为
`89f45a2612ef6e7ec1af6b7ff30bdd8263d1187c4308d7d795dca61871868292`。
最终源码与构建副本逐文件比较一致，源文件校验值保存于
`driver/build/vp9-offline/source-sha256.txt`。

同日按用户要求恢复标准 MMIO 后，再次通过 RISC-V 编译及严格 `MODPOST`。
上面的模块校验值对应此次标准 MMIO 构建，历史 relaxed 构建的校验值为
`330b9dafeb9f7c011d7e4b00fcf457b1dd1fff213f338472430ec21c465417cd`。

此检查采用上游 Linux 6.6.140 配置，证明对应版本的编译和符号兼容性。
目标板的 `6.6.140-th1520` 属于独立内核配置，部署时仍须使用对应配置、
头文件和符号表重新构建。

## 概率表与缓冲区

Host 工具 [vp9-offline-test.py](../tools/vp9-offline-test.py) 使用 Linux 的
`v4l2-vp9.c` 编译实际的 `th1520_vdec_vp9_probs.c`，执行 ASan 和 UBSan
检查，结果通过。测试覆盖硬件概率表的固定布局、表内容打包、计数器映射和
概率状态更新。此类测试验证内存访问与软件状态处理，真实硬件返回的计数器
数据仍需单独确认。

```sh
python3 driver/tools/vp9-offline-test.py --kernel-tree "$KDIR"
```

[vp9-backend-test.py](../tools/vp9-backend-test.py) 对实际 VP9 后端提供 Host
DMA、vb2 和寄存器数组替代实现，同样通过 ASan 与 UBSan。检查覆盖各次
内存分配失败、原生缓冲区尺寸、payload 与 `data_offset` 边界、DMA 基址
满足 16 字节边界的要求、segmentation、空 tile 行、参考帧验证、作业完成
与中止、启动前的 32 字节同步区域清零。

```sh
python3 driver/tools/vp9-backend-test.py --kernel-tree "$KDIR"
```

最终两个测试的输出位于 `driver/build/vp9-offline/prob-test.log` 和
`driver/build/vp9-offline/backend-test.log`，对应命令和源文件校验值见同目录
的 `test-commands.txt`、`test-source-sha256.txt`。寄存器数组检查证明软件
生成值与断言一致，硬件对这些值的解释仍以厂商证据和后续硬件测试为依据。

`decode-matrix.sh` 的 NV12 整理代码使用人工构造的逐行图像验证偶数尺寸、
奇数宽高、两个连续帧和截断输入。检查同时确认 VP9 使用 IVF demuxer。
全部检查通过，脚本保存在 `driver/build/vp9-offline/check-matrix.py`。

奇数尺寸 NV12 的参考文件长度按以下关系计算：

```text
frame_bytes = width * height + 2 * ceil(width / 2) * ceil(height / 2)
```

GStreamer 普通 NV12 的行距按 4 字节边界补齐，色度平面始于补齐至偶数行的
亮度平面之后；整理时同时移除行末填充和奇数高度末尾的填充行。依据为
[GStreamer 1.24.0 video-info.c 的 fill_planes](https://github.com/GStreamer/gstreamer/blob/1.24.0/subprojects/gst-plugins-base/gst-libs/gst/video/video-info.c#L963)。

## 软件样本

`make-vp9-matrix.sh` 使用 `libvpx-vp9` 生成 8 bit、4:2:0、Profile 0 的 IVF
样本，再用 FFmpeg 原生 `vp9` 软件 decoder 生成 NV12 参考文件。顶层目录
放置当前支持范围内的九个正向样本，`unsupported/` 存放奇数尺寸负向样本。
后者为有效 VP9 码流，当前驱动对其请求的预期结果为 `EINVAL`。检查工具
显式选择软件 decoder，并逐项核对实际码流头部、显示尺寸、帧数和参考文件
长度。编码参数、头部跟踪、逐例 JSON、TSV 和 SHA-256 随样本保存。

```sh
sh driver/tools/make-vp9-matrix.sh driver/test-results/vp9-profile0
```

环境变量 `MATRIX_CASES` 可指定逗号分隔的样本名称，`MATRIX_THREADS` 控制
编码线程数，默认值为 2。每个 FFmpeg 子进程限制为 180 秒，每个 ffprobe
子进程限制为 60 秒。

| 样本 | 显示尺寸 | 帧数 | 头部检查覆盖 |
| --- | --- | ---: | --- |
| `vp9-key` | 320×240 | 1 | 关键帧 |
| `vp9-all-key` | 320×240 | 8 | 连续关键帧 |
| `vp9-inter` | 640×360 | 12 | 关键帧与 inter 帧 |
| `vp9-frame-parallel` | 640×360 | 12 | frame-parallel 标志 |
| `vp9-error-resilient` | 640×360 | 12 | error-resilient 标志 |
| `vp9-tiles4x1` | 1920×1080 | 8 | 4 列 tile |
| `unsupported/vp9-odd-321x241` | 321×241 | 12 | 奇数宽高，预期拒绝请求 |
| `vp9-lossless` | 320×240 | 8 | 零量化参数 |
| `vp9-segmentation` | 640×360 | 12 | segmentation 标志 |
| `vp9-context-refresh` | 640×360 | 24 | frame-context refresh 标志 |

2026-09-23 使用 FFmpeg `9.0.2-full_build-www.gyan.dev`、libvpx
`v1.17.0-57-gd2413e2ca` 运行全部十个样本，共 109 帧，头部检查与软件解码
全部通过。其中正向样本共 97 帧，奇数尺寸负向样本为 12 帧。本机结果目录为
`driver/test-results/vp9-profile0-20260923`。奇数尺寸样本另在
`driver/test-results/vp9-negative-20260923` 单独验证输出分类。

`libvpx-vp9` 在本次配置中接受 `tile-rows=1` 参数后仍生成单行 tile，检查
工具通过真实头部识别了这一情况。因此矩阵仅记录实际生成的 4 列、1 行
样本；多行 tile 仍需补充测试输入。长序列中的概率更新位置、未显示帧、
`show_existing_frame`、superframe、分辨率变化和参考帧缩放也留待专门样本
验证。

## 后续硬件检查

`decode-matrix.sh` 增加 `.ivf` 输入支持，先检查 `DKIF` 和 `VP90` 标记，
再以 `ivfparse ! vp9parse ! v4l2slvp9dec` 提交 Request API。输入枚举仅检查
指定目录的文件，`unsupported/` 中的负向样本另行执行控件拒绝检查。有效 NV12
字节与软件参考比较，输出逐例日志、帧数、SHA-256、`summary.json` 和
`summary.tsv`。本次仅完成该工具的本地检查。

将来获得硬件验证授权后，需要检查 probe 与 remove 失败处理、STREAMON 与
STREAMOFF、并发 context、异常 controls、截断帧、超时后的关键帧恢复、
多行 tile、参考帧缩放及完整像素比较，并运行 `v4l2-compliance`。

上述内容记录离线阶段的验证范围；板端结果另列如下。

## 2026-09-23 板端记录

设备运行 `6.6.140-th1520`，ASIC ID 为 `0x80018000`，build ID 为 `0x1f88`。
测试目录为 `/home/debian/th1520-vp9-20260923`，使用匹配的目标内核头文件构建
驱动及三个 helper。仅临时加载模块，标准 `readl()`、`writel()` 和启动前
`wmb()` 保持启用。

13 组合法样本共 190 个显示帧全部与软件 NV12 逐字节相同。基础矩阵的 9 组为
97 帧；补充矩阵的 4 组为 93 帧，覆盖 322×242、串行概率适配、周期关键帧重置、
4 个 invisible frame、4 个 superframe，以及 `show_existing_frame` 的全部八个
参考槽位。后者由用户空间重复展示原图像，对应 186 次真实硬件解码。

首次截断测试发现：完整数据为 4611 字节，保留 169 字节 header 与一个 tile
字节后，硬件仍报告 `RDY`，并消费有效末尾之后的 32 字节。驱动新增消费范围
检查后，该请求在 12 ms 内返回双队列 ERROR；同一 context 的下一幅关键帧
115,200 字节全部匹配。修正版重新通过上述 190 帧矩阵。

独立模块丢弃一次完成通知后，watchdog 在 2316 ms 返回 ERROR，同一 context
的下一幅关键帧像素全部匹配。诊断及故障注入结束后均恢复普通模块。

异常控件及 Request 生命周期检查为 20 轮、230 项通过；
`v4l2-compliance 1.28.1` 为 56 项通过、失败 0、警告 0。
修正版的混合并发检查包含 VP9 240 帧、H.264 300 帧、HEVC 300 帧，全部像素
一致，并观察到三个并发设备使用者；单独两个 VP9 context 再次各检查 120 帧，
全部一致，设备打开记录确认两个 VP9 进程同时存在。

同源 FFmpeg 8.1 增补 VP9 Request 后，1080p 前八帧的 24,883,200 字节与软件
参考相同。此八帧与前述 190 帧分别记录，共 198 个显示帧的独立样本检查通过。
三轮 600 帧速度测量的硬件中位数为 224.01 fps，四线程软件为 68.94 fps，
条件与逐轮数值见 [VP9 benchmark](vp9-benchmark.md)。

最终目标板普通模块 SHA-256 为
`140756c8457bf3b16063c79a41993b316c6f0f7647e1d036aac7a2030fc76255`。
内核日志中的消费越界与超时信息对应本节有意构造的错误检查；最终日志未出现
Oops、BUG、WARNING 或 Call Trace。
完整板端记录、源码摘要及保留的首次失败证据见父仓库
`analysis/board-vp9-2026-09-23/`。
