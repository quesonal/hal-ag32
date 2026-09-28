# FLASH 布局（agrv2k_407，默认 on-die A/B）

> **这份文档是什么**:整颗芯片的地址地图 —— loader、A/B 镜像、boot record、
> 比特流（fabric）区、盐扇区、option 区、ROM bootloader —— 以及每条区域**谁写、谁读、
> 谁保证它不被踩**。以前这些散在 loader overlay 的注释、Kconfig help、[README.md](README.md) 的
> DFU 表和各章注释里，地址对不上时没人能一眼看出来；现在以这里为准。
>
> 相关文档:[`BOOT-DFU-STATUS.md`](BOOT-DFU-STATUS.md)（启动/DFU 现状与待办）、
> [`FLASH.md`](FLASH.md)（两条烧录通路）、[`FLASH-AND-CAPTURE.md`](FLASH-AND-CAPTURE.md)
> （开发板规则、RDP/撕裂演练）、[`SIGNED-IMAGES-PLAN.md`](SIGNED-IMAGES-PLAN.md) §12（盐/绑定设计）。

## 0. 一页速查（默认布局，片内 1 MiB @ `0x80000000`）

| 地址 | 大小 | 内容 | 谁写 | 谁读 |
|---|---|---|---|---|
| `0x80000000` | 96 KiB | **loader**（二级 bootloader） | `west flash` / AN3155 / mcumgr / ROM | 芯片自己（入口就是它） |
| `0x80018000` | 200 KiB | **side A**（A/B 镜像槽） | loader（上传/安装） | loader（拷进执行槽） |
| `0x8004a000` | 200 KiB | **side B** | 同上 | 同上 |
| `0x8007c000` | 200 KiB | **执行槽**（on-die application slot，XIP 链接地址） | loader（`install-slot`）/ AN3155 直写 | CPU 从这里跑 |
| `0x800ae000` | 8 KiB | **应用 boot record**（append log，2 份拷贝） | loader（`boot_agm.c`） | loader（每次启动） |
| `0x800b0000` | 4 KiB | **bind-salt 扇区**（`"AGMB"`） | `tools/agm_bind.py provision`（SWD） | loader（算绑定 key） |
| `0x800b1000` | 12 KiB | *空闲*（`app-size` 想长大就从这里拿） | — | — |
| `0x800b4000` | 100 KiB | **fabric update slot 1** | loader 的比特流上传 | `fcb.c`（启动时按 record 流） |
| `0x800cd000` | 100 KiB | **fabric update slot 2** | 同 slot 1 | 同 slot 1 |
| `0x800e6000` | 4 KiB | **fabric A/B record** | loader（提交比特流后最后一步） | `fcb.c`（**每个镜像**，含没有 boot 驱动的） |
| `0x800e7000` | 100 KiB | **factory fabric**，option byte 指的就是这里 | 只有外部工具（`west flash --bitstream-only`、`agm_oo.sh bitstream`、ROM） | ROM（上电）+ `fcb.c`（回落） |

**位置全部推导，不写死**：应用链从 0 往上排
`loader | A | B | slot | record | salt`，只吃三个尺寸（`loader-size`、`app-size`、
`record-size`）；比特流区从 factory 锚点往下紧排 `factory | record | slot 2 | slot 1`，
只吃两个配置项（`AGM_BITSTREAM_FACTORY_ADDR`、`AGM_BITSTREAM_SLOT_SIZE`）加一个擦除扇区。
两链在中间相遇，`boot_agm.c` 的 `BUILD_ASSERT` 负责"撞上就构建失败"。

合计 **正好 1024 KiB**，其中 12 KiB（`0x800b1000..0x800b4000`）是唯一空闲：
`app-size` 从 200 KiB 提到 204 KiB 正好把它吃满（4 KiB × 3 个区域）。
要加新区域，就得从某个现有区域里让出来，而不是"找块空地塞进去"。

