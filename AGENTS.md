# 仓库贡献指南

面向在**这个模块**上加驱动/样例、改引脚或比特流、提交补丁的人(用 agent 干活的也算)。

本仓库是面向 [Zephyr](https://zephyrproject.org) 的 **AGM AgRV2K HAL 模块**,同时是
工作区的 **west manifest** —— `west init -m <本仓库>` 一条命令就能拉起整个工作区
(本模块 + 伴生仓库
[`hal_ag32_samples`](https://github.com/quesonal/hal-ag32-samples) 作 west 子模块 + Zephyr),
Zephyr 本体钉在某个上游 SHA,树里没有任何 AGM 专属改动。

文档入口是 [`docs/README.md`](docs/README.md);样例在伴生仓库里,每个样例自己的
README 说明它证明什么、需要什么硬件。开发过程记录(逐外设 bring-up 日志、实测、
设计日记)不在本仓库发布范围内。

## 1. 仓库结构

| 路径 | 内容 |
|---|---|
| `CMakeLists.txt`、`west.yml`、`zephyr/module.yml`、`zephyr/Kconfig` | 模块入口、manifest、模块元数据、Kconfig 根。没有根级 `Kconfig`,也没有根级 `prj.conf` |
| `drivers/<subsys>/<name>.c` + 同目录 `Kconfig.agm` | 驱动本体。编译钩子在 `drivers/CMakeLists.txt`(`zephyr_library_sources_ifdef`,按字母序);`Kconfig.agm` 由 `zephyr/Kconfig` 在与 in-tree 时相同的子系统 `if` 里 `rsource` |
| `dts/bindings/**`、`dts/riscv/agm/**` | binding 只描述 DT 语义;dtsi 是 SoC 与共享板级的引脚/时钟来源 |
| `boards/agm/` | `agrv2k_103` / `_303` / `_407` / `_test` + `agrv2k/shared/`(引脚 dtsi、`board_common.cmake`、openocd 支持)。**每块板必须有 `twister.yaml`**,否则 twister 会静默丢掉这块 platform |
| `soc/agm/agrv2k/` | `soc.c`(门控/时钟/复位 init)、`pinctrl.c`、`clk.c`、`fcb.c`、`pm.c`、`reset.S`、`agm_sys.h` |
| `include/zephyr/drivers/**`、`include/zephyr/dt-bindings/pinctrl/` | 公共头;`AGM_PINCTRL(bank,pin,dir)` 是 dts 与 pinctrl 驱动的唯一来源 |
| (样例在伴生仓库 `hal_ag32_samples` 下,不在这棵树里) | 58 个样例(`hal_ag32_samples/samples/<name>/`),结构同以前的 `samples/<name>/`(`CMakeLists.txt`、`prj.conf`、`sample.yaml`、`src/main.c`,需要板级开关时 `boards/<board>.overlay`)。本仓库的 `west.yml` 把伴生仓库拉成 west 子模块,落在 `<ws>/modules/hal_ag32_samples/` —— twister 的根相应变成 `modules/hal_ag32_samples/samples`(以前是 `samples`) |
| `tests/` | 不需要开发板的用例(在 `native_sim` 上跑驱动 API) |
| `tools/` | 构建/烧录/抓取/签名/校验脚本,逐个说明见 [`tools/README.md`](tools/README.md) |
| `docs/` | 使用文档(索引 [`docs/README.md`](docs/README.md)) |

## 2. 环境与依赖

  - **Zephyr 侧**:Zephyr SDK + `west`(按 Zephyr 官方 getting-started 装好),
    `ZEPHYR_SDK_INSTALL_DIR` 指向 SDK;`west update` 之后 `ZEPHYR_BASE` 由 west 解析。
  - **Python 侧**:`tools/` 下的脚本要求一个**专用虚拟环境**,里面至少有 `west`、
    `pyserial`(串口抓取)、`pyusb`(CMSIS-DAP 探针)、`pyyaml`(AF 表/pin 路由)、
    `devicetree`(dts 生成器)、`pytest`(主机侧用例)。
    `tools/` 的脚本用 `$VIRTUAL_ENV` 判断环境是否激活,没激活直接 `exit 3`;
    **不要用系统 python** —— 缺模块时是静默失败。

    **每开一个新终端 / 新会话都要先激活一次**(不会全局生效),下面三种等价,任选一种:

    1. **用现成的脚本** —— 本机已经有 `~/activate_zephyr.sh`(venv + `~/.local/bin` +
       `ZEPHYR_SDK_INSTALL_DIR` / `ZEPHYR_TOOLCHAIN_VARIANT` + `SUPRA_HOME`):

       ```sh
       source ~/activate_zephyr.sh
       ```

    2. **自己建一个激活脚本** —— 放仓库外面(例如 `~/.zephyr-env.sh`),按自己的路径改:

       ```sh
       # ~/.zephyr-env.sh
       source "$HOME/zephyrproject/.venv/bin/activate"          # 你的 venv
       export PATH="$HOME/.local/bin:$PATH"                     # pip --user 的 west / ninja
       export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.1"   # 你的 Zephyr SDK 根
       export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
       export SUPRA_HOME="$HOME/AgRV_pio/packages/tool-agrv_logic"  # 仅综合比特流需要
       ```

       之后每个新终端 `source ~/.zephyr-env.sh`。

    3. **不建脚本,把变量直接带在命令里** —— 适合一次性调用;注意 `VIRTUAL_ENV` 也要一起给,
       否则 `tools/` 的守卫会拒绝执行:

       ```sh
       VIRTUAL_ENV="$HOME/zephyrproject/.venv" \
       PATH="$HOME/zephyrproject/.venv/bin:$PATH" \
       ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.1" \
       ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
         west build -b agrv2k_407 -d /tmp/b_hello \
                modules/hal_ag32_samples/samples/hello_world
       ```
  - **AgRV SDK(只有比特流综合与 ROM bootloader 才需要)**:厂商的 AgRV PlatformIO 平台包
    (`gen_vlog`、`pre_logic.tcl`、`af_cmd`、openocd、`agrv32flash`),外加工作站上的 Quartus。
    用环境变量指过去,工具都有默认值可覆盖:
    `AGRV_SDK_PATH`(平台安装根)、`SUPRA_HOME`(`tool-agrv_logic` 包)、
    `AGRV_PLATFORM_ETC`(`platforms/AgRV/etc`)、`AGRV_OPENOCD`(openocd 可执行)。
    纯固件构建与 `native_sim` 用例**不需要**它。
  - **探针**:板上 CMSIS-DAP(SWD)最省事;没有探针就 `BOOT0` 拉高 + 上电,走 UART ROM
    bootloader 只写固件。

## 3. 起工作区、构建、烧录、抓取

```sh
source ~/activate_zephyr.sh                    # 见 §2:venv + SDK,每个新 shell 都要做一次
west init -m <本仓库> --mr main <ws> && cd <ws> && west update
west build -b agrv2k_407 \
           modules/hal_ag32_samples/samples/hello_world   # canonical 200 MHz 比特流:不需要 overlay
west flash                                     # 固件 + 比特流一起写(需要板上 CMSIS-DAP 探针)
tools/test_uart_capture.sh                     # 先开串口再复位,才抓得到 banner
```

  - 只写固件 / 只写比特流:`west flash --skip-bitstream` / `--bitstream-only`(互斥);
    没有探针或 SWD 连不上:`west flash --runner agrv32flash`(UART,只写固件)。
  - **时钟必须与比特流一致**:canonical 那份是 200 MHz,与板级默认相同;换 100 MHz
    比特流要带 `-- -DEXTRA_DTC_OVERLAY_FILE=<100mhz overlay>`,否则 UART 分频不对
    (115200 请求 → 57600 实际,表现为满屏乱码)。
  - 改引脚或往比特流里加自己的逻辑:`west build -t logic` 生成 `<build>/logic/`
    → 在装 Quartus 的机器上 `cd <build>/logic && quartus_sh -t af_quartus.tcl`
    (**原样**综合,不要在目录里再跑一遍 SDK 的 prepare logic)→ 回传
    `simulation/modelsim/<design>.vo` → `west build -t bitstream` → `west flash`。细节见
    [`docs/BOARD-VE-FROM-DTS.md`](docs/BOARD-VE-FROM-DTS.md) 与
    [`docs/CUSTOM-IP.md`](docs/CUSTOM-IP.md)。
  - 烧录/抓取的规则、症状表与 ROM bootloader 救砖路径:先把
    [`docs/FLASH-AND-CAPTURE.md`](docs/FLASH-AND-CAPTURE.md) 的 §0 与故障决策树读一遍。

## 4. 测试与提交门槛

  - **提交前跑一次 twister**(它开 `CONFIG_COMPILER_WARNINGS_AS_ERRORS=y` 与
    `--edtlib-Werror`,而 `west build` 只打 warning):
    `west twister -T samples -p agrv2k_407 --build-only -O <out>`。
  - 改了**驱动**,或 SoC 的**时钟/门控/复位/DMA**:提交前用
    `tools/test_hil.sh` 上板跑一轮(构建 + 烧录 + 运行 + 串口断言,`-j 1` 串行)。
    `--build-only` 抓不到运行时回归 —— 只构建过的改动可能在板上整条外设路径失效。
    其中 `spi_flash_rw.hil` 会擦写片上 NOR 扇区 0,只在开发板上跑。
  - 碰**引脚 / 比特流 / AF 表 / pinctrl fragment / 相关工具**:跑
    `tools/check_pin_routing.sh`(秒级,不需要构建),它挡"pin list 改了但没重跑生成器"。
  - 不需要板子的用例放 `tests/`,用 `west twister -T tests -p native_sim` 跑(会真执行)。
  - 样例约定:样例在伴生仓库里,元数据在
    `modules/hal_ag32_samples/samples/<name>/sample.yaml`,用例 ID
    `sample.<name>.<platform>`;需要开发板的用 `build_only: true` + `platform_allow:`;
    twister 跑这些样例的根相应是 `modules/hal_ag32_samples/samples`;
    每个新增驱动 API 至少 **1 正例 + 1 反例**,判定必须能从串口输出看出来
    (`PASS`/`FAIL`,别让"读全 0xff"这种空白前提混成假 PASS)。

## 5. 关键设计约定(动外设/引脚/时钟前必读)

  - **门控**只由 `soc/agm/agrv2k/soc.c` 写(`SYS.APB_CLKENABLE` / `AHB_CLKENABLE` /
    `AHB_RESET`),开关集合从 devicetree 推导(GPIO bank、带 pinctrl state 的外设、
    `agm,*clkenable-bit`)。驱动不要自己开关门控。
  - **引脚去向**(哪个功能落到哪根脚)只由
    `soc/agm/agrv2k/pinctrl.c::pinctrl_configure_pins()` 写 AFSEL/DIR,
    数据来自各节点的 pinctrl state;驱动 init 里调 `pinctrl_apply_state()`。
  - **时钟**只有一处来源:板 dts 顶部的 `AGM_SYSCLK_HZ` / `AGM_HSE_HZ` /
    `AGM_FLASH_MAX_HZ`,`&clk0` / `&cpu0` / `&sys` 与 pins 节点都引用它;外设统一
    `clocks = <&clk0>` + `DT_INST_PROP_BY_PHANDLE(inst, clocks, clock_frequency)`。
    换比特流频率时这几个数字必须一起改,否则 tick 漂移或 UART 波特率错;
    `tools/generate_board_ve.py` 会在三者不一致时报错。
  - **DT 命名**:自定义属性一律 `agm,` 前缀(`agm,pins`、`agm,apb-clkenable-bit`…),
    节点名小写连字符。
  - **一个节点只能挂一个驱动**:`DEVICE_DT_INST_DEFINE()` 按节点序号命名设备,同一节点上
    两个带驱动绑定的 compatible 会在链接期撞 `multiple definition of
    '__device_dts_ord_<n>'`;要"厂商 compatible + 通用 compatible"时,只让其中一个真的有驱动。
  - **in-tree 零改动**:需要 Zephyr 树里的私有头就直接 include
    (`zephyr_library_include_directories_ifdef()` 提供目录),**不要 fork 上游 `.c`**,
    也不要把改动落进 `<ws>/zephyr`;确实需要时先确认,再考虑上流。
  - **真机优先**:功能改动先在开发板上验证(串口抓取 / 读寄存器 / 逻辑分析仪)再提交,
    提交正文写清"实测到了什么、哪些没验证";调试完成后把板子刷回 `hello_world`。
  - **换比特流 = 写 flash + 重启**,不做 runtime 热重载:AgRV2K 的 CPU 时钟由比特流产生
    (`CLKOUT[0] → sys_clk`),运行中重配 fabric 有把核冻死(串口/SWD 一起失联)的真实风险。
  - **区分实测与推断**:文档与提交信息里不要把推断写成实测结论。

## 6. 代码风格与命名

  - C 用 Linux 内核风格(Tab 缩进,单行 ≤100 列),YAML 2 空格;YAML 用 yamllint,
    C 用 `zephyr/scripts/checkpatch.pl`。
  - 公共符号 `agm_<驱动>_<动作>`;驱动文件名 `<驱动>_agm.c`;宏 `UPPER_SNAKE_CASE`。
  - 新增源文件必须带 `SPDX-License-Identifier: Apache-2.0` 头(许可证 Apache-2.0)。
  - 尽量复用上游 Zephyr 既有写法;文档里的代码块要与仓库实际状态一致。

## 7. 文档

  - 改外设 / 引脚 / 时钟 / 比特流 / 引导流程时,**同步更新对应的 `docs/` 页面**,
    而不是只写 commit message;索引是 [`docs/README.md`](docs/README.md)。
  - **开发过程记录不进树,只在本地 `dev-notes/`**:入口是
    [`dev-notes/README.md`](dev-notes/README.md) —— 目录**平铺**,文件名沿用树里的名字:
    逐外设 bring-up 日志在 `dev-notes/port-status/<chapter>.md`,一页汇总 / 章节索引 /
    生成器设计记录 / 附加笔记直接放 `dev-notes/`,清单见 `dev-notes/NOT-PUBLISHED.md`;
    **本机的 venv / SDK、west 工作区、`tools/devsync.sh`、烧录 / 抓串口 / HIL 怎么跑**
    在 [`dev-notes/ENVIRONMENT.md`](dev-notes/ENVIRONMENT.md)(机器本地事实)。
    树里只留面向使用者的页面,提到这些材料时统一写"开发记录(未随本仓库发布)"。
    注意 `git clean -x`/`-X` 会把这些本地记录一起删掉,没备份时别跑。
  - 凡是写"实测"的内容,都要带上可复现的上下文:板卡、比特流文件名(+ md5)、
    构建参数(overlay / `-D`)、抓取命令与输出。缺了这些,"通过"过一周就不可复现。
  - **驱动里的 HISTORY / 实测段怎么留**: 一行写清"测到了什么、因此否定了什么";
    行为改变时**追加**新结论,不要覆写旧结论 —— 旧结论里往往写着
    "当时为什么这么选",那正是后面 review 要用的第一手材料(审查项 N-7)。
    日期由提交信息承担(commit author date 与 commit message 头),不在注释里重复。
