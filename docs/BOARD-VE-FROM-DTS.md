# board.ve — dts-driven 自动渲染

**影响面**: `boards/agm/agrv2k*/` + `dts/riscv/agm/` + `dts/bindings/pinctrl/` + `modules/hal_ag32/tools/`

## 1. 背景

历史情况:4 个 AgRV2K board 变体(`agrv2k_103 / agrv2k_303 /
agrv2k_407 / agrv2k_test`)每个板目录里都有一个手工维护的
`board.ve`(68 行),4 份 `board.ve` 除了第 1 行 `SYSCLK <N>` 不同外,
**剩余 67 行 pinout 完全一致**。

总重复量 = 4 板 × 67 行 ≈ 268 行,任何一个 pin 改动要在 4 个文件里
同步改,容易漏。

中间过渡:曾经用「共享 `board.ve.template` + 每板 `dist/<b>.dist` +
`generate_board_ve.py` 字符串替换」的方案解决 67 行重复,但仍然存在
两个问题:
1. `dist` 是 Zephyr 生态外的 flat KEY=VALUE 文件,跟 `devicetree overlay`
   哲学不一致;sample 想覆盖引脚得另搞一套机制。
2. 字符串替换无法处理「`PIN_15` 既是 `SPI0_SCK` 又是 `GPTIMER4_CH0`」
   这类 pin 冲突 —— 因为模板按行模板化,删除/重定义一行无解。

## 2. 新架构:从 dts 渲染 board.ve

把 board.ve 渲染的输入改成 **Zephyr 标准 devicetree**:

| 层 | 路径 | 内容 |
|---|---|---|
| **共享 pins dtsi** | `dts/riscv/agm/agrv2k-pins.dtsi` | `agm,agrv2k-pins` 节点,67 行共享 pinout + 频率参数 |
| **每板 dts** | `boards/agm/agrv2k_<V>/<board>.dts` | `#include <agm/agrv2k-pins.dtsi>` + 覆盖 `sysclk-frequency` 等板级参数 |
| **Sample overlay** | `samples/<p>/boards/<board>.overlay` | 标准 Zephyr devicetree overlay,可追加 / 覆盖 pin 数组 |
| **Binding**(验证用) | `dts/bindings/pinctrl/agm,agrv2k-pins.yaml` | 节点 schema,只做 dts lint,不影响运行时 |
| **生成产物**(gitignored) | `<build>/logic/board.ve` | 由 `generate_board_ve.py` 从合并后的 dts 渲染 |

数据流:

```
dts/riscv/agm/agrv2k-pins.dtsi   ─┐
boards/agm/agrv2k_<V>/*.dts       ─┤
samples/<p>/boards/<b>.overlay   ─┼─→ devicetree.cmake 合并
                                   │     ↓
                                   │  PROJECT_BINARY_DIR/zephyr.dts (preprocessed)
                                   │     ↓
                                   │  generate_board_ve.py --dts <path>
                                   │     ↓
                                   ↓  boards/agm/agrv2k_<V>/board.ve ─┐
                                                                    ├─→ gen_vlog + pre_logic.tcl ─→ logic/board.{vx,hx,vex,qsf,asf}
```

**不需要 Zephyr 构建的那条路**(`AGM_DTS=auto`):

```
boards/agm/agrv2k_<V>/<board>.dts (+ AGM_DTS_OVERLAY) ─→ cpp -nostdinc -undef -D__DTS__
                    │                                        ↓
                    │                                   dtc -I dts -O dts
                    │                                        ↓
                    └──────────────→ generate_board_ve.py --dts <temp> ─→ board.ve ─→ gen_vlog + pre_logic.tcl ─→ logic/
```

一条命令即可(在 west 工作区内,或设 `ZEPHYR_BASE`):

```sh
cd ~/zephyrproject                       # 任何位置都行:ZB 从 west list 取
AGM_DTS=auto ~/zephyr-hal-ag32/tools/build_bitstream.sh \
    ~/zephyr-hal-ag32/boards/agm/agrv2k_407 /tmp/logic_407
```

