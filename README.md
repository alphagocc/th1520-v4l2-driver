# TH1520 VC8000D V4L2 stateless 解码驱动

`th1520-vdec.ko` —— 面向 TH1520 VPU 解码核（VC8000D）的 Linux V4L2
memory-to-memory **stateless** 解码驱动，支持 H.264（G1 模式）与 HEVC（G2 模式）。

> **状态：尚未在目标板上编译或运行过。** 本目录是根据 `analysis/` 中已验证的
> 寄存器分析与 `reference/` 中的真实资料写出的首版实现，所有推测性取值都在
> 代码注释和本文件 §6 中显式标出。上板调试前请通读 §5 和 §6。

---

## 1. 设计决定

| 项目 | 取值 | 依据 |
| --- | --- | --- |
| 解码模型 | **stateless + Media Request API** | 硬件是裸寄存器接口，码流解析与 DPB 管理在提交方；见 `memory/v4l2-m2m-driver-goal.md` |
| 寄存器编程方式 | **驱动侧影子寄存器 + 整片 flush** | 与真实交付栈（DWL 影子寄存器 + `HANTRODEC_IOCS_DEC_PUSH_REG`）同构 |
| 硬件访问路径 | 直接 MMIO + platform IRQ（**不走 VCMD**） | 见 §6.1 |
| 参考帧压缩 / L2 cache / DEC400 / 后处理 / 下采样 | 全部关闭 | 可选特性，先跑通基本通路 |
| OUTPUT 格式 | `V4L2_PIX_FMT_H264_SLICE`、`V4L2_PIX_FMT_HEVC_SLICE` | Annex-B 起始码，帧级提交 |
| CAPTURE 格式 | `V4L2_PIX_FMT_NV12`（8 bit 4:2:0） | 10 bit 与压缩格式待后续 |

### 为什么用影子寄存器而不是直接对 MMIO 做 read-modify-write

上游 `hantro` 驱动是直接在 MMIO 上做位域 RMW 的，这要求未被驱动写到的位
必须处于正确的硬件复位值。而 `analysis/vc8000d-register-config/README.md` §10.7
明确把"各寄存器复位值"列为**待硬件验证**项。

本驱动每帧把影子数组清零后重建全部配置，再一次性推给硬件，因此
**每个被写入寄存器的每一位都由驱动决定**，不依赖复位值，也不依赖上一帧残留。
这与真实交付栈的行为一致，也让"哪些位被写了什么"完全可从代码读出来。

推送顺序 `swreg3..N → swreg2 → swreg1` 直接来自厂商内核
`hantro_dec.c:860 DecFlushRegs()`：`swreg1` 里含 `DEC_E`，写它才真正启动硬件，
所以必须最后写；`swreg0` 是只读的 HW build id，从不写入。

---

## 2. 文件

| 文件 | 内容 |
| --- | --- |
| `th1520_vdec_regs.h` | swreg 位域定义。**全部数值转录自 `analysis/vc8000d-register-config/swreg-map-{h264,hevc}.md`**（该表由脚本从真实二进制的寄存器规格表 dump 得到） |
| `th1520_vdec.h` | dev / ctx / codec ops / 缓冲几何 |
| `th1520_vdec_drv.c` | platform driver：clock、IRQ、runtime PM、m2m 设备、media 设备、看门狗 |
| `th1520_vdec_v4l2.c` | vidioc ops、格式协商、vb2 队列、stateless 控件集合 |
| `th1520_vdec_hw.c` | 影子寄存器、公共配置、flush/启动、abort 复位、中断处理 |
| `th1520_vdec_h264.c` | H.264 控件 → G1 寄存器；DPB 匹配、参考列表、私有表 |
| `th1520_vdec_h264_cabac.c` | H.264 CABAC 初始化常量表（取自上游内核，见文件头版权） |
| `th1520_vdec_hevc.c` | HEVC 控件 → G2 寄存器；tile 表、缩放矩阵、参考帧地址 |
| `dts/th1520-vpu-dec-example.dtso` | 示例设备树节点（地址与中断号待验证） |

---

## 3. 平台资源证据

全部来自 `reference/vpu-vc8000d-kernel/linux/subsys_driver/`：

| 资源 | 值 | 出处 |
| --- | --- | --- |
| VPU 子系统基址 | `0xff_ecc0_0000` | `subsys.c` `subsys_array[]` |
| VC8000D 解码核偏移 / 窗口 | `+0x1000`，`1023 * 4` 字节 | `subsys.c` `core_array[]` |
| 时钟 | `cclk`（核心）、`aclk`（AXI）、`pclk`（APB） | `hantro_dec.c` probe 的三个 `devm_clk_get()` |
| 中断 | `platform_get_irq(pdev, 0)` | `hantro_dec.c` probe |
| 电源域 | `power-domains` 属性（`check_power_domain()`） | `hantro_dec.c` |
| 厂商 compatible | `thead,light-vc8000d`、`xuantie,th1520-vc8000d` | `hantro_dec.c` `isp_of_match[]` |