> **布局收紧（破坏性，不做迁移）**：原先比特流两槽之间留了 16 KiB 的间隙
> （`0x800c9000..0x800cd000`）给盐用，那是两个手挑地址撞出来的，不是设计；现在
> 比特流区紧排到 `slot1 = 0x800b4000`，盐改为应用链的最后一节、与 boot record 连续成
> 12 KiB 元数据块（`0x800ae000..0x800b1000`）。地址变化：`slot1` `0x800b0000` →
> `0x800b4000`，执行槽 `0x8007e000` → `0x8007c000`，盐 `0x800c9000` → `0x800b0000`；
> sign 过的**比特流容器**（戳是 `slot1 + 32`）与**应用容器**（戳是执行槽 + 32）都要重签，
> 盐要按新地址重新 provision。
>
> **真机复验（`agrv2k_407`，canonical 200 MHz）**：loader `info` 打印
> **`store A 0x018000` / `store B 0x04a000` / `load 0x8007c000` / `on-die slot at
> 0x8007c000`**（应用链推导）；通过 loader 上传一份比特流后，fabric record 的两份拷贝都写着
> **`slot = 0x800b4000`**，loader 报 *"fabric came from an update slot (0x800b4000)"*
> （比特流侧紧排）；盐在新地址 `0x800b0000` provision 后，又经过一次完整的 `west flash`
> （固件 + canonical 比特流），回读仍是 `"AGMB"` v1、指纹 `bcfe1706` —— "活过一次 reflash"
> 这条在新地址上重新量过了。收尾：清掉 fabric record 扇区（`0x800e6000`）、刷回
> `hello_world` + canonical 比特流（banner 100%、`FCB STAT=0x000f0002` ACTIVE、RDP off /
> `osc 0xff,0x57` / 指针 `0x800e7000`）。

> **别的板不是这张表**：256 KB 片内的 `agrv2k_103` / `_303` / `_test` 把比特流区钉在
> flash 末尾减 100 KiB（`0x80027000`，见各板 `board.cmake` 的 `AGRV_BITSTREAM_ADDR`），
> 而本文这一族地址 —— 两个比特流 update 槽 `0x800b0000`/`0x800cd000`、record `0x800e6000`、
> factory `0x800e7000` —— 是 `agm_bitstream.h` 里的 **SoC 常量，隐含"片内 ≥ 1 MiB"**，
> 也就是 407 这一级。A/B + 盐那套只在 1 MiB 的片上成立；loader 样例的
> `platform_allow` 也只有 `agrv2k_407`。换板先看那张板级表，别照抄地址。

## 1. 两条布局家族

布局由 devicetree 的 `boot` 节点（`dts/bindings/misc/agm,agrv2k-boot.yaml`）描述，
驱动 `drivers/misc/boot_agm.c` 从两个尺寸（`loader-size` / `app-size`）推所有偏移。
仓库里有两种：

| 家族 | overlay | store（A/B 镜像） | 执行槽 | 比特流 staging |
|---|---|---|---|---|
| **on-die A/B（默认）** | `samples/spi_boot_loader/boards/agrv2k_407.overlay` | 片内 | 片内 `0x8007e000` | 片内（SoC 固定槽） |
| **two-flash（外挂 NOR）** | 同目录 `agrv2k_407_ext_nor.overlay` | 板载 SPI NOR（2 MiB） | 片内 `0x80030000` | 片内（SoC 固定槽，见下） |

默认家族的地址就是 §0 那张表。它把 A/B、执行槽、比特流槽全部塞进片内 1 MiB，
好处是**不需要 SPI NOR**：没有外挂 flash 的板子能用，比特流没有把 SPI0 接回引擎也不再是
失败模式。

**两条家族共有的、由 SoC 而不是布局决定的部分**（`include/zephyr/drivers/misc/agm_bitstream.h`）：
两个比特流 update 槽 `0x800b0000` / `0x800cd000`、比特流 record `0x800e6000`、
factory 槽 `0x800e7000`。它们必须固定，因为 record 要**在任何驱动跑起来之前**就能被找到
（`soc/agm/agrv2k/fcb.c` 在 `PRE_KERNEL_1` 读它，每个镜像都跑这一段，包括没有 boot 驱动的）。
所以 two-flash 家族里"比特流 staging 在 NOR"的说法是不成立的 —— **比特流 A/B 永远在片内**，
外挂 NOR 只是放应用镜像的 store。

## 2. two-flash 家族（外挂 SPI NOR）

板载 SPI NOR 是 2 MiB JEDEC（RDID `68 40 15`，`samples/spi_nor_flash` 的接法），
地址空间独立：

| NOR 偏移 | 大小 | 内容 |
|---|---|---|
| `0x000000` | 8 KiB | 应用 boot record（2 个扇区，append log 的两份拷贝） |
| `0x002000` | 508 KiB | store A |
| `0x081000` | 508 KiB | store B |

片内侧:loader `0x80000000` + 192 KiB → 执行槽 `0x80030000`（512 KiB）→
`0x800b0000` 起仍是 §1 那组 SoC 固定的比特流槽/record/factory。

## 3. 两个 "record" 不是一回事

名字都叫 record，用处完全不同，改布局时最容易踩混：

