# AgRV2K dtsi 使用指南(写给从 SDK 过来的人)

> 这份文档只讲**怎么做**:改走线要动哪几个文件、每条命令是什么、出错看哪一条。
> 生成器/校验器背后的设计论证不在本次发布范围内,本文不重复。
>
> 配套:[BOARD-VE-FROM-DTS.md](BOARD-VE-FROM-DTS.md)(`board.ve` 渲染)、
> [FLASH-AND-CAPTURE.md](FLASH-AND-CAPTURE.md)(烧录/抓串口规则)、
> [README.md](../tools/README.md)(工具环境变量)、
> 本文 §6 的 MAC 引脚一节。

---

## 1. 对照表:SDK 里的东西现在写在哪

| SDK 里你熟悉的 | Zephyr 里对应 | 谁改它 |
|---|---|---|
| `board.ve` 里手写 `<FUNCTION> PIN_<n>` | pins 节点的 `mcu-functions` / `mcu-pins`(板级 `dts/riscv/agm/agrv2k-pins.dtsi`、sample 的 `boards/*.overlay`,或你自己的 board dts) | **人** |
| `[setup_logic] logic_device` | pins 节点的 `agm,logic-device`(必填);环境变量 `AGM_LOGIC_DEVICE` 可覆盖 | **人** |
| `AltaRiscv.h` 的 `MAC0_TXD0_AF_GPIO` / `_MASK` / `_OUTPUT` | `../tools/agm_af_pins.yaml`(114 条全量,`../tools/check_af_table.py` 对头文件比对) | **机器**,别手改 |
| `PERIPHERAL_*_ENABLE` 宏 | 节点的 `status = "okay"` + `agm,pins`(cell 由生成器填) | 人写使能,机器填 cell |
| `SYS_EnableAPBClock()` / AHB gate | 节点上的 `agm,apb-clkenable-bit` / `agm,ahb-clkenable-bit` / `agm,ahb-reset-bit` | **人**(寄存器由 `soc.c` 写) |
| `SYS_SetSclkAuto()` / `FLASH_MAX_FREQ` | `&sys { flash-max-frequency = <...>; }` | **人**,必须和比特流一致 |
| `SYS_SwitchPLLClock()` | `&clk0` + `&cpu0` + pins 节点的 `sysclk-frequency` / `hseclk-frequency`,**全部引用板 dts 顶部的 define** | **人**改那一处 define |
| `board.ve` 本身 | 生成物:`<board_dir>/board.ve`,由 pins 节点渲染 | **机器**,别手改 |

---

## 2. 你要写的东西

### 2.1 pins 节点(binding `agm,agrv2k-pins`)

三类行,按需要选:

| 类 | 属性 | 用途 |
|---|---|---|
| A:MCU 功能 → 外部 pin | `mcu-functions` / `mcu-pins`(平行数组,长度相等) | 最常用 |
| B:CPLD 信号 → pin | `cpld-signals` / `cpld-pins` / `cpld-directions`(`input`/`output`/`inout`) | 自己写的 user logic |
| C:MCU 功能 ↔ CPLD 信号(不占 pin) | `mcu-cpld-functions` / `mcu-cpld-signals` | 内部 cross-bar |

```dts
&agrv2k_pins {
	agm,logic-device = "AGRV2KL100";      /* 必填,见 §2.3 */
	agm,pull-ups   = <69>;                /* 可选:给这些 pin 开内部上拉 */
	agm,pull-downs = <56>;                /* 可选:内部下拉 */
	agm,drive-strength-pins = <68>;       /* 可选:加强驱动(配对下面的 mA) */
	agm,drive-strength-ma   = <8>;        /* 2 mA 一档,2..32 */
	sysclk-frequency = <200000000>;       /* = 比特流 SYSCLK */
	hseclk-frequency = <8000000>;         /* = 板上晶振 */

	mcu-functions = "UART0_UARTRXD", "UART0_UARTTXD";
	mcu-pins      = <69>,            <68>;
};
```