**等价性实测**:同一天用两条路各生成一遍 407 的逻辑目录,`board.ve` 逐字节一致,且
`board.vx`/`.hx`/`.vex`/`.qsf`/`.asf`/`agrv_fpga_decomp.inc` 全部相同(19 个文件集合一致)。
所以"必须先跑一次 Zephyr cmake 才能出比特流"这个约束可以去掉;要覆盖 pin(例如某个
sample 的 overlay 改了 pin 数组),用 `AGM_DTS_OVERLAY=` 追加即可。

**为什么不能把 `.ve` 整个省掉**:`.ve` 不是我们的中间格式,而是厂商工具
`AgRV_pio/platforms/AgRV/etc/gen_vlog`(2081 行 Python + 器件库)的**输入格式**。它读 `.ve`
后负责:MCU 硬宏实例化与端口清单、pin 能力校验(`addPin`/`check_pll` 等)、`.hx`/`.vx`/
`.vex`/`.asf`/`.qsf` 的生成,以及调用 `pre_logic.tcl`。要"dtsi 直接出 logic"就得把这些
全部重写一遍,并随厂商器件库更新而维护 —— 代价远大于 376 行的 dts→ve 渲染。当前做法是
让 `.ve` 变成**纯生成物**(gitignored、一条命令内产生),人不再碰它。

跟 Zephyr 已有 dts overlay 体系完全一致:sample 的 `boards/<b>.overlay`
由 `devicetree.cmake` 自动发现 + 自动 reparse,无需 cmake 侧手动注入。

## 3. `agm,agrv2k-pins` 节点

`dts/riscv/agm/agrv2k-pins.dtsi` 是单一信源。三个 parallel array
组对应 AG32 VE 配置说明.pdf 里的 3 类信号关联:

| 组 | 含义 | 输出格式 |
|---|---|---|
| **case A — `mcu-functions` + `mcu-pins`** | MCU 内部通路 → 外部引脚 | `<FUNCTION_NAME> PIN_<N>`(无方向;由 mcu function 自身决定,如 `UART0_UARTTXD` 永远 output) |
| **case B — `cpld-signals` + `cpld-pins` + `cpld-directions`** | CPLD 自定义信号 → 外部引脚 | `<cpld_signal_name> PIN_<N>:<DIRECTION>`,`direction` ∈ `{input, output, inout}`,空字符串默认 `inout` |
| **case C — `mcu-cpld-functions` + `mcu-cpld-signals`** | MCU 内部通路 ↔ CPLD 自定义信号(无外部引脚,比特流内部 cross-bar) | `<FUNCTION_NAME> <cpld_signal_name>` |

两处"按行加方向"的补充属性(都在 case A 行上):

* `mcu-directions` —— 与 `mcu-functions` **同下标**的可选数组,空串 = 该行不带方向
  (厂商 VE 里 `<FUNCTION> PIN_<n>:INPUT` 就是这个);适合"需要方向的行都在数组前几行"。
* `agm,mcu-input-pins` —— **按 pin 号**列出"MCU 还要读回来"的那几根,渲染时给对应的
  case A 行加上 `:INPUT`。理由:这种行常常排在 20+ 条数组的末尾(例如
  `samples/spi_quad_read` 的 `SPI0_SI_IO0 PIN_92` —— SI 默认是 output,得靠这条把
  flash 的 IO0 接回 SPI0 的接收通路),按下标写就得补一长串空串,而且中间插一条会
  静默错位;按 pin 号写还多一层校验 —— pin 不在 `mcu-pins` 里直接报错(exit 4)。
  实测:少了这一行,生成的 wrapper 把 MCU 的读通路接成 `1'b0`
  (`gpio0_io_in[0] = 1'b0`),板上 `RDID = 00 00 00`。

外加两个全局参数:

* `sysclk-frequency` — Hz,转成 board.ve 第 1 行的 `SYSCLK <MHz>`
* `hseclk-frequency` — Hz,转成第 2 行 `HSECLK <MHz>`

dts 属性命名是 lower-kebab,generator 在输出 board.ve 时把它原样写出去
(UPPER_SNAKE 的 `mcu-functions` 字符串直接复制)。

### 默认 25-pin pinout