| | **应用 boot record** | **fabric A/B record** |
|---|---|---|
| 地址（默认） | `0x80018000`，8 KiB | `0x800e6000`，4 KiB |
| 内容 | 模式、活动槽、每槽 `{len, CRC32, state, attempts}`、反回滚下限 | 哪个比特流槽、长度、CRC、以及签名容器信息 |
| 结构 | append log，`BOOT_CFG_RECORDS`(2) 份拷贝：copy 0 = 提交，copy 1 = 备胎 | `AGM_BITSTREAM_RECORD_STRIDE`(32) 字节一条，两条 |
| 谁读 | loader（决定拷哪个镜像、跑哪个） | `fcb.c`，**每个镜像的启动路径** |
| 读不到时 | 没有可启动镜像 | **回落到 factory 槽** `0x800e7000` |

## 4. 比特流区（fabric）

* **factory 槽 `0x800e7000`**（100 KiB）：ROM 上电时按 option byte 里的 `FPGA CONFIG`
  指针从这里流比特流；它**只有外部工具会写**（`west flash --bitstream-only`、
  `tools/agm_oo.sh bitstream`、ROM 通路），设备侧永远不写。压缩比特流时 option 指向的是
  里面的 config（`AGM_BITSTREAM_CONFIG_ADDR = factory + ALGO_SIZE`，
  例:`0x800e8100`），而不是 base。
* **update 槽 1/2 `0x800b0000` / `0x800cd000`**（各 100 KiB）：我们的 A/B。
  原因见 `agm_bitstream.h` 开头:CPU 的 `sys_clk` 由当前 fabric 产生，
  **不能在原地重写运行中的那份**（实测 写到 15.9 KB 整板掉线，
  下次启动把半截镜像流进 FPGA）。所以先写到"不在跑的那一槽"并校验，
  提交 record 的最后一步才切换；提交前任何复位都还留在原地跑的那份上。
* 槽里放宽的是 **99944 B**（24986 words，vendor SDK `FCB_AUTO_WORDS`）；
  100 KiB 是扇区取整后的槽大小。
* `CONFIG_BOOT_AGM_BITSTREAM_SIGNED=y` 时槽里放 MCUboot 容器，
  真比特流从 `槽基址 + AGM_BITSTREAM_HDR_SIZE`(32) 开始流；校验在**提交前**和
  **启动前**各做一次，验不过就不流，板子留在 factory fabric 上。

## 5. 盐扇区（per-chip binding salt）

* **位置**：默认（on-die）布局里它是应用链的**最后一节** —— `0x800b0000`，紧接应用 boot
  record（两者合计就是那 12 KiB 元数据块），地址由 `loader-size`/`app-size`/`record-size`
  推导（`boot_agm_priv.h::BOOT_SALT_OFF`），没有 Kconfig 地址。两片（外挂 NOR）家族另有一个
  `CONFIG_BOOT_AGM_BIND_SALT_OFFSET`（默认同为 `0x800b0000`）。
* **格式**：`"AGMB" + version + salt[16] + CRC-32`，其余 `0xff`；16 B 的盐就是全部秘密。
* **为什么在片内、而且要在这个间隙**：它必须**比固件活得久** —— `west flash`
  会擦写 loader 区和 boot record，挂在那一带的盐会随之作废，所有已绑定的镜像全部失效。
  实测:写好的盐在 `west flash --skip-bitstream` 和"固件 + canonical 比特流"
  的完整 `west flash` 之后都原样还在（厂商 flash 驱动的 auto-erase 只吃它写到的扇区）。
* **只写一次**：`tools/agm_bind.py provision`（设备在环:SWD 读回 → 写 → 逐字节复验）。
  扇区里已有一份**不同**的盐时默认拒绝，覆盖要显式 `--force`。
* **它不是机密**:只有在 RDP 打开时才是（[SIGNED-IMAGES-PLAN.md](SIGNED-IMAGES-PLAN.md) §12、§R2c）。
  未锁芯片上读到它 = 拿到一个"多了一道手续的 UID"。

## 6. option 区与 ROM bootloader

这两样都**不在**上面那 1 MiB 里，但布局里的几个指针由它们决定：

| 东西 | 地址 | 说明 |
|---|---|---|
| option RAM（ROM 视角） | `0x81000000`，128 B | ROM 协议读写整块 option 区（`../tools/rom_opt.py`） |
| option 寄存器（AP 通路） | 控制器 `0x40001000`，option 寄存器 `0x8100_0020+` | 只有 `oo`/SDK 那条路能碰（`../tools/agm_oo.sh`） |
| 其中 `FPGA CONFIG` | option 里的一个字段 | 现在指向 `0x800e7000`（未压缩）或 config 偏移（压缩） |
| ROM bootloader | 在 **ROM**，不在 flash | `BOOT0` 高 + `BOOT1` 低 + 上电/真 nRESET 进入；UART 协议 AN3155，`agrv32flash` 用的就是它 |