上面是 **overlay** 的写法(扩展共享节点)。如果这块板不用共享的
`dts/riscv/agm/agrv2k-pins.dtsi`,要在自己的 board dts 里新写一个
`agrv2k_pins: agrv2k-pins { … }` 节点 —— 完整写法见 §3.2。

规则:两个数组**下标一一对应**;同一个 pin 在同一个数组里出现两次会**报错**
(`AGM_ALLOW_PIN_CONFLICTS` 才能降级成 warning)。

`agm,pull-ups` / `agm,pull-downs` 写 **pin 号**(和 `mcu-pins` 同一套编号),不是功能名;
`agm,drive-strength-pins` / `agm,drive-strength-ma` 是**成对**的数组(同一下标 = 同一个 pin),
mA 只能是 2 的倍数且 ≤ 32。这三样都是比特流(CPLD 配置)的一部分,生成器会渲染成 ASF 赋值交给
Quartus(见 §3.3 步骤 1);写了一个没有路由的 pin 会有一条 `note:`。
40 kΩ 上下拉、`CFG_KEEP` 这类还没进 dts 的赋值,写在 `<board_dir>/board.asf` 里(手写文件,
会被一起折叠进 `logic/board.asf`)。

### 2.2 生成物(pinctrl 片段)放哪

| 放哪 | 它的 pin list 谁写的 | include 写法 | 谁引得到 |
|---|---|---|---|
| `dts/riscv/agm/pinctrl-<profile>.dtsi` | 板级 `dts/riscv/agm/agrv2k-pins.dtsi` | `#include <agm/pinctrl-<profile>.dtsi>` | 任何 board / sample |
| `boards/agm/<board>/pinctrl-<x>.dtsi` | 那个 board 的 `<board>.dts` | `#include "pinctrl-<x>.dtsi"` | 那个 board 的 dts |
| `<app>/boards/pinctrl-<x>.dtsi` | 那个 sample 的 `<board>.overlay` | `#include "pinctrl-<x>.dtsi"` | 那个 overlay |

一句话:**生成文件放在"它由哪份 pin list 生成"的那一层,同层用引号 include**;尖括号只能在
`dts/riscv` 等 `dts_root` 目录里用(论证与实测见 §13.3)。

### 2.3 四条硬规则

1. **时钟只改一个地方**:board dts 顶部的 define,其余节点全部引用它们。

   ```dts
   #define AGM_SYSCLK_HZ    200000000
   #define AGM_HSE_HZ         8000000
   #define AGM_FLASH_MAX_HZ 100000000

   &clk0 { clock-frequency = <AGM_SYSCLK_HZ>; };   /* 外设:pclk / UART 波特率 / 定时器 */
   &cpu0 { clock-frequency = <AGM_SYSCLK_HZ>; };   /* 内核 tick */
   &sys  { flash-max-frequency = <AGM_FLASH_MAX_HZ>; };

   &agrv2k_pins {                                  /* 比特流:board.ve 的 SYSCLK/HSECLK */
           sysclk-frequency = <AGM_SYSCLK_HZ>;
           hseclk-frequency = <AGM_HSE_HZ>;
   };
   ```

   上面三个值必须和你烧的比特流一致:`AGM_SYSCLK_HZ` 就是位流给出的 SYSCLK(同时喂给
   `&clk0`、`&cpu0` 与 pins 节点),`AGM_HSE_HZ` 是板上晶振,`AGM_FLASH_MAX_HZ` 是固件侧给
   SPI NOR 的上限。`../tools/generate_board_ve.py` 会检查
   `sysclk-frequency` == `&clk0` == `&cpu0`,不一致直接失败(exit 7)——
   以前 pins 节点里是写死的 200 MHz,板级改了 SYSCLK 它就静默不同步了。
