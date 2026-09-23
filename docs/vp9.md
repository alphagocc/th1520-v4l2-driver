# VP9 decoder 实现与依据

VP9 使用 Linux 6.6 stateless Request API。用户空间解析 uncompressed header、
compressed header 和 superframe index，维护参考帧槽位；内核管理四组概率上下文、
硬件计数器、segment map、原生参考图像和 MV。每个请求提交一帧 VP9 数据及
`V4L2_CID_STATELESS_VP9_FRAME`、`V4L2_CID_STATELESS_VP9_COMPRESSED_HDR`。
`show_existing_frame` 由用户空间展示已有 CAPTURE 图像。

OUTPUT 为 `V4L2_PIX_FMT_VP9_FRAME`，CAPTURE 为线性 NV12。当前实现范围为
Profile 0、8 bit、4:2:0，宽高为偶数，尺寸上限 4096×2304。每帧尺寸必须处于
协商的 OUTPUT 缓冲范围内；CAPTURE 行距和存储高度按 16 补齐。10 bit、其他
profile 和奇数宽高返回 `EINVAL`。请求错误、超时或 STREAMOFF 后，后续解码从
关键帧开始。2026-09-23 完成目标板的像素、异常请求、合规和错误恢复检查，
实际测试覆盖至 1920×1080，记录见 [VP9 验证](vp9-validation.md)。

OUTPUT DMA 基址要求为 16 字节边界，`data_offset` 允许任意字节偏移。驱动分配的
MMAP 缓冲满足此条件；导入的 DMA-BUF 基址未满足该条件时返回 `EINVAL`。

## 真实 SDK 依据

硬件参数来自 TH1520 原始 OMX decoder
`libOMX.hantro.VC8000D.video.decoder.so`，使用 ASIC `0x8001` 产品表
`0x4cbb80`，未沿用其他 Hantro 产品的寄存器坐标。源码注释中的函数地址均指
该交付物。完整调查位于父仓库 `analysis/vp9-vendor/`。

| 内容 | SDK 函数与证据 | 本实现 |
| --- | --- | --- |
| 解码模式 | `Vp9AsicInit` `0x144706` | mode 13；使用共用 clock、AXI、IRQ 配置 |
| 码流开始位置 | `Vp9AsicStrmPosUpdate` | 跳过两类 header；保留 `data_offset`，使用 swreg 258、259 和 start bit |
| 概率及计数器 | `Vp9AsicAllocateMem` `0x1447d6`、`Vp9Init*Probs`、`Vp9Adapt*Probs` | 概率表 3744 字节、计数器 13264 字节，52 个偏移锚点核查 |
| Tile 表 | `Vp9AsicSetTileInfoRegs` `0x1479a6` | 宽高各一个 little-endian u16，单位为 64×64 SB；处理空 tile 行 |
| 原生参考缓冲 | `Vp9CalculateBufSize` `0x1465ba`，尺寸计算位于 `0x146772` | 4×4 tiled Y/C，64 字节补齐，MV 为每个 SB 1024 字节 |
| 三组参考帧 | `Vp9AsicSetReferenceFrames` `0x148124` | LAST、GOLDEN、ALTREF 分别使用图像槽 0、4、5，单独配置 stride 和 scale |
| 时域 MV | 同上，`0x148966` 和 `0x148a12` | MV 槽 0 保存上次解码帧，槽 1 对应 LAST；槽 4、5 对应另外两幅参考帧 |
| Segmentation | `Vp9AsicSetSegmentationRegs` `0x1497d0`、`Vp9AsicRun` | 双 segment map，尺寸改变时清空 map；Q 和 loop filter 分别限制至 255 和 63 |
| NV12 后处理 | `Vp9AsicSetOutputRegs` `0x14a106`、PP 初始化 `0x143122` | 共用现有 PP，输入和输出配置使用偶数可见尺寸 |

