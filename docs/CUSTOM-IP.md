# 自定义 IP（fabric 里的用户逻辑）接入指南

> **这份文档要解决什么**:把"这片 fabric 里我自己写的逻辑"当成一等公民接进 Zephyr ——
> 寄存器怎么访问、引脚怎么接、电气属性怎么配、中断怎么回 MCU、RTL 怎么进构建。
> 现状:前三块**已经能用**(下面 §1 每条都有真机或构建证据),
> §2 是**还没做**的部分(其中第 5 条是"多个 IP"),§3 是建议的落地顺序。

## 1. 今天就能用的

### 1.1 寄存器窗口（AHB slave）

* 节点 `agm,agrv2k-cpld`(`dts/bindings/misc/agm,agrv2k-cpld.yaml`),驱动
  `drivers/misc/cpld_agm.c`。SoC 的地址译码把 **0x60000000–0x7FFFFFFF** 的 load/store
  送到 fabric 的 `mem_ahb_*` slave 口,写落到窗口内 `haddr[23:0]` 偏移。
* API:`include/zephyr/drivers/misc/cpld_agm.h`(`agm_cpld_read32` / `write32` /
  `update_bits` / `get_base` / `get_size`),驱动按 `reg` **逐个校验偏移**,越界返回
  错误而不是打到别人的解码上。
* 例子:`samples/cpld_reg`(+ `CONFIG_CPLD_AGM_SHELL` 给 shell 手敲)。
* **红线**:fabric 里必须有 `mem_ahb` slave 并应答握手 —— 不应答会**把 CPU stall 住**
  (窗口是同步总线)。`slave_spi`/`analog_ip` 这类厂商例子里有现成 slave 可抄。

### 1.2 引脚（三种 row）

pins 节点(`agm,agrv2k-pins`)的三种写法,`../tools/generate_board_ve.py` 渲染成 `board.ve`:

| 形式 | dts | 生成的 board.ve | 用途 |
|---|---|---|---|
| A | `mcu-functions` + `mcu-pins`(+ `agm,mcu-input-pins`) | `<FUNCTION> PIN_<N>[:INPUT]` | MCU 外设 → 引脚 |
| B | `cpld-signals` + `cpld-pins` + `cpld-directions` | `<signal> PIN_<N>:<DIR>` | 用户逻辑 → 引脚(带方向) |
| C | `mcu-cpld-functions` + `mcu-cpld-signals`(+ `mcu-cpld-directions`) | `<FUNCTION> <signal>[:<DIR>]` | MCU 外设 ↔ 用户逻辑,**不走引脚**(片内交叉) |

完整例子:`samples/slave_spi`(把 MCU 的 SPI0/SPI1 直连到用户逻辑里的 `sspi0/sspi1`,
同一批引脚两种角色)。注意 dts overlay 是**整条数组替换**,不是追加。

**混合使用(A+B+C)才是"IP 坐在 MCU 与外设中间"的写法** —— `samples/spi_quad_read`:
同一根 flash 引脚上,Case B 让 IP 驱动它,Case C 把 MCU 侧接过来,而
Case A 还得**再写一遍 MCU 要"读回"的那几根**(否则 MCU 的接收通路在 wrapper 里被接成
`1'b0`,IP 再对也读不到数据 —— 板上实测:`RDID = 00 00 00`)。
`SPI0_SI_IO0` 这类**输出功能**要读回时,Case A 行得带方向(`SPI0_SI_IO0 PIN_92:INPUT`),
用 `agm,mcu-input-pins = <92>` 声明(按引脚号而不是按下标 —— 这种行常常排在 20+ 条
数组的末尾,按下标的 `mcu-directions` 得写一长串空串,中间插一条就会静默错位)。
厂商参考 VE(`~/spi_full_mac_bitstream_200mhz/example_board.ve`)的 flash 那一段就是
A(SI/SO)+C+B 的这套组合。