`agrv2k-pins.dtsi` 把所有 4 板共用的 pin 都列在 `mcu-functions` /
`mcu-pins` 里(MAC 外设 11 个 function/pin 已全部移除让位给 LED):SPI0 6 pin、GPIO4 LED1..4(PIN_51..54)、buttons/switches 4 pin、
USB0_ID、UART0/1、CAN0、I2C0、GPTIMER1/4 CH0。

case B / C 默认空(eval board 的比特流没有 cpld 私有信号或 mcu-cpld
直连)。

### Per-board 覆盖

每个 board.dts 在 include pins dtsi 之后,用 `&agrv2k_pins { ... }`
覆盖本板独有的参数。当前唯一差异是 `sysclk-frequency`:

```dts
/* agrv2k_103 / agrv2k_test:SYSCLK 100 MHz */
&agrv2k_pins {
    sysclk-frequency = <100000000>;
};

/* agrv2k_303 / agrv2k_407:用 dtsi 默认 200 MHz,无需覆盖 */
```

## 4. Sample-level pin override

Sample 用标准 Zephyr devicetree overlay。`samples/<prj>/boards/<b>.overlay`
写 `&agrv2k_pins { ... }`,dts overlay 自动合并:

```dts
/* samples/slave_spi/boards/agrv2k_407.overlay */
&agrv2k_pins {
    /* 降频到 100 MHz */
    sysclk-frequency = <100000000>;

    /* 追加 SPI0 完整 pin(从默认 PIN_98/2/4/5 改到 slave_spi pins) + SPI1 全新 */
    mcu-functions = "SPI0_CSN", "SPI0_SCK", "SPI0_SI_IO0", "SPI0_SO_IO1",
                    "SPI1_CSN", "SPI1_SCK", "SPI1_SI_IO0", "SPI1_SO_IO1";
    mcu-pins      = <1>, <15>, <17>, <25>,
                    <29>, <46>, <48>, <52>;

    /* 加 sspi0_*/sspi1_* 作为 CPLD 自定义信号,与 master SPI 共享 pin */
    cpld-signals    = "sspi0_csn", "sspi0_sck", "sspi0_mosi", "sspi0_miso",
                      "sspi1_csn", "sspi1_sck", "sspi1_mosi", "sspi1_miso";
    cpld-pins       = <1>, <15>, <17>, <25>,
                      <29>, <46>, <48>, <52>;
    cpld-directions = "input", "input", "input", "output",
                      "input", "input", "input", "output";
};
```

dts overlay 语义是**整组替换**:Zephyr 只是把 overlay 追进 `dts_files`
(`cmake/modules/dts.cmake:189-198`)一起交给 dtc,所以按标准 dtc overlay
规则,同名属性赋值就是覆盖。

- **覆盖**:`mcu-functions = ...;` 整组替换 dtsi 的值。实测(
  slave_spi 407 构建):合并后的 `zephyr.dts` 里只剩 overlay 那 8 条
  `mcu-functions`,dtsi 的 SPI0/UART/CAN 行全部消失,渲染出的 `board.ve`
  也只有那 8 条 case A。
- **追加**:没有追加语义。要保留 dtsi 里的其它行,得把它们连同新增行一起
  写进 overlay。
- **删除**:不需要单独的语法 —— 覆盖已经能表达"换掉整组 pin"。

> 早前本节写的"数组级追加 + 需要去 dtsi 把冲突行注释掉"(`_merge_props`)
> 是错的,已作废;`samples/slave_spi` 的 overlay 注释与 README 里的同一条
> 说法也已按实测更正。

## 5. `../tools/generate_board_ve.py`(重写)

纯 Python 标准库,无外部依赖。读 `${PROJECT_BINARY_DIR}/zephyr.dts`
(由 Zephyr `devicetree.cmake` 处理过的全展开 dts),用 `re` 解析
`agm,agrv2k-pins` 节点下的 5 组 parallel array,渲染成 board.ve:

```bash
python3 tools/generate_board_ve.py \
    --dts ${PROJECT_BINARY_DIR}/zephyr.dts \
    --board-dir boards/agm/agrv2k_407 \
    --out boards/agm/agrv2k_407/board.ve
```

