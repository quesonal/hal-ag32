# 烧录与串口抓取:调试说明

> **布局已更新**：本文件里的 `0x800b0000` / `0x8007e000` / `0x800c9000` 是改动**前**的地址
> （比特流 slot 1 现为 `0x800b4000`、执行槽 `0x8007c000`、bind-salt 现为 `0x800b0000`）。这里保留当时的实测记录，
> 权威地图与新旧对照见 [`FLASH-LAYOUT.md`](FLASH-LAYOUT.md)。
> **本仓库的开发板操作只有两条路径:烧录和抓串口,**
> 但用错命令的代价很高 —— `west flash` 会**擦掉 FPGA 比特流**,抓取顺序错了
> **永远看不到启动 banner**,时钟配错则**满屏乱码但板子其实是好的**。
> 本文按"先规则、后原理、再决策树"组织,遇到问题直接跳 §6。

相关文档:[`hal_ag32_samples/docs/SAMPLE-WORKFLOW.md`](https://github.com/quesonal/hal-ag32-samples/blob/main/docs/SAMPLE-WORKFLOW.md)(建样例的完整流程)、
[`FLASH.md`](FLASH.md)(烧录原理与探针问题)、
[`BOOT-DFU-STATUS.md`](BOOT-DFU-STATUS.md)(启动/DFU 的现状与待办;含 BOOT0 陷阱)、
[`CDC-ACM-FLAKE-SOLUTIONS.md`](CDC-ACM-FLAKE-SOLUTIONS.md)(串口掉线)。

---

## 0. 六条硬规则(违反任意一条都会浪费一整轮)

1. **板级默认 runner 就是 `agrv_openocd`**:`west flash` 默认写
   firmware + bitstream(Plan A,2026-09)。
   它把 `flash write_image erase` 的 sector 限定在固件覆盖的 4 KB page,接着走
   `post_verify` 把 `<build>/zephyr/board.bin` 写到 `0x800e7000`。只写固件加
   `--skip-bitstream`,只写比特流加 `--bitstream-only`(互斥)。详细心智模型见
   [FLASH.md](FLASH.md) 的附录(Plan A)§5。**这三个 flag 都只属于 `agrv_openocd`**:没有探针时改走
   ROM bootloader `west flash --runner agrv32flash`(UART,只写固件;既没有
   `--bitstream-only`,也不会去 build dir 找比特流)。**旧的** `../tools/flash_fw.sh` / `../tools/flash_logic.sh` /
   `west build -t flash` / `-t flash-logic` 标 deprecated(直接调用仍能用,但
   west target 已删)。
2. **抓 banner 必须在 reset 之前把串口打开。** 用 `../tools/test_uart_capture.sh`;
   用 `minicom` / `screen` / `picocom` / `cat /dev/ttyACM0` 只能看到周期性
   输出,看不到启动 banner(它是一次性的,在 `reset run` 后几毫秒内打完)。
3. **比特流用 canonical 那份,固件时钟必须与它一致。**

   **开发阶段唯一的 canonical 比特流**(回归测试、驱动开发、sample 设计都用它,只有特殊
   需要才换别的):

   ```
   <your canonical bitstream>
   <your canonical bitstream>             ← 仓库内副本(同一 md5,随 .ve 一起)
   md5 6378549f3a8f82dd386353077f3d4a02   99944 B   SYSCLK 200 / BUSCLK 100 / HSECLK 8
   ```

   文件名是厂方给的 `example_board.bin`;只有**本仓库 build 树的产物**才叫
   `board.bin`(Quartus `TOP_LEVEL_ENTITY "board"`)。两者不是同一个名字。

   它与 407 板级默认(`AGM_SYSCLK_HZ` / `AGM_FLASH_MAX_HZ` = 200/100 MHz)**一致**,
   所以构建**不需要**任何 overlay。读回 `0x800e7000` 起的 100000 B 与这个文件
   逐字节相同(板上就是它);它的 `.ve` / `.v` 与 `tools/tests/fixtures/`
   里那份也逐字节相同(那是 pin-routing 工具的真实 fixture)。

   **用法**(Plan A 之后):

   * `AGM_BITSTREAM_BIN=<your canonical bitstream> west flash -d <build>`
     (直接指过去,免 cp。**是环境变量前缀,不是 `west flash -D...`** —— `west flash`
     没有 `-D` 参数,`-DAGM_BITSTREAM_BIN` 会被当成 runner 的未知参数报错;runner 会用它
     改写 `--cmd-post-verify` 的比特流路径,所以已配置好的 build dir 也不用重新 configure。
     用 `$HOME` 不用 `~`,shell 不展开 `=~/x` 里的波浪号);
   * 或 `cp <your canonical bitstream> <build_dir>/zephyr/board.bin`
     之后 `west flash -d <build_dir>` 走默认路径。

   **`example_board.bin` 这个旧命名** 现在不用了:Plan A 之前默认 fallback 是
   `<build_dir>/zephyr/board.bin`,与 Quartus project 命名 `board\` 对不齐;现在的
   `west build -t bitstream` 产物固定叫 `board.bin`(TOP_LEVEL_ENTITY=board)。

   **自己综合比特流时(改了引脚或加了 fabric IP)把 `<build>/logic/` 原样拿上
   Quartus 机器。** `board.v` 与 `board.vex` 是**同一次 `gen_vlog` 的两半**:在那个
   目录里再跑一遍 SDK 的 prepare logic 会让同一根引脚有两个名字(约束改按 MCU
   功能名写,`SPI0_SI_IO0 PIN_92` 对不上网表里的端口名 `si_io0`),而 **Quartus 不报错、
   Supra 只打 `Unrecognized pin … or location …` 然后把这些 IO 摆到别的引脚上**
   (实测,板上就是 SPI 读不到 flash、`RDID = 00 00 00`)。
   `../tools/build_bitstream.sh` 的 Step 1c 会在生成时挡住这种"约束指名不到端口"(exit 8,
   `AGM_ALLOW_VEX_PORT_MISMATCH=1` 可降级成 warning)。两个 IP 要放一个 wrapper、以及
   用户逻辑区只有 4 块 M9K 的预算,见 [`CUSTOM-IP.md`](CUSTOM-IP.md) §1.5。

   **换成别的比特流时**(例如两份 100 MHz 的 `~/spi_full_mac_bitstream/` 或
   `spi_full_bitstream*`):407 板级默认是 200 MHz,比特流若把 PLL 设成 100 MHz,不带
   overlay 构建出来的固件在 115200 下**必然是乱码**(实际波特率 57600),这时要带
   `-- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay`。**先确认频率,别假定**
   (两次构建 + 两次抓取,30 秒):同一份 `hello_world` 各编一次(不带 / 带
   `/tmp/agm100.overlay`),各抓一次 banner,`printable_ratio` 高的那个就是板上比特流的
   频率。实测canonical 那份是 200 MHz:不带 overlay 的 `hello_world`
   `printable_ratio = 100%`,带 overlay 的同一份是 50%(GARBAGE)。

   `/tmp/agm100.overlay` 是**不落库**的临时文件(重启/清理就没了),用
   `bash tools/make_agm100_overlay.sh` 重新生成;脚本里写清了它改的四个属性。
4. **AG32 控制台是 `/dev/ttyACM0`。** `/dev/ttyACM1` 是板上那颗 ESP32-C3,
   跟本仓库无关(不要对它用 `esptool` / `idf.py`)。
5. **所有 west / 工具命令之前先激活你自己的虚拟环境**:`source <your-venv>/bin/activate`。
   里面要有 `west`、`pyserial`、`pyusb`、`pyyaml`、`devicetree`、`pytest`;系统 `python3`
   缺这些模块,而且失败是**静默**的。
6. **收尾把开发板恢复成 `hello_world`**,并抓一次确认。别把开发固件留在板上。

---

## 1. 先决条件(失败一大半出在这里)

| 项 | 要求 | 检查方法 |
| --- | --- | --- |
| 虚拟环境 | 已 `source` 仓库 venv | `echo $VIRTUAL_ENV` 非空 |
| 探针 | 板载 CMSIS-DAP(`AGM CMSIS-DAP`)插好 | `lsusb` 里能看到 |
| 串口 | `/dev/ttyACM0` 存在 | `ls -l /dev/ttyACM0` |
| 厂商 SDK | `AGRV_SDK_PATH`(默认 `~/AgRV_pio`)指向真的 PlatformIO AgRV 安装 | `ls $AGRV_SDK_PATH/platforms/AgRV/etc` |
| OpenOCD | 默认 `$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin/openocd_cmd` | 脚本自己找;找不到会明确报错 |
| per-board cfg | `<build>/logic/openocd.cfg` —— **构建产物**,源码树不留副本(见 §9.8) | 抓取/烧录脚本按 `AGM_BUILD_DIR` → `~/zephyrproject/build*/logic/openocd.cfg` → `/tmp/b_*/logic/openocd.cfg` 顺序找;一个都没有时报 `openocd cfg not found` 并提示改用 `AGM_BUILD_DIR=<任一该板的 build 目录>` 或 `AGM_OPENOCD_CFG=<cfg 路径>`,或者先 `west build` 一次 |

> 这条 咬过一次:`/tmp` 里的 build 目录被清理后,`../tools/test_uart_capture.sh` 回落到源码树
> 的 `boards/agm/<board>/support/openocd.cfg`,而那份**早就不再生成到源码树**,
> 于是只看到一句"not found"。现在脚本会打印 `openocd_cfg = …`(真正传给 openocd 的那份)并
> 给出上面的两个变量;另外"只抓串口 + 机器上没有 SDK"这条路径会用 `../boards/agm/agrv2k/shared/support/agrv2k-minimal.cfg`,
> 本来就不需要 per-board cfg(此前它被这条检查挡死,现已修正)。

板子上的东西分两块,地址不同、脚本不同 —— 这是"烧错了"的根源:

| 区域 | 地址 | 写入内容 | 脚本 |
| --- | --- | --- | --- |
| MCU 固件 | `0x80000000` 起 | `zephyr.bin` | `west flash`(默认 runner `agrv_openocd`) |
| FPGA 比特流 | `0x800e7000` 起 | `board.bin` | `west flash --bitstream-only`(或同命令默认一起写) |

---

## 2. 命令选择表

| 目的 | 正确命令 | 不要用 |
| --- | --- | --- |
| 烧 firmware + 比特流(默认) | `west flash -d /tmp/b_x` | `west flash --runner openocd`(vanilla = 固件 + 比特流同 openocd 会话) |
| 只写固件(保留比特流) | `west flash -d /tmp/b_x --skip-bitstream` | 不传 flag(默认两个都写) |
| 只写比特流 | `west flash -d /tmp/b_x --bitstream-only` | `west flash --runner openocd` 不带 firmware 命令(罕见) |
| 只写比特流(临时换一份) | `AGM_BITSTREAM_BIN=<bin> west flash -d /tmp/b_x --bitstream-only` | `cp` 到 `<build_dir>/zephyr/board.bin`(Plan A 之后工具不再找那条 fallback) |
| 写产线的**绑定盐**(4 KiB 扇区) | `tools/agm_bind.py provision --salt-file <salt.bin> --uid <32 hex>`(SWD:先读回扇区 → 写 → 再读回校验;默认拒绝覆盖另一份盐) | 手搓 openocd `flash write_image`(**少了 `reset init` 会把 flash 控制器卡死**,见本文 §3) |
| 只写不擦(Program-only) | 工具链没有等价物 —— 旧 `flash_fw.sh -n` 要求目标页已是 0xFF;改用 sector-erase 后的重新 verify_image 路径,或跑一次 `flash_fw.sh <bin>`(默认就有 erase) | —— |
| 出比特流(Quartus 之后) | `west build -d /tmp/b_x -t bitstream -b agrv2k_407`(等价于 `tools/compile_bitstream.sh <logic_dir>`);Quartus 侧要把 `logic/` **原样**综合,只要 `simulation/modelsim/board.vo` 回传 | `west build -t bitstream`(vanilla Zephyr 没这 target);在 `logic/` 里重跑一遍 SDK prepare logic(`board.vex` 会换成另一种 flavor,§0 规则 3) |
| 烧固件 + 抓串口 | `bash tools/test_uart_capture.sh -t 30 /tmp/b_x/zephyr/zephyr.bin` | `minicom` + 手动 reset |
| 只抓串口(不烧) | `bash tools/test_uart_capture.sh`(不带固件参数) | `cat /dev/ttyACM0 &` + reset |
| 纯 reset | `bash tools/openocd_reset_run.sh [board]` | 手动按复位键(会漏 banner) |
| 看 semihosting / 前台 openocd | `bash tools/openocd_monitor.sh [board]` | —— |
| UART 硬件自检 | `bash tools/test_uart1_loopback.sh`(需 PIN_66↔PIN_67 跳线) | —— |
| 看上一次抓到的内容 | `strings "$(ls -t /tmp/uart_capture_agrv2k_407_*.bin \| head -1)"` | —— |
| 编译一个样例 | `west build -d /tmp/b_x -b agrv2k_407 --pristine=auto <sample>`(canonical 比特流 200 MHz,不用 overlay;换 100 MHz 比特流才加 `-- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay`) | 把 CMake 参数写在 `west build` 后面(没有 `--`) |

**比特流只能是 99944 B。** `west flash` 的 runner 会检查尺寸,不是 99944 B 一律拒绝:
那个尺寸只可能来自压缩或加密(`board_logic.compress` / `board_logic.encrypt`,或厂商 boot
的内置 logic),"这是压缩比特流"从文件头看不出来(两种都以 IDCODE/USERID 开头),而我们的
FCB 驱动只会原样流 24986 个 32-bit 字 —— 写进去 = fabric 变砖、CPU 时钟一起没。确实要写
就 `AGM_BITSTREAM_ANY_SIZE=1`(后果自负:恢复要 BOOT0 + §10)。
同一条检查也在 `tools/agm_oo.sh bitstream <bin>` 里(那是走 SDK `oo` 的写法,§10.1 用得上)。

**`west build` 的参数分隔**:`--` 之后才是 CMake 选项。多个 overlay 用 `;`
分隔并整体加引号:

```bash
west build -d /tmp/b_x -b agrv2k_407 --pristine=auto "$HAL_AGM/samples/hello_world" \
  -- -DEXTRA_DTC_OVERLAY_FILE="/tmp/agm100.overlay;/tmp/other.overlay"
```

### 2.1 工具对照:出问题先跑哪个

这一节是给**外部贡献者**的入口 —— 本仓库的开发板工具都在 `tools/`,
每个只管一件事;下面的表把"现象"直接映射到"第一条命令"。

| 现象 | 第一条命令 | 为什么是它 |
| --- | --- | --- |
| 板子还活着吗?上面跑的是什么? | `bash tools/test_uart_capture.sh -t 30` | 只 reset + 抓;看到 banner / tick 心跳 = CPU 在跑 |
| 串口一个字节都没有,但探针还在 | `bash tools/probe_state.sh` | **只读**:USB/tty、option bytes、SWD 设置、FCB STAT / APB_CLKENABLE / RST_CNTL,不改板子 |
| 探针像死了(`unable to find CMSIS-DAP`、AP 全 stall) | `bash tools/probe_recover.sh`(必要时 `--options-erase`) | 清 stale usbfs claim + warmup;仍不行才给 ROM bootloader 兜底(`BOOT0` 高 + `BOOT1` 低,**上电**,见 §10) |
| `cannot erase block 0 since it is protected` | `bash tools/agm_oo.sh info` → `bash tools/agm_oo.sh options-erase` | vendor `oo` 是唯一能读/擦 flash 控制器 option / WRPR 寄存器的路径;擦完必须重写比特流 |
| 同一条命令时好时坏 | **原样重跑** | 探针侧 flake,不是配置问题:工具内置重试(`AGM_OPENOCD_ATTEMPTS`,默认 3,`../tools/flash_logic.sh` 为 4) |
| 想让构建看到**未提交**的改动 | 把你编辑的那份 checkout 同步进 `<ws>/modules/hal_ag32`(rsync;`../tools/devsync.sh` 是可选的助手) | 粗心地在两份 checkout 里各改一半 —— 构建只读 workspace 这一份 |
| 要重新生成比特流工程 | `west build -b agrv2k_407 -t logic` | 综合/编译在 Quartus 工作站做;完成后 `west build -t bitstream` 跑 Supra + 落到 `<build>/zephyr/board.bin`,再 `west flash` 写两个 |

---

## 3. 被禁用的命令,以及它们真实造成的后果(实测)

| 错误命令 | 后果 | 恢复 |
| --- | --- | --- |
| `west flash`(Plan A 之前) | `flash write_image erase` + `--skip-bitstream` 默认 = 固件烧了但比特流被擦(或者根本看不到 board.bin)。症状:固件看着烧成功,但 LED 不亮、串口无输出、SPI/I2C 全废 —— FPGA fabric 没配置 | Plan A 已修:现在默认两个都写;真被擦只可能发生在用 `WEST FLASH_RUNNER` 显式选了 vanilla `openocd` 而非 `agrv_openocd`。补比特流:`west build -t bitstream`(需 Quartus 已走过 logic/) |
| `west flash --runner openocd` | 同上(bank erase) | 同上 |
| `west flash --runner agrv32flash` | 走 ROM bootloader 的整片路径,需要 `tool-agrv_flashloader`;固件迭代不需要它,也不该拿它代替 `../tools/flash_fw.sh` | 正常烧完即可,但别用它做日常迭代 |
| `minicom` / `screen` / `cat` 抓 banner | banner 在 reset 后几毫秒内打完,端口后开就永远错过;只能看到周期性 printk | 改用 `../tools/test_uart_capture.sh` |
| 在 `/dev/ttyACM1` 上抓 | 那是板上 ESP32-C3,不是 AG32 控制台 | 用 `/dev/ttyACM0` |
| 不带 overlay 构建,而板上比特流是 100 MHz | 115200 全是乱码(实际 57600),**板子和固件其实都是好的** | 带 `-DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay` 重建;先按 §0.3 的两抓法确认板上频率(实测本机板卡是 **200 MHz**) |
| 不激活 venv 就跑工具 | 脚本静默失败,或报找不到模块(系统 python 缺 `pyusb` / `devicetree`) | `source <your-venv>/bin/activate` |
| 用同一 build 目录反复改 overlay | CMake 缓存可能留着旧 DT | 加 `--pristine=auto` |
| 跑 `spi_flash_rw` 写板上 flash | 会真的擦写那颗粒;若板子从它启动则可能变砖 | 只想验证通路时用只读的 `spi_flash_id` |

---

## 4. 乱码:最常见的"看起来是坏的,其实是好的"

**症状**:`../tools/test_uart_capture.sh` 判为 `GARBAGE`,`printable_ratio` 只有 ~25%;
但换成 57600 读就清楚了 —— 这说明**实际时钟是 devicetree 声明值的一半**
(分频器按 200 MHz 算,实际跑 100 MHz)。

**判定(30 秒)**:

```bash
for b in 115200 230400 57600 38400; do
  stty -F /dev/ttyACM0 $b raw -echo
  timeout 2 cat /dev/ttyACM0 > /tmp/cap_$b.bin
  printf 'baud=%s size=%s printable=%s\n' "$b" \
    "$(stat -c%s /tmp/cap_$b.bin)" \
    "$(LC_ALL=C tr -dc '[:print:]\n' < /tmp/cap_$b.bin | wc -c)"
done
```

哪个波特率下 `printable` 接近文件大小,实际时钟就是
`声明值 × (该波特率 / 115200)`。

**处置(二选一,必须成对改)**:

* 固件侧(推荐,开发板比特流不动):重建时带
  `-- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay` —— 它把 `&clk0`、`&cpu0`、
  `&sys.flash-max-frequency` 一起改成 100 / 100 / 50 MHz;
* 比特流侧:重新出 200 MHz 比特流,并同步改板级 `AGM_SYSCLK_HZ` /
  `AGM_FLASH_MAX_HZ`。

> 只改一边的后果:UART 分频算错(乱码)、内核 tick 漂移、FLASH 超频。
> 板级那几个数字与比特流频率是**同一个 truth**,必须一起动。

固件自己也会报时钟:启动 banner 之后会打印从 DT 读出的
`SYSCLK: N MHz (clk0), cpu0 N MHz` —— 抓到时先看这一行。

---

## 5. 抓取的正确顺序与判读

`../tools/test_uart_capture.sh` 内部顺序是:
探针 warmup → 打开 `/dev/ttyACM0` 读线程 → (可选)擦固件区 + 写 + verify →
`reset run` → 收满窗口 → 分析。三个前提它都替你处理了:

1. 端口必须在 reset **之前**打开;
2. `openocd` 不能把 CPU 停在 halt(它用 `reset run`,不是 init 后停住);
3. `AGRV_SDK_PATH` 必须有效(要 source 厂商 cfg)。

参数:`-b <board>`(默认 `agrv2k_407`)、`-t <秒>`(默认 10)、
`-o <文件>`(默认 `/tmp/uart_capture_<board>_<时间戳>.bin`)、
`-d <设备>`(默认 `/dev/ttyACM0`)、`-n`(**不擦**固件区,仍然写)。

> **带固件参数时窗口要留够,用 `-t 30`。** 这次抓取里的"reset"发生在
> `erase + write + verify` **之后**,所以 banner 是压着会话末尾打出来的。
> 实测:同一份 `hello_world`,`-t 10` 只抓到 tick 心跳、漏掉
> banner;`-t 30` 完整抓到 `*** Booting Zephyr OS ...`。

**没有 `--no-flash` 这个参数**;"只抓不烧"的正确写法是**不带固件参数**。

退出码:

| 码 | 含义 | 下一步 |
| --- | --- | --- |
| 0 | 抓到 banner / 预期文本,可打印率健康(≥90%) | 看内容 |
| 1 | 一个字节都没抓到 | §6 第 1 行 |
| 2 | 抓到乱码(波特率/帧不符) | §4 |
| 3 | 前置检查或烧录步骤失败 | 看脚本日志里的第 4 段 |

---

## 6. 故障决策树

| 症状 | 最可能原因 | 判定 | 处置 |
| --- | --- | --- | --- |
| 抓到 **0 字节** | 端口被别的进程占用;探针没 warmup;CPU 停在 halt | `lsof /dev/ttyACM0`;看脚本 warmup 段是否成功 | 关掉 minicom/screen;重插 USB;重跑(脚本自带 warmup) |
| 抓到**乱码**(~25% 可打印) | 固件 / 比特流时钟不匹配(板上不是 canonical 那份?) | §4 的波特率扫描 | 先 `ls -l <your canonical bitstream>` 确认是 canonical;若是 100 MHz 比特流,带 `agm100.overlay` 重建 |
| 抓到**旧固件的输出**(比如 tick 已几百秒) | 这次烧录没生效 | 看脚本第 4 段有没有 `wrote N bytes ... verified N bytes` | 重跑;确认传进去的是**刚编出来的** `zephyr.bin` |
| 抓到 **tick 心跳但没有 banner** | 不是故障:带固件参数的抓取里,reset 发生在擦写之后,窗口太短就错过 banner | 看 `-t` 值 | 用 `-t 30`(实测:`-t 10` 只有 tick,`-t 30` 有 banner) |
| 一次**失败/超时**的烧录会话之后,后续抓取一直 **0 字节** | 会话中途出错,CPU 被留在 halt,`reset run` 不解除 | `bash tools/probe_state.sh`;UART 持续 0 字节 | 重新烧一次:`west flash -d <build> --runner agrv_openocd` 或 `bash tools/agm_oo.sh fw <bin>`(实测能重新启动);必要时 `west build -t bitstream` + `west flash` |
| 烧完 LED / 串口**全死** | 比特流被 bank-erase(Plan A 之前用了 `west flash --runner openocd` 默认 runner) | 比特流区读回不是有效比特流 | `west build -t bitstream`(需 Quartus 走过 logic/);或直接 `AGM_BITSTREAM_BIN=<your canonical bitstream> west flash --runner agrv_openocd` |
| `west flash` 报找不到 runner / cfg | `AGRV_SDK_PATH` 没设或 SDK 没装 | `echo $AGRV_SDK_PATH` | 设好环境;或走 `../tools/flash_fw.sh` 直接调(DEPRECATED 但能用) |
| openocd 报 `unable to find CMSIS-DAP` | 探针掉了 / CDC-ACM flake | `lsusb` | 拔插 USB;再试 `tools/openocd_warmup.py` |
| 链接期 `multiple definition of '__device_dts_ord_N'` | 同一 DT 节点被两个驱动绑定 | 报错会给出两个文件路径 | 检查节点 `compatible`:一个节点只能有一个可用的驱动 |
| `devicetree error: ... not declared in 'properties'` | 用了别家 compatible,节点上却带着 AGM 专有属性 | 报错会点名属性和 binding | 别混用 compatible;按 binding 改节点属性 |
| 103/303/test 上某些样例 build error | 那些样例带 `boards/agrv2k_407.overlay`,在别的板上不生效 | 报错指向缺失节点/属性 | 属预期,不是环境问题 |
| openocd 报 `stalled AP operation`(且 `init` 之后的命令都没执行) | CPU 没在跑:时钟由比特流提供,而 fabric 被拆了 | DPIDR 可读、AP 全 stall、UART 0 字节、`agrv32flash` 也不应答 | 见 §10:用 ROM bootloader 重写固件(不需要 SWD;`BOOT0` 高 + `BOOT1` 低,**上电**) |
| 同一条命令时好时坏(`Examination failed`、`dmstatus=0x0`) | 会话层的偶发失败 | — | **原样重试**(工具已内置整会话重试);仍失败再确认 10 MHz + CMSIS-DAP v2,或按 §10 走 ROM bootloader |
| `cannot erase block 0 since it is protected` | openocd 驱动读不到 flash 控制器的 WRPR(0x81000020 不在系统总线上),于是误判整片保护 | `tools/agm_oo.sh info` 会显示 `write protection register = 0x0` | `tools/agm_oo.sh options-erase`,然后重写比特流 |
| RDID 读回全 `00` / 全 `FF` | 比特流没把 flash 的 MISO 送回引擎 / SPI 上确实没有器件 —— **但也可能是启动方式不对**(实测:同一个固件从 ROM 的 `-g`(GO)**启动时 SPI 全 00**(`rc=0`),正常上电启动立刻读到 `68 40 15`) | 换比特流对比,或看 [`CUSTOM-IP.md`](CUSTOM-IP.md) §1.5 的比特流对照表。**若是自己综合的比特流**,再查两条(实测都是独立致命):wrapper 里有没有 case-A 的"读回行"(`gpio0_io_in` 是不是被接成 `1'b0`),以及 Supra 日志里有没有 `Unrecognized pin …`(约束与网表端口名不成对) | 先确认是正常上电还是 ROM-GO 起来的;前者才可信。再换 `spi_full_bitstream_97pad` 或 canonical 那份 `board.bin`;全 FF 是 `_without_flash` 的预期。自己综合的那两条见 [`CUSTOM-IP.md`](CUSTOM-IP.md) §1.5 与本文 §3。细节见本文 §11|
| openocd 每次 examine/reset 都报 `invalid command name "mmw"` | 已知缺陷:`boards/agm/agrv2k/shared/support/agrv2k-minimal.cfg:66` 的 `examine-end` 事件用了这个 openocd 构建里**不存在**的 `mmw` | 报错紧跟 `Examination succeed` / `reset run` | 只影响那条附带的 "DBG_CNTL \|= IWDG stop"(`reset run` 会在事件里报错,但复位本身仍发生);`mww` / `read_memory` 可用,需要时按它们自己读写。修不修是待定项 |
| 串口出现 `the fabric slot in the record was refused (...)` | 比特流槽没通过验签(`CONFIG_BOOT_AGM_BITSTREAM_SIGNED` 的构建):容器坏了、被改过、或者与记录不符 | 这一行就在 banner 之前(`boot_agm_init()` 打的);括号里是原因 | 重新上传一份签过名的比特流容器(`tools/sign_image.py --bitstream` → `agm_upload.py <port> bitstream …`);只想回 factory 就把记录擦空:32 B 的 `0xff` 写到 `0x800e6000`(`agm_oo.sh fw`) |
| `agrv32flash -S 0x800b0000:64 -r f` 在比特流窗口上报错(其它窗口正常) | 预期行为:比特流窗口的读只放行**本次会话写过**的那段,以免一把 UART 把厂商比特流倒出去 | 控制台的会话小结里有 `… N refused reads` | 同一次会话里写完再回读(`-w … -V`,厂商工具的校验形状)照常工作;要整段回读就打开 `CONFIG_BOOT_AGM_AN3155_READ_FABRIC=y` 重烧 |
| 上传一个**更旧**的签名镜像 → `loader NAK (code 5: image is older than the one installed)` | 防回滚(SIGNED-IMAGES-PLAN §10):record 里存着"已接受的最高版本"(控制台 `floor : vX.Y.Z`) | loader 会说 `refusing an older image (vA < vB already installed)` | 签一份更高版本的镜像(`sign_image.py --version`);`erase` **不会**清掉这层下限,只有整片擦(SWD/`erase-all`)才会 |
| 上传一个签名容器,loader 明明拒绝、`../tools/agm_upload.py` 却报 `done in Xs` | 旧版工具的 ACK 检测撞上 loader 输出文本里的 `0x79`(字母 `y`) | 用 `--verbose` 看 loader 的原话 | 已修:现在 NAK 优先判定;若仍在旧工具上,加 `--verbose` 看清 |

---

## 7. 收尾(每次开发板工作的最后一步)

```bash
# 把板子恢复成 hello_world,并抓一次确认(期望 exit 0 + tick/led 心跳)
bash "$HAL_AGM/tools/test_uart_capture.sh" -t 8 /tmp/b_hello/zephyr/zephyr.bin

# 确认比特流还是 canonical 那份(200 MHz → 该固件不带 overlay 构建)
ls -l "<your canonical bitstream>"
```

---

## 8. 速查卡片(可直接复制)

```bash
# 环境
source <your-venv>/bin/activate
export HAL_AGM=$HOME/zephyr-hal-ag32
export WS=$HOME/zephyrproject

# 编译(canonical 200 MHz 比特流:与板级默认一致,不用 overlay)
west build -d /tmp/b_x -b agrv2k_407 --pristine=auto "$HAL_AGM/samples/hello_world"

# 换成 100 MHz 比特流时才加(注意 `--` 之后才是 CMake 参数):
#   -- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay

# 烧固件 + 抓 30 秒
bash "$HAL_AGM/tools/test_uart_capture.sh" -t 30 /tmp/b_x/zephyr/zephyr.bin

# 只抓(板子上已有固件)
bash "$HAL_AGM/tools/test_uart_capture.sh" -t 5

# 补比特流(canonical;env 前缀,不是 west flash -D...)
AGM_BITSTREAM_BIN="<your canonical bitstream>" \
  west flash -d /tmp/b_check --runner agrv_openocd

# 看最后一次抓到的文本
strings "$(ls -t /tmp/uart_capture_agrv2k_407_*.bin | head -1)"
```

---

## 9. 容易误判的几件事

现场卡住时先对照这几条,它们比"接着调参数"省时间:

* **先确认"代码路径有没有被走到",再调参数。** 最典型的是 `CONFIG_SPI_EXTENDED_MODES`
  没开:此时 `spi_operation_t` 是 `uint16_t`,`SPI_LINES_*`(bit 16/17)**在编译期就被
  截断**,驱动的多线分派永远不成立 —— 请求静默走单线、返回 0、数据是 `0xff`。
  C 里参数被静默截断不报错,加一行临时 `printk` 或看宏展开最省事。
* **别把软件 bug 判成硬件限制。** 在说"比特流做不到"之前,先 `grep` **post-route** 网表
  (而不是只看顶层 `.v` 的端口表),并跑一遍**已知可用**的参考用例 —— 两样都做过再下结论,
  并把比特流文件名与 mtime 一起记下来。
* **`west build` 不是回归网。** twister 额外开 `CONFIG_COMPILER_WARNINGS_AS_ERRORS=y` 与
  `--edtlib-Werror`;改了驱动/样例先跑 twister,它常当场报出 `west build` 只 warning 的问题。
* **样例要有判定输出,数据前提要非空。** 读擦除后的区域等于读 `0xff`,"全 0xff 全一致"
  什么都不证明 —— 输出 `PASS`/`FAIL`/`INCONCLUSIVE` 三者之一,新样例至少一个正例一个反例。
  可照抄 `samples/spi_quad_read`:先扫首扇区找一个非空的 16 字节窗口,再做单线/多线三方比对。
* **结论只在验证之后写。** 文档里写"实测"必须能给出开发板输出或寄存器读数;只靠读代码得到的
  判断写"推断"。撤回既有结论前,把那个实验重跑一遍 —— 重跑三分钟,写错方向一天。
* **不要手改工作区里的模块 checkout。** 只改你自己的编辑树再同步(见
  [`hal_ag32_samples/docs/SAMPLE-WORKFLOW.md` §0.1](https://github.com/quesonal/hal-ag32-samples/blob/main/docs/SAMPLE-WORKFLOW.md) 的 `devsync.sh`),否则"编出来的
  二进制"和"仓库里的源码"不一致,排查时对着源码找不到现象。
* **并行 twister 偶发假失败。** 曾因并行任务抢写源码树生成物导致孤立的 CMake 失败 ——
  看到孤立失败先单跑该用例复验,再怀疑代码。

## 10. 救砖:SWD 连不上(`stalled AP operation`)

**症状**(实测):

```
Info : SWD DPIDR 0x2ba01477
Warn : Connecting DP: stalled AP operation, issuing ABORT
```

openocd `init` 在这里失败 → **整个脚本被中止,后面所有 `-c` 命令都不会执行**,于是
`west flash` / `../tools/test_uart_capture.sh` 全部失败(典型症状:`FATAL: no Zephyr virtualenv active`)。

> **更正:先查 SWD 配置,再怀疑板子。** 同一天把同一块板接到 Windows 上用
> PlatformIO 烧写,`examine` / `erase` / `write` 一次全过;对比两边的 openocd 调用,真正的
> 差异是 SWD 配置:
>
> * **速率**:PlatformIO 传 `variable ADAPTER_SPEED 10000`,而 SDK cfg 默认 15000(本探针
>   量化成 **14285 kHz**);
> * **后端**:必须用 **CMSIS-DAP v2(bulk)**。我们原来的 `../tools/flash_fw.sh` / `../tools/flash_logic.sh`(现在的 runner 路径用同一套工具)
>   直接 `-f agrv2k.cfg`,跳过了 board cfg 里的 `cmsis-dap backend usb_bulk`,于是落到
>   v1 HID 后端。
>
> 两条都修好之后(commit `9875530`)同样的操作在 Linux 上一次跑通。所以遇到本节症状
> **先确认这两条**(`AGM_SWD_SPEED=<kHz>` 可覆盖速率;工具现在默认走 board cfg),
> 再考虑下面"CPU 没在跑"的解释 —— 后者是当时的读数,已被上述反例削弱,属于
> **待验证的假设**,不是结论。
>
> 对应的工具:`../tools/probe_state.sh`(只读,一次打印 USB/串口、option bytes、当前 SWD
> 配置、FCB STAT/APB_CLKENABLE/RST_CNTL)、`../tools/probe_recover.sh`(清 host 端 usbfs
> claim + warmup;`--options-erase` 再走 §10.1 那步)。
>
> **先重试再说板子坏了。** 这条链路上的工具都自带整会话重试(`AGM_OPENOCD_ATTEMPTS`,
> 默认 3;`../tools/flash_logic.sh` 为 4),报错时原样再跑一遍通常就好了;重试预算用尽再往下查
> (先确认 10 MHz + CMSIS-DAP v2,再走 ROM bootloader)。

**先别怪探针**。这组现象的含义是 **CPU 没有在跑**:

| 观察 | 含义 |
|---|---|
| `DPIDR` 能读 | SWD 物理层与调试域正常(SWD 时钟来自探针) |
| 所有 AP 访问 stall | AP 需要内核/总线时钟 —— 时钟没了 |
| `/dev/ttyACM0` 0 字节 | 固件没在跑 |
| `agrv32flash -r` 也不应答 | ROM bootloader 跑在同一个核上,同样没在跑 |

根因通常是"在 CPU 运行时把 fabric 拆了":AgRV2K 的 CPU 时钟本身由比特流产生
(`CLKOUT[0] → sys_clk`),FCB `DEACTIVATE` 或任何重配 fabric 的动作都会当场冻住核。
详见本文 §3。

**为什么拔 USB 不管用**:板上固件若每次启动都再触发一次那个动作,重新上电后几毫秒内又冻住;
目标 rail 是否真的断过电也要单独确认(探针的 USB 不一定是目标的电源)。

**恢复(不需要 SWD)** —— 实测通过,步骤如下:

1. **插上 BOOT0(PIN_94)→3.3V 跳线**(BOOT1 板上已下拉到 GND,即 "BOOT1(低)")。
   厂商的要求就是串口下载时 BOOT0 高、BOOT1 低,并且要先把 boot0 拉高再上电。
2. **让芯片重启一次**,让 BOOT0 被重新采样 —— 复位后第 4 个 SYSCLK 上升沿锁存。两条路:
   * **上电/断电重启**(最稳);
   * 或不拔电:`../tools/probe_reset_target.py`(CMSIS-DAP `DAP_ResetTarget`,真 nRESET 脉冲)
     —— **注意 openocd 的 `reset run` 不算**:SDK cfg 没声明 srst,它是软件系统复位,
    不会重新采样 BOOT 脚(实测 7 次全部无应答),而 nRESET 脉冲实测有效。
     **脉冲之后要立刻**(≲0.2 s)启动下面的工具:ROM loader 的监听窗口很短,隔 1 s 就错过了。
     **"立刻"这件事靠 `agrv32flash` 自己做不到**(复现):它要先起进程再开串口,
     INIT 到的时候 ROM 已经不听 `0x7F` 了,而 ROM 随后停在命令态,于是每个 `0x7F` 都被当成
     命令(NACK),永远 "Failed to init device"。稳的做法是**先开串口、再发脉冲、立刻送第一个
     `0x7F`**(`../tools/rom_opt.py` 的 `Rom.init()`),ROM 进了命令态之后用厂商工具的 **`-c`**
     (resume:不再发 INIT)接着走即可。
3. 用 UART 重写固件:

   ```sh
   AF=$HOME/AgRV_pio/packages/tool-agrv_flashloader/bin/agrv32flash
   $AF -b 57600 -w <zephyr.bin> -v /dev/ttyACM0   # 或 west flash --runner agrv32flash
   $AF -b 57600 -g 0x80000000 /dev/ttyACM0        # 不重启直接跳进新固件(west 的 runner 自动做)
   ```

   会话里会打印设备信息,可用来确认真的进了 loader:
   `Interface serial_posix: 57600 8E1` / `Device : 0x40200001 (AGM32RV40xxx)` /
   `Flash : 0x80000000 1024KB`。
4. 撤掉 BOOT0 跳线、复位 → 正常启动刚写进去的固件。
   (**跳线还插着时,每次复位/上电都会进 loader,固件不会跑**。)

> **状态(实测)**:跳线接上 + nRESET 脉冲后,
> ① `agrv32flash -r` 读 16 B 与 SWD 读回逐字节一致;
> ② UART 写入两个**不同**固件(`hello_world` 23420 B、`tick_busy` 23124 B),
> SWD 回读均逐字节等于各自 build 的 `zephyr.bin`;
> ③ 两次写入后 `0x800e7000` 起的 99944 B 比特流区**逐字节未变**(仍是 canonical
> `6378549f…`)—— UART 这条通路只擦写固件覆盖的扇区;
> ④ `-g 0x80000000` 后两个固件都正常启动(串口各自打出自己的 banner)。
> ⑤ **偶发**:`Failed to init device` / `Wrong checksum …` 出现过,**原样重试即过** ——
> 别把它当成参数问题(工具已内置重试)。

**这条 UART 通路还能做更多**(厂商工具 `agrv32flash` 的其它旋钮):

| 旋钮 | 作用 | 状态 |
|---|---|---|
| `-w <code.bin>` | 写 code(固件) | ✅ 实测(上面那五条) |
| `-l <logic.bin> [-S <addr>]` | 写 logic(比特流) | ⚠️ **实测见下节**:不给 `-S` 会写错地方;**加了 `-S 0x800e7000` 才对**;设不了 opt 指针 |
| `-O` | 擦 option bytes | ⚠️ 未在这条通路上试过(`agm_oo.sh options-erase` 走 SWD/AP);擦完的后果见下节 |
| `-t/-u`、`-j/-k` | 写保护开/关、读保护开/关 | ⚠️ 未实测。`-k`(关读保护)按厂商说法**会触发全片擦除** —— 和 `agm_oo.sh unlock` 同一件事,别当常规操作 |
| `-T <addr>` | 把外部 booter 传进 RAM 再跑 | ⚠️ 未实测(厂商的"二级烧录"入口) |

#### 10.0.1 `agrv32flash -l`(写 logic)实测:能不能不用 SWD 就恢复比特流?

前提:BOOT0 跳线接 3.3V、ROM loader 正常应答(`agrv32flash /dev/ttyACM0` 打出
`Device : 0x40200001 … Flash : 0x80000000 1024KB`)。测试文件就是 canonical `example_board.bin`
(99944 B),先把它在 `0x800e7000` 的头 4 B 用 SWD 改成 `0x00000000`(整扇区被擦),这样"写回去了
没有"是可判定的。

| 命令 | 实测结果 |
|---|---|
| `agrv32flash -l <bin>`(**不给** `-S`) | ❌ 写到 **flash 开头 `0x80000000`** —— 不是 logic 地址。SWD 全片 dump:整个 bin 逐字节出现在偏移 0(**把固件覆盖了**),而 `0x800e7000` 仍是那 4 B 损坏值、opt 的 FPGA 地址也没变。**别这么用** |
| `agrv32flash -l <bin> -S 0x800e7000 -v` | ✅ 正确。输出只有 `Write to memory / Erasing memory / Verified and Done`(不打地址);SWD 回读 99944 B 的 md5 = `6378549f3a8f82dd386353077f3d4a02` = canonical,opt 指针不变 |
| `agrv32flash -w <bin> -S 0x800e7000 -v` | ✅ 一样正确(SWD 回读同为 canonical)。也就是说写比特流**字节**用 `-w` 就够,`-l` 在这块芯片上没提供额外能力 |
| opt 被清掉之后再 `-l <bin> -S 0x800e7000` | ❌ `Error: Got NACK from device on command 0x13.` → `Error: Unable to check option bytes.` → `Error: Failed to write FPGA configuration.`(随后它自己 `Resetting device... Done`)。`-l` 比 `-w` 多一步**读写 option bytes**——它正是靠这步去设 FPGA 地址——而 ROM loader 在这块芯片上不给写 |

**结论**:"BOOT0 + `agrv32flash -l` 就能在完全没有 SWD 的情况下把比特流恢复回去"这条**不成立**。
它能写**字节**,但设不了 **option 里的 FPGA 指针**,而指针恰好就是 `options-erase` 之后缺的那
一样。`-l` 真正的适用面是"opt 还有效、只想重写 logic 区",这时它与 `-w ... -S 0x800e7000`
等价(`-l` 的额外 option-bytes 步骤在 opt 有效时静默通过)。

**顺带一个方向相反的实测(注意起点)**:从 `option byte register = 0x3fffffd`(带
`option byte complement error`、read protection off、FPGA 地址有效)的状态执行
`agm_oo.sh options-erase` 之后,读回变成 **`0x3ffffff` + `read protection: on`**,ROM loader 也
报 `Flash : Protected`、`fpga configuration address is not valid` —— 这次 erase 把芯片**推向**
"看起来被保护"的一侧,而不是拉出来。恢复走 SWD:`agm_oo.sh bitstream <canonical>` →
`agrv wrote fpga configuration at 0x800e7000`,ORB 回到 `0x3fffffd`、read protection 回到 off、
指针回到 `0x800e7000`(实测)。

> §10.1 那次(erase 是解药)的起点是 `ORB=0x0 / WRPR=0x0`,这次是 `ORB=0x3fffffd`;**起点不同,
> 结论不能互推**。遇到 `… is protected`,先 `agm_oo.sh info` 读 ORB 再决定是 erase 还是
> 直接 `agm_oo.sh bitstream <bin>` 写回。

另外一条与烧录有关的厂商警告:**batch bin 里只要包含 opt 段,烧它就会 unlock,从而全片擦除**,
所以带 opt 的 batch 是"出厂烧录"专用、不能当升级包用。
我们自己的烧录路径只写固件/bitstream,不碰 opt。

**试过但无效的手段**(别在这些上面浪费时间):

| 手段 | 结果 |
|---|---|
| `python3 tools/openocd_warmup.py` | 探针响应正常,stall 不变 |
| `usb.util.dev.reset()`(pyusb) | 只清 host 端 stale 的 usbfs claim,stall 不变 |
| 探针 nRESET 复位(`DAP_ResetTarget` 0x0A) | 探针确实拉过 nRESET(应答 `0a 00 00`),stall 不变 |
| `adapter assert srst` | `Error: adapter has no srst signal` —— SDK 的 CMSIS-DAP cfg 没声明 srst,**结构上不可能生效** |
| `-c "adapter speed 100"` 降速 | 必须在 `init` 之前设置;SDK cfg 自带 `init`,之后改无效 |
| 重新插拔 USB | 只重置探针枚举;若目标另有供电则什么也没重置 |

> 想读到 init 失败之后的内部状态,可以 `openocd -f /tmp/wrap.tcl`,里面用
> `catch {source <board>.cfg} e` 包住配置 —— 这样 init 失败也不会中止脚本,还能继续
> 发 `riscv dmi_*` / `adapter` 命令(实测有效)。

### 10.1 `cannot erase block 0 since it is protected`

擦写时报这条,先别下结论说芯片被锁 —— **先读状态**。SDK 自己的 openocd 包装器
`<AgRV_pio>/platforms/AgRV/etc/oo` 有完整的读/擦/写入口(PlatformIO 的
`lock`/`unlock`/`wipe`/`opterase` 自定义 target 就是调它),用它读一遍:

```sh
OO=$HOME/AgRV_pio/platforms/AgRV/etc/oo
OC=$HOME/AgRV_pio/packages/tool-agrv_openocd/bin
python3 "$OO" -d "$OC" -A cmsis-dap -s 10000 -I agrv2k -i       # flash info
```

(同一件事的封装:`tools/agm_oo.sh info` / `tools/agm_oo.sh options-erase` —— 见
[`../tools/README.md`](../tools/README.md) 的"Probe state, recovery, and the SDK's
own runner"。)

实测(板子跑过 vendor 的 `example` flash 测试之后):

```
option byte register = 0x0
write protection register = 0x0      ← 设备层面根本没写保护
read protection: off
fpga configuration address = 0x800e7000 (non-compressed)
```

可 openocd 的 flash 驱动仍然把整片判成 protected:它要读 flash 控制器的
`WRPR`(0x81000020),而那个地址**不在系统总线上** —— 走系统总线读会报
`out of bound ... Target is reset`,驱动读不到就默认按"全保护"处理,于是
`flash erase_address` / `flash write_image`(以及直接调 SDK cfg 的 `flash protect off`)
全部失败。**能触到这个寄存器的只有 AP 通路**,而 SDK 的 `oo` 正好走那条:

```sh
python3 "$OO" -d "$OC" -A cmsis-dap -s 10000 -I agrv2k -o       # erase option bytes
# → "Option bytes are erased" / "agrv erase options complete."
```

擦完 option bytes 之后,`west flash --runner agrv_openocd --bitstream-only`(写比特流 + 重设 FPGA CONFIG 指针)或 `../tools/flash_logic.sh` 与
`oo ... -w <firmware.bin>` **都立刻恢复正常**,实测:

```
verified 99944 bytes … agrv wrote fpga configuration at 0x800e7000   ← 比特流
erased/wrote/verified 29660 bytes                                    ← 固件
```

注意两点:①擦 option bytes 会清掉 `fpga configuration address`,所以之后**必须**再写一次
比特流(`west flash --runner agrv_openocd --bitstream-only` 会顺带把 OPTBY 指回去);②这条修复走 SWD,**不需要 BOOT0**。

> **补充:把 FPGA 指针写回去不一定要 SWD(已实测)。** ROM bootloader 支持直接写
> option 区:**一次会话里先 `0xA3` 擦 option,再用普通 `0x31` 把 72 字节写到 `0x81000000`**
> (厂商"自制烧录机"参考就是这么做的)。实测:写完用 SWD 独立回读,option 区确实变成了写进去的内容
> (`a5 5a 12 ed …` vs 基线 `a5 5a ff ff …`)。
>
> **两个坑,都是实测踩出来的**:
>
> 1. **`agrv32flash` 干不了这件事**:它的 `-O` 会在擦完后**复位设备**,而复位后 ROM 在
>    init 时把"options 已擦"读成**被保护**,接下来读写全被 NACK(`Failed to read memory …
>    target write-protected?` 印在它自己的输出里);`-O -w` 组合它直接拒绝
>    ("Can't execute option bytes erase and memory write at the same time")。要写就必须像
>    厂商 master 那样**自己连发 `0xA3` → `0x31`**,中途不复位。
> 2. **擦掉 options 又不马上写,等于把板子推进"读保护"**:系统总线读不了 flash
>    (`Failed to read memory via system bus`)、ROM 拒写;只能走 SWD/AP 恢复
>    (`agm_oo.sh bitstream <canonical>` 会把 FPGA 指针和 RDP 一起写回 —— 这条当天复验过)。
>
> 另外两条实测:**写 option 会触发整片擦除**(厂商注释原话;实测固件区读回全 `0xFF`),
> 而且芯片会忙上十几秒(这期间 ROM 不响应,工具要把超时放大);所以顺序永远是"擦 opt →
> 写 opt → 再写 code/logic"。
>
> **上面的流程已封成工具:`../tools/rom_opt.py`**(`show` / `save` / `restore` /
> `set-fpga <addr> [--compressed --algo <addr>]`)。它按这条实测配方实现:同一次会话
> "擦 + 写"、不复位,写后用**新会话**回读校验,初始化时按 ~10 s 的整片擦除窗口耐心重试。
> 帧格式(命令+反码、地址大端+异或、N/data/校验)有 host 用例钉着
> (`../tools/tests/test_rom_opt.py`),option 镜像的字段约定(字 12/13 vs 14..17)也在那里。

## 11. 读保护(RDP):加锁、验证与解锁演练(实测)

> **这一节里有不可逆的一步**:解锁 —— 以及"设备被锁住"之后任何恢复 —— 都会**整片擦除**。
> 加锁本身不擦(下面有实测),但解锁一定擦,所以**先把 firmware / 比特流 / 盐都准备好再锁**。
> 这是 R2c 那一半("防复制"的物理边界)的操作面,设计记录见
> [`SIGNED-IMAGES-PLAN.md`](SIGNED-IMAGES-PLAN.md) §12.5。

### 11.1 这块芯片上的 RDP 是什么(全部实测)

| 动作 | 命令 | 实测结果 |
| --- | --- | --- |
| 加锁 | `tools/agm_oo.sh lock`(`oo -L` → `agrv lock 0`) | `Option bytes are erased / written` → `agrv locked`;ORB `0x3fffffd → 0x3ffffff`、`read protection: off → on`;**片内镜像一个字节没动**(控制台 hello_world 继续 tick),FPGA 指针仍是 `0x800e7000` |
| 锁上后的**外部**读 | SWD `dump_image` / `agm_oo.sh read` | 整片读回**全 `0x00`**,并报 `Failed to read memory via system bus`;1 MiB dump 从 1.49 s 变 11.18 s(全在读超时)。**盐扇区读出来是空的** —— 这正是 R2c 要的那条 |
| 锁上后的**设备自己** | 片内 XIP、flash 控制器的 flex-read | 照常:CPU 继续执行固件,`agm_boot_unique_id()` 仍能读到 UID(锁挡的是外部读,不挡核自己) |
| 锁上后读 option | `tools/agm_oo.sh info`(AP 通路) | 仍可读:ORB `0x3ffffff` + `read protection: on` —— 状态本身是可查的 |
| 解锁 | `tools/agm_oo.sh unlock`(`oo -u` → `agrv unlock 0`) | `agrv unlocked.` + `a reset or power cycle is required…`;**整片 1 MiB 读回全 `0xFF`**(firmware / 比特流 / 记录 / 盐全没),串口 0 字节;`info` 立刻回到 `read protection: off` |
| **不上探针**加锁 | `agrv32flash -j`(BOOT0 拉高进 ROM loader) | `Read-Protecting flash / Done`;之后 ROM 自己也报 **`Flash : Protected`**,读命令直接失败(输出 0 字节的文件)。落到设备上和 SWD 加锁**完全一样**:ORB `0x3ffffff`、`read protection: on`、SWD 读全 `0x00` |
| **不上探针**解锁 | `agrv32flash -k` | `Read-UnProtecting flash / Done`;**整片擦**(盐扇区回读全 `0xFF`),ROM 恢复 `Flash : 0x80000000 1024KB` |
| `agrv32flash -L` | 单独用 | **空转**:只打设备信息、仍 `Protected` —— 它是"写操作时顺带解锁"的修饰,解锁要用 `-k` |
| 不拔跳线先跑一下应用 | `agrv32flash -g 0x80000000` | `Starting execution at address 0x80000000... Done`,应用真的跑起来(BOOT0 只管**复位时**选谁,不锁运行期);下次复位仍回 ROM |

### 11.2 产线顺序,与一次完整演练

顺序永远是:**firmware + 比特流 → 写盐(`tools/agm_bind.py provision`)→ 逐项复验(串口日志 +
密钥指纹)→ 最后才 `tools/agm_oo.sh lock`**。一次完整演练(GPIO 出问题也回得来:恢复用 SWD,
见下):

```
加锁前 : ORB 0x3fffffd, read protection: off, 盐扇区 41474d42 01000000 34c4b414 85663168
         (= "AGMB" v1 + salt,provision --read-only → valid, fp bcfe1706)
         SWD 1 MiB dump 1.49 s, md5 fb4d787d6c5bf34009ec2c329aac4913
加锁   : Option bytes are erased / Option bytes are written / agrv locked
加锁后 : ORB 0x3ffffff, read protection: on;控制台仍每 500 ms 一行(镜像没动)
         SWD 1 MiB dump 11.18 s,内容全 0x00;provision --read-only → "blank (erased)"
解锁   : agrv unlocked.(+ "a reset or power cycle is required…")
解锁后 : read protection: off;整片全 0xFF;串口 0 字节(要重烧)
恢复   : west flash(firmware + canonical 比特流)→ 控制台复现,
         uid 不变(41503436 33343112 00d6b836 56060178),
         bind: no salt provisioned(盐确实被擦了)
         → agm_bind.py provision 重写盐 → fp bcfe1706(同一颗 + 同一份盐 = 同一把密钥)
```

恢复之后又跑了一次绑定验收:`tools/verify_flow.py --only 2,3` → **两步全 PASS**
(写盐 + 指纹比对,以及"未绑定 / 另一颗芯片的副本被拒")。

### 11.3 加锁状态下的现象速查

| 现象 | 含义 |
| --- | --- |
| 串口正常,而 `west flash` 报 `Device is in read protect mode, unlock it first` | 正常的加锁状态:CPU 在跑,外部**写**被拒(SDK cfg 的 `check_device_id` 先挡) |
| SWD 读 flash 全 `0x00` + `Failed to read memory via system bus` | RDP 生效(未加锁时读回真实内容) |
| `oo -i`:`read protection: on`、ORB `0x3ffffff` | 同上(AP 通路仍能看状态) |
| 要改板 → `agm_oo.sh unlock` | **会整片擦**:先准备好 firmware / 比特流 / 盐再动手 |

### 11.4 ROM 侧的同一次演练(BOOT0 拉高),与那个风险窗口

**两条通路都走过了,而且落到同一个设备状态**(同一块板):

```
ROM 基线      : agrv32flash <port> → Flash 0x80000000 1024KB(未保护时能读)
                agrv32flash -r f -S 0x800c9000:32 → 41474d42 01000000 …(盐可读!)
ROM 加锁      : agrv32flash -j → Read-Protecting flash / Done
加锁后        : ROM 自报 Flash : Protected;读命令失败(0 字节);
                SWD 侧同步变成 ORB 0x3ffffff + read protection: on + 读全 0x00
ROM 解锁      : agrv32flash -k → Read-UnProtecting flash / Done
解锁后        : 盐扇区回读全 0xFF(整片擦),ROM 恢复 Flash 0x80000000 1024KB;
                SWD 侧回到 ORB 0x3fffffd + read protection: off
恢复          : SWD 重烧 firmware + canonical 比特流 + 盐,
                逐字节核对:firmware 段 == 构建产物、比特流段 md5 6378549f…、盐扇区 AGMB v1
```

注意 `-L` 单独用不解锁(见 11.1 表);**要解锁就用 `-k`**。另外 ROM loader 是"外部读者"的
典型:未加锁时它能把盐整段读出来 —— 这就是 §11 存在的理由。

- **风险窗口仍然是"写 option 的那一刻"**:两条通路都要先擦 option 再写,中间掉电可能把片子
  留在"外部读写都进不去"的状态,那时唯一入口就是 BOOT0 + ROM loader。所以**插着 BOOT0
  跳线再锁**是最稳的做法 —— 代价只是多一根跳线。(这个窗口本身没法安全地演练:
  演练它意味着故意在写 option 中间断电。)
- **用 ROM 路径加锁之后,记得拔跳线再上电**:跳线在位时复位后进的是 ROM loader,
  应用不会跑(这不是故障,是 §10 那条"BOOT0 陷阱")。
- **ROM 侧 `-k` 解锁会把 option 镜像的其余字段也清成 `0xFF`**(实测:`osc config` 从
  `0xff, 0x57` 变 `0xff, 0xff`,`fpga configuration address is not valid`)。板子**照样能启动**
  —— ROM 拿不到有效指针就退到默认时钟,Zephyr 的 FCB 路径随后自己把配置流进 fabric
  (`FCB STAT` 仍是 `ACTIVE`)—— 但要让 ROM 的初始配置也正常,得把指针写回:
  `tools/agm_oo.sh bitstream <canonical>`(§10.0.1 那条配方)。
- **写 option 的那一步会整片擦,顺序别颠倒**:上面那次 `agm_oo.sh bitstream` 写完指针之后,
  firmware 与**盐**都变成 `0xFF` 了(实测)。所以恢复顺序是
  **① 先修 option(指针/OSC)→ ② 再烧 firmware + 比特流(`west flash`)→ ③ 最后重写盐
  (`tools/agm_bind.py provision`)**。`west flash` 写比特流那一步只写字节、不碰 option,
  所以它不会擦。
- Per the vendor's flash-protection note (AgRV/AG32 "lock flash /
  code-encrypt" section, not redistributed with this repo):
  `lock flash` blocks external read, and **any `unlock` wipes the
  whole chip**; the per-chip UID-based code encryption (the cipher
  reconstructed in `tools/agm_logic_crypt.py`) is a separate option
  this module does not implement.

### 11.5 演练:把 option 写入**打断**(断电 / SIGKILL),以及怎么救(实测)

§11.4 那个"写 option 中途掉电"的窗口原先只能靠警告,现在有工具和实测了:

```sh
tools/agm_rdp_tear_test.sh --window 60          # 打开窗口,期间拔电即可
tools/agm_rdp_tear_test.sh --kill-after 0.45    # 不用手:0.45 s 后 SIGKILL 掉会话
tools/agm_rdp_tear_test.sh --inspect-only       # 只看当前状态
```

为什么需要工具:一次 option 写只有几毫秒,而一次 openocd 会话光启动就要 ~300 ms —— 手工拔电
根本打不中。`--window` 就是**一个会话里连续写很多次**(实测 5 次写 113 ms,所以 60 s 窗口
= 约 2600 次写),任一刻拔电都有很大概率落在写入里;`--kill-after` 是它的可重复替代:
sigkill 整个进程组(openocd 的 wrapper 不 exec,所以必须整组杀),host 一停,设备就收不到
后续写序列。

**实测(`agrv2k_407`,canonical 200 MHz 比特流)**:

| 步骤 | 观察 |
|---|---|
| 普通断电(正常运行 hello_world 时拔电) | **无影响**:重新上电后 ORB `0x3fffffd`、RDP off、指针有效、盐扇区完好、固件 md5 与构建产物一致 |
| `--window 4`(连续写,不打断) | 结束时**干净上锁**:ORB `0x3ffffff`、`read protection: on`、FPGA 指针仍 `0x800e7000`、SWD 读盐扇区全 `0x00` |
| `--kill-after 0.45`(第一刀) | 上一句 `option writes reported locked: 1`,即**第一刀完成了一次上锁,第二刀被打断** → 撕裂态:ORB `0x3ffffff`、`read protection: on`、**`osc config = 0xff,0xff`、`fpga configuration address is not valid`**(= option 已擦、未写回的典型形态) |
| 继续对撕裂态连打 | 之后 14 次 `agrv lock 0` 全部报成功,**指针仍无效** —— 说明 `agrv lock 0` 只写 RDP 那个字段,不负责把整个 option 镜像写回 |
| 撕裂态下的板子 | **起不来**:串口 0 字节,`FCB STAT = 0`、`APB_CLKENABLE = 0`(ROM 没有有效指针去配 fabric,核拿不到它期望的时钟树)。这就是文档里说的"变砖"形态 |
| 撕裂态下的 SWD | **活着**:能读 option、能读 flash(内容被 RDP 挡成全 `0x00`)、能执行 unlock —— 也就是说 §11.4 那句"唯一入口是 BOOT0 + ROM loader"**过于悲观**,这一轮两次恢复走的都是 SWD |
| `--reset-storm` | **无效**:openocd 会话占着 DAP,风暴的 `../tools/probe_reset_target.py` 脉冲打不进去(会话照常跑完,状态只是干净上锁)。它留作占位,真要测"复位打断写入"得靠拔电 |

**恢复阶梯(实测走通,顺序不能颠倒)**:

```
1. tools/agm_oo.sh unlock                 # SWD;RDP 回到 off,但指针仍是 invalid
                                          # (unlock 写的那份 option 镜像不含 FPGA 指针)
2. tools/agm_oo.sh 比特流 <canonical>  # 把 FPGA 指针写回;这一步会整片擦
3. west flash(firmware + canonical 比特流)  # AGM_BITSTREAM_BIN=… west flash -d <build>
4. tools/agm_bind.py provision --salt-file <salt> --uid <uid>
5. 复验:串口 banner、ORB 0x3fffffd/read protection off、fpga 指针 0x800e7000、
   firmware md5 == 构建产物、比特流 md5 6378549f…、盐扇区 41474d4201000000
```

第 5 步本次逐项都对上了(固件 md5、比特流 md5、盐 `AGMB` v1、串口 `hello_world`)。

**顺带记两个小事实**:①解锁后 `osc config` 是 `0xff,0x58`,不是基线那份 `0xff,0x57` ——
驱动写的那份 option 镜像与厂商原始镜像并不逐字节相同(板子照常跑,只是说明这几位不是"必须
等于某个值");②**还没做**的是"真的在窗口里拔电"这一格:窗口机制已验证(4 s 窗口就能锁上),
但断电本身是你动手——想补就把 `--window 60` 跑起来,在窗口里拔电,插回来再 `--inspect-only`。

**窗口自己的两处实测修正(同一天)**:

* **一次 option 写 ≈ 206–210 ms**,不是"会话 113 ms / 5 次"那种算法能外推出来的(那样会把
  会话启动的 ~300 ms 摊进去,把窗口算短)。工具改成用 50 次与 100 次两个会话的**斜率**定
  `--window`,并按 5 s 打印 "N 次写完 / CUT THE POWER NOW",这样倒计时和真实写次数对得上
  (实测:60 s 窗口 291 次写,288 次成功)。
* **`unlock` 之后 FPGA 指针的状态不稳定**:R2c 那次演练里解锁后指针仍是 `0x800e7000`,
  这一轮从"干净上锁"解锁后却读到 `fpga configuration address is not valid`(两次都记录了)。
  所以阶梯第 2 步(`bitstream <canonical>` 把指针写回)**不是可选项**;反过来说,`unlock`
  之后先跑它再烧固件/盐,顺序就不会踩到"板子起不来"。
* 两次自动化窗口(20 s / 60 s)都是**干净上锁**收尾(指针仍有效、无补码错误),即"窗口本身
  不制造撕裂";撕裂来自**打断**(§11.5 的 sigkill 那格已实测)。

**真的在窗口里拔电(180 s 窗口,两次)**:两次都插在窗口内(窗口在跑、计数在涨),
插回后读到的**都是干净上锁** —— RDP on、FPGA 指针仍 `0x800e7000`、`osc config 0xff,0x57`,
**没有撕裂**;按 11.5 的阶梯恢复后逐项复验通过。

为什么拔电没打中(推断,未逐项测量):一次 `agrv lock 0` 实测 206 ms,但真正"擦 option +
写 option"的临界区只占其中一小段,其余是主机/驱动开销,所以随手拔电大概率落在临界区之外;
而撕裂态是在 host 侧 SIGKILL 采样里抓到的(那一刀正好停在"擦完未写回")。
**结论:手工拔电不是稳定的撕裂手段**(两次都没命中),要复现撕裂用 `--kill-after`,
或者做一轮点位扫描去估命中率。

### 11.6 窗口内换一个 reset 源:IWDG(`--iwdg-reset` / `--iwdg-after N`),不用跳线

问"能不能在窗口里用 nRST 打断" —— 分两半答:

* **字面意义的 nRESET 引脚**:openocd 整段会话独占 DAP,外部 `../tools/probe_reset_target.py` 的脉冲
  打不进去(这就是 `--reset-storm` 无效的原因,实测会话照常跑完)。要让它可用,option 写必须
  改由 **ROM/UART 通**驱动(`agrv32flash -j` 循环或 `../tools/rom_opt.py` 的裸协议),那时探针是空闲的
  —— 代价是要插 **BOOT0** 跳线进 ROM loader。
* **片内 IWDG(不需要跳线、不占探针)**:备份域独立看门狗,最短档 **64 ms**(prescaler /2,
  LSI 名义 32 kHz)。工具现在能在**同一个会话里**、按指定位置布防:
  `--iwdg-reset` 在第一次写之前布防,`--iwdg-after N` 在第 N 次写**完成之后**布防 ——
  64 ms 后芯片被硬件复位,正好落在第 N+1 次写中间。布防命令就是 `wdt_iwdg_agm.c` 的那套
  (BDRST 脉冲 → BDCR = RTCEN|RTCSEL=LSI → IWDG 寄存器**读-改-写**置 prescaler/ENABLE,
  裸写会被这颗 IP 丢掉),会话结束后工具立刻清 ENABLE 位,否则片子每 64 ms 复位一次
  (备份域跨复位存活)。

**实测(三次)**:布防点 `N = 1/2/3` 都**成功把复位打进了窗口** —— 完成的写次数
正好停在 N(`option writes reported locked: 1/2/3`),会话随即被打断;而三次的 option 区
**都保持完整**(RDP on、指针 `0x800e7000` 有效、`osc config 0xff,0x57`),恢复阶梯照常走通。
也就是说:**复位确实能打断窗口,但和手工拔电一样,它落在"擦 option"那次寄存器写之前**
—— 而危险窗口是**"擦已触发、写还没触发"那一段**(见下)。

**这轮把危险窗口定清楚了**:一次 `agrv lock 0` 的 206 ms 里,主机依次做
`reset init` → 触发 option 擦(驱动打 `Option bytes are erased`)→ 触发 option 写
(`Option bytes are written`)→ 置 RDP 位。**撕裂只在"擦已触发、写未触发"之间发生** ——
host 侧 SIGKILL 能精确停在那里(实测第一刀就命中,留下 `osc 0xff,0xff` + 指针失效),
而复位/断电只是让芯片停止响应,主机那条序列还没走到擦除,于是 option 区保持原样。

### 11.7 意外进入撕裂态之后:重启能不能救?(实测 + 复现性说明)

**能救,但救它的不是"重启"**。撕裂态是 flash 里的**静态内容**,复位/断电只会让 ROM 再读一遍
同一份坏 option —— 不会自愈;板子会一直起不来(实测:串口 0 字节、`FCB STAT = 0`、
时钟门控全关)。真正救它的是**外部通路**:

```
1. tools/agm_oo.sh unlock                 # SWD;RDP 回 off(全片擦)
2. tools/agm_oo.sh 比特流 <canonical>  # 把 FPGA 指针写回(这一步也擦)
3. west flash(firmware + canonical 比特流)
4. tools/agm_bind.py provision --salt-file <salt> --uid <uid>
```

**实测**:那次完整撕裂态(option 区整体被擦:`osc 0xff,0xff` + 指针失效 + RDP 读作 on)
就是按这四步救回来的,而且**每一步都在撕裂状态下做的**(板子自始至终没起来过),
恢复后固件 md5、比特流 md5、盐 `AGMB` v1、串口 banner 逐项对上。**代价**:第 1 步是全片擦,
所以救回来的是"一块需要重新 provisioning 的板子"——产线工装能救,现场修不了。

**为什么可以说"任何撕裂态都救得回来"(结构性理由,不只是那一次)**:option 区的读写只走
**AP 通路**,而这条通路与 option 内容无关(否则 `unlock` 这个动作本身就不存在)——
所以不管撕裂成什么样,`unlock`/`bitstream <canonical>` 都能把 option 镜像重写一遍。
BOOT0 + ROM loader 是**第二条**独立通路(`agrv32flash -k` / `-L`,§11.4),用来兜底
"连 SWD 都进不去"的极端情况。

**复现性的诚实交代**:这一轮又试了 10 次打断(3 次 IWDG 复位、2 次人工拔电、若干次
定时/盯日志的 SIGKILL),只有**最初那一刀**产出了"option 区整体被擦"的完全撕裂态;
其余都是良性变体:

| 打断方式 | 落到哪里 | 结果 |
|---|---|---|
| 定时 SIGKILL(第一刀,0.45 s) | 擦已生效、写未触发 | **完全撕裂**:option 区整体被擦,板子起不来 → 四步救回 |
| 盯日志 SIGKILL(擦报告后立刻杀,10 次) | 擦/写之间但写已追上 | 良性:option 区完好、RDP 字段被清(off)或保持(锁着),板子照常启动 |
| 人工拔电(2 次)、IWDG 复位(3 次) | 主机还没走到擦除 | 良性:option 区完好,状态 = 上一刀的干净结果 |

所以:**"救得回来"有实测 + 结构性理由;"能不能稳定复现撕裂"没有解决** —— 危险间隙是
206 ms 里几毫秒,按时间抽样命中率只有百分之几。要稳定复现,下一步应该盯着
`Option bytes are written` 那一行**在它出现之前**杀(而不是在 `Option bytes are erased`
之后),或者干脆走 ROM 通路用探针 nRESET 打(那时探针是空闲的,需要 BOOT0 跳线)。
> **后续(见 §11.9)**:ROM 通路那条试过了,**打不进去**(15 刀 0 命中),
> 而且"撕裂签名"可以用一条 `agrv32flash -O` 无复位地造出来 —— 所以现在不追"稳定复现"了,
> 改成**确定性造态**:`--rom-erase-tear`。

### 11.8 撕裂到底怎么产生:把"危险间隙"量到底(续)

§11.7 里那句"危险间隙是 206 ms 里几毫秒"是**猜的**;给会话的每行打上时间戳之后,实测是:

```
Option bytes are erased   1.121s
Option bytes are written  1.271s     ← 间隙 150 ms(不是几毫秒)
agrv locked               1.271s
(一次 `agrv lock 0` 共约 210 ms,其中绝大多数是擦+写的等待)
```

**两个结论(都推翻了先前记录)**:

1. **host 侧打断不会撕裂 option 区**。用 `--kill-on-erase --kill-delay` 在间隙里扫了 8 个点
   (20/60/100/130/145/170 ms 等,`../tools/kill_on_pattern.py` 逐行读管道、毫秒级落刀),
   **每一次都留下"上一刀的内容"**(RDP 状态与 osc/指针完全不变)——也就是说
   `agrv lock 0` 对"主机死掉"是**原子**的:擦除没有被提交就不算数。
   ⇒ 先前把那次撕裂归因于 SIGKILL 是**错的**;它紧跟在一次 IWDG 复位之后,真正的推手是
   **芯片侧复位**。
2. **芯片侧复位落在窗口里确实能撕裂**(那次:option 区整体被擦,`osc 0xff,0xff` + 指针失效
   + RDP 读作 on,板子起不来)。机理:擦已经由**芯片自己**提交,程序还没触发。

**怎么瞄准芯片侧复位**:探针的 nRESET 在 openocd 持 DAP 时打不进去,所以只能用
**片内 IWDG**;它有两个**实测出来的前置条件**,不满足时布防是静默空操作:

* **必须是未锁状态**:RDP 开着时布防的寄存器写直接失败
  (`Error: Failed to write memory (addr=0x40000032)`);
* **备份域必须可用**:在 `hello_world` 这类 build 上,读 `0x40000034` 直接
  `Failed to read memory`,或读回 `0x0000`(= 写被丢弃,vendor SDK 的 `RTC_Init()` 注释讲的就是
  这条:没有 BDCR.RTCEN 就没有锁存)。工具现在**先读一次并明说**,不再静默空跑。

**相位旋钮是 IWDG 的 8 档分频**(`--iwdg-tap 0..7` = 64 ms…8192 ms,LSI 名义 32 kHz):
布防点固定在"第一次写之前",用档位把 64 ms…8 s 的到期时刻推着走,总有一档落在
上面那 150 ms 的间隙里。**剩下的那一步**(还没跑):在一个备份域可用的 build 上
(例如带 trial-watchdog 的 loader,或 `&rtc0`/`&iwdg0` 打开的配置)把 8 档扫一遍,
记录哪一档产出撕裂 —— 那之后"稳定复现"就是一条命令。
> **后续(见 §11.9)**:这条 8 档扫描**没有再必要**了 —— 同一波用探针 nRESET
> 在 ROM 通路上做了更干净的 15 刀(触发点已知、探针空闲),**0 命中**,而且落点不在写里时
> 写照样完成。结论是:芯片侧复位(nRESET 或 IWDG)打不裂这个写;要演练恢复,用
> `agrv32flash -O` 确定性造态。

**续测:主机侧布防 IWDG 还没打通(当天又推进了几步)**

| 步骤 | 观察 |
|---|---|
| 用 **32 位**访问 RTC 块(`mww 0x40000032`) | 直接失败:`Failed to write memory` —— 这块是**16 位寄存器区**,必须用 `mwh`/`mrh`(工具已改) |
| 用 16 位访问:脉冲 BDRST、写 `BDCR = RTCEN|LSI` | **成功**:回读 `BDCR = 0x8200` ✓(备份域被打开) |
| 16 位写 IWDG 寄存器(prescaler + ENABLE + feed `0xa000`) | 写本身不报错,但**回读 `0x40000034` 打不出值**(驱动侧用 `uint16_t` 指针读得到 ⇒ 是**调试器访问**这条路的细节,不是寄存器坏了) |
| **功能性判据**:布防 tap 0(64 ms)→ 等 300 ms → 读 `SYS.RST_CNTL` | 复位原因里**没有 IWDG 位**(bit29 未置,值仍是那个基线 `0x0c000000`)⇒ **这次布防没锁存**、芯片也没被看门狗复位 |

所以"主机侧布防 IWDG"这条路还差一截调试(要么是调试器对 `0x34` 的访问方式,要么是布防
序列里少一步这个 IP 特有的东西)。**已经确定可用、而且不需要再摸索的那条路**是:
**option 写改由 ROM/UART 通驱动**(`agrv32flash -j` 循环,或 `../tools/rom_opt.py` 那套裸协议),
这时探针是空闲的,可以用 `../tools/probe_reset_target.py` 的 **nRESET 脉冲**落在窗口里 ——
代价只是要插 **BOOT0** 跳线进 ROM loader。这条是下一步该做的实验。

### 11.9 ❌ 撤回:"ROM 侧写 + 探针 nRESET 瞄准"不是稳定复现;确定性路径是 option 擦(复核)

> 本节原先写的是"✅ 稳定复现(第一刀命中)"。**同一天复核把它推翻了**,原文与证据都留在这里,
> 免得下次再踩。结论一句话:**芯片侧复位(nRESET / IWDG)打不裂 ROM 的 option 写;
> "撕裂态"应该用 `agrv32flash -O` 确定性地造出来,而不是抽签抽出来。**

**复核实测(跳线在插着,芯片在 ROM loader)**

1. **写窗口确实只有几毫秒**。给 `agrv32flash -j` 的输出逐行打时间戳(cold/warm 多次):
   `Read-Protecting flash` → `Done` = **1–3 ms**;而**它前面那段握手**(`Initializing device…`
   → 设备信息 → 写)在 0.019 s 到 **1.00 s** 之间跳(取决于端口/芯片前置状态;同一会话里
   实测到过 0.096、0.137、0.249、0.611、0.975 五档)。**抖动是写窗口的 300 倍** ——
   所以 `sleep <offset>` 本质上是抽签,`0.080` 那次命中只是抽中了。
2. **瞄到窗口里也没用:P0 的 nRESET 停在写里面,写照样完成**。从**规范基线**出发
   (`read protection: off` + `osc 0xff,0x57` + 指针 `0x800e7000`,每条用
   `agrv32flash -k` + `tools/rom_opt.py restore` 重建并核对),打了 15 刀:

   | 触发方式 | 刀数 | 结果 |
   |---|---|---|
   | 固定 offset 0.084 / 0.088 / 0.092 s(进程内 `DAP_ResetTarget`,生效点 ≈ 调用后 1–2 ms) | 9 | **0 撕裂**:每一刀都是 `read protection: on` + `osc 0xff,0x57` + 指针有效 = **写完成了** |
   | 盯 `Read-Protecting flash` 行、见到立刻打(0 ms / 2 ms 延迟) | 6 | 同上,**0 撕裂** |

   机理(都由上面那些时间戳读数支撑):nRESET 落在**写之前**时,芯片重启后 ROM loader 又
   站起来,把迟到的 `0x82` 命令收下 → 写完成;落在**写里面**时,ROM 侧输出**没有 NACK**、
   `Done` 照常返回 → **那一次 option program 没有被复位打断**。所以"探针自由"并不等于
   "能打进那 3 ms"。
3. **"撕裂签名"本身不是撕裂的证据**(这是旧结论最可能的来源):在规范基线上跑一条
   **没有任何复位**的 `agrv32flash -O`(只擦 option),回读就是
   `read protection: on`、`osc config = 0xff, 0xff`、`fpga configuration address is not
   valid` —— 与旧记录逐字段相同。旧那一刀前面正好跑过一堆 `-k` / 选件操作,option 区当时
   是不是"已经擦过"没有留证,所以那次观察**只能撤回**(按调试纪律:同时有互斥记录时先重跑)。
4. 唯一**没测**的一格:真的把**供电**在写窗口里拉掉。手动拔电瞄不进 3 ms 的窗口,
   所以"掉电会不会撕裂"仍然没有实测;有实测的是"芯片侧复位不会"。

**确定性造出撕裂态(现在推荐的做法)**:`agrv32flash -O` 擦掉 option 区,回读即撕裂态
(RDP 读出 on、osc `0xff,0xff`、无 FPGA 指针、板子起不来)——不依赖任何复位时序。
工具里就是 `--rom-erase-tear`,而且判据改成**转移**而不是快照:

```sh
# 只撕裂(先核对基线是规范态,再擦;判据 = canonical -> erased,否则退出码 4/1)
tools/agm_rdp_tear_test.sh --rom-erase-tear
tools/agm_rdp_tear_test.sh --rom-erase-tear --recover \
    --build-dir /tmp/b_hello3 --bitstream <canonical> \
    --salt-file <salt> --uid <32 hex>            # 撕裂 → 恢复 → 复验,一条命令
```

`--rom-nreset S` 保留,但**降级为抽签模式**:它现在同样要求先核对规范基线,并且只有在
"进前 canonical、出后 erased"时才叫命中,界面里也直接写明实测 0/15 —— 旧的"看末态里有没有
`0xff,0xff`"判据会让一块**本来就被擦过**的板子被误判成撕裂,已经改掉。

**为什么这条能稳,而 host 侧打断不能**:

* ROM 侧 `agrv32flash -j` 的**时间戳**:整个调用约 130 ms,而 `Read-Protecting flash` →
  `Done`(它自己的 option 擦+写)只有 **~4 ms** —— 时间戳是判断"该往哪儿打"的唯一依据;
* 这条路上 option 写走 **UART**,**探针是空闲的**,所以 nRESET 可以精确落在那 4 ms 里;
  §11.8 那些 host 侧 SIGKILL 打不进"芯片自己的写",所以怎么扫都是良性状态(实测 8 个延迟点)。

**从撕裂态恢复(端到端实录,BOOT0 还插着;`--rom-erase-tear --recover` 一条命令)**:

```
>>> option area before the attempt:
    read protection: off osc config = 0xff, 0x57 fpga configuration address = 0x800e7000 …
>>> tear: agrv32flash -O (erase the option area, then reset)
>>> state after the option erase:
    read protection: on osc config = 0xff, 0xff fpga configuration address is not valid
>>> TEAR STATE REACHED -- the option area went from canonical to erased …
>>> recover 1/5: unlock (this is the full erase the ladder starts with)
>>> recover 2/5: put the FPGA pointer back (also erases the array)
>>> recover 3/5: firmware + bitstream over SWD
>>> recover 4/5: salt
    wrote    : 4096 B at 0x800c9000, read back and verified
    key      : fp bcfe1706
>>> recover 5/5: verify      # 退出码 0
```

```sh
agrv32flash -k                          # 1. ROM 解锁:RDP 回 off(option 其余字段仍缺)
tools/agm_oo.sh 比特流 <canonical>   # 2. 指针写回(这一步会整片擦)
west flash(firmware + canonical 比特流)   # 3. 固件 + 比特流
tools/agm_bind.py provision …           # 4. 盐
# 5. 拔掉 BOOT0 跳线 + 上电,复验
```

本次第 1 步走的是 **ROM 通路**(不是在撕裂态下唯一可行的那条 —— SWD 那条此前也救过同样状态),
恢复后逐项复验:RDP off、`osc config 0xff,0x57`、指针 `0x800e7000`、固件 md5 与构建产物一致、
盐扇区 `41474d4201000000`(固件回读 md5 `bbbd98d953fa3fd7545823ab8e63ff45` ==
`/tmp/b_hello3/zephyr/zephyr.bin`)。跳线还插着时串口是空白的 —— BOOT0 高时芯片一直在 ROM
loader,应用根本不在跑。**拔线 + 真 nRESET 后复验通过**:串口出 hello_world banner
(`printable_ratio = 100%`,`RESULT: OK`)、`FCB STAT = 0x000f0002`(ACTIVE,fabric 已配置)、
`SYS.APB_CLKENABLE = 0x04201d01`、option 区仍为 RDP off / `osc 0xff,0x57` / 指针
`0x800e7000`、盐扇区 `AGMB` v1(`fp bcfe1706`)、固件回读 md5 与构建产物一致。
注意最后这次复位要用**探针 nRESET**(`../tools/probe_reset_target.py`):抓取工具里的
`reset run` 是软复位,**不会重采样 BOOT0**。

**一个必须记住的坑(当天被它咬了半小时)**:IWDG 一旦布防成功,**备份域跨复位存活** ——
它会让芯片每 64 ms 复位一次,于是 ROM loader 的握手总被打断(`Failed to init device`),
看上去像"BOOT0 没起作用"。判据:`SYS.RST_CNTL` 的 **bit29**(IWDG 复位标志)会置位;
清法:16 位写 `IWDG`(0x40000034)把 ENABLE(bit8)清掉 —— 这也说明**主机侧 IWDG 布防
其实是会锁存的**,先前"没生效"的结论是误判(布防后的回读读不出值、且复位标志当时还没置)。