**对照物**:SDK 对厂商 SPI 例程跑一遍 `prepare logic` 的产物放在
`~/agm-logic/prepare_logic_folder/`(`example_board.v` md5 `7bd79c3b…`、
`example_board.vex` md5 `8e46e147…`)。它和我们为 `samples/spi_quad_read` 生成的
`board.{v,vex}` 在 **SPI0/flash 那几根线上逐字相同** —— 六行引脚的 vex 行、`assign
PIN_9x_in/out_*`、`gpio0_io_*` 全部一致,唯一差别是 `gpio0_io_in` 高 4 位(他们的 VE
还路由了 SPI1,407 的 pin map 没有)。所以这条 A 路线不是"另做一套",就是原厂
prepare-logic 的产物。

### 1.3 电气属性（上下拉 / 驱动能力）

`agm,pull-ups` / `agm,pull-downs` / `agm,drive-strength-pins` + `agm,drive-strength-ma`
→ `board.generated.asf` → `logic/board.asf`(厂商拼写 `WEAK_PULL_UP_RESISTOR` /
`CFG_KEEP 2'b01 -extension` / `CURRENT_STRENGTH`,2 mA 档 ≤32 mA)。
例子 `samples/lcd_40pin`;规则与陷阱见 [`DTSI-GUIDE.md`](DTSI-GUIDE.md) 与
[`AG32-PINOUT.md`](AG32-PINOUT.md) §11.4;测试 `../tools/tests/test_generate_board_ve_asf.py`。
**注意**:这些是 **CPLD 配置**,上电到上下拉起效之间端口会 floating 20+ ms;运行期改不了。

### 1.4 时钟 / 复位前提

* 用户逻辑跟着 fabric 的 `SYSCLK` 走;它和 `&clk0`/`&cpu0`/pins 节点三处必须同频,
  `generate_board_ve.py` 会在不一致时 exit 7(见 [`BOARD-VE-FROM-DTS.md`](BOARD-VE-FROM-DTS.md))。
* 换比特流 = 写 flash + 重启,不做 runtime 热重载(见本文 §4 的换比特流一节)。

### 1.5 一个 IP 还是多个 IP：声明是列表,实例化只有一个

这条写的是**边界**:今天能用的是"一个 IP(可以带子模块)";两个 IP **sibling 同时实例化**
还没验证过(待办见 §2 第 5 条)。SDK **1.8.10**(`~/AgRV_pio/platforms/AgRV/`)的实际行为:

| 环节 | 代码 | 行为 |
|---|---|---|
| 声明 | `builder/common.py:513` `ips = get_project_options(["ip_name", "ips"], "")` + `string_to_list()`(`common.py:119`) | `ip_name`(复数写法 `ips` 也认)是**列表**,按空白/换行/逗号切分 |
| 准备 | `builder/main.py:353` `for ip in ips:` | 为**每个**名字收集 `vx/ve/sdc/asf/pre.asf/post.asf/vv/libdir` |
| 编译 | `builder/frameworks/agrv_sdk.py:245/278` | 每个 IP 定义 `IPS_<NAME>` C 宏,各自的 `agrv_ips` 库参与构建 |
| 传给 Supra | `builder/main.py:626/647` → `etc/pre_logic.tcl:235/251` | **所有** IP 的 lib 目录拼成 `LIB_DIRS`,当额外搜索路径 |
| **实例化** | `builder/main.py:600-608`(`ip = ips[0]`、`ip_vv = ip_vvs[0]` …)+ `:622` `-m "<ip_vv>"` | **只取第 0 个**;`gen_vlog -m/--macro`(`etc/gen_vlog:1997`)是单值参数,一次只读一个 macro 文件、生成一个 `macro_inst` |
| 登记源文件 | `etc/pre_logic.tcl:81-83`(`VERILOG_FILES = $LOGIC_VV + $IP_VV`) | `IP_VV` 是字符串,可以塞多个 `.v`(多条 `VERILOG_FILE`),但**不会**实例化它们 |