特性:
- 退出码语义化:`0` 成功 / `1` I/O 错 / `2` 参数错 / `3` 节点找不到 /
  `4` parallel array 长度不匹配 / `5` 缺必需属性 / `6` pin 重复绑定
- 幂等:内容不变时跳过写,保 mtime → ninja 不重跑
- parallel array 长度不匹配 → 硬错误(防 typo)
- 节点找不到(忘了 include dtsi) → 硬错误 + 提示
- **重复绑定检测(默认硬错误)**:同一个 pin 被两条 case A(或两条 case B)
  认领、同一个 function/signal 名字绑到两个 pin、或 case C 里同一个 function
  出现两次,都在渲染前报错并列出冲突双方。gen_vlog 自己的判据是带方向的
  (`platforms/AgRV/etc/gen_vlog::DeviceInfo.addPin`):同一 pin 上全是输入
  合法("multiple loads driven by that device pin"),一旦有一条能驱动该 pin
  就 `ErrorOut("<pin> is used for both <a> and <b>")`;同一 function 绑两个
  pin 则只在输入时冲突。generator 没有 function 方向表,不做方向判断,
  统一报错,由调用方确认:intended 的话用 `--allow-pin-conflicts`
  (或 `AGM_ALLOW_PIN_CONFLICTS=1`)降级成 warning。
- **cross-bar 提示**:同一 pin 同时出现在 case A 和 case B(MCU 功能 + CPLD
  信号)是**合法**的,比特流内部把两者接起来(slave_spi 依赖它),
  以 `note:` 打印,不计入错误。
- `cpld-directions` 可以整体省略(每条默认 `inout`);写了就必须与
  `cpld-signals` 等长

## 6. `../tools/build_bitstream.sh`(改)

Step 0 改成读 `AGM_DTS` 环境变量:

```sh
if [ -n "${AGM_SKIP_BOARD_VE_GEN:-}" ]; then
    # CI 缓存命中场景
    :
elif [ -n "${AGM_DTS:-}" ]; then
    python3 "$SCRIPT_DIR/generate_board_ve.py" \
        --dts "$AGM_DTS" \
        --board-dir "$BOARD_DIR" \
        --out "$BOARD_DIR/board.ve"
else
    # 旧路径:用磁盘上现有的 board.ve(若在)
    :
fi
```

`AGM_DTS` 由 `board_common.cmake` 通过 `${CMAKE_COMMAND} -E env` 注入,
无需手设。

## 7. `board_common.cmake`(改 in-tree)

`add_custom_command(OUTPUT logic/board.qsf ...)` 简化:

- `DEPENDS`:`${PROJECT_BINARY_DIR}/zephyr.dts` + `agrv2k-pins.dtsi` +
  `<board>.dts` + `generate_board_ve.py` + `build_bitstream.sh`(去掉
  旧 `dist/<b>.dist` 和 `board.ve.template` 引用)
- `COMMAND`:`cmake -E env AGM_DTS=<path> ...`

**自动发现**:sample 改了 `boards/<board>.overlay` → `devicetree.cmake`
自动重新生成 `zephyr.dts` → `ninja logic` 检测到 dts mtime 更新 →
重渲染 board.ve → 重生成 logic/。无需 `west build` 手动 reconfigure,
**这是 dts overlay 机制对上一版「cmake 可选依赖」限制的根本改进**。

## 8. 添加新 pin / 新板的流程

### 8.1 添加共享 pin(影响所有 4 板)

1. 改 `dts/riscv/agm/agrv2k-pins.dtsi`:在 `mcu-functions` / `mcu-pins`
   数组里追加 1 项,保持索引对齐
2. `west build -b agrv2k_407 -t logic` — 自动重渲染 + 重生成 logic/

### 8.2 添加板变体

1. `boards/agm/agrv2k_<new>/board.cmake`(继承 `shared/board_common.cmake`)
2. `boards/agm/agrv2k_<new>/<board>.dts`:`#include <agm/agrv2k-pins.dtsi>` +
   `#include <agm/agrv2k-board.dtsi>`(LED/按键等共享板级节点;必须放在
   `zephyr/dt-bindings/*` 之后)+ `&agrv2k_pins { sysclk-frequency = <...>; }`
