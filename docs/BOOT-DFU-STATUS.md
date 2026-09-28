# 启动 / DFU 入口(单页)

本文件记录启动/DFU 的当前状态、待办与相关文档(§6)。实现过程在
`开发记录（未随本仓库发布）` 起;设计取舍在 [SIGNED-IMAGES-PLAN.md](SIGNED-IMAGES-PLAN.md) 与
[SIGNED-IMAGES-PLAN.md](SIGNED-IMAGES-PLAN.md)(其 **§11** 是生产锁 R2 的设计 —— 原 `R2-PRODUCTION-LOCK-PLAN.md`
在 2026-09-24 并了进去),文中注明仍未落地的部分。

**口径**:写"实测"的是板子上跑出来、有串口日志/寄存器读数/抓包支撑的结论;写"推断/未验证"
的是从代码、手册或设计意图推出来的判断。全篇不把推断写成实测。

## 0. 当前状态(2026-09-20)

**能用的部分都在板上跑通了**:两级启动(loader + payload)、A/B 双槽 + TRIAL/确认/回退、
三条上传路径(console / AN3155 / mcumgr)、比特流上传与**比特流 A/B**、签名镜像
(**ECDSA-P256 与 RSA-2048-PSS 两档**)、**比特流槽也签名**(启动路径先验后流,验不过退回 factory)、
试启动看门狗 + 应用侧确认(没人按复位也能回退),以及 **R2 生产锁**:锁定档里 publish / erase /
开一次上传会话各要一条签名命令,出厂形状由预设 `CONFIG_BOOT_AGM_PRODUCTION_PROFILE` 一次拉齐。

**当前的布局决定(2026-09-18 拍板并落地)**:默认**全在片内、不依赖外部 NOR**,应用大小是配置
项 `app-size`(**默认 200 KiB**)、loader 96 KiB;两 flash(NOR)变体保留为
`boards/agrv2k_407_ext_nor.overlay`。

**规模(干净树 `7842355` + 当天随后的文档/样例提交)**:native
`west twister -T modules/hal_ag32/tests -p native_sim` → **10 配置 / 131 例 / 0 失败**
(8 skip);`pytest tools/tests` → **106 passed / 4 skipped**;samples `-p agrv2k_407`
当前选中 **60 场景**(2026-09-24;`--build-only` 下 7 个静态过滤、**53 个构建**),
并行 twister 偶发的抢写单独构建均通过(那一两次报错的用例单跑即过,见
`开发记录（未随本仓库发布）` 末尾)。

**签名的开机代价已经量出来了(2026-09-24,§5.5)**:应用档 ECDSA **+101 ms/次启动**,
比特流**槽**签名 **+3.13 s/次启动**(验签跑在切 PLL 之前,CPU 还在 10 MHz 的 HSI 上),
两档全开时 reset → 应用第一行 **1.72 s → 5.01 s**。威胁面/残余风险的完整对账在 §5。

### 0.1 当前流水线一览(2026-09-24 核对)

> 这一节是**现状的单一入口**。括号里指的是历史记录(日期、命令、输出);两者若有出入,
> 以本节和它指的实现为准。端到端可复现的验收是 `../tools/verify_flow.py`(**12 步全绿**,
> §3.26.38 + §3.26.39)。

**① 片上有什么(静止形态,默认布局,全在片内 flash)**

| 区域 | 大小 | 内容 |
|---|---|---|
| `0x000000` | 96 KiB | loader(`samples/spi_boot_loader`,XIP 运行) |
| `0x018000` | 8 KiB | 启动记录:双扇区 append log,当前 **v4**(`magic/version/seq/mode/active/slot[2]/sec_ver/crc`) |
| `0x01a000` / `0x04c000` | 各 200 KiB | store A / B(更新目标 = 非活动的那一侧) |
| `0x07e000` | 200 KiB | 应用执行槽(容器在这里 XIP 跑) |
| `0x0b0000` | 216 KiB | 两个比特流更新槽(各 100 KiB);上传先落**非活动**那一槽。**2026-09-24 起紧排**: slot 1 = `0x800b4000`(原 `0x800b0000`,间隙已去掉),slot 2 = `0x800cd000` 不变 —— 见 [`FLASH-LAYOUT.md`](FLASH-LAYOUT.md) |
| `0x0c9000` | 4 KiB | **bind-salt**:每芯片绑定的盐(`BOOT_AGM_BIND`;**机密性靠 RDP**,而 RDP 是产线的显式动作 —— 工具与演练都有了,板子默认没锁) |
| `0x0e6000` | 4 KiB | 比特流记录(两副本,`BSR1`,`seq/slot/len/crc`)——跟在槽区后面 |
| `0x0e7000` | 100 KiB | fabric 预留(factory 比特流;**设备永不写**) |

**② 每次启动(冷启动或任何复位)**

1. ROM 按 option byte 的指针先把 **factory** config 流进 fabric(CPU 的时钟要它;
   `0x800e7000`,当前 `non-compressed`/`non-encrypted`;option byte 只有 AP/ROM 通路能改)。