2. **`agm,logic-device` 必填**,且和芯片封装一致。可取值:
   `AGRV2KL100` / `AGRV2KL100H` / `AGRV2KL64` / `AGRV2KL64H` / `AGRV2KL48` / `AGRV2KQ32`;
   查该封装有哪些脚:`python3 $AGRV_PLATFORM_ETC/gen_vlog -p -d <device>`。
3. **`agm,pins` 的 cell 不要手写**:由 `../tools/generate_pinctrl_dtsi.py` 从 pin list + AF 表渲染。
4. **`board.ve` 和 `logic/` 都是生成物**,别手改;要改就改 dts。

---

## 3. 示例:自己的一块 L100 板(整条流程)

例子:自己设计的 L100 板,走线 = **UART0 控制台 + RMII 以太网**,自己的比特流。
顺序是**先比特流、后固件**(理由见 §13.1),其中 3.3 的步骤 2 必须在**你自己的 Quartus 机器**上做。

| 步骤 | 在哪台机器 | 工具 |
|---|---|---|
| 1 出 Quartus 工程 | 本机(Linux) | `west build -t logic` |
| 2 Quartus 综合 + 布局布线 | **你的 Quartus 工作站** | `quartus_sh -t af_quartus.tcl`(或 GUI 打开 board.qpf) |
| 3 Supra 出比特流 | 本机 | `../tools/compile_bitstream.sh` |
| 4 烧比特流 + 核对时钟 | 本机 + 板子 | `../tools/flash_logic.sh` |
| 5 生成 pinctrl cells | 本机 | `../tools/generate_pinctrl_dtsi.py` |
| 6 编固件 + 校验 | 本机 | `west build` / `../tools/check_pinctrl.py` |
| 7 烧固件 + 验证 | 本机 + 板子 | `../tools/flash_fw.sh` / `../tools/test_uart_capture.sh` |

### 3.1 目录树与起点

`BOARD_ROOT` 指向"**含 `boards/` 子目录**"的那一层,布局必须是 `boards/<vendor>/<board>/`:

```
~/myboards/                        ← -DBOARD_ROOT=$HOME/myboards
└── boards/agm/my_l100/
    ├── board.yml                  ← 板名 / vendor / soc
    ├── Kconfig.my_l100            ← config BOARD_AGM_MY_L100 / select SOC_AGM_AGRV2K
    ├── Kconfig.defconfig          ← config BOARD default "agm_my_l100" + rsource 共享 body
    ├── my_l100_defconfig          ← CONFIG_SOC_AGM_AGRV2K=y / CONFIG_BOARD_AGM_MY_L100=y …
    ├── board.cmake                ← 复用 module 的 shared/board_common.cmake
    ├── my_l100.dts                ← 走线(pins 节点)+ 时钟四处
    ├── my_l100-pinctrl.dtsi       ← 生成物(步骤 5)
    └── logic/                     ← 生成物:Quartus 工程(步骤 1)
```

起点直接拷一块现成板:

```sh
mkdir -p ~/myboards/boards/agm/my_l100
cp -r <hal_ag32>/boards/agm/agrv2k_407/. ~/myboards/boards/agm/my_l100/
```

然后改四件事:① 文件名与 `board.yml` / `Kconfig` / `<board>_defconfig` 里的板名;
② `board.cmake` 里那句 include 指向 module(`$ZEPHYR_HAL_AGM_HOME/boards/agm/agrv2k/shared/board_common.cmake`);
③ `<board>.dts` 里的走线(3.2);④ 时钟四处。

```sh
source <your-venv>/bin/activate
export ZEPHYR_HAL_AGM_HOME=$HOME/zephyr-hal-ag32     # 外部 board 复用 module 的胶水
cd $HOME/zephyrproject
```

### 3.2 dts / dtsi 示例

`my_l100.dts` 头部照旧引 SoC 和板级公共件(module 的 `dts_root` 把 `dts/riscv` 挂进了 include 路径,
board 放在哪个 board_root 都能引):

