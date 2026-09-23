# TH1520 VC8000D 寄存器指南

本文描述 TH1520 VC8000D 的解码寄存器，适用产品 `0x8001`、build `0x1f88`。
板端读取的 ASIC ID 为 `0x80018000`。内容覆盖当前 H.264、HEVC、VP9 后端及
PP0 线性 NV12 输出；其他编码模式、参考帧压缩和额外 PP 功能保留为待验证范围。

文档组织参考 linux-sunxi 的
[VE Register guide](https://linux-sunxi.org/index.php?title=VE_Register_guide&oldid=25004)：
先列地址总表，再按功能说明位域、取值和关联缓冲。本文中的地址与数值依据
TH1520 交付物及本项目测试，核查日期为 2026-09-23。

[地址总览](#register-index) · [公共控制](#common) · [H.264](#h264) ·
[HEVC](#hevc) · [VP9](#vp9) · [DMA 地址](#dma) · [码流窗口](#stream) ·
[图像与辅助内存](#memory) · [PP0](#pp0) · [来源](#sources)

## 地址与记法

每个 `swregN` 是一个 32bit 整数，占 4 字节。本文的十六进制偏移均相对
**解码核基址**，计算方式为：

```text
decoder_base = device_tree_reg_base + 0x1000
swregN_address = decoder_base + 4 * N

本项目目标板：
device_tree_reg_base = 0xffecc00000
decoder_base         = 0xffecc01000
```

同一个 swreg 在不同 `DEC_MODE` 下可以具有不同含义。H.264、HEVC、VP9 共用
同一地址空间，本文的 codec 分组表示字段解释方式。位编号以最低位为 0，
`31:27` 表示两端均包含的位范围；`uN`、`sN` 分别表示 N 位无符号值和采用
低 N 位补码编码的有符号值。

| 数量 | 所指范围 |
| --- | --- |
| 512 个 32bit 整数 | 当前驱动的 shadow register 数组，索引 0 至 511 |
| 544 个 32bit 整数 | 此 SDK decoder 容器中的寄存器存储尺寸 |
| 1023 个 32bit 整数 | 当前驱动映射窗口，最后一个寄存器为 swreg1022，偏移 `0xff8` |

这三项分别描述软件数组、SDK 存储和映射窗口。实际寄存器功能按下表和产品表
逐项核查；未说明的地址及位域保留为未知。

### 证据和访问属性

**T** 表示寄存器号、位宽和 shift 来自真实产品表；**S** 表示语义或算法的
依据为真实 SDK 调用；**D** 表示当前驱动的编码与配置策略；**B** 表示板端读数、
状态观测或像素测试。组合配置通过测试，仅证明记录中的配置和输入。

本文记录软件的读取、配置和确认操作。硬件逐位的 RO、RW、W1C、W0C 属性及
完整复位值尚未确定。以下“当前值”均为驱动写入值；软件清零 shadow register 数组及 abort
后的写零操作不能用于推导上电复位值。其余未列位也不能统一解释为 reserved。

真实 SDK 产品表每项为四个 32bit 整数：`swreg`、`width`、`shift`、批量写入
列表筛选标记。`SetDecRegister` 和 `GetDecRegister` 只使用前三项，操作对象
是 shadow register 数组；`swreg=0` 的普通表项被跳过，ASIC ID 另行读取。第四项由
`FlushDecRegisters` 构造提交列表时筛选，不能当作硬件访问属性。
静态调用索引中的 R、W 也仅表示发现 Get、Set 调用，数组索引访问可能未被收录。

<a id="register-index"></a>

## 寄存器总览

以下每个 swreg 均为 4 字节；范围行包含多个 swreg。

| swreg | 字节偏移 | 名称或当前用途 | 说明 |
| --- | --- | --- | --- |
| 0 | `0x000` | ASIC ID | 产品识别 |
| 1 | `0x004` | 控制与 IRQ | 启动、中止、中断状态 |
| 2 | `0x008` | 数据交换与时钟 | 各类数据 swap、clock gating |
| 3 | `0x00c` | 解码模式与控制 | `DEC_MODE` 及 codec 共用控制 |
| 4 | `0x010` | 图像 CB 尺寸 | 宽高与参考数量 |
| 5 | `0x014` | 码流起点及 codec 参数 | start bit、QP 等 |
| 6 | `0x018` | STREAM_LEN | 32bit 整数，单位为字节 |
| 7–13 | `0x01c`–`0x034` | codec 参数 | 依模式解释 |
| 14–19 | `0x038`–`0x04c` | 初始参考列表或 VP9 segment | H.264 mode 15、HEVC、VP9 复用 |
| 20 | `0x050` | 4×4 尺寸 | 部分 CTB 标志及宽高 |
| 30–39 | `0x078`–`0x09c` | 参考状态 | 帧号、位图、VP9 参考尺寸与比例 |
| 42–49 | `0x0a8`–`0x0c4` | 参考与滤波参数 | H.264 列表、HEVC POC、VP9 stride 等 |
| 55 | `0x0dc` | 预取配置 | 当前阈值为 8 |
| 58–60 | `0x0e8`–`0x0f0` | AXI 与 VP9 参数 | swreg59 在 VP9 下承载 sign bias 和滤波 delta |
| 63 | `0x0fc` | PERF_CYCLE_COUNT | SDK 读取的计数器，当前驱动未用于调度 |
| 64–165 | `0x100`–`0x294` | 原生图像与 MV 地址 | 含输出、参考及 VP9 segment map 复用 |
| 166–183 | `0x298`–`0x2dc` | 码流与辅助表地址 | tile、概率、CABAC、scaling 等 |
| 258–259 | `0x408`–`0x40c` | 码流窗口 | 缓冲长度与起始偏移 |
| 261 | `0x414` | 诊断字段 | 候选错误位置；当前只记录原始值 |
| 265–266 | `0x424`–`0x428` | 扩展控制 | 部分语义待确认 |
| 309 | `0x4d4` | Build ID | 目标值 `0x1f88` |
| 314 | `0x4e8` | 原生图像 stride | 四行 tile 的 Y、C 字节行距 |
| 318–319 | `0x4f8`–`0x4fc` | 硬件超时计数器 | 当前均写 `0x80500000` |
| 320–332 | `0x500`–`0x530` | PP0 基本配置 | 线性 NV12 输出 |
| 394 | `0x628` | PP0 附加缩放因子 | 当前横纵因子均为 1 |
| 342、359、376、451、477 | `0x558`、`0x59c`、`0x5e0`、`0x70c`、`0x774` | 其他 PP 的使能寄存器 | 当前使能位均为 0 |

<a id="common"></a>

## 公共控制

### swreg0 与 swreg309 — 识别信息

| 寄存器 | 字段 | 当前观测与检查 | 依据 |
| --- | --- | --- | --- |
| swreg0[31:16] | ASIC product | 实测 `0x8001`，`probe` 比较这 16 位 | D、B |
| swreg0[15:0] | ASIC ID 低 16bit | 实测 `0x8000`；本文保留原始值 | B |
| swreg309[31:0] | Build ID | 实测 `0x00001f88`，`probe` 比较完整的 32bit 寄存器值 | D、B |

完整 ASIC 值 `0x80018000` 是观测值；当前代码对产品的判断使用其高 16 位。

### swreg1 — 控制与中断状态

偏移 `0x004`。依据为 T、厂商内核中断处理、SDK 状态分支和当前
[`th1520_vdec_irq()`](../th1520_vdec_hw.c)。

| 位 | 名称 | 软件用途或当前解释 |
| --- | --- | --- |
| 25:11 | DEC_IRQ_STAT | 15 位聚合状态；产品表 id 2127 |
| 25 | DEC_SCAN_RDY | 候选名称，触发条件待确认 |
| 24 | DEC_PIC_INF | SDK H.264 有读取；VP9 下的独立含义待确认 |
| 23 | DEC_TILE_INT | SDK VP9 等待循环处理的 tile 中断 |
| 22 | DEC_LINE_CNT_INT | 候选名称，触发条件待确认 |
| 21 | DEC_EXT_TIMEOUT_INT | 候选名称，与普通 TIMEOUT 的关系待确认 |
| 20 | DEC_NO_SLICE_INT | 候选名称，触发条件待确认 |
| 19 | DEC_LAST_SLICE_INT | 参考名称；驱动另有同位 DEC_BUSBUSY 别名，语义待确认 |
| 18 | DEC_TIMEOUT | SDK 与当前驱动作为错误状态处理 |
| 17 | DEC_SLICE_INT | 候选名称，触发条件待确认 |
| 16 | DEC_ERROR_INT | SDK 与当前驱动作为解码错误处理 |
| 15 | DEC_ASO_INT | SDK VP9 作为 picture error；当前 VP9 拒绝该状态 |
| 14 | DEC_BUFFER_INT | 输入耗尽；当前完整帧请求作为错误处理 |
| 13 | DEC_BUS_INT | 总线错误状态 |
| 12 | DEC_RDY_INT | 本次硬件任务完成 |
| 11 | DEC_ABORT_INT | 中止状态，当前驱动作为错误处理 |
| 8 | DEC_IRQ | 中断标志；handler 先检查此位 |
| 7 | DEC_TILE_INT_E | tile 中断使能，当前完整帧模式为 0 |
| 6 | DEC_SELF_RESET_DIS | 候选名称，当前为 0；控制语义待确认 |
| 5 | DEC_ABORT_E | 当前软件以 1 请求中止 |
| 4 | DEC_IRQ_DIS | 正常作业为 0；请求中止时为 1 |
| 3 | DEC_TIMEOUT_SOURCE | 候选名称，当前为 0；控制语义待确认 |
| 2 | DEC_BUS_INT_DIS | 候选名称，当前为 0；控制语义待确认 |
| 1 | DEC_STRM_CORRUPTED | SDK H.264 读取；当前驱动作为错误条件 |
| 0 | DEC_E | 最后写入 1 启动；中止处理中等待其清零 |

聚合状态与逐位条目重叠。当前 handler 读回 swreg1 后，把 `[25:11]`、bit8、
bit0 清零，再写回完整的 32bit 整数。这是经过验证的软件操作序列，逐位 W1C、W0C 行为仍待
独立确认。旧 `[23:11]` 掩码少包含两位。

正常 VP9 和一个截断样本均曾返回 `0x00001100`。RDY 需要结合输入范围和
图像结果判断；当前 VP9 在发布参考帧和概率状态之前，另行检查码流消费位置。
ASO 属于 VP9 专用结果检查，尚未加入其他 codec 共用的 ERROR_MASK。

### swreg2 — 数据交换与时钟

偏移 `0x008`，依据 T、S、D。swap 编码的完整置换关系尚未整理。

| 位 | 字段 | 当前配置 |
| --- | --- | --- |
| 31:28 | DEC_STRM_SWAP | 0 |
| 27:24 | DEC_PIC_SWAP | 0 |
| 23:20 | DEC_DIRMV_SWAP | 0 |
| 17 | TILED_MODE_MSB | 0 |
| 15:12 | DEC_TAB_SWAP | H.264 为 3；HEVC、VP9 为 0 |
| 10 | DEC_CLK_GATE_E | 1 |
| 7 | TILED_MODE_LSB | H.264 为 1；HEVC、VP9 为 0 |
| 4 | DRM_E | 当前为 0；其他取值的完整行为待确认 |

当前寄存器值为 H.264 `0x00003480`，HEVC、VP9 `0x00000400`。后两者仍使用
原生 tiled 参考图像，单独依据 bit7 的值不能推导所有 codec 的图像布局。

### swreg3 — 模式与解码控制

偏移 `0x00c`，依据 T、S、D。

| 位 | 字段 | 当前配置或模式范围 |
| --- | --- | --- |
| 31:27 | DEC_MODE | H.264 Baseline 为 0，Main、High 为 15；HEVC 为 12；VP9 为 13 |
| 24 | RLC_MODE_E | H.264 为 0，采用完整帧 VLC 输入 |
| 23 | PIC_INTERLACE_E | H.264 当前逐行输入为 0 |
| 22 | PIC_FIELDMODE_E | H.264 当前逐行输入为 0 |
| 19 | PIC_TOPFIELD_E | H.264 当前逐行输入写 1 |
| 16 | EC_WORD_ALIGN | 当前 8bit、关闭原生参考压缩的配置为 0；其他模式待验证 |
| 15 | DEC_OUT_DIS | 当前为 0，保留原生图像输出 |
| 14 | FILTERING_DIS | H.264 为 0；HEVC 按 PPS deblocking-disable；VP9 为 `lf.level == 0` |
| 13 | MVC_E | H.264 当前为 0 |
| 12 | WRITE_MVS_E | H.264 Main、High 的参考图像为 1；HEVC 为 1；VP9 非关键帧为 1 |
| 10 | SEQ_MBAFF_E | H.264 当前逐行输入为 0 |
| 9 | PICORD_COUNT_E | H.264 profile_idc 大于 66 时为 1 |
| 8 | DEC_OUT_EC_BYPASS | HEVC、VP9、H.264 mode 15 写 1；H.264 mode 0 保持 0，其对应压缩语义单独界定 |
| 7 | 控制 bit7 | HEVC、VP9 写 0；参考名称 APF_ONE_PID 的完整语义待确认 |
| 6 | 参考读取控制 | 当前保持 0，保留参考读取；其他模式组合待确认 |
| 5 | 控制 bit5 | 当前保持 0，语义待确认 |
| 2 | BUFFER_EMPTY_INT_E | 当前完整帧策略写 1 |
| 1 | BLOCK_BUFFER_MODE_E | 当前写 0 |
| 0 | LAST_BUFFER_E | 当前写 0 |

模式 15 的历史名称包含 HIGH10，当前驱动在此模式只接受 8bit 输入。
bit2、bit1、bit0 的 `1、0、0` 是本项目配置；SDK VP9 的初始化未显式设置
这三个字段，因而此值不能称为 SDK VP9 默认值。

### swreg4、swreg12、swreg20 — 图像尺寸

偏移依次为 `0x010`、`0x030`、`0x050`。令 W、H 为编码尺寸，MW、MH 为
`ceil(W/16)`、`ceil(H/16)`，HEVC 的 `min_cb`、`max_ctb` 来自 SPS。

| swreg | 位 | 字段 | H.264 mode 0、15 | HEVC mode 12 | VP9 mode 13 |
| --- | --- | --- | --- | --- | --- |
| 4 | 31:19 | PIC_WIDTH_IN_CBS | `2 * MW` | `W / min_cb` | `ceil(W/8)` |
| 4 | 18:6 | PIC_HEIGHT_IN_CBS | `2 * MH` | `H / min_cb` | `ceil(H/8)` |
| 4 | 4:0 | REF_FRAMES | SPS 最大参考数 | `max(1, 当前 RPS 引用数)` | 当前保持 0 |
| 12 | 15:13 | MIN_CB_SIZE | 3 | `log2(min_cb)` | 3 |
| 12 | 12:10 | MAX_CB_SIZE | 4 | `log2(max_ctb)` | 6 |
| 20 | 31 | PARTIAL_CTB_X | 0 | `W != ALIGN(W,max_ctb)` | 当前保持 0 |
| 20 | 30 | PARTIAL_CTB_Y | 0 | `H != ALIGN(H,max_ctb)` | 当前保持 0 |
| 20 | 27:16 | PIC_WIDTH_4X4 | `4 * MW` | `W / 4` | `ALIGN(W,8) / 4` |
| 20 | 11:0 | PIC_HEIGHT_4X4 | `4 * MH` | `H / 4` | `ALIGN(H,8) / 4` |

依据 T、S、D。build `0x1f88` 的 H.264 两种模式均使用 CB 字段。
头文件中的 `h264_pic_mb_width`、`h264_pic_mb_height_p`、`h264_pic_mb_h_ext`
属于未调用的旧字段定义。HEVC 输入校验要求编码尺寸可整除最小 CB。

### swreg55、swreg58、swreg60 — 总线配置

| swreg | 偏移 | 位 | 字段 | 当前值与解释 |
| --- | --- | --- | --- | --- |
| 55 | `0x0dc` | 31 | APF_DISABLE | 0 |
| 55 | `0x0dc` | 15:0 | APF_THRESHOLD | 8；实际字段宽度为 16bit |
| 58 | `0x0e8` | 15 | REFER_DOUBLEBUFFER_E | 0 |
| 58 | `0x0e8` | 14 | AXI_RD_ID_E | 1 |
| 58 | `0x0e8` | 13 | AXI_WD_ID_E | 0 |
| 58 | `0x0e8` | 10:8 | BUSWIDTH | 编码 2，对应当前 128bit 接口配置 |
| 58 | `0x0e8` | 7:0 | MAX_BURST | 16 |
| 60 | `0x0f0` | 31:16 | AXI_WR_ID | 0 |
| 60 | `0x0f0` | 15:0 | AXI_RD_ID | 0 |

这些值来自当前驱动与 SDK 公共配置的核查。SDK 静态 AXI read unique-ID
enable 为 0，本项目沿用经过板端测试的 1，两者分别记录。

### swreg261、swreg265、swreg266 — 诊断与扩展控制

| swreg | 偏移 | 位 | 字段或候选名称 | 当前用途 |
| --- | --- | --- | --- | --- |
| 261 | `0x414` | 31:22 | ERROR_ADDR_X | 诊断标签，单位和全部触发条件待确认 |
| 261 | `0x414` | 21:12 | ERROR_ADDR_Y | 同上 |
| 261 | `0x414` | 5:3 | ERROR_SLICE_DATA | 同上 |
| 261 | `0x414` | 0 | ERROR_SLICE_HEADER | 同上 |
| 265 | `0x424` | 31 | 未命名 bit31 | 当前保持 0，完整语义待确认 |
| 265 | `0x424` | 27:18 | 未命名字段 | HEVC、VP9 为 64；H.264 为 0 |
| 265 | `0x424` | 17:8 | 未命名字段 | HEVC、VP9 为 64；H.264 为 0 |
| 266 | `0x428` | 31 | 错误处理控制位 | 当前为 0，其他取值待确认 |

swreg265 的当前寄存器值为 HEVC、VP9 `0x01004000`，H.264 为 0。
swreg261 的坐标有依据 T，但现有 SDK 静态访问记录未发现对应字段读取，当前
驱动只输出原始值作为诊断。旧资料中的 swreg260 和旧位宽需要按本表更正。
swreg261 非零值及 PIC_INF 均没有被扩展为本项目 VP9 的失败判据。

### swreg318、swreg319 — 超时计数器

| swreg | 偏移 | 位 | 字段 | 当前值 |
| --- | --- | --- | --- | --- |
| 318 | `0x4f8` | 31 | EXT_TIMEOUT_OVERRIDE_E | 1 |
| 318 | `0x4f8` | 30:0 | EXT_TIMEOUT_CYCLES | 5242880 |
| 319 | `0x4fc` | 31 | TIMEOUT_OVERRIDE_E | 1 |
| 319 | `0x4fc` | 30:0 | TIMEOUT_CYCLES | 5242880 |

两个寄存器的当前值均为 `0x80500000`。计数器时钟来源和全部触发条件仍待核查，因此本文
保留周期数。驱动另有独立的 2 秒软件 watchdog，其恢复测试见
[VP9 验证记录](vp9-validation.md)。

<a id="h264"></a>

## H.264 参数

本节采用 T、真实 H.264 SDK 调用和
[`th1520_vdec_h264.c`](../th1520_vdec_h264.c) 的最终赋值。当前范围为逐行
8bit 4:2:0；以下写零的场相关字段仅描述当前配置。

### swreg5 — 量化及场参数

| swreg | 位 | 字段 | 编码 |
| --- | --- | --- | --- |
| 5 | 24 | TYPE1_QUANT_E | PPS 存在 scaling matrix 时为 1 |
| 5 | 23:19 | CH_QP_OFFSET | `chroma_qp_index_offset`，s5 |
| 5 | 18:14 | CH_QP_OFFSET2 | `second_chroma_qp_index_offset`，s5 |
| 5 | 0 | FIELDPIC_FLAG_E | 当前为 0 |

swreg5[31:25] 的码流起点见“码流窗口”。

### swreg7 — 熵编码与帧号

| swreg | 位 | 字段 | 编码 |
| --- | --- | --- | --- |
| 7 | 31 | CABAC_E | PPS entropy_coding_mode_flag |
| 7 | 30 | BLACKWHITE_E | 当前 4:2:0 配置为 0 |
| 7 | 29 | DIR_8X8_INFER_E | SPS direct_8x8_inference_flag |
| 7 | 28 | WEIGHT_PRED_E | PPS weighted_pred_flag |
| 7 | 27:26 | WEIGHT_BIPR_IDC | PPS weighted_bipred_idc |
| 7 | 20:16 | FRAMENUM_LEN | `log2_max_frame_num_minus4 + 4` |
| 7 | 15:0 | FRAMENUM | 当前 frame_num |

### swreg8、swreg9、swreg12、swreg13 — PPS 与 IDR

| swreg | 位 | 字段 | 编码 |
| --- | --- | --- | --- |
| 8 | 31 | CONST_INTRA_E | PPS constrained_intra_pred_flag |
| 8 | 30 | FILT_CTRL_PRES | PPS deblocking_filter_control_present_flag |
| 8 | 29 | RDPIC_CNT_PRES | PPS redundant_pic_cnt_present_flag |
| 8 | 28 | 8X8TRANS_FLAG_E | PPS transform_8x8_mode_flag |
| 8 | 27:17 | REFPIC_MK_LEN | dec_ref_pic_marking_bit_size，单位 bit |
| 8 | 16 | IDR_PIC_E | 当前图像的 IDR 标志 |
| 8 | 15:0 | IDR_PIC_ID | **仅 mode 0**；idr_pic_id |
| 8 | 7:6 | BIT_DEPTH_Y_MINUS8 | **mode 15**；当前为 0 |
| 8 | 5:4 | BIT_DEPTH_C_MINUS8 | **mode 15**；当前为 0 |
| 9 | 31:24 | PPS_ID | pic_parameter_set_id |
| 9 | 23:19 | REFIDX1_ACTIVE | `num_ref_idx_l1_default_active_minus1 + 1` |
| 9 | 18:14 | REFIDX0_ACTIVE | `num_ref_idx_l0_default_active_minus1 + 1` |
| 9 | 7:0 | POC_LENGTH | pic_order_cnt_bit_size，单位 bit |
| 12 | 31:16 | mode 15 IDR_PIC_ID | **仅 mode 15**；idr_pic_id，产品表 id 339 |
| 13 | 31 | START_CODE_E | 当前 Annex-B 输入写 1 |
| 13 | 30:24 | INIT_QP | `pic_init_qp_minus26 + 26` |

mode 15 的 id 339 在参考枚举中曾被标为 PJPEG_QTABLE_SEL1；真实
`H264SetupVlcRegs` 向该项传递 idr_pic_id，字段宽度为 16bit。另一个参考名称
IDR_PIC_ID_H10 的窄字段与实际调用不同，本文采用真实调用关系。

### 参考列表、帧号与位图

列表元素为 5bit 参考槽号，`i` 从 0 起计。

| 数据 | swreg | 字段位置或公式 |
| --- | --- | --- |
| mode 0 B0、B1 | `42 + floor(i/3)` | i=0..15；B0 shift=`10*(i%3)`，B1 再加 5 |
| mode 15 B0、B1 | `14 + floor(i/3)` | i=0..15；同上 |
| 两种 mode 的 P0..P3 | 47 | shift 10、15、20、25 |
| 两种 mode 的 P4..P9 | 10 | shift 0、5、10、15、20、25 |
| 两种 mode 的 P10..P15 | 11 | shift 0、5、10、15、20、25 |
| REFERn_NBR | `30 + floor(n/2)` | n=0..15；偶数在低 16bit，奇数在高 16bit |
| REFER_LTERM_E | 38 | 当前逐行模式，第 i 槽使用 bit `31-i` |
| REFER_VALID_E | 39 | 当前逐行模式，第 i 槽使用 bit `31-i` |

最后一个 B0、B1 列表寄存器只使用低 10bit，swreg47 的其余列表位置容纳 P0..P3。
H.264 POC 保存在 QTABLE 的内存中；同地址 swreg46..49 的 HEVC POC 解释见后节。

### swreg48、swreg49 — 错误处理和预测滤波

| swreg | 位 | 字段 | 当前配置 |
| --- | --- | --- | --- |
| 48 | 13:12 | ERROR_CONC_MODE | 0；非零模式尚待验证 |
| 49 | 31:22 | PRED_BC_TAP_0_0 | 1 |
| 49 | 21:12 | PRED_BC_TAP_0_1 | -5，s10 |
| 49 | 11:2 | PRED_BC_TAP_0_2 | 20 |

<a id="hevc"></a>

## HEVC 参数

本节采用 T、真实 HEVC SDK 调用和
[`th1520_vdec_hevc.c`](../th1520_vdec_hevc.c) 的最终赋值，适用 mode 12。

### swreg5 — 量化和编码工具

| swreg | 位 | 字段 | 编码 |
| --- | --- | --- | --- |
| 5 | 24 | SCALING_LIST_E | SPS scaling_list_enabled_flag |
| 5 | 23:19 | CH_QP_OFFSET | pps_cb_qp_offset，s5 |
| 5 | 18:14 | CH_QP_OFFSET2 | pps_cr_qp_offset，s5 |
| 5 | 12 | SIGN_DATA_HIDE | PPS sign_data_hiding_enabled_flag |
| 5 | 11 | TEMPOR_MVP_E | SPS 启用 temporal MVP 且当前为非 IDR |
| 5 | 10:5 | MAX_CU_QPD_DEPTH | CU_QPD_E=1 时为 diff_cu_qp_delta_depth，否则 0 |
| 5 | 4 | CU_QPD_E | PPS cu_qp_delta_enabled_flag |

MAX_CU_QPD_DEPTH 使用原始 PPS 差值，真实 `HevcSetRegs` 对 id 91 的赋值支持
这一解释；其单位没有再按 CTB 尺寸换算。

### swreg7 — slice 与滤波工具

| swreg | 位 | 字段 | 编码 |
| --- | --- | --- | --- |
| 7 | 31 | CABAC_INIT_PRESENT | PPS cabac_init_present_flag |
| 7 | 30 | BLACKWHITE_E | 当前为 0 |
| 7 | 28 | WEIGHT_PRED_E | PPS weighted_pred_flag |
| 7 | 27:26 | WEIGHT_BIPR_IDC | PPS weighted_bipred_flag，取 0 或 1 |
| 7 | 25 | FILT_SLICE_BORDER | PPS loop_filter_across_slices_enabled_flag |
| 7 | 24 | FILT_TILE_BORDER | PPS loop_filter_across_tiles_enabled_flag |
| 7 | 23 | ASYM_PRED_E | SPS amp_enabled_flag |
| 7 | 22 | SAO_E | SPS sample_adaptive_offset_enabled_flag |
| 7 | 21 | PCM_FILT_DISABLE | SPS pcm_loop_filter_disabled_flag |
| 7 | 20 | SLICE_CHQP_FLAG | PPS slice_chroma_qp_offsets_present_flag |
| 7 | 19 | DEPEND_SLICE_E | PPS dependent_slice_segment_enabled_flag |
| 7 | 18 | FILT_OVERRIDE_E | PPS deblocking_filter_override_enabled_flag |
| 7 | 17 | STRONG_SMOOTH_E | SPS strong_intra_smoothing_enabled_flag |
| 7 | 16:12 | FILT_OFFSET_BETA | pps_beta_offset_div2，s5 |
| 7 | 11:7 | FILT_OFFSET_TC | pps_tc_offset_div2，s5 |
| 7 | 6 | SLICE_HDR_EXT_E | PPS slice_segment_header_extension_present_flag |
| 7 | 5:3 | SLICE_HDR_EBITS | PPS num_extra_slice_header_bits |

### swreg8、swreg9、swreg10 — 位深、header 与 tile

| swreg | 位 | 字段 | 编码 |
| --- | --- | --- | --- |
| 8 | 31 | CONST_INTRA_E | PPS constrained_intra_pred_flag |
| 8 | 30 | FILT_CTRL_PRES | PPS deblocking_filter_control_present_flag |
| 8 | 16 | IDR_PIC_E | 当前驱动按 **IRAP_PIC** 标志赋值 |
| 8 | 15:12 | PCM_BITDEPTH_Y | PCM 开启时为 pcm_sample_bit_depth_luma_minus1+1 |
| 8 | 11:8 | PCM_BITDEPTH_C | PCM 开启时为 pcm_sample_bit_depth_chroma_minus1+1 |
| 8 | 7:6 | BIT_DEPTH_Y_MINUS8 | 当前为 0 |
| 8 | 5:4 | BIT_DEPTH_C_MINUS8 | 当前为 0 |
| 9 | 23:19 | REFIDX1_ACTIVE | num_ref_idx_l1_default_active_minus1+1 |
| 9 | 18:14 | REFIDX0_ACTIVE | num_ref_idx_l0_default_active_minus1+1 |
| 9 | 13:0 | HDR_SKIP_LENGTH | 硬件跳过的部分 slice header 长度，单位 bit |
| 10 | 23:17 | NUM_TILE_COLS_8K | 实际 tile 列数，关闭 tile 时为 1 |
| 10 | 16:12 | NUM_TILE_ROWS_8K | 实际 tile 行数，关闭 tile 时为 1 |
| 10 | 1 | TILE_ENABLE | PPS tiles_enabled_flag |
| 10 | 0 | ENTR_CODE_SYNCH_E | PPS entropy_coding_sync_enabled_flag |

HDR_SKIP_LENGTH 的当前算法如下，`fls(x)` 为 x 的最高有效位位置，从 1 起计：

```text
skip = output_flag_present ? 1 : 0
skip += separate_colour_plane ? 2 : 0
if 当前为非 IDR:
    skip += log2_max_pic_order_cnt_lsb_minus4 + 4
    skip += 1
    if short_term_ref_pic_set_size != 0:
        skip += short_term_ref_pic_set_size
    else if num_short_term_ref_pic_sets > 1:
        skip += fls(num_short_term_ref_pic_sets - 1)
    skip += long_term_ref_pic_set_size
```

这只表示驱动跳过的 header 中间部分。当前选择 8K tile 字段，旧版的
NUM_TILE_COLS、NUM_TILE_ROWS 位段与其重叠，不能在同一配置中同时使用。

### swreg12、swreg13 — CB、PCM 与变换树

CB 尺寸字段见公共尺寸表。PCM 关闭时，相关 PCM 尺寸和位深保持初始化的零值。

| swreg | 位 | 字段 | 编码 |
| --- | --- | --- | --- |
| 12 | 9:7 | MIN_PCM_SIZE | log2_min_pcm_luma_coding_block_size_minus3+3 |
| 12 | 6:4 | MAX_PCM_SIZE | MIN_PCM_SIZE + log2_diff_max_min_pcm_luma_coding_block_size |
| 12 | 3 | PCM_E | SPS pcm_enabled_flag |
| 12 | 2 | TRANSFORM_SKIP_E | PPS transform_skip_enabled_flag |
| 12 | 1 | TRANSQ_BYPASS_E | PPS transquant_bypass_enabled_flag |
| 12 | 0 | REFPICLIST_MOD_E | PPS lists_modification_present_flag |
| 13 | 31 | START_CODE_E | 当前 Annex-B 输入写 1 |
| 13 | 30:24 | INIT_QP | init_qp_minus26+26 |
| 13 | 15:13 | MIN_TRB_SIZE | log2_min_luma_transform_block_size_minus2+2 |
| 13 | 12:10 | MAX_TRB_SIZE | MIN_TRB_SIZE + log2_diff_max_min_luma_transform_block_size |
| 13 | 9:7 | MAX_INTRA_HIERDEPTH | max_transform_hierarchy_depth_intra |
| 13 | 6:4 | MAX_INTER_HIERDEPTH | max_transform_hierarchy_depth_inter |
| 13 | 3:0 | PARALLEL_MERGE | log2_parallel_merge_level_minus2+2 |

### swreg14–19、swreg38、swreg46–49 — 参考列表和 POC

| 数据 | swreg | 编码 |
| --- | --- | --- |
| 初始 L0、L1 第 i 项 | `14 + floor(i/3)` | i=0..15；L0 为 shift `10*(i%3)` 的 u5，L1 再加 5 |
| REFER_LTERM_E | 38 | 第 i 个参考槽使用 bit `15-i`，与 H.264 位序不同 |
| POC 差第 i 项 | `46 + floor(i/4)` | shift=`24-8*(i%4)`，s8 |

L0 初始顺序为 ST_CURR_BEFORE、ST_CURR_AFTER、LT_CURR；L1 交换前两组，
随后重复填充至 16 项。有效参考槽的 POC 差按 `current_poc - reference_poc` 计算，饱和至
`[-128,127]` 后编码。当前帧自身占紧随活动参考项的一个槽，其 POC 差为 0；
当前实现最多使用 15 个活动参考加一个自身槽。其他空槽的 POC 字段写当前 POC
的低 8bit，地址清零。

<a id="vp9"></a>

## VP9 参数

本节适用 mode 13，依据 T、[SDK VP9 核查](../../analysis/vp9-vendor/README.md)、
[`th1520_vdec_vp9.c`](../th1520_vdec_vp9.c) 和板端记录。

### 帧类型、tile 与变换

| swreg | 位 | 字段 | 编码 |
| --- | --- | --- | --- |
| 5 | 11 | TEMPOR_MVP_E | 满足下述上一帧 MV 条件时为 1 |
| 8 | 16 | IDR_PIC_E | 当前帧为 key 或 intra-only 时为 1 |
| 8 | 7:6 | BIT_DEPTH_Y_MINUS8 | 当前 Profile 0 为 0 |
| 8 | 5:4 | BIT_DEPTH_C_MINUS8 | 当前 Profile 0 为 0 |
| 10 | 23:17 | NUM_TILE_COLS_8K | `1 << tile_cols_log2` |
| 10 | 16:12 | NUM_TILE_ROWS_8K | 按下述空 tile 行规则编码 |
| 10 | 1 | TILE_ENABLE | 配置的行数或列数大于 1 时为 1 |
| 11 | 29:27 | TRANSFORM_MODE | compressed header 的 tx_mode |
| 11 | 10:8 | MCOMP_FILT_TYPE | 0=smooth，1=eighttap，2=sharp，3=bilinear，4=switchable；帧内配置写 0 |
| 11 | 7 | HIGH_PREC_MV_E | allow_high_precision_mv |
| 11 | 5:4 | COMP_PRED_MODE | 0=single，1=compound，2=select |

tx_mode 编码为 0=ONLY_4X4、1=ALLOW_8X8、2=ALLOW_16X16、3=ALLOW_32X32、
4=SELECT。当前无损量化组合要求 ONLY_4X4。

TEMPOR_MVP_E 要求当前为 inter、非 error-resilient，上一成功帧有效且显示、
上一帧为非 key，并且前后原始宽高相同。上一帧为 intra-only 的情形遵循
当前 SDK 分支，条件中单独排除的是上一 key frame。

令 R=`1 << tile_rows_log2`，SR=`ceil(H/64)`。当 SR 大于 2 时，行数字段写
`min(R,SR)`；否则写 R。tile 表在 `R=SR+1` 时从行 1 开始，在 `R=SR+2`
时从行 2 开始，其余从行 0 开始；跳过的高度合并进首个实际表项。多行 tile
算法的依据为 SDK 调用与离线检查，本轮板端样本覆盖单行 tile。

### swreg13 — 量化、参考模式与 segmentation

| swreg | 位 | 字段 | 编码 |
| --- | --- | --- | --- |
| 13 | 28:23 | QP_DELTA_Y_DC | delta_q_y_dc，s6 |
| 13 | 22:17 | QP_DELTA_CH_DC | delta_q_uv_dc，s6 |
| 13 | 16:11 | QP_DELTA_CH_AC | delta_q_uv_ac，s6 |
| 13 | 10 | LAST_SIGN_BIAS | LAST 的 sign bias |
| 13 | 9 | LOSSLESS_E | base_q_idx 与三个量化 delta 均为 0 |
| 13 | 8:7 | COMP_PRED_VAR_REF1 | 第二个可变参考编号 |
| 13 | 6:5 | COMP_PRED_VAR_REF0 | 第一个可变参考编号 |
| 13 | 4:3 | COMP_PRED_FIXED_REF | 固定参考编号 |
| 13 | 2 | SEGMENT_TEMP_UPD_E | segmentation 开启且更新 map、启用 temporal update |
| 13 | 1 | SEGMENT_UPD_E | segmentation 开启且更新 map |
| 13 | 0 | SEGMENT_E | segmentation_enabled |

复合参考字段的编号为 LAST=1、GOLDEN=2、ALTREF=3，按三者 sign bias 关系
选择固定和可变参考。没有允许的复合参考组合时，当前代码把三个字段写 0。

### swreg14–19、swreg31、swreg32 — 八个 segment

segment 0 至 5 分别使用 swreg14 至 19，segment 6、7 分别使用 swreg31、32。
每个寄存器的共同布局如下：

| 位 | 字段 | 编码 |
| --- | --- | --- |
| 17:15 | REFPIC_SEGn | 0 表示没有启用此 feature；启用时为 VP9 ref index+1；key frame 写 0 |
| 14 | SKIP_SEGn | skip feature 是否启用 |
| 13:8 | FILT_LEVEL_SEGn | 基础 level 加 delta 或使用 absolute 值，限制至 0..63 |
| 7:0 | QUANT_SEGn | 基础 Q index 加 delta 或使用 absolute 值，限制至 0..255 |

swreg14 还在 `[23:18]` 保存全局 FILT_LEVEL，取 `lf.level`。更新 segment 0
低 18bit 时保留这一字段。segment feature 数据在未更新时沿用先前状态；
key、intra-only、error-resilient 时清空历史状态。图像尺寸改变时清空两份
segment map，保留 feature 数据。读写 map 只在成功完成后交换。

### swreg30、swreg59 — Loop filter

| swreg | 位 | 字段 | 编码 |
| --- | --- | --- | --- |
| 30 | 30:28 | FILT_SHARPNESS | lf.sharpness |
| 30 | 27:21 | FILT_MB_ADJ_0 | 第 0 个 mode delta，s7 |
| 30 | 20:14 | FILT_MB_ADJ_1 | 第 1 个 mode delta，s7 |
| 59 | 29 | GREF_SIGN_BIAS | GOLDEN 的 sign bias |
| 59 | 28 | AREF_SIGN_BIAS | ALTREF 的 sign bias |
| 59 | 27:21 | FILT_REF_ADJ_0 | ref_delta[0]，s7 |
| 59 | 20:14 | FILT_REF_ADJ_1 | ref_delta[1]，s7 |
| 59 | 13:7 | FILT_REF_ADJ_2 | ref_delta[2]，s7 |
| 59 | 6:0 | FILT_REF_ADJ_3 | ref_delta[3]，s7 |

delta_enabled 为 0 时，当前代码把所有 delta 字段写 0。这里的坐标不同于
旧 G2 的 swreg46、swreg47 布局。

### swreg33–38、swreg42–44 — 参考尺寸、比例与 stride

| 参考 | 原始尺寸寄存器 | 缩放比例寄存器 | 原生 stride 寄存器 |
| --- | --- | --- | --- |
| LAST | 33 | 36 | 42 |
| GOLDEN | 34 | 37 | 43 |
| ALTREF | 35 | 38 | 44 |

尺寸寄存器的 `[31:16]`、`[15:0]` 分别保存原始参考宽高。比例寄存器的高、低 16bit
分别为 `floor(ref_width * 16384 / current_width)`、
`floor(ref_height * 16384 / current_height)`。stride 寄存器的高、低 16bit
分别保存 Y、C 的四行 tile 字节行距。参考尺寸使用原始像素值，存储尺寸的
补齐规则见“原生图像布局”。

<a id="dma"></a>

## DMA 地址寄存器

以下地址对均为 **高 32bit 所在寄存器在前，低 32bit 所在寄存器在后**，
每个寄存器保存一个 32bit 整数。当前驱动使用
32bit DMA mask，仍显式提交高 32bit。地址表示与大于 4 GiB 的平台 DMA 能力
分别判断，后者尚未进行板端验证。

### 原生输出及参考

| MSB、LSB swreg | MSB、LSB 偏移 | 当前用途 |
| --- | --- | --- |
| 64、65 | `0x100`、`0x104` | 当前原生 Y 基址 |
| 98、99 | `0x188`、`0x18c` | 当前原生 C 基址；H.264 仅 mode 15 显式使用 |
| 132、133 | `0x210`、`0x214` | 当前 MV 基址；H.264 名为 DIR_MV_BASE |
| 66+2i、67+2i | `0x108+8i`、`0x10c+8i` | 参考 Y，i=0..15 |
| 100+2i、101+2i | `0x190+8i`、`0x194+8i` | 参考 C，i=0..15 |
| 134+2i、135+2i | `0x218+8i`、`0x21c+8i` | 参考 MV，i=0..15；VP9 的具体映射见下表 |

H.264 mode 0 以原生图像基址使用 Y 槽；mode 15 另外配置 C、MV 参考地址。
H.264 参考 Y 地址低 32bit 中的 bit1、bit0 分别复用为 FIELD、TOPC 标志。当前代码的
TOPC 取顶场 POC 与当前 POC 的距离小于底场距离这一条件；参考 MV 地址先
清除低两位，再增加 MV 偏移。字段存在并不扩大本驱动的逐行输入支持范围。

VP9 使用下列复用，segment map 会占用其他模式的部分参考 Y 地址：

| VP9 数据 | MSB、LSB swreg |
| --- | --- |
| LAST Y、C | 66、67；100、101 |
| GOLDEN Y、C | 74、75；108、109 |
| ALTREF Y、C | 76、77；110、111 |
| 上一次成功解码帧的 MV | 134、135 |
| LAST 参考 MV | 136、137 |
| GOLDEN 参考 MV | 142、143 |
| ALTREF 参考 MV | 144、145 |
| Segment map 写地址 | 78、79 |
| Segment map 读地址 | 80、81 |

上一解码帧的 MV 与 LAST 参考槽分别保存。当前驱动为前者分配 context 私有
缓冲，避免 CAPTURE 再次排队改变其有效期。

### 码流和辅助表

| MSB、LSB swreg | MSB、LSB 偏移 | H.264 | HEVC | VP9 |
| --- | --- | --- | --- | --- |
| 166、167 | `0x298`、`0x29c` | 当前未使用 | tile 宽高表 | tile 宽高表 |
| 168、169 | `0x2a0`、`0x2a4` | 码流 | 码流 | 码流，完成后读取消费位置 |
| 170、171 | `0x2a8`、`0x2ac` | 当前 VLC 未使用 | scaling lists | 概率计数器 |
| 172、173 | `0x2b0`、`0x2b4` | 当前 VLC 未使用 | 当前未使用 | 概率表 |
| 174、175 | `0x2b8`、`0x2bc` | QTABLE：CABAC、POC、scaling | 当前未使用 | 当前未使用 |
| 176、177 | `0x2c0`、`0x2c4` | 当前未使用 | 当前未使用 | 当前未使用 |
| 178、179 | `0x2c8`、`0x2cc` | 当前未使用 | 纵向 tile filter 辅助缓冲 | tile filter 辅助缓冲 |
| 180、181 | `0x2d0`、`0x2d4` | 当前未使用 | tile SAO 辅助缓冲 | 当前未使用 |
| 182、183 | `0x2d8`、`0x2dc` | 当前未使用 | BSD 辅助缓冲 | BSD 辅助缓冲 |

H.264 的 DIR_MV_BASE 为 132、133；旧资料中的 DIFF_MV_BASE 为另一种 SDK
配置，二者应分别解释。源码中 `RS_OUT_*`、`DS_*` 等未使用地址宏的名称，
尚不足以说明该产品当前模式的硬件用途。

<a id="stream"></a>

## 码流窗口

| swreg | 偏移 | 位 | 字段 | 单位 |
| --- | --- | --- | --- | --- |
| 5 | `0x014` | 31:25 | STRM_START_BIT | bit，当前相对 16 字节边界 |
| 6 | `0x018` | 31:0 | STREAM_LEN | byte |
| 258 | `0x408` | 31:0 | STRM_BUFFER_LEN | byte |
| 259 | `0x40c` | 31:0 | STRM_START_OFFSET | byte |

以下精确描述当前后端的赋值。令 B=`plane_dma`，D=`data_offset`，
U=`bytesused`，C=`plane_size`，其中 U 包含前缀 D。

**H.264、HEVC 的 Annex-B 输入**：令 A=B+D，p=A mod 16。

```text
STREAM_BASE       = A - p
STRM_START_BIT    = 8 * p
STREAM_LEN        = U - D + p
STRM_START_OFFSET = 0
STRM_BUFFER_LEN   = U - D + p    # H.264
STRM_BUFFER_LEN   = C - D + p    # HEVC
START_CODE_E      = 1
```

**VP9 完整帧输入**：要求 B 为 16 字节边界，header_bytes 为两类 header
长度之和。令 A=D+header_bytes，Q=`floor(A/16)*16`。

```text
STREAM_BASE       = B
STRM_START_BIT    = 8 * (A - Q)
STRM_START_OFFSET = Q
STREAM_LEN        = U - Q
STRM_BUFFER_LEN   = C
```

VP9 在硬件开始处理 tile 前跳过 uncompressed header 和 compressed header。
完成时读取 swreg168、169 组成的地址，当前要求其处于 `[B+A, B+U]`。板端
186 次合法解码的读回位于有效末尾或末尾前一字节；一个截断输入越过末尾
32 字节，虽然 IRQ 只含 RDY，驱动仍将其返回为 ERROR。swreg6 在该实验中
保持原配置值，本文将其解释为配置长度，未将读回值解释为剩余字节数。

<a id="memory"></a>

## 原生图像与辅助内存

### swreg314 — 原生 stride

偏移 `0x4e8`，`[31:16]` 为 DEC_OUT_Y_STRIDE，`[15:0]` 为 DEC_OUT_C_STRIDE。
当前 8bit 4:2:0、原生 4×4 tiled 配置下，两字段相同，描述四个图像行的
字节行距。它与 PP 线性 NV12 的 bytesperline 分开配置。

| 模式 | 原生 stride S | Y 区长度 L | C 区长度 | MV 起点 | MV 容量 |
| --- | --- | --- | --- | --- | --- |
| H.264 mode 0 | `ALIGN(4W,64)` | `S*ALIGN(H,16)/4` | `ALIGN(L/2,64)` | `3L/2` | 分配覆盖 `ALIGN(80*ALIGN(MW*MH,4),64)` |
| H.264 mode 15 | `ALIGN(4W,64)` | 同上 | 同上 | `L+ALIGN(L/2,64)+64` | 同上 |
| HEVC | `ALIGN(4W,64)` | `S*ALIGN(H,16)/4` | `ALIGN(L/2,64)` | `L+ALIGN(L/2,64)+64` | `ceil(W/64)*ceil(H/64)*256` |
| VP9 | `ALIGN(4*ALIGN(W,8),64)` | `ALIGN(S*ALIGN(H,8)/4,64)` | `ALIGN(S*ALIGN(H,8)/8,64)` | `L+C长度+64` | `ceil(W/64)*ceil(H/64)*1024` |

这里的 HEVC MV 容量按覆盖图像的 64×64 区域计算，SPS 的实际 CTB 尺寸可以
更小。H.264 私有分配采用覆盖两种模式的较大容量；输出 MV 地址只在当前
Main、High 的参考图像中启用。HEVC、VP9 每次复用前清零 `MV起点-32` 开始的
32 字节同步区。上述算式记录本项目支持模式的配置，参考帧压缩布局另行验证。

H.264 的总分配始终为 `mode 15 的 MV 起点 + MV 预留容量`，mode 0 的 MV
起点较早，末尾因而具有额外余量。HEVC、VP9 的总分配为各自 MV 起点加容量。

### 辅助表格式

| 基址 | 当前内容和容量 |
| --- | --- |
| H.264 QTABLE | 920 个 u32 CABAC 常量，随后 34 个 u32 POC，再保存 scaling 数据 |
| HEVC SCALE_LIST_BASE | 分配 1024 字节；有定义的数据占 1008 字节，矩阵按列优先排列 |
| HEVC TILE_BASE | 每项两个 u16：宽、高，单位 CTB；按 tile 行优先排列 |
| VP9 TILE_BASE | 每项两个 little-endian u16：宽、高，单位 64×64 SB |
| VP9 PROB_TAB_BASE | 3744 字节，含固定概率、segment 概率与自适应概率 |
| VP9 CTX_COUNTER_BASE | 13264 字节，软件在成功完成后读取并进行适配 |
| VP9 SEGMENT_READ、WRITE | 两份 map，每份 `ceil(W/64)*ceil(H/64)*32` 字节 |

H.264 QTABLE 内 CABAC 占字节 0..3679，POC 占 3680..3815。mode 0 的
224 字节 scaling 数据从 3816 开始，mode 15 从 3824 开始；分配总量为
4048 字节。scaling 数据的字节组织与 DEC_TAB_SWAP=3 配套。

VP9 概率与计数器的完整 C 布局位于
[`th1520_vdec_vp9_probs.h`](../th1520_vdec_vp9_probs.h)，字段坐标经真实 SDK
尺寸、复制长度及 52 个偏移锚点核查。tile filter、SAO、BSD 缓冲的内部格式
尚未完整说明；当前分配公式见各 codec 后端。

<a id="pp0"></a>

## PP0 线性 NV12 输出

以下配置适用于 8bit 4:2:0、偶数处理宽高、零裁剪起点、输入输出等尺寸。
依据为真实 `ppu_regs` id 数组、`PPSetRegs`、相关缓冲计算函数和板端像素比较。
当前驱动在配置前清零 swreg320 至 511，只启用 PP0。

令 P_W、P_H 为传入 `th1520_vdec_set_postproc()` 的处理尺寸。H.264 取
`ctx->src_fmt.width/height`，HEVC 取 SPS 的 `pic_width/height_in_luma_samples`，
VP9 取当前帧宽高。H.264 的帧裁剪和 HEVC conformance window 可能使最终
可见区域小于这些处理尺寸，应由用户空间按码流信息处理。

| swreg | 偏移 | 位 | 字段或用途 | 当前配置 |
| --- | --- | --- | --- | --- |
| 320 | `0x500` | 0 | PP0 输出使能 | 1 |
| 320 | `0x500` | 1 | Cr-first | 0，输出 CbCr 顺序 |
| 320 | `0x500` | 3 | Tile 输出 | 0，输出线性图像 |
| 321 | `0x504` | 31:0 | 输出交换等参数 | 寄存器值为 0，其他编码待确认 |
| 322 | `0x508` | 31:27 | 输入格式 | 1 |
| 322 | `0x508` | 26:25 | 横向缩放 mode | 0 |
| 322 | `0x508` | 24:23 | 纵向缩放 mode | 0 |
| 322 | `0x508` | 22:18 | 输出格式 | 0，结合当前其他字段形成 NV12 |
| 323 | `0x50c` | 31:0 | 范围与缩放相关配置 | 当前寄存器值为 0，完整语义待确认 |
| 324 | `0x510` | 31:0 | 逆缩放比例相关配置 | 当前寄存器值为 0，完整语义待确认 |
| 325 | `0x514` | 31:0 | PP Y 基址 MSB | 输出 DMA 地址高 32bit |
| 326 | `0x518` | 31:0 | PP Y 基址 LSB | 输出 DMA 地址低 32bit |
| 327 | `0x51c` | 31:0 | PP UV 基址 MSB | 输出 DMA 地址高 32bit |
| 328 | `0x520` | 31:0 | PP UV 基址 LSB | 输出 DMA 地址低 32bit |
| 329 | `0x524` | 31:16 | Y stride | CAPTURE bytesperline |
| 329 | `0x524` | 15:0 | UV stride | 与 Y stride 相同 |
| 330 | `0x528` | 28:16 | 裁剪 x 起点 | 0 |
| 330 | `0x528` | 12:0 | 裁剪 y 起点 | 0 |
| 331 | `0x52c` | 31:16 | 输入宽度 | P_W/2 |
| 331 | `0x52c` | 15:0 | 输入高度 | P_H/2 |
| 332 | `0x530` | 31:16 | 输出宽度 | P_W，单位像素 |
| 332 | `0x530` | 15:0 | 输出高度 | P_H，单位像素 |
| 394 | `0x628` | 31:24 | PP0 附加横向缩放因子 | 1 |
| 394 | `0x628` | 23:16 | PP0 附加纵向缩放因子 | 1 |

swreg331 的减半来自目标 build 的尺寸单位，等尺寸输出时也采用这一编码。
swreg394 的两字段各为 8bit，当前寄存器值为 `0x01010000`；旧参考枚举中的
PP1_OUT_E_U、PP1_OUT_SWAP_U 名称与这里的真实 PP0 id 对应关系有误。

PP UV 基址为 `Y_dma + bytesperline * capture_height`。capture_height 是
协商后的存储高度，可能包含填充；swreg331、332 使用上述处理尺寸。320×240、
stride=320 时，swreg329、331、332 分别为 `0x01400140`、`0x00a00078`、
`0x014000f0`。1920×1080 的 VP9 测试使用 1920×1088 CAPTURE 存储。

真实 id 数组含六组 PP 描述，各组使能位置为 swreg320[0]、342[0]、359[0]、
376[0]、451[0]、477[0]。当前仅 PP0 具有本项目的完整配置与像素验证，其他组
的坐标存在不等于全部功能通过测试。

## 提交、完成与中止

当前驱动每个作业重新建立包含 512 个 32bit 整数的 shadow register 数组，使用标准 MMIO 访问：

```text
配置 codec、原生参考、辅助表和 PP0
写入 swreg3 至 swreg511
写入 swreg2
wmb()
最后写入 swreg1，DEC_E=1
```

swreg0 跳过写入。`flush_all=0` 为诊断用稀疏提交选项，现有验证使用默认完整
提交。完整数组中的写零也包含本文尚未说明的寄存器，不能因此把它们定义为普通 RW。

中断处理先取得状态、确认 IRQ，再完成 codec 专用结果检查。VP9 的概率、
segment map 和参考有效性只在成功后发布；失败后要求关键帧恢复。
中止处理在 DEC_E=1 时设置 DEC_ABORT_E、DEC_IRQ_DIS，轮询 DEC_E 清零，
最长等待 100 ms。当前实现即使轮询超时，也在记录错误后继续清空配置；
真实硬件持续忙碌时的完整 abort 行为仍待专门验证。

## 子系统边界

以下偏移相对设备树中的 VPU 子系统基址，依据厂商内核 `subsys.c`：

| 子系统块 | 相对子系统偏移 | 与本文的关系 |
| --- | --- | --- |
| VCMD | `0x0000` | 厂商命令提交引擎 |
| Decoder | `0x1000` | 本文 swreg 的基址 |
| L2CACHE | `0x2000` | 独立块，字段另行核查 |
| MMU | `0x3000` | 独立块，当前采用旁路 |
| DEC400 | `0x6000` | 独立块，压缩相关功能另行核查 |

MMU 自身的索引为 6、226 的寄存器分别用于标识和控制；它们具有 MMU 基址，不能与
decoder swreg6、swreg226 混用。当前驱动检查 MMU 控制 bit0，必要时将完整的 32bit 寄存器值写为零。
现有 MMIO 解码通过板端测试，VCMD、参考帧压缩及其他总线模式有各自的验证范围。

## 尚待确认的条目

当前资料未给出完整上电复位值和逐位访问副作用。IRQ bit19 的名称冲突、若干
扩展状态位、swreg265 的字段细义、swreg266 的非零配置、额外 PP 和参考帧压缩
继续保留为待确认项。

驱动中 `hevc_output_8_bits`、`hevc_output_format`、`hevc_down_scale_e` 当前
只写零，其旧 G2 字段形状没有对应的目标产品表项；同类未调用宏也未用于本指南
推导硬件语义。H.264 mode 15 的 IDR、当前 CB 尺寸、VP9 filter delta 及 PP0
附加因子均按真实产品坐标和调用解释，避免混用旧表。

完整产品表有 2129 项，其中 1902 项的 swreg 非零，涉及 469 个寄存器，最大编号
为 511。各项还包含其他 codec 的重叠别名，不能视为 1902 个相互独立的硬件
字段。未纳入本文语义说明的项目可查阅
[产品表目录](../../analysis/vc8000d-register-config/swreg-map-vc8000d.md)，其中
参考枚举名称和静态 SW 标记需按本文的证据规则解释。

<a id="sources"></a>

## 来源与核查

真实 SDK 对象为
`reference/vpu-omxil/usr/lib/omxil/libOMX.hantro.VC8000D.video.decoder.so`，SHA-256：

```text
dc6dbe50b4698ec000d07a64b4dae4c894355ff70bac35c395c9252c432d7e73
```

| 来源 | 用途 |
| --- | --- |
| 产品 `0x8001` 的二进制表 `0x4cbb80` | 本次重新读取寄存器号、width、shift |
| SetDecRegister `0x169e5e`、GetDecRegister `0x16a178` | 选表及影子位域操作 |
| 表列表辅助函数 `0x16a3aa`、FlushDecRegisters `0x16a534`、RefreshDecRegisters `0x16a5ee` | 第四个整数的筛选用途 |
| H264ReconfigurePictureBuffers `0x707b6`，相关分支 `0x7087a` 至 `0x70936` | 目标 build 的 CB 尺寸，另见当前 H.264 后端 |
| H264SetupVlcRegs `0x7aef4`，相关分支 `0x7b1b6` 至 `0x7b1ea` | mode 15 的 id 339 使用完整 idr_pic_id |
| HevcDecodePicParamSet 内 `0x121458`、HevcSetRegs 内 `0x11721e` 和 `0x117228` | PPS 的 QP delta 深度解析与赋值 |
| [VP9 SDK 核查](../../analysis/vp9-vendor/README.md) | VP9 位域、概率、计数器与 native 布局 |
| [VP9 错误处理核查](../../analysis/board-vp9-2026-09-23/sdk-error-audit.md) | IRQ 分类与消费地址读回 |
| PPSetRegs `0x163fc4`、ppu_regs 数组 `0x50bed8` | PP id 坐标；`0x164cfc` 和 `0x16516c` 设置 swreg394 两个因子 |
| [驱动位域定义](../th1520_vdec_regs.h)、[VP9 位域定义](../th1520_vdec_vp9_regs.h) | 与当前软件定义比较，另核查实际调用 |
| [H.264、HEVC 验证](validation.md)、[VP9 验证](vp9-validation.md) | 板端配置及实际覆盖范围 |

历史综述曾将 GetDecRegister 入口记为 `0x16a158`；本次核查的函数入口为
`0x16a178`。本指南保留更正说明，SDK 原始反编译与数据库继续保存在 Git
排除目录中。Linux 参考源码的固定版本与许可见 [来源说明](sources.md)。

跨出 `driver` 的链接位于上层资料仓库。工作目录另外保留了
`analysis/h264-frame-config-review.md`、`analysis/hevc-field-audit/README.md`
和 `analysis/vc8000d-pp-output.md` 等历史核查材料，它们当前属于未提交的
可选研究记录；本文将必要的真实符号、地址和配置关系列在正文中。