2. `soc.c`(PRE_KERNEL_1)**再流一遍** —— 这一步才是比特流 A/B 的落点:先开 AHB 门控
   (CRC 单元要有电)→ `agrv2k_fcb_program()`:
   * 记录若指向比特流**更新槽** → 先 `agm_boot_bitstream_verify()`(记录 CRC 预检 + 容器
     **SHA-256 / KEYHASH / ECDSA-P256**);**过了才流**,不过就退回 factory 并把原因报到
     控制台(`agm_fcb_bitstream_refused`)——比特流槽坏掉永远不会把板子弄成砖;
   * 再 `agrv2k_clk_switch_pll()`。**注意这一步在切 PLL 之前**(CPU 还在 10 MHz HSI),
     所以签名比特流槽的验签代价是 **~3.13 s/次启动**(§5.5;移出这一窗口的方案见 §4 第 5 条)。
3. 启动驱动读记录(双扇区取最新 + `crc`)、打 `fabric came from …`。
4. A/B 策略:活动槽、TRIAL 尝试计数(`BOOT_MAX_ATTEMPTS=3`)、试启动看门狗(IWDG,backup 域)。
5. 选中的**应用容器再验一次**(每次启动都验,防"发布后被改写";带 `BOOT_AGM_BIND` 时这一步
   也核**这颗芯片的绑定标签**)→ 需要则拷进应用槽 → 跳转。
6. 应用回写 trial ticket(`agm_boot_trial_confirm()`,只写 backup 寄存器)→ 下次该槽
   CONFIRMED;不写/卡死 → 看门狗复位,3 次后 BAD → 回退另一侧。

**③ 更新(DFU)**

| 阶段 | 谁在验什么 | 失败语义 |
|---|---|---|
| 开上传会话 | R2b:一条**签名命令**(ERASE) | 会话不开、flash 一字未动(NAK 6 / `EACCESSDENIED`) |
| 传数据 | 无(纯写) | —— |
| publish | 容器 **SHA-256 + KEYHASH + 签名**,再 **绑定标签**(`BIND`,若开),然后 **floor**(记录 v4 的 `sec_ver`) | 被拒镜像:控制台/AN3155 NAK 4,mcumgr `EACCESSDENIED`;旧版本:NAK 5 → `EBADSTATE`;不是这颗芯片的(或没绑定/没写盐):**NAK 7** → `EACCESSDENIED`。**记录一个字节不写**(但被写的那一侧字节已被覆盖,见 §3.26.38 ②) |
| 启动 | 见 ②5(再验一次) | 不拷不跳、停在控制台;TRIAL 超限回退 |
| confirm | 应用自己(ticket) | 3 次起不来 → BAD → 回退 |

**锁定档只有 SMP 一条带内通路**:`tools/smp_cli.py <port> upload <img> <file> --authorize <key.pem>`
(它按顺序花掉 ERASE/PUBLISH 两条 grant);console 与 `agrv32flash` 走不完这两条 grant(§3.26.38 ①)。

**第四条上传通道:USB MSC(UF2 式,样例)**:`hal_ag32_samples/samples/usb_msc_dfu_boot`
+ `bin_to_uf2.py` 与 `gen_uf2_volume.py`(两个都在伴生仓库的 `tools/`)。
按 UF2 bootloader 的做法(RP2040 BOOTSEL / Adafruit nRF52):设备
**伪造一个 64 MiB FAT16 卷** —— 卷背后没有存储,`gen_uf2_volume.py` 只生成 boot
sector 与 FAT 首扇区,其余扇区读回零 —— host 把 `.uf2` 拷到盘上;设备把每个 512 B 写当 UF2
块解析,payload **直接**流进记录**不指向**的那一侧(`agm_boot_upload_begin/write`);**末块**
到达就**关掉 MSC 接口**(host 看到盘符消失),再 `upload_finish` 发布并重启,由 loader 的
A/B 策略接管。镜像**不整份进 SRAM**、也不落文件系统/暂存区(这就是假 FAT 的意义:写进来的
就是传输本身)。它是**应用(payload)**,不是 loader 里的 server —— loader 的 96 KiB 区装不下
USB 设备栈;host 工具只依赖 python3,不需要 SDK/探针。

**实测(2026-09-27,`agrv2k_407` + canonical factory 比特流)**:Linux 直接认盘(设备报
`131071/512`,即 64 MiB),`spi_boot_app.uf2` 拷进去后整条链(发布进 store B → 关 MSC → 重启
→ 新镜像自证)一次走通。**Windows 需要先修上游一处**:`usbd_msc.c::msc_handle_bulk_in()`
在 host 请求长度大于命令返回长度(Hi > Di)时会 STALL bulk-IN,而 Windows 的
`READ FORMAT CAPACITIES` 要 252 B、命令固定只回 12 B(容量表就这么大)→ **没有 CSW** → 枚举
停住;同一条命令 Linux 只要 12 B(residue 0),所以 Linux 不受影响。去掉那一处
`msc_stall_bulk_in_ep()`(短包 + 带 residue 的 CSW 本就能收尾,见 BOT 6.7.2 case 5)之后,
Windows 即认出 64 MiB 的 `AGM-DFU` 卷,`spi_boot_app.uf2` → store B(22604 B,crc
`0x331d4ce3`)→ 盘符消失 → 重启进 `spi_boot_app`。补丁属**上游 Zephyr**(本模块不 fork 上游
`.c`,所以它不在本仓库里);样例 README 有同样的说明。