```dts
/dts-v1/;
#include <mem.h>
#include <agm/agrv2k.dtsi>              /* SoC:外设 + pinctrl 控制器 */
#include <agm/agrv2k-board.dtsi>        /* 板级公共件:LED / 按键 / 控制台接线 */
#include <zephyr/dt-bindings/gpio/gpio.h>

#define AGM_SYSCLK_HZ    200000000
#define AGM_HSE_HZ         8000000
#define AGM_FLASH_MAX_HZ 100000000

/ { /* model / compatible / chosen(console,flash,sram)/ flash0 / sram0 …照抄现成板 */ };

&clk0 { clock-frequency = <AGM_SYSCLK_HZ>; };
&cpu0 { clock-frequency = <AGM_SYSCLK_HZ>; };
&sys  { flash-max-frequency = <AGM_FLASH_MAX_HZ>; };

/* 比特流侧的时钟:board.ve 的 SYSCLK/HSECLK 就是这两行(§2.3 规则 1) */
&agrv2k_pins {
	sysclk-frequency = <AGM_SYSCLK_HZ>;
	hseclk-frequency = <AGM_HSE_HZ>;
};

/* 自己的走线 */
&{/} {
	agrv2k_pins: agrv2k-pins {
		compatible = "agm,agrv2k-pins";
		agm,logic-device = "AGRV2KL100";

		sysclk-frequency = <200000000>;
		hseclk-frequency = <8000000>;

		mcu-functions = "UART0_UARTRXD", "UART0_UARTTXD",
				"MAC0_RXD0", "MAC0_RXD1", "MAC0_TXD0", "MAC0_TXD1",
				"MAC0_TX_EN", "MAC0_TX_CLK", "MAC0_MDC", "MAC0_MDIO",
				"MAC0_CRS";
		mcu-pins = <69>, <68>, <47>, <46>, <44>, <43>, <45>, <57>,
			   <58>, <59>, <56>;
	};
};

/* 生成物(步骤 5)。第一次进来时这个文件还不存在 —— 先别写这行,或者先 `touch` 空文件 */
#include "my_l100-pinctrl.dtsi"
```

步骤 5 生成出来的 `my_l100-pinctrl.dtsi`(**不要手写**):

```dts
/* AUTO-GENERATED by tools/generate_pinctrl_dtsi.py -- DO NOT EDIT. */
#include <zephyr/dt-bindings/pinctrl/agm-agrv2k-pinctrl.h>

&eth0_default {
	agm,pins =
		<AGM_PINCTRL(7, 7, AGM_PINCTRL_NO_DIR)>,   /* MAC0_RXD0  */
		<AGM_PINCTRL(8, 0, AGM_PINCTRL_NO_DIR)>,   /* MAC0_RXD1  */
		<AGM_PINCTRL(9, 1, AGM_PINCTRL_OUTPUT)>,   /* MAC0_TXD0  */
		<AGM_PINCTRL(9, 2, AGM_PINCTRL_OUTPUT)>,   /* MAC0_TXD1  */
		<AGM_PINCTRL(9, 5, AGM_PINCTRL_OUTPUT)>,   /* MAC0_TX_EN */
		<AGM_PINCTRL(7, 5, AGM_PINCTRL_NO_DIR)>,   /* MAC0_TX_CLK(REF_CLK) */
		<AGM_PINCTRL(9, 7, AGM_PINCTRL_OUTPUT)>,   /* MAC0_MDC   */
		<AGM_PINCTRL(4, 0, AGM_PINCTRL_OUTPUT)>,   /* MAC0_MDIO  */
		<AGM_PINCTRL(4, 0, AGM_PINCTRL_INPUT)>,    /*   ↑ bidir 两条 */
		<AGM_PINCTRL(8, 5, AGM_PINCTRL_NO_DIR)>;   /* MAC0_CRS   */
};

&uart0_default {
	agm,pins =
		<AGM_PINCTRL(6, 1, AGM_PINCTRL_INPUT)>,    /* UART0 RX */
		<AGM_PINCTRL(7, 6, AGM_PINCTRL_OUTPUT)>;   /* UART0 TX */
};
```