3. `boards/agm/agrv2k_<new>/<board>.defconfig` / `.overlay` 等既有流程
4. `boards/agm/agrv2k_<new>/twister.yaml`——没有它 twister 会静默丢掉该板
5. `west build -b agrv2k_<new> -t logic` 自动渲染 board.ve

### 8.3 Sample 想覆盖引脚

直接用标准 Zephyr overlay,无需改共享模板:

```dts
/* samples/<prj>/boards/<b>.overlay */
&agrv2k_pins {
    mcu-functions = ...; /* 整组覆盖或追加 */
    mcu-pins      = ...;
};
```

`west build` 自动 reparse dts。

## 9. 验证

| 项 | 工具 | 结果 |
|---|---|---|
| dts → board.ve 渲染 4 板 | `AGM_DTS=... bash build_bitstream.sh ...` | 4/4 PASS,PLL/USB/MAC 输出与手写版 byte-equivalent |
| 渲染后 logic/ 与旧 dist+template 流对比 | `diff /tmp/logic_<b>/board.{vx,hx,vex,qsf,asf}` | 4 板全部 MATCH |
| 增量:无改动 | `ninja logic` | `ninja: no work to do.` |
| 增量:`touch <board>.dts && ninja logic` | | 触发重渲染 |
| 增量:改 sample overlay | `touch samples/<p>/boards/<b>.overlay && ninja logic` | 触发重渲染(devicetree.cmake 自动重扫) |
| Sample sysclk 覆盖 | `samples/<p>/boards/<b>.overlay` 设 `sysclk-frequency=<X>` | render 出的 board.ve `SYSCLK X/1e6` |
| gitignore 命中生成的 board.ve | `git check-ignore -v boards/agm/agrv2k_407/board.ve` | 命中 |

## 10. 已知限制

- **dts overlay 不支持删除 / 单项修改 array**。如果 sample 想
  *移除* dtsi 里的某条 pin 分配(比如 slave_spi 想删除
  `GPTIMER4_CH0 PIN_15` 因为它跟 `SPI0_SCK PIN_15` 撞 pin),需要
  改 `dts/riscv/agm/agrv2k-pins.dtsi` 把那条注释掉。后续扩展点:
  - 在 dtsi 用 `status = "disabled"` 的属性 + generator 跳过 disabled 行
  - 或让 dts overlay 支持 `/delete-node/` 之类的 op(非标准)
- **case C(mcu ↔ cpld 直连)目前没有 eval board 在用**,dtsi 给的
  placeholder 数组是空的,功能已实现但未被验证。
- **`sysclk-frequency` 数值要跟 `cpu0::clock-frequency` /
  `clk0::clock-frequency` / `sys::clock-frequency` 三个值手写保持
  一致**(dts 没有跨节点引用属性值的机制)。改 SYSCLK 时每个 board
 的 dts 文件要同时改这 4 个值。
- **generator 只查"重复绑定",不查"能不能绑"**:它没有 gen_vlog 的
  `FUNC_PINS` 表,所以功能名拼错(例如 `CAN0_TX`)、或把功能绑到不支持该
  功能的 pin 上,都不会在这里报错,要到 `gen_vlog`/Quartus 才失败;
  binding 里的 `mcu-functions` 也只是裸 `string-array`,没有 enum。
- **同一个物理脚在 dts 里被描述两遍**:pinctrl state 里的
  `AGM_PINCTRL(7, 3, INPUT)`(MCU 侧 AFSEL bank/bit)和 pin 表里的
  `CAN0_RX0 PIN_38` 说的是同一个脚,但两者之间没有任何一致性校验,只能人工对齐。

## 11. 关联提交

- `boards/agm/agrv2k/shared/board.ve.template` 删除(被 dtsi 取代)
- `boards/agm/agrv2k_{103,303,407,test}/dist/<board>.dist` 删除 × 4
  (共 -52 行)