别和 [`hal_ag32_samples/samples/usb_msc_dfu`](https://github.com/quesonal/hal-ag32-samples/blob/main/samples/usb_msc_dfu/README.md) 混淆:那是 Zephyr
**USB DFU 类**的组合设备(gadget),与 loader 的 DFU 不是一回事。

**④ 机制的开关与默认值**

| Kconfig | 默认 | 作用 |
|---|---|---|
| `BOOT_AGM_SIG_NONE / _ECDSA_P256 / _RSA2048_PSS`(choice) | `NONE`;`PRODUCTION_PROFILE` 下为 `ECDSA_P256` | 应用容器的验签档 |
| `BOOT_AGM_ANTI_ROLLBACK` | 签名档 `y` | 安装时拒旧版(记录 `sec_ver`) |
| `BOOT_AGM_BITSTREAM_SIGNED` | `n`(由 profile `select`) | 比特流槽也走容器验签 |
| `BOOT_AGM_LOCK_PRODUCTION` | `n`(由 profile `select`) | R2b 门控(会话/publish/erase) |
| `BOOT_AGM_SMP` | `y` | mcumgr 服务端(锁定档唯一通路) |
| `BOOT_AGM_TRIAL_WATCHDOG` | `y`(需 `iwdg0` 节点) | 试启动看门狗 |
| `BOOT_AGM_BIND` | `n`(且需签名档) | 每芯片绑定:盐 + `BIND` 标签,publish 与启动两处强制;**要先去产线写盐**(`tools/agm_bind.py provision`) |
| `BOOT_AGM_PRODUCTION_PROFILE` | `n` | 一次拉齐"出厂形状":lock + SMP + 比特流签名 + ECDSA |

**⑤ 当前"没有做"的(别读成有)**

* **没有加密**:比特流明文、固件明文;`RDP`/读保护**默认不开**(仪器状态),开了之后外部读全零 —— 但那是产线动作,不是这里的默认;厂商的 per-chip
  比特流加密被明确不做(算法已读出,§7.12;它买的是绑定不是保密,且与设备侧更新互斥,§7.4/§7.8)。
* **固件加密未实现**。**每芯片绑定已实现**(2026-09-24):`KDF(UID, salt)` + `BIND` 标签在
  publish 与启动两处强制拒收(§12、§3.26.39),盐由 `tools/agm_bind.py provision` 写、
  设备把指纹打在 loader 的 `info` 里。**绑定的强度等于"锁没锁"**:盐的机密性完全依赖 RDP ——
  RDP 本身**已实现并演练**(`tools/agm_oo.sh lock/unlock`,§3.26.40),但它是产线的显式动作,
  一块没锁的板子上,能读 flash 的人就能读到盐(§12.1 的表)。
  另外 `BOOT_AGM_BIND` 不随 production profile 打开:没写盐的板子开了绑定就升不了级,
  写盐该是产线的显式动作。
* 没有 loader 自验签、记录/floor 无认证、factory 比特流不验签;也不依赖外部安全元件——这些的
  后果与优先级在 §5.4。

**⑥ 成本(每次启动)**

| 项 | 代价 | 出处 |
|---|---|---|
| fabric 槽 CRC(硬件 CRC,99944 B) | ~3.5 ms | §3.29 |
| FCB 流比特流本身 | ~9 ms | §3.8.3 |
| 应用容器验签(ECDSA P-256 @200 MHz) | ~+101 ms | §5.5 |
| 比特流槽验签(HSI,切 PLL 之前) | **~+3.13 s** | §5.5 |
| (可选)把比特流验签挪到切 PLL 之后 | 估算 ~0.2 s,未实现 | §4 第 5 条 |

## 1. 已完成(不用再讨论)

| 事项 | 证据 / 指针 |
|---|---|
| 两级启动 + 外部镜像 → 片内槽 XIP | peripherals §3.26.11 |
| A/B 双槽、TRIAL / 尝试额度 / CONFIRMED / BAD / 自动回退 | §3.26.13(真机串口实录) |
| 三条 host 上传路径(console / `agrv32flash` / `smpmgr`) | §3.26.15 / §3.26.17 |
| 比特流上传(staging + BSB1 + CRC + 片内拷贝 + 回读校验) | §3.26.14 |
| 比特流 A/B(更新只写非活动槽,factory 区永不写) | §3.26.31(设计与 native)/ §3.26.34(真机) |
| 比特流槽签名(容器进槽 + 发布/启动两处验签 + 退回 factory) | §3.26.34(真机:上传→启动→篡改被拒→回退→重传恢复) |
| 记录双扇区 append log(抗撕裂);当前记录 **v4**,v2/v3 旧格式按 invalid 处理 | §3.26.24 / §3.26.36 |
| 签名镜像 CF-3(MCUboot header + TLV,公钥编进 loader),`NONE`/`ECDSA-P256`/`RSA-2048-PSS` 三档 | SIGNED-IMAGES-PLAN §2.2–§2.6 / §6 实测表;§3.26.32(RSA 档体积 + 真机序列) |
| 默认布局 = 全片内 A/B + `app-size`(200 KiB)/`loader-size`(96 KiB),构建期断言兜底 | §3.26.30(推导表 + 实测) |
| 防回滚:**安装时**拒旧版(NAK code 5 / SMP `EBADSTATE`),`erase` 抹不掉下限 | §3.26.36(真机:v2 装上升 floor → v1 拒 → `erase` 后仍拒) |
| 生产锁 R2a + R2b:publish/erase/开上传会话各要一条签名命令;AN3155 在产线档编掉;预设 `PRODUCTION_PROFILE` 一次拉齐并强制有公钥 | [`SIGNED-IMAGES-PLAN.md`](SIGNED-IMAGES-PLAN.md) §11(设计 + 接口契约)、§3.26.37(开发板四条验收) |
| 空记录上的 `image erase` 是**幂等 no-op**(一个字节都不写);记录损坏/读不到才报错 | §3.26.23(实现 + native)/ §3.26.37(开发板) |
| 被拒镜像有专用 NAK code(控制台 4 / mcumgr `EACCESSDENIED`) | §3.26.33 |
| AN3155 读命令不再能倒出比特流(按会话写入水位放行) | §3.26.35(native + 真机) |
| loader 装载区上限、容器链接地址校验、试启动看门狗 | §3.26.27 / §3.26.28 / §3.26.29 |
| 控制台上传相 + SMP `0x40` 组 + AN3155 都有 native 覆盖(含"开机即坏记录"场景) | §3.26.25 / §3.26.33 / `tests/drivers/misc/boot_agm*` |
| 驱动审查 10 批全部收口 | `code_review/boot-agm-driver/status.md`(gitignore,不入库) |
| **端到端验证流程**(`samples/verify_flow` + `../tools/verify_flow.py`):12 步一次跑完并逐步断言 —— 每芯片绑定的 provisioning、签名应用、签名比特流槽、防回滚、R2b 授权,加六条失败路径(未绑定/另一颗芯片/篡改/旧版/未授权/伪造槽) | §3.26.38 + §3.26.39(2026-09-24 实测 **12/12 PASS**,~7 min) |

## 2. 待决定(第 1 条待定;第 2 条 2026-09-24 已结案,留档)

1. **启动失败的语义**:照 MCUboot(未确认 → 下次任何复位都回退,含拔电)还是保持现在的
   "3 次额度"?两者都能用,区别只在"试验期被断电"时谁更友好。
2. ~~**简化范围**:是否砍掉 `mode`(auto/internal/external)+ RTC `once` + 老的 `external`
   (把镜像拷到 SRAM 再跑)那条路?~~ **2026-09-24 已按前半条执行**:`mode external`(连
   payload 的 RAM 变体)与 `install`/`install-bitstream`(编译期把镜像/bitstream嵌进 loader)
   都删掉了 —— 前者的用途只剩"给同一份记录维护两套链接地址",后者被三条 host 上传路径完全
   覆盖。`mode auto|internal` 与 RTC `once` 保留(它们是"上传前回到控制台"的操作手段)。
   记录兼容:老记录里的 `mode == 2`(`external`)现在读作 `auto`,不认识的模式值同样回落,
   所以升级后不会因为一条旧记录停在控制台上。

> 原先的"容量与形态"和"签名档位"两条已拍板并落地(§3.26.30 / §3.26.32),不再讨论。

## 3. 明确暂缓 / 不做

| 事项 | 结论 | 重估条件 |
|---|---|---|
| **整体迁到 MCUboot** | **不做**(论证见 SIGNED-IMAGES-PLAN §3.2):没有 RISC-V 端口;swap 同样是"slot0 + slot1 + scratch";会带走 AN3155 / 比特流 / 控制台 | ①需要 image encryption 或 multi-image;②"双等尺寸可执行槽"变成架构本身;③上游有人做了 RV32 端口 |
| **E1(片内 A/B 即执行槽)** | **大部分被 §3.26.30 取代**:默认布局已经是片内 A/B + 一份链接产物 + 200 KiB 边;E1 只剩"单槽要 > ~350 KiB"这一种用途 | `app-size` 提到 200 KiB 还不够 |
| **E2(启动时重定位)** | 不做:RISC-V 这棵树没有 PIC,自研重定位表 + 生成工具成本高、失效模式新 | — |
| **比特流 runtime 热重载** | 已明确不做(§3.8.1:CPU 时钟由 fabric 产生) | §3.8.2 的 4 条前提 |
| **加密比特流** | 默认不做:**2026-09-23 密钥流已完整读出并落成工具**(两轮 Salsa20 变体;每芯片输入 = **片内 flash 的 128-bit 唯一 ID**(16 B)经 3 个 XOR 组合,维护者本地开发记录 + [`../tools/agm_logic_crypt.py`](../tools/agm_logic_crypt.py)),所以"设备侧无解"不再成立 —— 剩下的拦路虎是它买不到安全性(维护者本地开发记录:两轮 Salsa20、逐颗变化的部分只有 `id[2]`、读它只要两把公开常量),以及"设备该不该自己重封/怎么接进 A/B"的取舍(维护者本地开发记录);另外,一块已经烧了加密 factory 的板子,今天这版 loader 仍起不来(它照旧去重流那份密文) | 需要"比特流也加密"时:先按维护者本地开发记录的四条改 `fcb.c`(按来源分流 + trust 开关),算法与 host 工具已有(见维护者本地开发记录) |
| **R2c:RDP / 写保护** | **两条通路 + 撕裂态恢复都演练过**(2026-09-24):SWD 侧 `tools/agm_oo.sh lock/unlock`,ROM 侧 `agrv32flash -j/-k`(BOOT0),两条落到同一设备状态;"已擦未写回"的撕裂态(板子起不来)由 `tools/agm_rdp_tear_test.sh --rom-erase-tear` **确定性**造出(`agrv32flash -O`,判据是 canonical→erased 的转移),恢复阶梯 `unlock` → `bitstream` → `west flash` → `provision` 一条命令走通并逐字节核对([FLASH-AND-CAPTURE.md](FLASH-AND-CAPTURE.md) §11 / §11.9、§3.26.40.1)。**注意**:用复位/打断去"复现"这个态不可靠 —— 探针 nRESET 15 刀 0 命中,且该签名本身不是撕裂证据(§11.9)。**默认不开锁**,它是产线动作 | 需要真边界时:一条 `agm_oo.sh lock` |
| **默认布局把执行槽放大到 844 KiB** | 与 §3.26.30 的默认冲突:执行槽吃掉 844 KiB 后片内没有第二块放持久副本 | 目标 app > 200 KiB 且愿意回到两 flash 布局 |

## 4. 建议的下一步

1. **量一下真实 app 的体积**(默认只有 200 KiB/边):真超了要么调大 `app-size`
   (片内到 ~350 KiB 还有余量),要么动 E1。先量再决定。
2. §2 那条待决定(启动失败语义)可以顺手做。
3. ~~厂商 bootloader 资料里只剩一条开放项:**加密比特流与远程比特流升级互斥**~~ —— **2026-09-23
   已收口**:密钥流从厂商工具里完整读出并逐字节验证(`开发记录（未随本仓库发布）`
   §7.12),"互斥"的准确边界是"密文得由上位机产生 + `encrypted` 位只有 AP 通路能写",而设备侧
   自己重封现在只是一段可移植的代码(定位是兼容性,不是安全边界)。另两条早已收口(压缩比特流做成
   可选项;`agrv32flash -l` 实测不成立,见 FLASH-AND-CAPTURE §10.0.1)。
4. ~~**签名开机的真实代价没有量过**~~ —— **已做(2026-09-24,§5.5)**:应用档 +101 ms、
   比特流槽 +3.13 s(全在切 PLL 之前付)。新工具 [`../tools/boot_timing.py`](../tools/boot_timing.py)
   就是这套计时回路;顺带修掉了 `agrv2k-minimal.cfg` 里 `mmw` 未定义导致 `reset run`
   中途报错的老 bug。
5. **把比特流验签挪到 PLL 之后(设计选项,未实现)** —— 上一条的 3.13 s 来自"在 10 MHz 的
   HSI 上做 SHA-256 + 验签"。同一条 boot 路径已经证明 fabric 可以**运行时**重流
   (§3.8.3,`fcb_hotswap` 实测窗口 ~9 ms),所以顺序可以是:先流 factory 比特流(factory 区是
   "从不被写"的信任来源)→ 切 PLL → 在 200 MHz 上验比特流槽 → 验过再流一次并重切。
   按已测分量估算 ≈ 9 ms + ~0.15 s + 9 ms,**3.13 s → ~0.2 s 量级**(推断,未实现;
   代价是启动早期多跑一份 factory 比特流,以及 `fcb.c` 的"按来源分流"要落地)。
6. ~~**R2c:RDP/写保护 + 恢复演练**~~ —— **SWD 侧 2026-09-24 做完**(§3.26.40,操作面在
   [FLASH-AND-CAPTURE.md](FLASH-AND-CAPTURE.md) §11;§5.4 第 1、2、8 条指着的那道物理边界现在手边有了)。
   **ROM 侧也补完了**(同日,插上 BOOT0 跳线):`agrv32flash -j` 加锁(ROM 自报
   `Flash : Protected`)、`-k` 解锁并整片擦,与 SWD 侧落到同一设备状态;
   **"撕裂态"也演练了**(§3.26.40.1 / [FLASH-AND-CAPTURE.md](FLASH-AND-CAPTURE.md) §11.9):板子起不来但 SWD
   能救,恢复阶梯一条命令走通。**同日复核改了做法**:不再用复位/打断去"复现"(15 刀 0 命中,
   而"已擦未写回"这个签名用无复位的 `agrv32flash -O` 就能造),改用确定性 `--rom-erase-tear`。
   **剩的一格**:真的在窗口里手拔供电(手动瞄不进 3 ms 窗口,所以仍未测)。
7. **`confirm` 时作废对面槽** —— §5.4 第 3 条里性价比最高的一条:小改动 + native 用例,
   把"回不去"从"长期存在"收窄到"试验态"。
8. ~~**R2c + 每芯片绑定(加盐)**~~ —— **绑定那一半 2026-09-24 做完**(设计记录 §12,
   真机记录 §3.26.39):主机工具 `../tools/agm_bind.py`(HKDF + HMAC + `embed`/`check`/
   `provision`)、设备侧 `agm_boot_unique_id()`/`agm_boot_bind_key()`/`agm_boot_bind_tag()`
   与 `BIND` TLV 的两处强制(`AGM_BOOT_E_UNBOUND`,NAK 7),native 8 例 + `verify_flow`
   12 步全绿。**RDP(R2c)的 SWD 侧也已做完**(2026-09-24,§3.26.40):新增
   `tools/agm_oo.sh lock`,并演练了"加锁不擦镜像 / 外部读全零 / 解锁整片擦 / 恢复后重烧
   重绑"整条路;只剩 ROM loader 侧那条兜底通路没在板上走过(§12.5)。
   在**没锁的板子上**,绑定买到的仍然只是"防顺手复制"——盐的机密性等于 RDP 的强度。
9. **`k_busy_wait` / tick 的时间基准** —— 现状与三档实测在
   `开发记录（未随本仓库发布）`:常数是**编译期**
   的板级 `AGM_SYSCLK_HZ`,不是运行期频率(两者错配时墙上时间 2×,而芯片自己打印的数字
   全都"正常")。三条方向的结论(对比表在 §3.31.1):

   * ③ **构建期闸门已做(2026-09-24)**:新工具 `../tools/check_bitstream_clock.py` 把要烧的
     比特流的 SYSCLK(自己的 `.ve`,没有就从生成的 Verilog 的 PLL 参数算)与这次 build 的
     `CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC`(外加 dts 的 HSE/BUSCLK)比对,`west flash`
     在写比特流前调用,**不一致直接拒烧**;`AGM_SKIP_CLOCK_CHECK=1` 是逃生开关。
   * ① **接上游运行期机制:做不了(不做)**:tick 的数学缓存在 timer 驱动里
     (`timer_core_cyc_per_tick`),换频率要驱动自己调 `timer_core_rescale()`,而
     `riscv_machine_timer.c` 在上游树(本仓库对 zephyr 零改动)。真要做就是给上游提 patch。
     触发条件:shipped 路径出现"内核启动后换时钟"(DVFS,或 §4 第 5 条那比特流验签搬迁)。
   * ② **SDK 式 `arch_busy_wait()`:已做**(2026-09-24,`soc/agm/agrv2k/busy_wait.c`,
     `SOC_AGM_AGRV2K` select `ARCH_HAS_CUSTOM_BUSY_WAIT`):`k_busy_wait` 改成数 64 位的
     `mcycle`,每次调用从 `SYS.CLK_CNTL` 现读当前时钟源(HSI/HSE/PLL)。实测 200 MHz 下
     21 574 836 µs 的请求从"只等 100 ms"变成"等满 21 575 ms",而 100 µs…100 ms 的固定
     超出量与通用路径同级(+100 cycle);代价是与上游 `k_busy_wait` 行为分叉
     (上限本来是跨平台固有、无承诺),HSI/HSE 两支没有板上实测(没有 shipped 路径在那儿
     调用它)。细节与那处 `__udivdi3` 的教训见 §3.31.2。

10. **可选:把"每次 `k_busy_wait(1)`"的轮询改成自旋读状态寄存器(非必需,待触发条件)** ——
    `k_busy_wait` 本体的下限已量清(约 300 cycle ≈ 1.5 µs,两条实现同级,见 §3.31.2),
    再压只能改调用点:目前树里只有 `drivers/spi/spi_agm.c::spi_agm_wait_done()` 是"每次
    SPI 事务一轮"(USB/ETH 那两处是上电一次性的复位轮询,不值)。改成直接读状态寄存器 +
    用 `k_cycle_get_32()` 周期差判超时,粒度从 ~1.5 µs 降到几十 ns;**收益完全取决于负载**
    (NOR 页读里 ~1%,4 B 寄存器读里可能 ~35%),当前 SPI 的主要用户是 `jedec,spi-nor`
    的页读/整片扫描,没有大量小事务。**代价**:动的是敏感热路径(要配改前/改后同一份
    板上基准)、纯自旋比"每轮睡 1.5 µs"更费电、`SPI_AGM_DONE_TIMEOUT_US` 的语义从
    "迭代数 ≈150 ms"变成真微秒(100 ms,行为变更)、约半天工作量。完整分析(含三处调用点
    对照与触发条件)在 `开发记录（未随本仓库发布）`
    §3.31.2.1。

## 5. 威胁面对账与残余风险(2026-09-24)

> 这一节把"谁防住了谁"收成一张表,并列出**还没关的门**以及关它们的成本。
> 实测数字来自 2026-09-24 那轮签名开机测量(§5.5);实现过程与真机记录在维护者
> 开发记录（未随本仓库发布）。

### 5.1 信任链

```
boot ROM(厂商固件,不验签)
  └─ loader(片内 flash,XIP 运行,**无签名**)     ← 信任根 = 写保护;RDP 工具已有,默认不上锁
       ├─ boot_cfg 记录 + sec_ver 下限          ← 未签名、未认证
       ├─ 应用容器(MCUboot header + TLV)         ← 发布时 + 每次启动前各验一次
       └─ 比特流槽容器(BSB1)                       ← 同上;factory 区例外,从不验

DFU 通道(console / AN3155 / mcumgr)           ← 不可信输入;R2b 只给"状态变更"加签名命令
```

**loader 自己没有任何完整性保护**,它的可信度等于"那片 flash 没被改过"。所以上面这张图在今天
的所有权边界是:**物理接触 = 全权**(没上锁时);RDP 是把它变成"物理接触 = 全片擦除"的唯一动作,
而它现在是**一条命令**(`tools/agm_oo.sh lock`)+ 一次演练过流程,不再是"未做"。

### 5.2 四种性质是正交的,混在一起谈最容易出错觉

| 性质 | 谁给 | 现状 |
|---|---|---|
| 完整性(没坏) | CRC-32 | ✅ 全程都有,但谁都能算,挡不住对手 |
| 真实性(谁写的) | ECDSA P-256 / RSA-2048-PSS 容器签名 | ✅ 应用 + 比特流槽都验,发布时与每次启动前各一次 |
| 机密性(读不走) | `lock_flash` / RDP | ⚠️ **工具与演练都有**(R2c SWD 侧 2026-09-24:`tools/agm_oo.sh lock`,加锁后外部读整片全 `0x00`),**但默认不开** —— 锁上是产线动作;unlock 会全片擦,这是它的代价 |
| 每芯片绑定(防克隆) | `KDF(UID, salt)` + 容器 `BIND` 标签 | ✅ **已强制**(发布 + 启动,NAK 7 / `-1002`),,**前提是盐不被读走** —— 也就是 R2c;厂商那套按 UID 的比特流加密同样已有算法与工具但未接入(`开发记录（未随本仓库发布）`) |

### 5.3 按攻击者**有什么**对账

| 攻击者能力 | 能做什么 | 现状 | 缺口 |
|---|---|---|---|
| 只有 DFU 通道(总线访问) | 传自己的镜像 | ✅ 发布要签名,失败 NAK 4 / `EACCESSDENIED`,记录一字不写 | — |
| 同上 | 开上传会话写坏回退槽 | ✅ `upload_begin` 要 ERASE 授权(评审 RF-004) | — |
| 同上 | `rollback` / `confirm` / `mode` | ❌ 不签、不看 floor | §5.4 第 3 条 |
| 物理写 flash | 改 loader / 记录 / floor / factory 比特流 | ❌ 无防线(没上锁时) | 上锁后要走"解锁 = 全片擦";但 loader 本身仍无签名,改完照跑 |
| 物理读 flash | 拿走 loader 里的公钥、比特流、固件、**盐** | ⚠️ 上锁后外部读全 `0x00`(实测) | 不上锁就照读;这也是"绑定强度 = RDP 强度"的由来 |
| 手上一份旧的合法签名镜像 | 重装旧版 | ✅ 安装时被 floor 拦(NAK 5,§3.26.36) | 已在槽里的旧副本仍可 `rollback` 回去 |
| 克隆硬件 | 同一份固件装第二块板 | ⚠️ 签名天生可移植,但开了 `BOOT_AGM_BIND` 就只认"绑到本芯片"的那一份(2026-09-24 实测) | **没上锁**的板子上,读一次 flash 就能把盐一起抄走;上锁后这条路断掉(`agm_oo.sh lock`),但要接受"解锁 = 全擦";更强的是每台在线签 |
| 故障注入 / 侧信道 | 绕过验签、读密钥 | ⚠️ 未评估 | 与"验签→跳转"的 TOCTOU 同类 |

### 5.4 残余风险(按优先级)

1. **"板子没上锁"= 上面所有 ❌ 的总开关。** loader 无签名、记录无认证、factory 比特流不验、
   SWD 可读可写,全都挂在"能不能写/读那片 flash"上。开启成本现在很小:一条
   `tools/agm_oo.sh lock`(§3.26.40 已演练),代价是解锁一定全片擦;剩下的缺口是
   "无探针时怎么解锁/上锁"(ROM 侧 `agrv32flash -j/-k/-L`,未演练)与写 option 的掉电窗口。
2. **记录与 floor 没有认证。** `boot_cfg` 只有自校验 CRC,能写 flash 的对手可以改 active、
   把 BAD 改回 good、把 `sec_ver` 抹回 0。文档早已承认("能改记录的对手也能抹 floor"),
   要强调的是:**防回滚的存储只有这一处,所以 R1 的强度实际上 = R2c 的强度。**
3. **"回不去"这一面有三条路,不止 `rollback` 一条**(代码位置见 `drivers/misc/boot_agm.c`):
   `agm_boot_rollback()` 只翻 `active` + 标 TRIAL,不查 floor;`agm_boot_confirm()` 只把当前槽
   标 CONFIRMED,**不作废另一槽**;`payload_install_slot()` 从 store 拷进片内槽时不重验签名、
   也不查 floor。安全网真正需要旧镜像的只有"新镜像还是 TRIAL"那一段 —— 一旦 CONFIRMED,
   留着旧副本就只剩风险。**收获**:`confirm` 时作废对面槽(或"CONFIRMED 状态下才按 floor 拦"),
   是本节里性价比最高的一条改动。
4. **nonce 唯一性不是密码学保证,授权是设备级的。** R2b 的 nonce 来自记录 seq +
   `k_cycle_get_32` + 会话计数(设计上只要求唯一),残余窗口是"抢先消费一次已授权命令"
   (需要与合法主机同总线)。后果有限(DoS,或替他执行一次已授权命令 —— 内容仍要签名)。
   想收紧就把"未消费的 nonce 只保留最近一个"写成显式规则。注意:**芯片里没有 TRNG**
   (SIGNED-IMAGES-PLAN §2.7),想提到密码学强度没有便宜的硬件路。
5. **参数签了但不绑定**(一次被授权的 `erase` 可以擦任意目标)。R2b 契约里已声明不做;
   真要收紧应连着第 4 条一起改契约。
6. **验签与跳转之间的 TOCTOU。** 验签读 flash、跳转执行的还是同一片 flash(XIP 片内槽)。
   利用它要同时有物理写通路 + 精确时序,归入故障注入一类;未评估。
7. **DoS 面。** `rollback` / `confirm` / `mode` 都不要授权,对手能把板子切到另一槽或卡在
   console(可恢复,但产线档要不要留这条余地是 §6 的决定点)。
8. **绑定的强度等于"板子锁没锁"。** 绑定本身已经强制(§3.26.39),但盐是**明文存在片内
   flash 里**的:没上锁时把整片读出来(或读那一个扇区)就能把盐一起复制到第二块板,绑定随之
   失效;上锁后外部读全 `0x00`,这条路断掉(`agm_oo.sh lock`,§3.26.40 实测),代价是
   解锁一定全片擦。所以:**默认没锁的板子上,别把它当防克隆的安全边界读**,它是"防顺手复制"。
   真正强的替代是每台在线签(盐不落地),代价见 §12.1。

### 5.5 签名买到的与付掉的(2026-09-24 实测)

界面:一块 `agrv2k_407`,canonical 200 MHz 比特流(md5 `6378549f…`),workspace 树
`modules/hal_ag32` @ `22839d1`(干净树),Zephyr `v4.4.0-15143-g403a7d79adbc`;
三个 `samples/spi_boot_loader` 构建(NONE / ECDSA 应用档 / ECDSA 应用档 + 比特流签名)+
`samples/spi_boot_app` 的 internal_signed 负载;密钥一次性
(`imgtool keygen -t ecdsa-p256`),容器与上传走 `../tools/sign_image.py` +
`../tools/agm_upload.py`;计时用 **`../tools/boot_timing.py`**(openocd 常驻 + telnet `reset run`,
读数取 4–5 次的中位数)。

| 构建 | reset → 第一条控制台输出 | reset → 应用第一行 | 扣掉 1.5 s 控制台窗口后的"策略 + 校验" |
|---|---|---|---|
| A 无签名(裸镜像,底层 CRC) | **+124 ms** | +1719 ms | **+35 ms** |
| B ECDSA 应用档(比特流走 factory) | +124 ms | +1840 ms | **+136 ms** |
| C ECDSA 应用档 + 比特流签名(**槽**) | **+3274 ms** | +5011 ms | +136 ms |
| C(同一个二进制,擦掉比特流记录 → factory) | +144 ms | +1861 ms | +156 ms |

读法:

* **应用档签名 ≈ +101 ms/次启动**(B−A 的"策略 + 校验"列)。
* **比特流槽签名 ≈ +3.13 s/次启动**(C 与"同二进制回 factory"那行之差),而且它是**在第一条
  控制台输出之前**付掉的 —— 也就是开机黑屏 3 秒。
* 合计:reset → 应用第一行从 **1.72 s(A)变成 5.01 s(C)**;只开应用档是 1.84 s(B)。

**为什么这么贵(推断,依据是账算得平)**:比特流验签跑在 `agrv2k_fcb_program()` 里,即
`agrv2k_clk_switch_pll()` **之前** —— CPU 还在 HSI(内部振荡器)上。同一份工作(99944 B 的
SHA-256 + 一次 P-256 验签)在 200 MHz 上按应用档的数字外推约 0.15 s,实测 3.13 s,
比值 ≈ 20×,与 `board.hx` 的 `BOARD_HSI_FREQUENCY = 10000000`(10 MHz)一致。
**推论**:这一档的代价主要由"验签发生在切 PLL 之前"决定,换签名算法救不了它 ——
在 10 MHz 上 SHA-256 扫 100 KB 本身就要 ~1 s;要降到 ms 级,得让验证发生在 PLL 起来之后
(§4 第 5 条给了那条路)。

## 6. 上板前必读

* [`FLASH-AND-CAPTURE.md`](FLASH-AND-CAPTURE.md) —— 烧录/抓串口的硬规则、故障对照表、
  §9 调试纪律、§10 的 ROM bootloader(BOOT0)恢复路径。上板前按它执行。
* [`FLASH-LAYOUT.md`](FLASH-LAYOUT.md) —— **地址地图**:loader / A/B / 执行槽 /
  比特流槽 / 两个 boot record / 盐扇区 / option 区的确切地址与大小、两条布局家族
  (on-die 默认、外挂 NOR)、每块谁写谁读,以及 `boot_agm.c` 里那些防止踩踏的 build 断言。
* **BOOT0 陷阱**(§3.26.37 末尾):BOOT0 还接在 3.3V 时 loader 根本不跑 —— 串口 0 字节、
  `FCB STAT=0x00000001`、`reg pc` 停在 ROM;拔掉跳线重新上电即恢复。上一轮 ROM 恢复之后
  最容易忘的就是这一步。

## 7. 相关文档

实现过程与真机实测记录(哪块板、哪条命令、什么输出)在`开发记录（未随本仓库发布）`里;下面是随仓库发布的文档。

| 文档 | 内容 |
|---|---|
| [`FLASH-AND-CAPTURE.md`](FLASH-AND-CAPTURE.md) | 烧录与抓串口规则、故障对照表 |
| [`FLASH.md`](FLASH.md) | runner 与擦除范围的背景,附 Plan A("默认双写")的实现记录 |
| [`SIGNED-IMAGES-PLAN.md`](SIGNED-IMAGES-PLAN.md) | 签名镜像的设计与取舍(算法 / 格式 / 密钥 / 验证时机) |
| [`SIGNED-IMAGES-PLAN.md`](SIGNED-IMAGES-PLAN.md) §11 | 生产锁(R2)设计:门控集合、签名命令的线格式、生产档规则;R2a/R2b 已落地,R2c 见 §12.5 |
| [`SIGNED-IMAGES-PLAN.md`](SIGNED-IMAGES-PLAN.md) §12 | 每芯片绑定(加盐)与 R2c 施工记录:KDF/标签定义、盐区与 provisioning、两处强制、`BIND` TLV;12.5 是 RDP(**SWD 侧已做并演练,ROM 侧待补**) |
| `code_review/boot-agm-driver/`、`code_review/r2-production-lock/` | 评审记录(gitignore,不入库) |

查询顺序:现状看本文件,实现细节看 `peripherals.md` §3.26,设计取舍看对应 PLAN,
上板操作看 [FLASH-AND-CAPTURE.md](FLASH-AND-CAPTURE.md)。