> 这个文件里没有 pin:cell 只有 SoC 固定的 (bank, bit, dir),pin 在 `board.ve` 那边。

### 3.3 七步

**1. 出 Quartus 工程(本机)**

```sh
west build -d /tmp/b_mine -b my_l100 modules/hal_ag32/samples/hello_world -t logic \
    -- -DBOARD_ROOT=$HOME/myboards
# 或者直接调脚本(AGM_DTS=auto 时连 Zephyr 配置都不需要):
#   tools/build_bitstream.sh <board_dir> <logic_dir>
```

这条只配置 + 跑比特流前端,**不编固件**。输出在 build 目录的 `<build>/logic/`
(与 `zephyr.bin` 同级;之前它落在 board 目录里):

> 反过来,只想编固件、机器上没有 AgRV SDK:`west build … -- -DAGM_LOGIC_TARGET=OFF`
> 会跳过整个 `logic/` 生成(实测在 `AGRV_SDK_PATH=/nonexistent` 下也能编出 `zephyr.elf`)。
> 那时 `board.ve` / `logic/` 得在别的机器上出。

| 文件 | 作用 |
|---|---|
| `board.ve` | function→pin 文本(从 pins 节点渲染) |
| `board.asf` | 交给 Quartus 的 ASF:我们的上下拉(来自 pins 节点)+ 手写 `board.asf` 的内容 |
| `board.vx` / `board.hx` / `board.vex` | gen_vlog 出来的 Verilog / 头 / 约束 |
| **`board.qsf` / `board.qpf`** | **Quartus 工程**(`TOP_LEVEL_ENTITY = board`) |
| **`af_quartus.tcl`** | **Quartus 侧的入口脚本**:先 `af_ip.tcl` 建分区,再跑 flow(或用 GUI 打开 `board.qpf`) |
| `simulation/modelsim/board.vo` | Quartus 的 post-route 网表(第 3 步的输入,Quartus 跑完才有) |

顺手看一眼电气属性有没有进去(写了 `agm,pull-*` / `agm,drive-strength-*` 时):

```sh
grep -E "WEAK_PULL|CFG_KEEP|CURRENT_STRENGTH" /tmp/b_mine/logic/board.asf
# set_instance_assignment -name WEAK_PULL_UP_RESISTOR -to PIN_69 ON
# set_instance_assignment -name CFG_KEEP -to PIN_56 2'b01 -extension
# set_instance_assignment -name CURRENT_STRENGTH -to PIN_68 8MA
```

会看到(外部 board 也一样,只要 `ZEPHYR_HAL_AGM_HOME` 指对):

```
>>> [0/2] logic device from dts: AGRV2KL100
>>> [0/2] package check: 11 pin(s), all present on AGRV2KL100
>>> [1/2] gen_vlog: <build>/logic/board.ve → <build>/logic/
>>> [2/2] pre_logic.tcl
```

**2. 在 Quartus 工作站上编译**

把整个 `<build>/logic/` 目录拷到装了 Quartus 的机器(本机没有 Quartus;厂商流程用
Windows),**按生成的样子**用 —— 不要在目录里再跑一遍 SDK 的 prepare logic:

```sh
cd <build>/logic
quartus_sh -t af_quartus.tcl      # 或在 GUI 里打开 board.qpf
```

产出 post-route 网表 `<build>/logic/simulation/modelsim/board.vo` —— 下一步唯一要的东西。

**3. 回本机,Supra 出比特流**

把 `logic/`(至少 `simulation/modelsim/board.vo` 在原位置)拷回来:

```sh
west build -d /tmp/b_mine -t bitstream          # Supra: board.vo -> <build>/zephyr/board.bin
# 或者直接调脚本:tools/compile_bitstream.sh /tmp/b_mine/logic /tmp/b_mine/zephyr/board.bin
# >>> bitstream: .../board.bin (99944 bytes, md5 ...)
#     batch file: .../board_batch.bin
```