option 区被擦（撕裂态/`agrv32flash -O`）时 `FPGA CONFIG` 失效，板子起不来 ——
恢复阶梯见 [`FLASH-AND-CAPTURE.md`](FLASH-AND-CAPTURE.md) §11.9。

## 7. 谁保证这些地址不重叠

`drivers/misc/boot_agm.c` 开头把每条关系写成 `BUILD_ASSERT`，所以布局写错是**构建失败**
而不是运行时静默踩内存（真的踩过一次:store B 的尾巴和 slot 的首扇区重合，
上传 B 把正在跑的镜像擦了）。被检查的关系:

* 每个区域长度是 record 扇区的整数倍（擦除按扇区步进，否则会erase 出界）；
* 应用 boot record 不压 store A；store A/B 依次排开不重叠；
* 执行槽不伸进比特流 reservation（`slot-address + slot-size <= bitstream-address`）；
* reservation 不超出片内 flash（DT 声明了 `reg` 时）；
* 两个比特流槽不重叠、slot 2 不压 record、record 不压 factory 槽；
* A/B 布局不伸进比特流 slot 1；
* **盐扇区必须落在两个比特流槽之间且扇区对齐**（`CONFIG_BOOT_AGM_BIND`）。

## 8. 本次整理动过的地址

| 位置 | 原状 | 问题 | 现在 |
|---|---|---|---|
| `samples/flash_internal/boards/agrv2k_407.overlay` | scratch = `0x800e6000` | 正好是**比特流 A/B record 扇区**；注释说"没人用的最后一块" | 挪到 `0x800a0000`（见下） |
| `samples/fcb_hotswap/Kconfig` | slot B 默认 `0x800c0000` | 99944 B 从那里写到 `0x800d8668`，**压过盐扇区和 slot 2** | 默认改 `0x800cd000`（真正的比特流 slot 2） |
| `samples/fcb_hotswap`（overlay + main.c + Kconfig） | 两个槽地址是 Kconfig hex | 地址漂了没人拦；`0x800c0000` 会写到别人区域 | 改成 DT 分区 `hotswap_slot_a/b` + `DT_REG_ADDR()` + 两条 `BUILD_ASSERT`（钉在 `AGM_BITSTREAM_FACTORY_ADDR`/`SLOT2_ADDR`），Kconfig 与样例自带 Kconfig root 删除 |
| `samples/spi_boot_loader/boards/agrv2k_407.overlay` | "比特流 stage 104 KiB / 116 KiB 空闲" | 漏了 slot 2、盐扇区、比特流 record；空闲算错一个数量级 | 换成 §0 的完整表 |
| `samples/spi_boot_loader/boards/agrv2k_407_ext_nor.overlay` | "比特流 staging 在 SPI NOR" | 与 SoC 固定槽矛盾 | 改为"比特流 A/B 永远在片内" |
| [README.md](README.md) / `../README.zh-CN.md` | DFU 表只给 two-flash 家族的地址，读起来像唯一布局 | 默认布局其实全在片内 | 加指向本文档 + 标明是哪条家族 |

**`flash_internal` 的 scratch 为什么是 `0x800a0000`**:那个 self-test 只开 flash 驱动，
不开 boot 驱动，所以片内**执行槽那段**（`0x8007e000..0x800b0000`）在它眼里没有用途；
而 `0x800e6000` 是**每个镜像**都会读的比特流 record 扇区（`fcb.c` 在 `soc.c` 里跑），
把它当 scratch 擦掉，会让一次已提交的比特流 A/B 选择悄悄回落 factory —— 这正是 self-test
不该做的事。新地址在 `0x800b0000` 以下，离比特流区/record/盐都远，且仍是 4 KiB 对齐。

## 9. 参考来源（改动时对着这几处一起看）

* 布局参数:`dts/bindings/misc/agm,agrv2k-boot.yaml` + 两个 loader overlay；
* 推导与断言:`drivers/misc/boot_agm.c`（文件头 + `BUILD_ASSERT` 段）；
* SoC 固定地址:`include/zephyr/drivers/misc/agm_bitstream.h`；
* 盐:`drivers/misc/Kconfig.agm`（`BOOT_AGM_BIND_SALT_OFFSET`）、`../tools/agm_bind.py`；
* 片内 flash 硬件节点:`../dts/riscv/agm/agrv2k.dtsi`（`flash@80000000`，1 MiB，4 KiB 扇区）；
* 外挂 NOR:`samples/spi_boot_loader/boards/agrv2k_407_ext_nor.overlay`（2 MiB，`68 40 15`）。