**本驱动使用独立的 compatible `xuantie,th1520-vc8000d-v4l2`**，原因有二：

1. 避免与仍可能加载的厂商 `hantrodec.ko` 争抢同一个平台设备和同一片寄存器；
2. 厂商 DT 的 `reg` 是**子系统基址**，驱动内部再加 `0x1000`；本驱动要求
   `reg` 直接指向解码核，不在代码里藏魔数偏移。

---

## 4. 构建

### 已验证的构建环境（2026-08-06 实测）

目标板：LicheePi 4A / RevyOS，Debian 13 trixie，内核 **6.6.140-th1520**，gcc 14.3（板上原生编译）。

```sh
# 板上：/lib/modules/6.6.140-th1520/build 是个悬空符号链接，
# 头文件实际在 /usr/src/linux-headers-6.6-th1520（其 kernel.release 就是 6.6.140-th1520），
# 所以必须显式给 KDIR。
make KDIR=/usr/src/linux-headers-6.6-th1520 \
     KBUILD_EXTRA_SYMBOLS=/home/debian/v4l2-helpers/Module.symvers \
     modules
```

### 前置：内核缺两个 V4L2 helper 模块

RevyOS 的 6.6.140 配置里没有任何 m2m 驱动（`# CONFIG_VIDEO_HANTRO is not set`），
而 `CONFIG_V4L2_MEM2MEM_DEV` 和 `CONFIG_V4L2_H264` 是由 in-tree 驱动 `select` 的隐藏选项，
因此这两个 helper 根本没被编译，链接会缺 32 个符号：

- `v4l2_m2m_*`（29 个）← `drivers/media/v4l2-core/v4l2-mem2mem.c`
- `v4l2_h264_init_reflist_builder` / `build_p_ref_list` / `build_b_ref_lists`
  ← `drivers/media/v4l2-core/v4l2-h264.c`

`videodev.ko` / `mc.ko` / `videobuf2-*.ko` 都是现成的，所以**不需要重编内核**，
把这两个文件按外部模块编出来即可（它们只用到已导出的符号）：

```sh
mkdir -p ~/v4l2-helpers && cd ~/v4l2-helpers
B="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/drivers/media/v4l2-core"
curl -fsSL -o v4l2-mem2mem.c "$B/v4l2-mem2mem.c?h=v6.6.140"
curl -fsSL -o v4l2-h264.c    "$B/v4l2-h264.c?h=v6.6.140"
printf 'obj-m += v4l2-mem2mem.o\nobj-m += v4l2-h264.o\n' > Kbuild
printf 'modules:\n\t$(MAKE) -C $(KDIR) M=$(shell pwd) modules\n' > Makefile
make KDIR=/usr/src/linux-headers-6.6-th1520 modules
```

> 版本必须与目标内核一致。用更新版本（例如仓库里 6.17 快照的
> `reference/v4l2-m2m/source/framework/v4l2-mem2mem.c`）会因 vb2 API 漂移编不过。

### 加载顺序

```sh
sudo rmmod hantrodec            # 它绑着同一个 vdec@ffecc00000 节点
sudo modprobe videodev          # 带出 mc / videobuf2-common / videobuf2-v4l2
sudo modprobe videobuf2-dma-contig
sudo insmod ~/v4l2-helpers/v4l2-mem2mem.ko
sudo insmod ~/v4l2-helpers/v4l2-h264.ko
sudo insmod ~/th1520-vdec/th1520-vdec.ko
```

`vc8000.ko`（编码器）绑的是 `venc@ffecc10000`，与本驱动无关，**不需要卸载**。

### 目标内核版本适配

代码按 `reference/v4l2-m2m/` 固定的 commit `8ba098e6b6ff`（约 6.17）的 API 编写，
以下几处按 6.6 做了适配：

| API | 6.6 上的情况 | 处理 |
| --- | --- | --- |
| `platform_driver` 移除回调 | 6.5–6.10 用 `.remove_new`（返回 `void`） | `LINUX_VERSION_CODE` 两路适配 |
| `v4l2_m2m_buf_copy_metadata()` | 6.6 有第三个参数 `copy_frame_flags` | `LINUX_VERSION_CODE` 两路适配，解码器传 `true` |
| `vb2_ops.wait_prepare/wait_finish` | 6.6 仍需要 | 保留；升级到更新内核时需删掉 |
| `vb2_find_buffer()` | 6.1 引入 | 直接使用 |
| `V4L2_CID_STATELESS_HEVC_*` | 6.0 起稳定 uAPI | 直接使用 |
| `v4l2_ctrl_hevc_decode_params.{short,long}_term_ref_pic_set_size` | 6.6 已有（实测编译通过） | 直接使用 |