所以"两个 IP 都进网表"在 SDK 里**没有直接开关**。厂商自己的做法是**层次化**:
`examples/custom_ip/` 中 `ip/` 把 `custom_ip`(`ip/logic/{custom_ip.v,ram2ahb.v,ahb2ram.v}`)
编成可复用 IP(`logic_ip = true`),`top/` 只声明 `ip_name = custom_ip` + `ips_dir = ../ip/logic`,
由顶层 IP 自己去 instantiate 子模块。examples 里**没有一处**真的写两个 `ip_name`
(`examples/dfu` 的 `i2c_ip`/`spi_ip` 是两个 `[setup_*]` 变体各声明一个)。

**我们这条链的现状**:

* `AGM_USER_RTL=<files>` 本来就是**列表**:每个文件都会 stage 进 `<build>/logic/`,并作为
  `IP_VV` 登记成 Quartus 的 `VERILOG_FILE`(`board.proj` 的 `verilogFiles=` 里能看到多份);
  `AGM_LOGIC_IP=<name>` 还会顺带找 `<name>_core.v` —— 所以"一个 IP = wrapper + core"
  今天就能用(wrapper 由 `-m` 实例化,core 作为子模块由 Quartus 从已登记的文件里解析)。
* 实例化仍然只有一条 `-m`(`../tools/build_bitstream.sh` 的 `GEN_VLOG_IP="-m $IP_NAME.v"`),
  与 SDK 同限。

**要两个 IP 同时进网表,优先按厂商的层次化写法**:写一个顶层 wrapper `.v` 把第二个模块
instantiate 进去,把 wrapper 声明成 IP(`AGM_LOGIC_IP=<wrapper>`),第二个 IP 的源文件
走 `AGM_USER_RTL` 一起登记。不要自己往 `board.v` 里生成第二条实例化 —— 那正是已经删掉的
B 路线(`bec57f3`),要回来得先说明"层次化不够用"。

**这个形状已经有现成例子**:`samples/dual_ip` —— 厂商的 `custom_ip` AHB RAM 块
(`examples/custom_ip/ip/logic/`) + `full_duplex_spi`,由 `ip/dual_ip.v` 这个 wrapper
一起实例化,窗口按 `haddr[15:11]` 一分为二(RAM 占 `+0x000..0x7FF`,SPI IP 占其余)。
构建级已验证:5 个 RTL 文件全部进 `board.qsf`,生成的 `board.v` 里**只有一条**
`macro_inst`(即 wrapper),`dual_ip_tmpl.v` 的 42 个端口名与 wrapper 逐一对上,
两个子 IP 的实例端口也 42/42、41/41 对齐。**上板也已实测 PASS**(比特流 md5
`8b959a9a…`):RAM 6 个字写读回、SPI 的 `0x03/0x3B/0x6B` 与单 IP 版逐字节一致、
`+0x800` 的写没有落到 RAM 里。

**预算:用户逻辑区只有 4 块 M9K**(综合时实测)—— 那块 LogicLock
(`core_logic`,20×12 @ X43_Y1)里放得下的块 RAM 上限就是 4,原厂自己的工程也这么约束
(`set_global_assignment -name MAX_RAM_BLOCKS_M4K 4`,vendor 的
`~/spi-logic/example_board.qsf` 里同样一行)。两个 IP 一起用就得**分配**:
`full_duplex_spi` 的 RX FIFO(`lpm_width 32`×`lpm_numwords 256` = 8 Kbit)占 1 块,
`custom_ip` 的 RAM 按原厂默认 `RAM_SIZE=4096`(32 Kbit)要 4 块 —— 5 > 4,Quartus 直接
`Error (170051) ... limited the RAM location(s) of type M9K to 4` +
`Error (171000): Can't fit design in device`。`RAM_SIZE` 是该模块的参数(`ADDR_BITS`
随之变化),wrapper 里实例化成 `RAM_SIZE=2048`(16 Kbit → 2 块,总共 3/4)就能放下 ——
原厂默认值能过,只是因为它是当时设计里唯一的 IP。

## 2. 还不支持的（要做成一等公民）

1. **IP 的寄存器映射:用宏,不用 devicetree**。
   实例地址/中断这类**板级事实**可以进 DT,但**寄存器映射与块内偏移属于比特流与 RTL**,
   写在 `samples/user_ip/src/user_ip_regs.h` 这种头里,RTL 可以共用/生成;
   缺少的是"从 RTL 生成这个头"的一步(手工维护也是可以的,构建期会挡住越界)。