- `dts/riscv/agm/agrv2k-pins.dtsi` 新增
- `dts/bindings/pinctrl/agm,agrv2k-pins.yaml` 新增
- `boards/agm/agrv2k_{103,303,407,test}/<board>.dts` include pins dtsi +
  覆盖 sysclk(103 / test)
- `boards/agm/agrv2k/shared/board_common.cmake` DEPENDS + 工具发现改写
  (走 dts 路径)
- `../tools/generate_board_ve.py` 整文件重写(读 dts 而非 dist+template)
- `../tools/build_bitstream.sh` Step 0 改成读 `AGM_DTS`
- `.gitignore` 注释更新(说明生成源改成 dts)

## 12. `west build -t bitstream` — Quartus 之后接 Supra

前面 §3 走完 `west build -t logic` 得到 `<build>/logic/board.qsf`,把
`logic/` 拿到 Quartus 安装版上跑 `quartus_sh -t af_quartus.tcl`,产物
`simulation/modelsim/board.vo` 落回 `<build>/logic/`。Plan A(2026-09)新增的
`west build -t bitstream` target 接住 Quartus 之后的最后一步:

```bash
west build -d /tmp/b_x -t bitstream -b agrv2k_407
```

跑 `tools/compile_bitstream.sh <build>/logic <build>/zephyr/board.bin`,内容:
读 `simulation/modelsim/board.vo`,调 SDK `af_cmd -B --mode QUARTUS` 走 Supra
`gen_logic.tcl`,产物 `<build>/logic/board.bin`(99944 B,二进制非确定性 ——
2 次 compress/route 结果 md5 不同,但都 boots OK,见 `../tools/compile_bitstream.sh`
头注),然后 `cp` 到 `${AGM_BITSTREAM_BIN}`(默认 `<build>/zephyr/board.bin`)。

`AGM_BITSTREAM_BIN` 的查找链在 `boards/agm/agrv2k/shared/board_common.cmake` 维护
(2026-09 Plan A 之后版本),默认 `${CMAKE_BINARY_DIR}/zephyr/board.bin`,所以
`west build -t bitstream` 的最终产物就在 build dir 里,跟 `zephyr.bin` 同目录。

### 12.1 用法

```bash
# Day 0 / 改 dts / 改 pin map
$ west build -b agrv2k_407                      # firmware
$ west build -t logic                            # dts -> board.qsf (本机,亚秒)
# [工作站] cd <build>/logic && quartus_sh -t af_quartus.tcl
$ west build -t bitstream                        # Supra -> board.bin (本机,~5 s)
$ west flash --runner agrv_openocd              # firmware + 比特流一起写

# 日常 firmware 迭代
$ west build -b agrv2k_407
$ west flash --runner agrv_openocd              # 默认两个都写
$ west flash --runner agrv_openocd --skip-bitstream  # 只换 firmware,跳过 board.bin 写

# 日常比特流迭代(已经 Quartus 走过 logic/)
$ west build -t bitstream                        # 重 Supra,board.bin 覆盖
$ west flash --runner agrv_openocd --bitstream-only  # 只换比特流,firmware 不动
```

### 12.2 Quartus 未跑 / 跑失败的失败模式

`compile_bitstream.sh` 的第一件事是检查 `simulation/modelsim/board.vo` 在不在:
不在就 exit 5 + "go run quartus_sh -t af_quartus.tcl first" 的明确提示。
所以 `west build -t bitstream` 第一次跑几乎一定 exit 5 —— **这是预期失败**,
不是 tool 坏。`west build -t logic` 跟 `west build -t bitstream` 是两个独立 target,
前者纯本机、后者要求工作站 Quartus 步骤走过。

### 12.3 跟 elink sample / elink_monitor 的关系

`west build -t bitstream` 不依赖 `west build`(firmware)target 跑过;它只读
`logic/board.vo`。所以"只想重新 Supra 一下固件先不动"的场景下,build dir 可以没有
`zephyr.bin`,直接 `west build -t bitstream` 出 board.bin。
但 `west flash --runner agrv_openocd` 默认两个都写 —— board.bin 缺就降级为
firmware-only + WARNING(Plan A 的 graceful degradation,见 [FLASH.md](FLASH.md) 附录 §3 B1)。