---

## 5. 上板前的检查清单

1. **确认厂商 `hantrodec.ko` 未加载**，否则两个驱动会同时操作同一片寄存器。
2. **确认 DT 的 `reg` 指向 `子系统基址 + 0x1000`**，长度至少 `0x1000`。
3. **确认 CMA 池在 4 GiB 以下**（见 §6.2）。
4. 加载后先看 `dmesg`：probe 应打印 `registered th1520-vdec-dec as /dev/videoN`。
5. 跑 `v4l2-compliance -d /dev/videoN`（先不加 `-s`，确认静态 ABI 无误）。
6. 再用 GStreamer / `v4l2-request` 类用户态跑实际码流。
   建议顺序：**HEVC I 帧 → HEVC IPPP → H.264 baseline → H.264 CABAC/B 帧**。

出问题时最有价值的信息：`swreg1` 的中断状态位（驱动会把每个错误位打印成
`hw decode timeout` / `stream decode error` / `bus error` 等）。

---

## 6. 硬件验证记录

### 6.0 已确证（2026-08-06，LicheePi 4A / RevyOS 6.6.140-th1520）

| 项目 | 结果 |
| --- | --- |
| DT 节点 | `vdec@ffecc00000`，compatible `xuantie,th1520-vc8000d`，reg = `<0xff 0xecc00000 0x0 0x800000>`，interrupts `<131 4>`，clock-names `aclk/cclk/pclk`，有 power-domains |
| 子系统基址 + 解码核偏移 | **`0xffecc00000 + 0x1000` 正确** |
| **HW build id** | `swreg0 = 0x80018000`，product = **`0x8001`** = `IS_VC8000D()`。这条同时证明了 MMIO 映射、三个时钟、power domain 和 pm_runtime 都正常 |
| probe | `th1520-vdec ffecc00000.vdec: registered th1520-vdec-dec as /dev/video0`，无错误 |
| v4l2-compliance（静态） | 46 项 41 通过；5 个失败全部源于 v4l-utils 1.22.1 过旧（见下） |
| DECODER_CMD 语义 | `FLUSH → 0`，`START/STOP/PAUSE/RESUME/非法值 → EINVAL`，符合 stateless 语义 |
| GStreamer 探测 | `v4l2codecs` 插件自动注册 `v4l2slh264dec` / `v4l2slh265dec`，说明 stateless ABI 被上游客户端认可 |

**关于那 5 个 compliance 失败**：内核在 5.19 前后把 NV12 的 `fmtdesc.description`
从 `Y/CbCr 4:2:0` 改成 `Y/UV 4:2:0`，而该字符串由 **V4L2 core** 填写（驱动根本不写
`f->description`），v4l-utils 1.22.1 的期望表还是旧值。
**决定性验证：内核自带的参考驱动 `vivid` 在同一块板上报完全相同的错。**
我们这边额外连锁失败 3 个格式测试，是因为 CAPTURE 只有 NV12 一个格式，
ENUM_FMT 恰好在它身上中断导致格式集为空；`VIDIOC_(TRY_)DECODER_CMD` 失败
则是因为格式没枚举成功，工具无法判定这是 stateless 解码器，改用了 stateful 的期望。
换用较新的 v4l2-compliance 应可全部消除。

### 6.0.1 首次上硅调试日志（2026-08-06）

**已修复的真实 bug**

1. **重叠位域被同时写入 → 值被破坏。**
   swreg10 里 `INIT_QP`[30:24]（7 bit）与旧修订的 `INIT_QP_V0`[30:25]（6 bit）重叠，
   `NUM_TILE_COLS`[23:19] 与 `_V0`[24:20]、`NUM_TILE_ROWS`[18:14] 与 `_V0`[19:15] 同理。
   两套都写会把值整体左移一位：实测 QP 26 → 52，tile 数 1 → 3。
   analysis 记录的"`.so` 两套都写"依赖特定写入顺序，不能照搬。
   **修复**：只写新版位域（本板 product id = 0x8001，是新修订）。