产物固定落在 `<build>/zephyr/board.bin`,也就是 `west flash` 默认找的文件(或用
`AGM_BITSTREAM_BIN` 指别处)。
比特流**不可字节复现**(每次摆放/布线种子不同),判据是"能起来、行为对",不要用 md5。

**4. 烧比特流 + 核对时钟**

```sh
west flash -d /tmp/b_mine --runner agrv_openocd --bitstream-only  # 只擦写 0x800e7000,固件区不动
# (Plan A 之后等价命令;旧的 flash_logic.sh ~/myboards/board.bin 仍可用)
grep -E "BOARD_(PLL|BUS|HSE)_FREQUENCY" /tmp/b_mine/logic/board.hx
# BOARD_PLL_FREQUENCY 必须等于 pins 节点的 sysclk-frequency(= board dts 的 AGM_SYSCLK_HZ)
# BOARD_HSE_FREQUENCY 必须等于 hseclk-frequency
```

`BOARD_BUS_FREQUENCY`(BUSCLK)目前由 gen_vlog 自己定(我们不写这一行,见 §13.6)。

**5. 生成 pinctrl cells**

```sh
python3 $ZEPHYR_HAL_AGM_HOME/tools/generate_pinctrl_dtsi.py \
    --dts /tmp/b_mine/zephyr/zephyr.dts \
    --out $HOME/myboards/boards/agm/my_l100/my_l100-pinctrl.dtsi
# -> my_l100-pinctrl.dtsi (2 state(s), 12 cells)
```

生成完打开 3.2 里那行 `#include "my_l100-pinctrl.dtsi"`。
(`/tmp/b_mine/zephyr/zephyr.dts` 是步骤 1 配置时产出的合并 dts;如果只想拿这份 dts,
`west build … --cmake-only` 也够,不产出 `.elf`。)

这一步**不需要比特流产物**:输入只有 pin list(功能名)和 AF 表,bank/bit 是芯片固定的,比特流
只决定 pin。所以固件可以先编;比特流编完之后再用它**校验**(`check_pinctrl.py --netlist`,
见步骤 6)—— 那是比特流在固件侧唯一的用处(原因见 §13.2)。

眼下哪些 state 已经有 cell:`<agm/agrv2k-board.dtsi>` 里 include 的**板级默认片段**覆盖了
控制台(UART0)和 SPI/CAN/I2C/GPTIMER1 那几个 state —— 只要功能名一样,cell 就一样;只有
**你这块板特有的**功能(这里是以太网的 `eth0_default`)是空的,要靠这一步补上(原因见 §13.2)。

**6. 编固件 + 校验**

```sh
west build -d /tmp/b_mine -b my_l100 modules/hal_ag32/samples/hello_world \
    -- -DBOARD_ROOT=$HOME/myboards          # 正常固件编译,产出 zephyr.elf/.bin

python3 $ZEPHYR_HAL_AGM_HOME/tools/check_pinctrl.py \
    --dts /tmp/b_mine/zephyr/zephyr.dts --netlist <你的比特流网表>/board.v
python3 $ZEPHYR_HAL_AGM_HOME/tools/generate_pinctrl_dtsi.py --dts /tmp/b_mine/zephyr/zephyr.dts --check
python3 $ZEPHYR_HAL_AGM_HOME/tools/check_af_table.py
```

cells 必须在这次编译**之前**生成好(它们是 devicetree 数据,编译期进固件);改了 pin list 就
重跑 1 → 5 → 6。

**7. 烧固件 + 验证**

```sh
tools/flash_fw.sh /tmp/b_mine/zephyr/zephyr.bin                 # 只烧固件区,保留比特流
tools/test_uart_capture.sh -t 8 /tmp/b_mine/zephyr/zephyr.bin   # 先开串口再复位,带判定
```

以太网再 `ping -c 5 <host>` / iperf(见 `samples/lan8720_*`)。