公共 AXI read-ID 配置沿用本驱动的 TH1520 平台设置。SDK 静态默认值中的
read unique-ID enable 为 0，现有平台设置为 1，二者在记录中分别保留。

原生缓冲的计算方式为：

```text
width8 = ALIGN(width, 8)
height8 = ALIGN(height, 8)
stride = ALIGN(width8 * 4, 64)
Y size = ALIGN(stride * height8 / 4, 64)
C size = ALIGN(stride * height8 / 8, 64)
MV offset = Y size + C size + 64
MV size = ceil(width / 64) * ceil(height / 64) * 1024
```

每帧启动前清零 `MV offset - 32` 起始的 32 字节同步区，对应
`Vp9AsicInitPicture` `0x14ad66`。

参考尺寸和缩放比例使用原始码流宽高。stride 和原生区域偏移随 CAPTURE
缓冲保存，参考查找要求对应缓冲完成成功解码，且与当前输出缓冲分离。
当单帧尺寸小于协商尺寸时，用户空间根据该帧控件的宽高展示有效区域；CAPTURE
剩余补齐区域属于存储空间，其字节内容没有图像语义。

## 概率、错误恢复与调度

概率打包及计数器转换改编自公开 Linux GPL 实现，使用目标内核的
`v4l2-vp9` helper 完成 forward update、frame-context reset 和 backward
adaptation。概率上下文先复制至本帧的临时副本，硬件成功完成后才更新持久状态。
PARALLEL_DEC_MODE 使用 forward update 的结果，ERROR_RESILIENT 遵循 VP9
规定的并行解码及禁止刷新组合。

segment map 也只在成功完成后交换读写索引。失败时要求下一帧为关键帧，避免使用
失败帧的部分计数器和分段状态。上一幅成功解码图像的 MV 保存于 context 私有缓冲，
其有效期独立于用户空间对 CAPTURE 的再次排队。

板端截断帧曾返回仅含 RDY 的 IRQ，而 STREAM_BASE 读回位置超过 `bytesused`
32 字节。真实 SDK 的 HEVC 对同一寄存器执行消费范围检查；SDK VP9 缺少此项。
本驱动通过 `check_result` 在发布状态前检查 VP9 消费位置，同时将 ASO 状态
视为错误。186 次合法 VP9 解码的消费位置均为有效末尾或末尾前一字节；
修复后的截断请求返回双队列 ERROR，后续同 context 关键帧的像素恢复正确。
聚合 IRQ 清除掩码按真实产品表覆盖 `swreg1[25:11]`。

全部辅助 DMA 内存归属于打开的 context，初始化任一分配失败都会释放前面的资源。
CAPTURE 原生图像随队列缓冲释放。硬件作业沿用 M2M 单核调度；job abort、IRQ 和
watchdog 通过 `active_ctx` 竞争作业完成权，启动与取消使用同一互斥锁。

## 构建和验证

构建需要目标内核的 `v4l2-vp9` helper。`tools/board-build.sh` 对缺少该模块的
内核获取匹配版本的 GPL 源文件，与现有 `v4l2-mem2mem`、`v4l2-h264` 一起构建。
本机 RISC-V 交叉编译与目标板匹配头文件构建均通过，执行记录见
[VP9 验证记录](vp9-validation.md)。

样本生成入口为 `tools/make-vp9-matrix.sh`。该脚本核查实际 header，保存 IVF、
软件 NV12 和逐例编码信息；`tools/decode-matrix.sh` 增加 VP9 的比较入口。
补充样本由 `tools/make-vp9-extra.sh` 提供；Request 控件检查和真实关键帧重放
分别见 `tools/vp9-request-test.c`、`tools/vp9-decode-test.c`。

多行 tile、复杂 segmentation 更新、参考图像缩放和长期解码仍待单独验证。
watchdog 检查覆盖丢失一次软件完成通知，真实硬件持续忙碌时的 abort 仍需单独检查。