2. **`LAST_BUFFER_E`(swreg3[8]) 必须显式置 1。**
   为 0 时硬件认为码流还有后续，一直等待，**既不完成也不中断** →
   驱动 2 秒软件看门狗超时。置 1 后硬件才会跑完并产生中断。
   上游 mainline 不写这一位（依赖复位值），但本驱动每帧清零影子寄存器，
   所以必须显式写。

**已排除的假设**（都做了实验，不是推理）

| 假设 | 排除依据 |
| --- | --- |
| 必须走 VCMD | `/proc/interrupts` 显示 IRQ 131 正常递增，中断直达本驱动 |
| Hantro MMU 未旁路 | probe 读 MMU 块：`hw_id=0x4d4d1200`，`control=0x00000000`，本来就是关的 |
| cache 一致性 | vb2-dma-contig 的 MMAP buffer 由 `dma_alloc_attrs()` 分配，在非一致性平台上即非缓存内存 |
| 寄存器写入未生效 | 超时/错误时回读全部寄存器，**shadow 与 hw 逐条相等** |
| "全量 flush 强制清零"破坏了复位值 | 加 `flush_all` 模块参数对比稀疏/全量两种模式，**结果完全相同** |
| swreg2 多余的 swap 域取值错误 | 把 `PIC/TAB0..3/RSCAN_SWAP` 从 0xf 改回 0（对齐 mainline），**结果完全相同** |
| WPP / SAO | 用 `x265 --no-wpp --no-sao` 重新编码，**结果完全相同** |

**当前卡点**

`swreg1 = 0x00010102` = `DEC_ERROR_INT`(bit16) + `DEC_IRQ`(bit8) + bit1
（bit1 在 H.264 表中是 `DEC_STRM_CORRUPTED`，两个 codec 共用同一物理寄存器）。
硬件会写出约 4 KB 输出后报错，即已经开始解码但很快判定码流有问题。

送进硬件的数据经确认无误：`stream len=2511 head=00 00 01 28 01 ...`，
3 字节 Annex-B 起始码 + NAL header `0x28` → `nal_unit_type = 20 = IDR_W_RADL`。
逐位手工解析该 slice header 也证实 `HDR_SKIP_LENGTH = 0` 是正确的。

**下一步建议**：从 `reference/vpu-vc8000d-kernel/`（GPL）编译带 printk 的
`hantrodec.ko`，用 ffmpeg 的 `hevc_omx` 解同一个文件，抓取**一次成功解码实际
写入的全部寄存器**，与本驱动的转储逐条 diff。这能把"逐个猜寄存器"变成直接比对。

**另一个已知的正确性问题（尚未触发，但必然要处理）**：G2 的原生输出是
4x4 tiled 格式（上游 mainline 对应 `V4L2_PIX_FMT_NV12_4L4`），要得到线性 NV12
需要启用后处理器或 raster-scan 输出通道。本驱动目前直接声称输出 NV12
而没有做这个转换，即便解码成功，输出布局也会是错的。



### 6.0.2 Golden 寄存器抓取：TH1520 走 VCMD（重大结论）

> **⚠️ 2026-08-13 更正（见 §6.0.3）：本节部分结论基于 G1/G2 寄存器表解码 golden，
> 已在 `analysis/vc8000d-product-table.md` 中被推翻。保留原文作比对记录，解读时以 §6.0.3 为准。**

用厂商 GPL 源码编了一个带 instrumentation 的 `hantrodec.ko`，用 ffmpeg 的
`hevc_omx` 解同一个测试文件，抓到了**一次成功解码实际使用的寄存器集合**。

先确认参考基准有效：**厂商硬解的输出与软解 golden 逐字节相同**
（`cmp /tmp/omx.nv12 golden-nowpp.yuv` 通过）。同时厂商驱动自己打印的
`core 0 HW ID=0x80018000` 和 `Supported HW found at 0xffecc01000`
与本驱动读到的完全一致，证明寄存器映射无误。

**结论 1：TH1520 的 VC8000D 运行在 VCMD 模式。**

```
hantrovcmd: HW at base <0xffecc00000> with ID <0x43421101>
vc8000_vcmd_driver: request IRQ <43> successfully for subsystem 0
Init: vcmd_registers_mem_pool.mmu_bus_address=0x401000
 43: ... SiFive PLIC 131 Level     vc8000_vcmd_driver
```

因此 `hantro_dec.c` 的 `DecFlushRegs()` 根本不会被调用 —— 寄存器由用户态写进
命令缓冲、VCMD 引擎 DMA 进各子模块。这直接回答了 §6.2 那个头号待验证项。

**但直接 MMIO 并未被硬件禁止**：本驱动独占该设备时，`DEC_E` 能拉起硬件、
IRQ 131 能正常送达本驱动、硬件也会写出部分输出。VCMD 是厂商的选择而非硬性要求。