### 3.4 改了东西要重跑哪几步

| 你改了什么 | 重跑 |
|---|---|
| `mcu-pins`(功能集合不变) | 1 → 2 → 3 → 4(新比特流)。**固件不必重编** |
| pin list 加了/减了功能 | 1 → 2 → 3 → 4 → 5 → 6(新 cells + 重编固件;新增的外设要有节点消费它) |
| 只改别的 dts(比如 LED) | 6(重编固件),比特流不用动 |
| 换封装 | `agm,logic-device` + 按 `gen_vlog -p -d` 剪 `mcu-pins` → 全套重跑(步骤 1 的 package check 会先拦下不匹配的 pin) |
| 时钟 | 改 §2.3 那一处 define(比特流和固件都要重来:1 → 2 → 3 → 4 → 6) |

### 3.5 快路:比特流不用动,只换走线(overlay)

比特流已经存在时(比如本仓库的 dev 比特流),不必做自己的 board,用外部 overlay 换 pin list 即可
—— **不需要 Quartus**:

```sh
cat > ~/my_l100.overlay <<'EOF'
&agrv2k_pins {
        agm,logic-device = "AGRV2KL100";
        sysclk-frequency = <200000000>;
        hseclk-frequency = <8000000>;
        mcu-functions = "UART0_UARTRXD", "UART0_UARTTXD";
        mcu-pins = <69>, <68>;
};
EOF
west build -d /tmp/b_mine -b agrv2k_407 modules/hal_ag32/samples/hello_world \
    -- -DEXTRA_DTC_OVERLAY_FILE=$HOME/my_l100.overlay
python3 $ZEPHYR_HAL_AGM_HOME/tools/generate_pinctrl_dtsi.py \
    --dts /tmp/b_mine/zephyr/zephyr.dts --out $HOME/my_l100-pinctrl.dtsi
echo '#include "my_l100-pinctrl.dtsi"' >> $HOME/my_l100.overlay
west build -d /tmp/b_mine -b agrv2k_407 modules/hal_ag32/samples/hello_world \
    -- -DEXTRA_DTC_OVERLAY_FILE=$HOME/my_l100.overlay
```

`EXTRA_DTC_OVERLAY_FILE` 排在 sample 自带的 `boards/<board>.overlay` **之后**应用,所以你的
pin list 赢(即使那个 sample 自己也改了 pin list)。

---

## 4. 陷阱表

| 症状 | 真相 / 原因 | 谁抓住它 |
|---|---|---|
| 把 PIN_57 标成 `RX_CLK 7.6` | 网表里 REF_CLK 走 TX_CLK(7.5);7.6 是同一时钟的另一半,那块 bit 还是 UART0 TX | `../tools/check_pinctrl.py` ③ |
| `agm,pins` 里有 (8,5) 但 pin list 没 `MAC0_CRS` | 重生成的 `board.ve` 会丢掉 CRS | `../tools/check_pinctrl.py` ④ |
| 照 SDK 注释写 `MDC=bank4.5` | 头文件是 9.7;4.5 是 SPI0_SCK/GPTIMER1_ETR | `../tools/check_af_table.py` |
| `mcu-functions` 少写几个功能 | overlay 是**整段替换**数组,不是追加 | `../tools/check_pinctrl.py` ④ |
| `agm,pins = <>` 看着像"没配" | 故意留空:值由 include 的生成文件覆盖。先查 include 丢没丢 | `../tools/check_pinctrl.py` ③ |
| 从网表注释推映射 | 注释会骗人;只信 `assign` / `gpio*_io_in` | 工具只读结构化行 |
| AF 表里没有的功能(`GPIO4_1`、`USB0_ID`) | 它们不是 AF 功能,是 fabric pin 信号,没有 AFSEL 位 | `note:`(非失败) |
| 功能在 AF 表里但没有 state | 只有列在 pin list 里的功能会被渲染 | `note:`(非失败) |
| 改了 pin list 忘了重生成 fragment | 构建仍然成功,但 AFSEL 是旧的 | `generate_pinctrl_dtsi.py --check` |
| 串口/MAC 完全没动静,代码看着没错 | 两种原因,先分清:① cells 没生成 / include 丢了 → 固件里根本没有那次 AFSEL 写(看合并 dts 里该 state 的 `agm,pins` 是不是空);② cells 对、但板上的比特流是旧的(pin 没连)→ 写进去也没信号 | ① `../tools/check_pinctrl.py` ③;② `--netlist` 对当前比特流网表,或重烧比特流 |
| fragment 生成到 `<ws>/modules/hal_ag32` | 那是 devsync 镜像树,`restore` 会丢掉 | 生成器拒绝(exit 7,`--force` 可覆盖) |
| `logic_device` 设小/忘了设 | gen_vlog 只 warning、退出码 0,pin 静默不接 | `../tools/build_bitstream.sh` package check(exit 5) |
| 手改 `board.ve` / `board.qsf` | 生成物,下次构建被覆盖 | 改 dts / 在 Quartus 侧留档 |