2. ~~**构建期纳入用户 RTL**:用户 `.v/.sv` 从哪进、`.qsf` 里怎么登记~~ **已做
   ** —— 两种声明方式:**按名字**(`-DAGM_LOGIC_IP=<name>`,样例在 `CMakeLists.txt` 里
   写一行即可,等价于原厂 `platformio.ini` 的 `ip_name`;源码按约定从应用自己的
   `<app>/ip/<name>.v`(+ `<name>_core.v`)取),或**按路径**(`-DAGM_USER_RTL=<files>`,空格/分号
   分隔,显式覆盖)。两者都把文件拷进
   `<build>/logic/`,并交给 `pre_logic.tcl` 的 **`IP_VV`** 槽位登记成 Quartus 的
   `VERILOG_FILE` —— 与原厂 PlatformIO 的 `ip_name` 流程同一条路(实测:
   `board.qsf` 出现 `set_global_assignment -name VERILOG_FILE "full_duplex_spi.v"`)。
   **实例化也做了(同日,当天晚些时候改成 A 路线)** —— 现在走的是原厂自己的
   `gen_vlog -m <ip>.v`:gen_vlog 读 IP 的 `module` 端口表,按**端口名 == net 名**在
   `board.v` 里生成 `<ip> macro_inst(.port (port), …)`(与厂商 `example_board.v` 一致),
   外加 Step 0b 的 IP-prepare(`logic_ip/`,原厂 `ip_name` 流程;`AGM_LOGIC_IP_FLOW=off` 可跳过;
   实测:`board.v` 里 `full_duplex_spi macro_inst(` 与厂商自己 prepare 出来的
   `example_board.v` 逐端口一致,13 条 "no slave" 默认 `assign` 全部消失 —— 那 13 条正是
   Quartus 报 12014/12015 的根因)。
   与此同时**把自己插实例的 B 路线删掉了** —— `tools/ip_instance.py` 及其测试都已移除,
   它只是把原厂 `-m` 的效果手搓一遍,两套实现并存只会让 `board.v` 的来源说不清。
3. **中断回 MCU 的规范路径**:今天只有 **CPLD → GPIO 引脚 → MCU GPIO 中断**(`agm,agrv2k-cpld.yaml`
   里写明的现状),没有 fabric → PLIC 的直连线。需要把"哪几根可用作 IP IRQ"写成规则,
   并在 DT 里表达(`interrupt-parent = <&gpioX>` + `interrupts = <pin flags>`)。