**抓取方法**（可重复，工具已入库）

```sh
# 1. 厂商驱动自带 cmdbuf 反汇编，只要打开这个宏，无需改源码
ccflags-y += -DVCMD_DEBUG_INTERNAL
# 2. 用 OMX 解一个文件，dmesg 里就是整份命令缓冲
ffmpeg -c:v hevc_omx -i test.h265 -f rawvideo /tmp/out.nv12
dmesg | grep "current cmdbuf" > analysis/golden-cmdbuf-raw.txt
# 3. 解析成 swreg -> 值
python3 analysis/parse_golden_cmdbuf.py analysis/golden-cmdbuf-raw.txt
```

产物：`analysis/golden-cmdbuf-raw.txt`（原始转储）、
`analysis/golden-registers.txt`（解码核寄存器）、
`analysis/parse_golden_cmdbuf.py`、`analysis/instrument_hantro{dec,vcmd}.py`。

**结论 2：命令缓冲不止配置解码核。**

| WREG 偏移 | 子模块 | 寄存器数 |
| --- | --- | --- |
| `0x1008`（len 510） | **VC8000D** 解码核 swreg2..511 | 整片 |
| `0x2024` / `0x2204` | **L2CACHE** | 87 |
| `0x6800`–`0x7200` | **DEC400** | 69 |
| `0x3184` | **MMU**（reg 97 = `MMU_REG_FLUSH`） | 1 |
| `0x1000`（len 2，最后） | swreg0..1，拉 `DEC_E` 启动 | — |

**本驱动完全没有配置 L2CACHE 与 DEC400。**

**结论 3：golden 与本驱动的关键差异**

| swreg | golden | 本驱动 | 说明 |
| --- | --- | --- | --- |
| 2 | `00000400` | `f0f00000` | **swap 域全为 0**，不是 mainline 的 0xf；`0x400`=bit10，按 G1/legacy 布局是 `DEC_CLK_GATE_E` —— 暗示 VC8000D 对两个 codec 都用 legacy 的 swreg2 布局，而非 analysis 假定的 8 个 swap 域 |
| 3 | `6000104c` | `60f21000` | golden 置 **bit3**（L2 cache 通道，对应 `DWLEnableHw`）；`OUT_EC_BYPASS`=0，即**启用参考帧压缩** |
| 10 | `00021000` | `9a084000` | **`START_CODE_E`=0** —— 厂商在软件里剥掉起始码，直接指向 slice 数据 |
| 13 | `9a005402` | `00005402` | 高位 `0x9a00_0000` 本表未定义，待查 |
| 58 | `00004210` | `00010210` | `AXI_RD_ID_E`=1、`CLK_GATE_E`=**0**（本驱动为 1）|
| 190+/224+ | 一整批 | 未写 | 参考帧**压缩表**地址 |
| 265 | `81004000` | 未写 | **cache/shaper 主使能**（analysis §8 的 `swreg265 |= BIT(31)`）|
| 318/319 | `80500000` | 未写 | 超时覆盖，周期 `0x500000`=5242880，与 analysis 记录一致 |
| 320,322,326-332 | 一批 | 未写 | **后处理器启用** —— 印证 G2 原生输出不是线性 NV12 |
| 314 | `014000a0` | `01400140` | 色度 stride=160（非 320），与"原生输出需 PP 转换"一致 |

已单独验证：只把 swreg2 改成 golden 值仍报同样的错，说明需要成套对齐而非单点修改。

**下一步的明确方向**（按依赖顺序）

1. `START_CODE_E=0` + 软件剥离起始码，并据此设置 `STRM_START_BIT`。
2. 对齐 swreg2 / swreg3 / swreg58 / swreg13 的整字值。
3. 补上 swreg265、swreg318/319。
4. 决定是否启用后处理器（swreg320+）以获得线性 NV12，或改为导出
   `V4L2_PIX_FMT_NV12_4L4` 让上层转换。
5. L2CACHE 与 DEC400 的 87+69 个寄存器：先确认在旁路配置下能否不写。



### 6.0.3 产品表更正：TH1520 实际使用 0x4CBB80 表（2026-08-13）