---

## 5. 命令速查

```sh
# --- bitstream侧 -----------------------------------------------------------
west build -d <build> -b <board> -t logic      # dts → <build>/logic(Quartus 工程)
tools/build_bitstream.sh <board_dir> <logic_dir>   # 同上,独立调用(AGM_DTS=auto 免配置)
#   你的 Quartus 机器上:quartus_sh -t af_quartus.tcl → simulation/modelsim/board.vo
tools/compile_bitstream.sh <logic_dir> [board.bin] # Supra → 比特流
west flash -d <build> --runner agrv_openocd --bitstream-only
                                                   # 只写 0x800e7000(旧 tools/flash_logic.sh 已 deprecated)

# --- 固件侧 -----------------------------------------------------------
tools/generate_pinctrl_dtsi.py --dts <zephyr.dts> --out <dtsi>
tools/generate_pinctrl_dtsi.py --dts <zephyr.dts> --check    # 新鲜度闸门(exit 6)
tools/check_pinctrl.py --dts <zephyr.dts> [--netlist <board.v>]   # 0 ok / 7 失败
tools/check_af_table.py                                       # AF 表 vs SDK 头文件

# --- 真机 -------------------------------------------------------------
tools/flash_fw.sh <zephyr.bin>                  # 只写固件区(保留比特流)
tools/test_uart_capture.sh -t 8 [zephyr.bin]    # 先开串口再复位,带 PASS/FAIL
tools/probe_state.sh                            # "板子像死了"先跑这个(只读)
```

常用环境变量:`AGRV_SDK_PATH`(默认 `~/AgRV_pio`)、`ZEPHYR_HAL_AGM_HOME`(外部 board 复用
module 胶水时必设)、`AGM_LOGIC_DEVICE`(覆盖 dts 里的封装)、`AGM_BITSTREAM_BIN`
(`west flash` 找比特流的位置)、`AGM_SUPRA_*`(Supra 摆放/布线参数)。完整列表见
[README.md](../tools/README.md)。

---

## 6. 边界

- 仍然是**手写**的 pinctrl state:`spi1_default`、`i2c1_default`、`gpt0|2|3|4_pwm_default`
  —— 它们的 pin 没有被任何比特流路由,也没有 sample 声明过;要收干净就先给它们做一份片段。
- `../tools/build_bitstream.sh` 还没挂生成器(生成器手动跑,靠 `--check` 兜底);仓库没有 CI。
- **Quartus 那一步必须在你的机器上做**(本机没有 Quartus);`../tools/compile_bitstream.sh`
  只做 Quartus 之后的 Supra 步骤。
- `BUSCLK` 还不能从 dts 声明。
- 可选优化(未验证):让一对同 bank/bit 的"采样 + 驱动"功能合租一根 pin、pin list 只写真要用的
  功能。

为什么这么设计、每一步的实测证据,见本文各节标注的板卡与构建参数。