4. **DMA 握手**:IP 想发 DMA 请求(DMAC 的 periph 请求线)时的约定与例子。
5. **多个 IP 同时实例化(待测,记下边界)** —— 现状见 §1.5:SDK 的 `ip_name`
   是列表但只实例化 `ips[0]`,`gen_vlog -m` 是单值;我们这边 `AGM_USER_RTL` 已经是文件
   列表,实例化同样只有一条 `-m`。待测两件事:
   ① **不改代码**:按厂商层次化写法,用一个顶层 wrapper 把第二个 IP 当子模块 instantiate
   —— **已做完并上板 PASS**(`samples/dual_ip`:`custom_ip` RAM + `full_duplex_spi`,
   wrapper 里按 `haddr[15:11]` 分窗口)。构建级:`board.v` 只有一条 `macro_inst`、5 个源
   文件都在 `board.qsf`/`board.proj`、模板端口名与 wrapper 逐一对上、twister 选中并构建;
   板上(:RAM 写读回 PASS、SPI 四读 PASS(与单 IP 版逐字节
   相同)、窗口解码 PASS。顺带量到用户逻辑区**只剩 4 块 M9K** 可分,见 §1.5;
   ② **要代码**:真要 sibling 两条 `-m`,得自己生成第二条实例化语句(删掉的 B 路线),
   做之前先说明为什么 ① 不够用。

## 3. 建议落地顺序

1. **本文档**(现状 + 边界);
2. ~~`agm,agrv2k-user-ip` binding + `samples/user_ip`~~ **已做**:
   `samples/user_ip` 演示"**窗口走 DT、寄存器映射走宏**"——DT 只提供 `cpld0`(以及将来的
   IRQ 引脚),块的 `offset/size` 与四个寄存器、位域在 `src/user_ip_regs.h`;构建期断言
   把"块必须在窗口内""寄存器必须在块内"钉死(越界在这条总线上是 **CPU 卡死**而不是异常,
   所以必须构建期挡)。两个 scenario(默认不动总线 / `CONFIG_APP_USER_IP_TOUCH_BUS=y`)
   都构建通过。原来的 `agm,agrv2k-user-ip` binding **没有入库** —— 单实例、没有 driver
   要绑定它时,那只是多一份要跟 RTL 同步的 schema;见 §4 的升级判据;
3. ~~用户 RTL 的构建槽位~~ **已做**:`AGM_LOGIC_IP=<name>`(按名字声明,源码取自
   `<app>/ip/`)或 `AGM_USER_RTL=<files>`(按路径);`samples/spi_quad_read` 是现成例子
   (`set(AGM_LOGIC_IP "full_duplex_spi")` + `ip/full_duplex_spi.v`),生成的 `board.v` 里也
   确实实例化了(原厂 `gen_vlog -m`),而且这条 A 路线**当天就上板 PASS 了** —— 自己的
   pin map + IP → 工作站综合 → 本机 Supra → `RDID = 68 40 15`、`0x03/0x3B/0x6B` 逐字节
   一致。**仍未做**:把 `samples/slave_spi`
   也改成这种声明式(它现在还是"pin map 里有 RTL 的端口、IP 由用户自己接");
4. IRQ/DMA 语义稳定后再进 DT(与 `agm,agrv2k-cpld.yaml` 里那句"语义稳定前不进 binding"一致);
5. ~~多个 IP~~ **已做**:按 §1.5 的层次化写法(`samples/dual_ip`,不改代码)
   真实构建 + 上板验证都过了。给 `AGM_LOGIC_IP` 加列表、或自己生成第二条实例化(§2 第 5 条
   的 ②)目前**没有需求** —— 需要时再谈。

**证据索引**:[`BOARD-VE-FROM-DTS.md`](BOARD-VE-FROM-DTS.md)、
[`DTSI-GUIDE.md`](DTSI-GUIDE.md)、`samples/cpld_reg`、`samples/slave_spi`。

## 4. 为什么寄存器映射不进 devicetree（后来决定）

直觉上"把 IP 描述进 DT"很像 Zephyr 的常规做法,但这里有两块信息必须分开:

| 信息 | 本质 | 放哪 | 理由 |
|---|---|---|---|
| 窗口 `0x60000000+0x8000`、是否使能 | SoC/bitstream提供的一段地址空间,驱动要按它做边界检查 | **DT**(`agm,agrv2k-cpld`,已有) | 板级/SoC 级事实,已有 driver 绑它 |
| 块在窗口内的 `offset`/`size` | 这份比特流的属性,但要和窗口一起校验 | **宏**(`user_ip_regs.h`) | 换比特流就变;仍然用 `BUILD_ASSERT` 对着 `DT_REG_SIZE(cpld0)` 校验 |
| 寄存器偏移、位域、FIFO 深度 | 用户 RTL 的内部布局 | **宏**(与 RTL 共用/由 RTL 生成) | DT 表达不了位域;两边各存一份必然走散 |
| fabric 中断走哪根 GPIO | 由比特流决定,不是 SoC 事实 | **DT**(`interrupt-parent`/`interrupts`)或宏一对 | 落到哪根脚是板级事实 |

**什么时候才值得加 DT 实例节点**:出现**多个实例**(要按名字枚举、分别 `DEVICE_DT_GET`),
或者要写一个真正的 driver 去绑定并 `device_is_ready()` 它。到那时加 binding 描述
"实例 = 窗口内一段 `reg` + 可选中断",寄存器映射仍在头文件里 —— 这与
`agm,agrv2k-cpld.yaml` 里那句"语义稳定到能进 devicetree 之前,扩展 driver 而不是
binding"是同一条原则。