用 IDA MCP 复核 `.so` 的 `SetDecRegister`（`lib/common/regdrv.c`）：运行时按
`HIWORD(container->regs[0])` 三路选表 —— `0x6731`→G1 表、`0x6732`→G2 表、
**`0x8001`→`0x4CBB80` 表**；`HevcDecInit` 把 `DWLReadAsicID()` 写入 `regs[0]`。
TH1520 实测 `swreg0=0x80018000`（product `0x8001`），所以**真实硅片上 H.264/HEVC
解码用的都是 `0x4CBB80` 这张全 codec 联合表**，G1/G2 表只对旧 product 有效。
早期分析把 `0x8001` 标成 “JPEG 表”是错的。完整证据与字段级 golden 解码见
`analysis/vc8000d-product-table.md`（新增）与 `analysis/vc8000d-register-config/swreg-map-vc8000d.md`
（新生成的产品表位域图，469 swreg / 1902 字段）。

对 §6.0.2 的逐条更正（原文保留如上）：

| §6.0.2 旧结论 | 更正（产品表实测） |
| --- | --- |
| golden `START_CODE_E=0`，厂商软件剥离起始码 | **`START_CODE_E=1`（swreg13[31]）**，码流带 3 字节起始码，硬件自行搜索。旧结论是把 golden 按 G2 表（swreg10[31]）解码造成的误读 |
| golden swreg10 `00021000` 的 `INIT_QP=0` | INIT_QP 在产品表中位于 **swreg13[30:24]，golden=26**（与 GStreamer 计算一致）；`0x21000` 是 `NUM_TILE_COLS_8K`[23:17] / `NUM_TILE_ROWS_8K`[16:12] 各为 1 |
| swreg13 高位 `0x9a00_0000` “未定义，待查” | = `START_CODE_E`[31] + `INIT_QP(26)`[30:24] |
| §6.0.1：`LAST_BUFFER_E`(swreg3[8]) 必须置 1 | 产品表 swreg3[8] = **`DEC_OUT_EC_BYPASS`**；真正的 `LAST_BUFFER_E` 是 swreg3[0]，golden 为 **0**。当年“置 1 后硬件才跑完”的真实机制是 bit8=1 关闭了参考帧压缩（否则压缩表基址=0 导致挂死）——该实验作为 EC_BYPASS 证据仍有效，但字段名/位号错了 |
| swreg58 `CLK_GATE_E`[16]=0 | 产品表 swreg58 无此字段；真实 `CLK_GATE_E` = **swreg2[10]**（golden=1） |
| “本驱动完全没有配置 L2CACHE 与 DEC400” | 仍成立：golden 含 L2CACHE 87 + DEC400 69 + MMU 1 寄存器（`golden-cmdbuf-raw.txt`），驱动未配置；在 EC_BYPASS=1 旁路模式下是否需要，待上板验证 |

**当前解码失败的直接根因**：驱动把 `START_CODE_E=1` 写进 swreg10[31]（真实硅片上该位
无此语义），真实 `START_CODE_E`（swreg13[31]）保持 0 —— 硬件按“无起始码”解析带
`00 00 01` 前缀的 Annex-B 缓冲 → `DEC_ERROR_INT + DEC_STRM_CORRUPTED`（4 KB 输出后报错）。

**下一步修复**（详见 `analysis/vc8000d-product-table.md` §5，按依赖顺序；
**2026-08-13 已全部实施**，见本文件 §6.0.4）：

1. `th1520_vdec_regs.h` HEVC 位域按产品表重定义：swreg3 低位区（EC_BYPASS[8]/APF_ONE_PID[7]/
   REF_READ_DIS[6]/L2_SHAPER_E[5]/BUFFER_EMPTY_INT_E[2]/BLOCK_BUFFER_MODE_E[1]/LAST_BUFFER_E[0]）、
   swreg10 8K tile 位域、swreg13 增加 START_CODE_E[31]+INIT_QP[30:24]、
   swreg58 删 CLK_GATE 两项、AXI ID 移到 swreg60。
2. `th1520_vdec_hw.c`：删 comp_table_swap；AXI_RD_ID_E=1；补 swreg318/319=0x80500000。
3. `th1520_vdec_hevc.c`：START_CODE_E=1、INIT_QP→swreg13、tile 计数→8K 位域、
   REF_READ_DIS（I 帧=1）；dump_list 增补 60/265/318/319/320+ 等。
4. H.264 后端同样整体重查：产品表把 H.264 地址 id 统一到 HEVC 风格位置
   （STREAM→168/169、OUT→65、REF→66+2i、DIFF_MV→173…），驱动 `TH1520_H264_ADDR_*`
   全部失效；START_CODE_E/INIT_QP 也在 swreg13；STREAM_LEN 为 32bit。

### 6.0.4 产品表修复实施记录（2026-08-13）

按 §6.0.3 的清单修改了驱动（`th1520_vdec_regs.h` 的 H.264/HEVC 两段全部
按产品表重定义并保留逐字段依据注释；`th1520_vdec_hw.c` 的 common config
对齐 golden；`th1520_vdec_hevc.c` / `th1520_vdec_h264.c` 后端相应迁移）：

