# hal_ag32 — AGM AgRV2K 的 Zephyr 模块

[English](README.md) | **简体中文**

面向 **AGM AgRV2K** SoC(RV32IMAFC 硬核 + 片内可编程 FPGA fabric)的 out-of-tree
[Zephyr](https://zephyrproject.org) 模块:SoC 胶合层、四块板级定义、devicetree
binding、驱动、west runner 与样例都在这里;本仓库同时是工作区的 **west manifest**,
一条命令就能把整个工作区拉起来:

```sh
west init -m <本仓库> --mr main ~/agrv-ws
cd ~/agrv-ws && west update
```

Zephyr 本体钉在上游某个 SHA,树里没有任何 AGM 专属代码。

## 这里有什么

| | |
|---|---|
| **开发板** | `agrv2k_407`(下面所有内容都在它上面验证过)、`agrv2k_103`、`agrv2k_303`、`agrv2k_test`(仅构建) |
| **驱动** | UART(PL011 寄存器兼容)、GPIO + pinctrl、PLIC、DMA、I2C、SPI(含 dual/quad 线与经 fabric 的全双工)、TIMER/GPTIMER/RTC 计数、PWM、WDOG/IWDG、CAN、以太网 MAC0 + RMII、USB device(CDC-ACM / HID)、片内 flash、外挂 SPI NOR、CRC、CPLD 窗口 |
| **Fabric(CPLD)** | 比特流里的用户逻辑:MCU 到 fabric 的 AHB 窗口、三种引脚写法、电气属性、自定义 IP 接入 —— [CUSTOM-IP.md](docs/CUSTOM-IP.md) |
| **Bootloader / DFU** | 二级 loader(`drivers/misc/boot_agm.c`):A/B 镜像、片内执行槽、fabric 比特流槽;三条 host 上传路径(console、`agrv32flash`、mcumgr);签名镜像、按芯片绑定、产线加锁 —— [BOOT-DFU-STATUS.md](docs/BOOT-DFU-STATUS.md),地址地图见 [FLASH-LAYOUT.md](docs/FLASH-LAYOUT.md) |
| **样例** | 58 个应用在伴生仓库 [`hal_ag32_samples`](https://github.com/quesonal/hal-ag32-samples) 里(由本仓库的 west.yml 作为 west 子模块拉取,落在 `<ws>/modules/hal_ag32_samples/`);每个 README 都写明它证明什么、需要什么硬件 |
| **比特流** | 样例统一对着同一份 200 MHz 参考比特流;两个仓库都不分发 —— 详见伴生仓库的 `samples/bitstreams/README.md`(通过 `AGM_BITSTREAM_BIN` 指向自己的副本) |

## 环境要求

构建需要 Zephyr SDK(带 RV32IMAFC multilib)与 `west`,装法见 Zephyr 官方
getting-started。本模块的工具就是普通 Python + `west` 命令,读这几个环境变量:

| 变量 | 取值 |
|---|---|
| `ZEPHYR_SDK_INSTALL_DIR` | Zephyr SDK 安装目录 |
| `ZEPHYR_TOOLCHAIN_VARIANT` | `zephyr` |
| `AGRV_SDK_PATH` | AgRV PlatformIO 安装目录(默认 `~/AgRV_pio`)。只做固件有 `tool-agrv_openocd` 就够;`west build -t logic` 还需要 `tool-agrv_logic` |
| `SUPRA_HOME` | `$AGRV_SDK_PATH/packages/tool-agrv_logic`(只有重建比特流才需要) |

只有重建比特流和走 ROM bootloader 才真正需要 AgRV SDK。伴生仓库
[`hal_ag32_samples`](https://github.com/quesonal/hal-ag32-samples) 由本仓库的 west.yml
拉取,写一个新样例的完整流程见
[`hal_ag32_samples/docs/SAMPLE-WORKFLOW.md`](https://github.com/quesonal/hal-ag32-samples/blob/main/docs/SAMPLE-WORKFLOW.md)。

## 快速开始

```sh
# 在 west init -m <本仓库> 拉起来的工作区里,west update 把伴生仓库放到
# <ws>/modules/hal_ag32_samples/,所以构建路径多了一层目录:

west build -b agrv2k_407 \
           modules/hal_ag32_samples/samples/hello_world   # canonical 200 MHz 比特流:不需要 overlay
west flash                                              # 固件 + 比特流一起写(需要板上 CMSIS-DAP 探针)
tools/test_uart_capture.sh                              # 先开串口再复位,才抓得到 banner
```

`west flash` 默认把固件**和**比特流一起写 —— 只写一个用 `--skip-bitstream` 或
`--bitstream-only`,`tools/flash_fw.sh` / `tools/flash_logic.sh` 是同样的两件事。
调试探针由 `AGRV_ADAPTER` 选择(默认 `cmsis-dap`,即板上那颗)。

没有探针(SWD 连不上)时:`BOOT0` 拉高、重新上电,走 ROM bootloader:

```sh
west flash --runner agrv32flash                   # 走 UART,只写固件
```

## 文档

| 问题 | 文档 |
|---|---|
| 怎么建/编译/烧录/调试一个样例 | [SAMPLE-WORKFLOW.md](https://github.com/quesonal/hal-ag32-samples/blob/main/docs/SAMPLE-WORKFLOW.md) |
| 烧录与抓串口的规则、症状表、ROM bootloader 救砖路径 | [FLASH-AND-CAPTURE.md](docs/FLASH-AND-CAPTURE.md) |
| 两个 flash runner 各写什么、怎么切换 | [FLASH.md](docs/FLASH.md) |
| flash 里各区域在哪:loader、A/B、执行槽、fabric 槽、boot record、bind-salt、option 区 | [FLASH-LAYOUT.md](docs/FLASH-LAYOUT.md) |
| Bootloader / DFU:上传路径、签名镜像、按芯片绑定、产线加锁 | [BOOT-DFU-STATUS.md](docs/BOOT-DFU-STATUS.md) |
| 签名与密钥:算法选择、容器格式 | [SIGNED-IMAGES-PLAN.md](docs/SIGNED-IMAGES-PLAN.md) |
| 把自己的逻辑接进 fabric(AHB 窗口、引脚写法、电气属性) | [CUSTOM-IP.md](docs/CUSTOM-IP.md) |
| dtsi 怎么组织、SDK 概念对照 | [DTSI-GUIDE.md](docs/DTSI-GUIDE.md) |
| 重新生成 `board.ve` 与 Quartus 输入 | [BOARD-VE-FROM-DTS.md](docs/BOARD-VE-FROM-DTS.md) |
| 各封装的引脚名 | [AG32-PINOUT.md](docs/AG32-PINOUT.md) |
| host 侧 USB/CDC-ACM 掉线及处理 | [CDC-ACM-FLAKE-SOLUTIONS.md](docs/CDC-ACM-FLAKE-SOLUTIONS.md) |

驱动 API 看 `include/zephyr/drivers/` 下的头文件;每个样例都带 `sample.yaml`
说明它面向哪块板。完整索引见 [README.md](docs/README.md)。

## 容易踩的坑

* **串口乱码但板子没坏** —— 比特流时钟与固件时钟不一致。canonical 那份是 200 MHz,
  与板级默认一致(不用 overlay);换 100 MHz 比特流必须带
  `-- -DEXTRA_DTC_OVERLAY_FILE=<100mhz overlay>`,否则 UART0 分频不对
  (请求 115200 → 实际 57600)。
* **`west flash` 默认固件 + 比特流一起写**。只想写一个用 `--skip-bitstream` 或
  `--bitstream-only`(互斥);只写固件不会动 fabric 里已有的比特流。
* **启动 banner 只在复位时出现** —— 必须在复位**之前**把串口打开
  (`tools/test_uart_capture.sh` 就是这么做的);`minicom`、`cat /dev/ttyACM0`
  或晚接上只能看到周期性输出。
* **`west build` 不等于回归网**:twister 还会开
  `CONFIG_COMPILER_WARNINGS_AS_ERRORS=y` 与 `--edtlib-Werror`,构建全绿也可能有
  这些检查过不去的地方。
* **不要在生成好的 `logic/` 目录里再跑一遍 SDK 的 prepare logic**:
  `board.v` 与 `board.vex` 是同一次生成的两半,第二次会让同一根引脚有两个名字
  (`SPI0_SI_IO0 PIN_92` 对不上 `si_io0`),绑引脚的工具只打 warning 就把这些 IO
  摆到别的引脚上 —— 板子会完全够不到 flash。
* **dual/quad SPI 读需要比特流把 IO2/IO3 接到 flash 的 WP#/HOLD#**;别的比特流下这些
  调用会返回 `-ENOTSUP` 或读到噪声,哪些比特流可用见
  [`samples/spi_quad_read`](https://github.com/quesonal/hal-ag32-samples/blob/main/samples/spi_quad_read/README.md)。
* **别碰 `/dev/ttyACM1`** —— 评估板上那是板载 ESP32-C3,AgRV2K 的控制台是
  `/dev/ttyACM0`。
* **烧完像"死"了**:`west flash` 把固件写到片内 flash、把比特流写到 fabric 地址,
  写错时钟域之后板子看起来就是死的 —— 见
  [FLASH-AND-CAPTURE.md](docs/FLASH-AND-CAPTURE.md) §10(用 ROM bootloader 恢复)。

## 许可

Apache-2.0,见 [LICENSE](LICENSE);[NOTICE](NOTICE) 列出本模块构建时依赖的厂商
SDK 组件。