| 项 | 修改前 | 修改后 |
| --- | --- | --- |
| swreg2（两 codec） | H.264 写一票 G1 位；HEVC 写 raw 0x400 | 都写 raw `0x00000400`（swap=0 + CLK_GATE_E） |
| START_CODE_E | swreg10[31]（HEVC）/ swreg6[31]（H.264） | **swreg13[31]**，值 1（Annex-B 缓冲） |
| INIT_QP | swreg10[30:24] / swreg6[25] | **swreg13[30:24]**，7 bit |
| OUT_EC_BYPASS | swreg3[17]（写错位） | **swreg3[8]**，保持 1（旁路，见下） |
| LAST_BUFFER_E / BUFFER_EMPTY_INT_E | swreg3[8]/[10]（错位） | **swreg3[0]/[2]**，golden 值 0 / 1 |
| tile 计数 | 旧位域 [23:19]/[18:14] | **8K 位域 [23:17]/[16:12]** |
| CLK_GATE_E（HEVC） | swreg58[16]（垃圾位） | swreg2[10]（含于 raw 0x400） |
| 超时看门狗 | 不写 | swreg318/319 = `0x80500000`（golden） |
| AXI_RD_ID_E | 0 | 1（golden） |
| AXI ID 寄存器 | swreg59 | **swreg60** |
| H.264 地址寄存器 | G1 位置（12/122、13/123、14+i…） | 统一位置（168/169、64/65、66+2i、132/133、174/175） |
| H.264 STREAM_LEN | 24 bit + 长度检查 | **32 bit**（产品表 id 161） |
| H.264 swreg2 旧位 | timeout_e/swap32/endian/latency/max_burst… | 全部删除（产品表不存在） |
| REF_READ_DIS | 无 | swreg3[6] = **0**（旁路路径；golden 压缩路径=1，待硬件验证） |
| swreg265 | 无 | 不写（旁路路径；golden 压缩路径=0x81004000，记录为差异） |
| dump_list | 到 314 | 增补 60/265/318/319/320/322/326/328/329/331/332/394 |

**遗留的已知差异（不影响本次流解析修复，上板后逐步处理）**：

1. **压缩路径**：golden 是 OUT_EC_BYPASS=0 + L2CACHE(87 寄存器) + DEC400(69) +
   MMU flush + swreg3[3] + swreg265；驱动是旁路=1。旁路能否端到端工作待实测。
2. **输出格式**：解码核原生输出非线形 NV12（golden C stride=160 vs Y=320），
   线性 NV12 需要后处理器（swreg320-332 + 394）或导出 `V4L2_PIX_FMT_NV12_4L4`。
3. **REF_READ_DIS 的置位规则**：golden（压缩路径）为 1；写者不在 regcalls.json
   捕获集内。旁路路径暂取 0，P 帧行为待硬件验证。

**上板重测顺序**：HEVC I 帧 → 看 `swreg1` 状态与 `swreg260` 错误定位 →
H.264 baseline → CABAC/B 帧。若有错，用扩表后的 dump 与 golden 逐字段比对。





以下每一项在代码里都有对应注释。注意 §6.0.2 已经用 golden 抓取回答了
其中的 VCMD 一项，并把 swap 域一项从"推测"变成了"实测为 0"。

### 6.2 是否必须走 VCMD

`reference/.../subsys.c` 的 `core_array[]` 里含 `HW_VCMD` 条目，
`CheckSubsysCoreArray()` 会因此把 `vcmd = 1`，也就是说**厂商这份源码的构建
是走命令缓冲（VCMD）路径的**。但同时：

- 真实 `.so` 的 `DWLEnableHw` 走的是 `HANTRODEC_IOCS_DEC_PUSH_REG`（非 VCMD 路径）；
- VCMD 只是一个 DMA 命令队列前端，最终写的仍是同一批 swreg。

本驱动选择直接 MMIO + IRQ（与上游 mainline hantro 相同）。
**若目标板上中断不来或寄存器写入无效，第一个要怀疑的就是 IRQ 是否被路由到
VCMD 块、以及解码核是否必须由 VCMD 拉起。**

### 6.2 地址位宽 / DMA 掩码

驱动当前设 `DMA_BIT_MASK(32)`。依据是 `analysis/.../README.md` §6.4 记录的
二进制断言：**非 High10 的 H.264 模式下 `regs[122]`（码流基址 MSB）必须为 0**。
驱动仍然显式把 MSB 寄存器写 0，而不是不写。

HEVC 的地址寄存器是规整的 (MSB, LSB) 对，理论上支持 64 bit。
若确认目标硬件的 AXI 地址位宽和 MSB 寄存器行为，可以把掩码放宽，
`th1520_vdec_write_addr()` 无需改动。

### 6.3 G2 的 swap 域取值

> **2026-08-13 已废弃**：产品表更正后 swreg2 只保留四组 swap 域
> （STRM/PIC/DIRMV/TAB），厂商栈全部写 0，驱动直接写整字 `0x00000400`
> （= CLK_GATE_E[10]）。旧内容保留如下：

~~`th1520_vdec_hw.c` 的 `TH1520_G2_SWAP_LE = 0xf` 应用于 swreg2 的全部 8 个 swap 域。~~

~~- `DEC_STRM_SWAP` / `DEC_DIRMV_SWAP` / `DEC_COMP_TABLE_SWAP` 取 `0xf`
  与上游 mainline 一致（位置也逐位吻合）—— 已验证。
- `DEC_PIC_SWAP` / `DEC_TAB0..3_SWAP` / `DEC_RSCAN_SWAP` 在上游是不同修订的
  位宽与位置，上游 HEVC 路径根本不写它们；本驱动按同一小端约定取 `0xf`
  —— **参考性推测**。~~

~~若目标板上出现色度平面或 direct-MV 数据字节序错乱，把该常量改成 `0` 重测。~~

### 6.4 H.264 CABAC 表的排布

`th1520_vdec_h264_cabac.c` 的数值是 H.264 规范常量，与 SoC 无关；
但**表在 DMA buffer 中的排布顺序**是 Hantro/Rockchip G1 的约定，
`analysis/` 中没有直接证据证明 VC8000D 用完全相同的排布。
CABAC 码流解码异常时先查这里。

### 6.5 `DEC_MODE` 的位宽

二进制里 `.so` 同时写 4 bit 的 `DEC_MODE_G1V6[31:28]` 和 5 bit 的
`DEC_MODE[31:27]`，以兼容不同 HW 修订。本驱动只写 5 bit 版本。
H.264 baseline/main（值 0）两者结果相同，因此当前配置不受影响；
将来支持 High10（值 15）时必须先确认目标硅采用哪一套语义。

### 6.6 硬件超时看门狗

**2026-08-13 更新**：产品表更正后确认两个 codec 的超时覆盖都位于
swreg318/319（HEVC 的 G2 表位置 swreg44/45 在产品表中不存在），
且 golden 抓取给出了厂商实测值 `0x80500000`（OVERRIDE_E=1 +
周期 5242880）。本驱动现在对两个 codec 都写该值（`TH1520_TIMEOUT_OVERRIDE`），
外加 2 秒软件看门狗兜底。之前“不写覆盖寄存器”的理由（没有厂商周期值可依）
已不成立。

### 6.7 尺寸上限

`PIC_MB_WIDTH` 只有 9 bit（最大 511 MB = 8176 像素），
`PIC_MB_HEIGHT_P` 8 bit 加 `swreg7[25]` 一位扩展。
驱动当前把上限定在 4096x2304，放开更大尺寸前需要实测扩展位的行为。

### 6.8 其他

- `swreg265` 与 L2 cache / shaper 寄存器块（`swreg8200+` / `swreg8320+`）
  在 TH1520 上是否实现、是否必须初始化 —— 本驱动完全不碰这两块。
- HEVC tile 中断：驱动关闭 `DEC_TILE_INT_E`，一次提交整帧只期待一个
  `DEC_RDY_INT`。若带 tile 的码流解不出来，需要按 `analysis` §7.5
  实现逐 tile 的中断循环。
- 交错（field）解码路径按上游 hantro 的做法实现，但 V4L2 层目前只导出
  `V4L2_FIELD_NONE`，场解码尚未端到端验证。

---

## 7. 与参考资料的关系

- **寄存器语义**：`analysis/vc8000d-register-config/`（源自真实二进制，四重验证）。
- **平台资源**：`reference/vpu-vc8000d-kernel/`（厂商内核侧资料）。
- **驱动结构与 V4L2 ABI**：上游 Linux commit `8ba098e6b6ff0db8edf28528d1552be261af30d4`
  的 `drivers/media/platform/verisilicon/`（GPL-2.0）与
  `Documentation/userspace-api/media/v4l/dev-stateless-decoder.rst`。
  引用处均在文件头保留了 SPDX 与版权信息。
- **未使用** `reference/omx_il_g-master/` 的任何实现代码。该目录不是 VC8000D
  的真实源码；本驱动只在 `analysis` 已完成三重验证的前提下间接使用了
  它提供的寄存器**名称**（数值一律取自二进制）。
