# AG32 / AGRV2K 引脚说明(100 / 64 / 48 / 32 + AGRV2K + 407 系列)

**来源**: 厂商的引脚 xlsx 表(受控资料,不随本仓库分发)。
**用途**: 引脚/封装级权威对照。软件(DTS/.ve)与文档统一使用 `PIN_xx` 命名;模拟/固定功能
标注(ADC/DAC/CMP/UART0/USB/JTAG/OSC/RTC/WKUP/BOOT)来自本表 MCU 功能列。

## 1. Sheet 清单与读法

| 表 | 封装 / 器件 | MCU 侧 | FPGA 侧 | 行数 |
|---|---|---|---|---|
| `100` | LQFP-100(组合封装) | AG32VFxxxV | AGRV2KL100 | 100 |
| `64` | LQFP-64(组合封装) | AG32VFxxxR | AGRV2KL64 | 64 |
| `48` | LQFP-48(组合封装) | AG32VFxxxC | AGRV2KL48 | 48 |
| `32` | QFN-32(组合封装) | AG32VFxxxK | AGRV2KQ32 | 32 |
| `AGRV2K` | AGRV2K 独立封装 | — | AGRV2KL100/L64/L48/Q32 | 100+64+48+32 |
| `AG32VH407RCT6` | LQFP-64 407:VH407RCT6(列 B/C)vs VF407RGT6(列 E/F) | 双列对照 | — | 64 |
| `AG32VH407VGT6` | LQFP-100 407:VH407VGT6(列 B)vs VF407VGT6(列 C) | 双列对照 | — | 100 |

- 同一物理 pin 双视角:`PIN_xx`(MCU GPIO 名)+ MCU 功能标签(如 `IO_ADC_IN10`、`IO_UART0_TX`)
  与 FPGA(AGRV2K)侧名(`IO`、`IO_GB`、`TMS`/`TCK`)。
- 407 开发板 = L100 组合 SoC(RV32IMAFC + 片内 L100 FPGA,默认 `analog_ip` 比特流),其引脚
  布局 = `100` 表(MCU 列 == AG32VF407VGT6;FPGA 列 == AGRV2KL100)。
- 电源/固定专用脚(VBAT、VDD33、GND、VDDA、VREFP、NRST、OSC、USB DP/DM、JTAG 等)
  不可作普通 IO;ADC/DAC/CMP/UART0 等为 MCU 侧能力标注,最终外设路由由 VE 决定。

## 2. 上电默认状态(列 E)

| 引脚 | 默认状态 |
|---|---|
| `NRST` | Pull-up |
| BOOT1(100/64/48/32pin = 37/28/20/15) | Pull-up |
| UART0_TX(100/64/48/32pin = 68/42/30/20) | 仅 Boot1=0, Boot0=1 时: Output |
| UART0_RX(100/64/48/32pin = 69/43/31/21) | 仅 Boot1=0, Boot0=1 时: Input Pull-up |
| JTAG `JTMS/JTDI/JNTRST`(100pin 72/77/90) | Pull-up |
| JTAG `JTCK`(100pin 76) | Pull-down |
| 其余普通 IO | 浮空(floating) |

## 3. LQFP-100 全表(407 板;`AG32VFxxxV` ↔ `AGRV2KL100`)

| No. | Pin name | MCU 功能(AG32VFxxxV) | FPGA(AGRV2KL100) | 默认状态 |
|---|---|---|---|---|
| 1 | PIN_1 | IO | IO | — |
| 2 | PIN_2 | IO | IO | — |
| 3 | PIN_3 | IO | IO | — |
| 4 | PIN_4 | IO | IO | — |
| 5 | PIN_5 | IO | IO | — |
| 6 | VBAT | VBAT | VDD33 | — |
| 7 | PIN_7 | IO_RTC | IO_GB | — |
| 8 | OSC32_IN | OSC32_IN | NC | — |
| 9 | OSC32_OUT | OSC32_OUT | NC | — |
| 10 | VSS33 | GND | GND | — |
| 11 | VDD33 | VDD33 | VDD33 | — |
| 12 | OSC_IN | OSC_IN | NC | — |
| 13 | OSC_OUT | OSC_OUT | NC | — |
| 14 | NRST | NRST | NRST | Pull-up |
| 15 | PIN_15 | IO_ADC_IN10 | IO_GB | — |
| 16 | PIN_16 | IO_ADC_IN11 | IO | — |
| 17 | PIN_17 | IO_ADC_IN12 | IO | — |
| 18 | PIN_18 | IO_ADC_IN13 | IO | — |
| 19 | NC | NC | NC | — |
| 20 | VSSA | GNDA | GND | — |
| 21 | VREFP | VREFP | VDDA33 | — |
| 22 | VDDA | VDDA | VDDA33 | — |
| 23 | PIN_23 | IO_WKUP_ADC_IN0_CMP_PA0 | IO | — |
| 24 | PIN_24 | IO_ADC_IN1_CMP_PA1 | IO | — |
| 25 | PIN_25 | IO_ADC_IN2_CMP_PA2 | IO | — |
| 26 | PIN_26 | IO_ADC_IN3_CMP_PA3 | IO | — |
| 27 | VSS33 | GND | GND | — |
| 28 | VDD33 | VDD33 | VDD33 | — |
| 29 | PIN_29 | IO_ADC_IN4_CMP_PA4_DAC0 | IO | — |
| 30 | PIN_30 | IO_ADC_IN5_CMP_PA5_DAC1 | IO | — |
| 31 | PIN_31 | IO_ADC_IN6 | IO | — |
| 32 | PIN_32 | IO_ADC_IN7 | IO | — |
| 33 | PIN_33 | IO_ADC_IN14 | IO | — |
| 34 | PIN_34 | IO_ADC_IN15 | IO | — |
| 35 | PIN_35 | IO_ADC_IN8 | IO | — |
| 36 | PIN_36 | IO_ADC_IN9 | IO | — |
| 37 | PIN_37 | IO_BOOT1 | IO | Pull-up |
| 38 | PIN_38 | IO | IO | — |
| 39 | PIN_39 | IO | IO | — |
| 40 | PIN_40 | IO | IO | — |
| 41 | PIN_41 | IO | IO | — |
| 42 | PIN_42 | IO | IO | — |
| 43 | PIN_43 | IO | IO | — |
| 44 | PIN_44 | IO | IO | — |
| 45 | PIN_45 | IO | IO | — |
| 46 | PIN_46 | IO | IO | — |
| 47 | PIN_47 | IO | IO | — |
| 48 | PIN_48 | IO | IO | — |
| 49 | NC | NC | NC | — |
| 50 | VDD33 | VDD33 | VDD33 | — |
| 51 | PIN_51 | IO | IO | — |
| 52 | PIN_52 | IO | IO | — |
| 53 | PIN_53 | IO | IO | — |
| 54 | PIN_54 | IO | IO | — |
| 55 | PIN_55 | IO | IO | — |
| 56 | PIN_56 | IO | IO | — |
| 57 | PIN_57 | IO | IO | — |
| 58 | PIN_58 | IO | IO | — |
| 59 | PIN_59 | IO | IO | — |
| 60 | PIN_60 | IO | IO | — |
| 61 | PIN_61 | IO | IO | — |
| 62 | PIN_62 | IO | IO | — |
| 63 | PIN_63 | IO | IO | — |
| 64 | PIN_64 | IO | IO | — |
| 65 | PIN_65 | IO | IO | — |
| 66 | PIN_66 | IO | IO | — |
| 67 | PIN_67 | IO | IO | — |
| 68 | PIN_68 | IO_UART0_TX | IO | Boot1=0, Boot0=1: Output |
| 69 | PIN_69 | IO_UART0_RX | IO | Boot1=0, Boot0=1: Input Pull-up. |
| 70 | PIN_70 | IO_USBDM | IO | — |
| 71 | PIN_71 | IO_USBDP | IO | — |
| 72 | PIN_72 | IO_JTMS | TMS | Pull-up |
| 73 | NC | NC | NC | — |
| 74 | VSS33 | GND | GND | — |
| 75 | VDD33 | VDD33 | VDD33 | — |
| 76 | PIN_76 | IO_JTCK | TCK | Pull-down |
| 77 | PIN_77 | IO_JTDI | IO | Pull-up |
| 78 | PIN_78 | IO | IO | — |
| 79 | PIN_79 | IO | IO | — |
| 80 | PIN_80 | IO | IO | — |
| 81 | PIN_81 | IO | IO | — |
| 82 | PIN_82 | IO | IO | — |
| 83 | PIN_83 | IO | IO | — |
| 84 | PIN_84 | IO | IO | — |
| 85 | PIN_85 | IO | IO | — |
| 86 | PIN_86 | IO | IO | — |
| 87 | PIN_87 | IO | IO | — |
| 88 | PIN_88 | IO | IO | — |
| 89 | PIN_89 | IO_JTDO | IO | — |
| 90 | PIN_90 | IO_JNTRST | IO | Pull-up |
| 91 | PIN_91 | IO | IO | — |
| 92 | PIN_92 | IO | IO | — |
| 93 | PIN_93 | IO | IO | — |
| 94 | BOOT0 | BOOT0 | GND | — |
| 95 | PIN_95 | IO | IO | — |
| 96 | PIN_96 | IO | IO | — |
| 97 | PIN_97 | IO | IO | — |
| 98 | PIN_98 | IO | IO | — |
| 99 | VSS33 | GND | GND | — |
| 100 | VDD33 | VDD33 | VDD33 | — |

## 4. 固定功能跨封装索引(脚号,按封装)

- `VBAT(备份电源)`: 100pin 6 | 64pin 1 | 48pin 1 | 32pin —
- `IO_RTC(VBAT 域)`: 100pin 7 | 64pin 2 | 48pin 2 | 32pin 1(IO/RTC)
- `NRST`: 100pin 14 | 64pin 7 | 48pin 7 | 32pin 4
- `OSC_IN / OSC_OUT`: 100pin 12/13 | 64pin 5/6 | 48pin 5/6 | 32pin 2/3(IO/OSC)
- `OSC32_IN / OSC32_OUT`: 100pin 8/9 | 64pin 3/4 | 48pin 3/4 | 32pin —(无)
- `BOOT0`: 100pin 94 | 64pin 60 | 48pin 44 | 32pin 30
- `BOOT1(上电 Pull-up)`: 100pin 37 | 64pin 28 | 48pin 20 | 32pin 15
- `UART0_TX / UART0_RX`: 100pin 68/69 | 64pin 42/43 | 48pin 30/31 | 32pin 20/21
- `USBDM / USBDP(固定,不进 .ve)`: 100pin 70/71 | 64pin 44/45 | 48pin 32/33 | 32pin 22/23
- `JTMS(默认 Pull-up)`: 100pin 72 | 64pin 46 | 48pin 34 | 32pin 24
- `JTCK(默认 Pull-down)`: 100pin 76 | 64pin 49 | 48pin 37 | 32pin 25
- `JTDI(默认 Pull-up)`: 100pin 77 | 64pin 50 | 48pin 38 | 32pin 26
- `JTDO`: 100pin 89 | 64pin 55 | 48pin 39 | 32pin 27
- `JNTRST(默认 Pull-up)`: 100pin 90 | 64pin 56 | 48pin 40 | 32pin 28
- `WKUP(ADC_IN0 + CMP_PA0)`: 100pin 23 | 64pin 14 | 48pin 10 | 32pin 7
- `ADC_IN0–3 / CMP_PA0–3`: 100pin 23–26 | 64pin 14–17 | 48pin 10–13 | 32pin 7–10
- `ADC_IN4–5 / CMP_PA4–5 / DAC0–1`: 100pin 29–30 | 64pin 20–21 | 48pin 14–15 | 32pin 11–12
- `ADC_IN6–15`: 100pin 31–36 | 64pin 22–27 | 48pin 16–19 | 32pin 5,13–14

## 5. LQFP-64(`AG32VFxxxR` ↔ `AGRV2KL64`)— 非普通 IO 引脚

普通行(MCU `IO` + FPGA `IO`,上电浮空)未列出;其余引脚见 §4 索引与源 xlsx。

| No. | Pin name | MCU 功能 | FPGA | 默认状态 |
|---|---|---|---|---|
| 1 | VBAT | VBAT | VDD33 | — |
| 2 | PIN_2 | IO_RTC | IO_GB | — |
| 3 | OSC32_IN | OSC32_IN | NC | — |
| 4 | OSC32_OUT | OSC32_OUT | NC | — |
| 5 | OSC_IN | OSC_IN | NC | — |
| 6 | OSC_OUT | OSC_OUT | NC | — |
| 7 | NRST | NRST | NRST | Pull-up |
| 8 | PIN_8 | IO_ADC_IN10 | IO_GB | — |
| 9 | PIN_9 | IO_ADC_IN11 | IO | — |
| 10 | PIN_10 | IO_ADC_IN12 | IO | — |
| 11 | PIN_11 | IO_ADC_IN13 | IO | — |
| 12 | VSSA | GNDA | GND | — |
| 13 | VDDA | VDDA | VDDA33 | — |
| 14 | PIN_14 | IO_WKUP_ADC_IN0_CMP_PA0 | IO | — |
| 15 | PIN_15 | IO_ADC_IN1_CMP_PA1 | IO | — |
| 16 | PIN_16 | IO_ADC_IN2_CMP_PA2 | IO | — |
| 17 | PIN_17 | IO_ADC_IN3_CMP_PA3 | IO | — |
| 18 | VSS33 | GND | GND | — |
| 19 | VDD33 | VDD33 | VDD33 | — |
| 20 | PIN_20 | IO_ADC_IN4_CMP_PA4_DAC0 | IO | — |
| 21 | PIN_21 | IO_ADC_IN5_CMP_PA5_DAC1 | IO | — |
| 22 | PIN_22 | IO_ADC_IN6 | IO | — |
| 23 | PIN_23 | IO_ADC_IN7 | IO | — |
| 24 | PIN_24 | IO_ADC_IN14 | IO | — |
| 25 | PIN_25 | IO_ADC_IN15 | IO | — |
| 26 | PIN_26 | IO_ADC_IN8 | IO | — |
| 27 | PIN_27 | IO_ADC_IN9 | IO | — |
| 28 | PIN_28 | IO_BOOT1 | IO | Pull-up |
| 32 | VDD33 | VDD33 | VDD33 | — |
| 42 | PIN_42 | IO_UART0_TX | IO | Boot1=0, Boot0=1: Output |
| 43 | PIN_43 | IO_UART0_RX | IO | Boot1=0, Boot0=1: Input Pull-up. |
| 44 | PIN_44 | IO_USBDM | IO | — |
| 45 | PIN_45 | IO_USBDP | IO | — |
| 46 | PIN_46 | IO_JTMS | TMS | Pull-up |
| 48 | VDD33 | VDD33 | VDD33 | — |
| 49 | PIN_49 | IO_JTCK | TCK | Pull-down |
| 50 | PIN_50 | IO_JTDI | IO | Pull-up |
| 55 | PIN_55 | IO_JTDO | IO | — |
| 56 | PIN_56 | IO_JNTRST | IO | Pull-up |
| 60 | BOOT0 | BOOT0 | GND | — |
| 63 | VSS33 | GND | GND | — |
| 64 | VDD33 | VDD33 | VDD33 | — |

## 6. LQFP-48(`AG32VFxxxC` ↔ `AGRV2KL48`)— 非普通 IO 引脚

普通行(MCU `IO` + FPGA `IO`,上电浮空)未列出;其余引脚见 §4 索引与源 xlsx。

| No. | Pin name | MCU 功能 | FPGA | 默认状态 |
|---|---|---|---|---|
| 1 | VBAT | VBAT | VDD33 | — |
| 2 | PIN_2 | IO_RTC | IO_GB | — |
| 3 | OSC32_IN | OSC32_IN | NC | — |
| 4 | OSC32_OUT | OSC32_OUT | NC | — |
| 5 | OSC_IN | OSC_IN | NC | — |
| 6 | OSC_OUT | OSC_OUT | NC | — |
| 7 | NRST | NRST | NRST | Pull-up |
| 8 | VSSA | GNDA | GND | — |
| 9 | VDDA | VDDA | VDD33 | — |
| 10 | PIN_10 | IO_WKUP_ADC_IN0_CMP_PA0 | IO | — |
| 11 | PIN_11 | IO_ADC_IN1_CMP_PA1 | IO | — |
| 12 | PIN_12 | IO_ADC_IN2_CMP_PA2 | IO | — |
| 13 | PIN_13 | IO_ADC_IN3_CMP_PA3 | IO | — |
| 14 | PIN_14 | IO_ADC_IN4_CMP_PA4_DAC0 | IO | — |
| 15 | PIN_15 | IO_ADC_IN5_CMP_PA5_DAC1 | IO | — |
| 16 | PIN_16 | IO_ADC_IN6 | IO | — |
| 17 | PIN_17 | IO_ADC_IN7 | IO | — |
| 18 | PIN_18 | IO_ADC_IN8 | IO | — |
| 19 | PIN_19 | IO_ADC_IN9 | IO | — |
| 20 | PIN_20 | IO_BOOT1 | IO | Pull-up |
| 23 | VSS33 | GND | GND | — |
| 24 | VDD33 | VDD33 | VDD33 | — |
| 30 | PIN_30 | IO_UART0_TX | IO | Boot1=0, Boot0=1: Output |
| 31 | PIN_31 | IO_UART0_RX | IO | Boot1=0, Boot0=1: Input Pull-up. |
| 32 | PIN_32 | IO_USBDM | IO | — |
| 33 | PIN_33 | IO_USBDP | IO | — |
| 34 | PIN_34 | IO_JTMS | TMS | Pull-up |
| 36 | VDD33 | VDD33 | VDD33 | — |
| 37 | PIN_37 | IO_JTCK | TCK | Pull-down |
| 38 | PIN_38 | IO_JTDI | IO | Pull-up |
| 39 | PIN_39 | IO_JTDO | IO | — |
| 40 | PIN_40 | IO_JNTRST | IO | Pull-up |
| 44 | BOOT0 | BOOT0 | GND | — |
| 47 | VSS33 | GND | GND | — |
| 48 | VDD33 | VDD33 | VDD33 | — |

## 7. QFN-32(`AG32VFxxxK` ↔ `AGRV2KQ32`)— 非普通 IO 引脚

普通行(MCU `IO` + FPGA `IO`,上电浮空)未列出;其余引脚见 §4 索引与源 xlsx。

| No. | Pin name | MCU 功能 | FPGA | 默认状态 |
|---|---|---|---|---|
| 1 | PIN_1 | IO/RTC | IO_GB | — |
| 2 | PIN_2 | IO/OSC_IN | IO | — |
| 3 | PIN_3 | IO/OSC_OUT | IO | — |
| 4 | NRST | NRST | NRST | Pull-up |
| 5 | PIN_5 | IO_ADC_IN12 | IO | — |
| 6 | VDDA33 | VDDA33 | VDDA33 | — |
| 7 | PIN_7 | IO_WKUP_ADC_IN0_CMP_PA0 | IO | — |
| 8 | PIN_8 | IO_ADC_IN1_CMP_PA1 | IO | — |
| 9 | PIN_9 | IO_ADC_IN2_CMP_PA2 | IO | — |
| 10 | PIN_10 | IO_ADC_IN3_CMP_PA3 | IO | — |
| 11 | PIN_11 | IO_ADC_IN4_CMP_PA4_DAC0 | IO | — |
| 12 | PIN_12 | IO_ADC_IN5_CMP_PA5_DAC1 | IO | — |
| 13 | PIN_13 | IO_ADC_IN6 | IO | — |
| 14 | PIN_14 | IO_ADC_IN7 | IO | — |
| 15 | PIN_15 | IO_BOOT1 | IO | Pull-up |
| 16 | VDD33 | VDD33 | VDD33 | — |
| 17 | GND | GND | GND | — |
| 20 | PIN_20 | IO_UART0_TX | IO | Boot1=0, Boot0=1: Output |
| 21 | PIN_21 | IO_UART0_RX | IO | Boot1=0, Boot0=1: Input Pull-up. |
| 22 | PIN_22 | IO_USBDM | IO | — |
| 23 | PIN_23 | IO_USBDP | IO | — |
| 24 | PIN_24 | IO_JTMS | JTMS | Pull-up |
| 25 | PIN_25 | IO_JTCK | JTCK | Pull-down |
| 26 | PIN_26 | IO_JTDI | IO | Pull-up |
| 27 | PIN_27 | IO_JTDO | IO | — |
| 28 | PIN_28 | IO_JNTRST | IO | Pull-up |
| 30 | BOOT0 | BOOT0 | GND | — |
| 32 | VDD33 | VDD33 | VDD33 | — |

## 8. AGRV2K 独立封装(手指号 = 组合封装 PIN 号;L100 与 §3 FPGA 列相同)

> **封装 = 能引出哪些外设。** 下面 §8.1–§8.3 是 datasheet 视角的"非普通 IO 引脚"表;
> 想拿"这个封装有哪些 pin"的**权威、机器可读**列表,直接问 SDK 自带的 gen_vlog:
>
> ```sh
> python3 $AGRV_PLATFORM_ETC/gen_vlog -p -d AGRV2KL64     # 也可 L100/L100H/L64H/L48/Q32
> ```
>
> 实测用户 IO 脚数:**L100 79 / L100H 67 / L64 49 / L64H 45 / L48 34 / Q32 26**。
> 本仓库 4 块 eval 板 = `AGRV2KL100`,pin map 在 dts 里用 `agm,logic-device` 声明,
> `../tools/build_bitstream.sh` 会拿它跟 board.ve 里的 pin 对照、不在封装里就拒绝出比特流
> (写在哪、怎么查:见 [DTSI-GUIDE.md](DTSI-GUIDE.md) §2.3 与本文 §8 的每封装脚数表)。

### 8.1 L64(IO: 47)

普通 IO(`IO/PIN_xx`):9–11,14–17,20–31,33–45,47,50–59,61–62

| Finger | AGRV2K 名称 |
|---|---|
| 1 | VDD33 |
| 2 | IO_GB/PIN_2 |
| 3 | NC |
| 4 | NC |
| 5 | NC |
| 6 | NC |
| 7 | NRST |
| 8 | IO_GB/PIN_8 |
| 12 | GND |
| 13 | VDDA33 |
| 18 | GND |
| 19 | VDD33 |
| 32 | VDD33 |
| 46 | TMS |
| 48 | VDD33 |
| 49 | TCK |
| 60 | GND |
| 63 | GND |
| 64 | VDD33 |

### 8.2 L48(IO: 32)

普通 IO(`IO/PIN_xx`):10–22,25–33,35,38–43,45–46

| Finger | AGRV2K 名称 |
|---|---|
| 1 | VDD33 |
| 2 | IO_GB/PIN_2 |
| 3 | NC |
| 4 | NC |
| 5 | NC |
| 6 | NC |
| 7 | NRST |
| 8 | GND |
| 9 | VDD33 |
| 23 | GND |
| 24 | VDD33 |
| 34 | TMS |
| 36 | VDD33 |
| 37 | TCK |
| 44 | GND |
| 47 | GND |
| 48 | VDD33 |

### 8.3 Q32(IO: 24)

普通 IO(`IO/PIN_xx`):2–3,5,7–15,18–23,26–29,31

| Finger | AGRV2K 名称 |
|---|---|
| 1 | IO_GB/PIN_1 |
| 4 | NRST |
| 6 | VDDA33 |
| 16 | VDD33 |
| 17 | GND |
| 24 | JTMS |
| 25 | JTCK |
| 30 | GND |
| 32 | VDD33 |

## 9. 407 系列差异(AG32VH407x vs AG32VF407x)

### 9.1 LQFP-100:AG32VH407VGT6 vs AG32VF407VGT6(67 vs 78 IO)

| No. | VH407VGT6 | VF407VGT6 | 说明 |
|---|---|---|---|
| 15 | RWDS | PIN_15_ADC_IN10 | RWDS 脚,与另一 RWDS 脚外部短接;VDD33 需磁珠隔离 |
| 29 | NC | PIN_29_ADC_IN4_CMP_PA4_DAC0 | |
| 31 | NC | PIN_31_ADC_IN6 | |
| 33 | NC | PIN_33_ADC_IN14 | |
| 35 | RWDS | PIN_35_ADC_IN8 | RWDS 脚,与另一 RWDS 脚外部短接;VDD33 需磁珠隔离 |
| 39 | NC | PIN_39 | |
| 41 | NC | PIN_41 | |
| 43 | NC | PIN_43 | |
| 45 | NC | PIN_45 | |
| 46 | VDD33 | PIN_46 | |
| 47 | GND | PIN_47 | |
| 48 | VDD33 | PIN_48 | |
| 49 | GND | NC | |
| 73 | PIN_73 | NC | |

注:407 板(L100)按 VF 列可用 CAN(PIN_38/39)、UART0(PIN_68/69)、USB(PIN_70/71)等;
VH 变体把这些脚让给电源/NC/RWDS,需对照上表。

### 9.2 LQFP-64:AG32VH407RCT6 vs AG32VF407RGT6(45 vs 49 IO)

| No. | VH407RCT6 | VH407RCT6 功能 | VF407RGT6 | VF407RGT6 功能 | 说明 |
|---|---|---|---|---|---|
| 8 | RWDS | RWDS | PIN_8 | IO_ADC_IN10 | RWDS 脚,与另一 RWDS 脚外部短接;VDD33 需磁珠隔离 |
| 20 | PIN_20 | IO_ADC_IN5_CMP_PA5_DAC1 | PIN_20 | IO_ADC_IN4_CMP_PA4_DAC0 | |
| 21 | PIN_21 | IO_ADC_IN7 | PIN_21 | IO_ADC_IN5_CMP_PA5_DAC1 | |
| 22 | RWDS | RWDS | PIN_22 | IO_ADC_IN6 | RWDS 脚,与另一 RWDS 脚外部短接;VDD33 需磁珠隔离 |
| 23 | PIN_23 | IO_ADC_IN15 | PIN_23 | IO_ADC_IN7 | |
| 24 | PIN_24 | IO_ADC_IN9 | PIN_24 | IO_ADC_IN14 | |
| 25 | PIN_25 | IO_BOOT1 | PIN_25 | IO_ADC_IN15 | |
| 26 | PIN_26 | IO | PIN_26 | IO_ADC_IN8 | |
| 27 | PIN_27 | IO | PIN_27 | IO_ADC_IN9 | |
| 28 | PIN_28 | IO | PIN_28 | IO_BOOT1 | |
| 30 | VDD33 | VDD33 | PIN_30 | IO | |
| 31 | GND | GND | PIN_31 | IO | |

## 10. 备注

- 比特流 / VE 侧引脚规则(固定脚、PLL、.asf 电气)见 `AGRV2K 逻辑设置.pdf`;USB0 的 DP/DM
  不经过 .ve,直接是上述固定脚。
- 407 板 CAN 实测在 PIN_38/39(**就是 GPIO7_3 / GPIO8_7**,不是"绕过 GPIO bank 直连")。
  比特流把 `PIN_38_in` 接进 `gpio7_io_in[3]`、把 `PIN_39_out_data` 接自 `gpio8_io_out_data[7]`,
  所以 CAN 内核要靠这两个 bit 的 **AFSEL** 才上 pin(由 pinctrl state `can0_default` 设置,
  见 §11.x 与 `soc/agm/agrv2k/pinctrl.c`)。早前"与 GPIO bank 无关"
  的说法来自一份**未经编译验证**的生成 netlist(`board.vx`,现在是 `west build -t logic`
  在 `<build>/logic/` 里的产物,不随仓库发布);实际烧录的位流对应厂商参考设计
  `~/spi_full_mac_bitstream_200mhz/example_board.v`(第 114-120 行:`PIN_38` 只接输入、
  `CAN0_TX0` 经 `PIN_39_out_en` 三态输出)。
## 11. GPIO 配置:bank ↔ pin 路由与 Zephyr DT 约定

### 11.1 硅片 GPIO bank(AgRV2K)

The AgRV2K GPIO block has **10 banks of 8 pins** (80 pins total;
GPIOx_y with x in 0..9, y in 0..7). Layout per the SDK's
`AltaRiscv.h`: GPIO_BASE = 0x40014000 with consecutive 4 KiB pages,
so GPIO8_BASE = 0x4001C000 and GPIO9_BASE = 0x4001D000.

The table **must list all 10 banks** — do not stop at GPIO7. A previous
only acknowledged GPIO0..7 and missed GPIO8, which
`can_agm.c` actually uses.

| bank | 基址 | PLIC IRQ | SYS.APB 位 | 说明 |
|---|---|---|---|---|
| GPIO0 | 0x40014000 | 7 | 1<<4 | pin 与 SPI0/SPI1 复用,各出厂比特流均未引出 |
| GPIO1 | 0x40015000 | 8 | 1<<5 | GPTIMER0 CH0-3(bits0-3)+ GPTIMER1 CH0-3(bits4-7)的 PWM 输出,由 gpt0/gpt1_pwm_default state 设 AFSEL |
| GPIO2 | 0x40016000 | 9 | 1<<6 | GPTIMER2 CH0-3(bits0-3)+ GPTIMER3 CH0-3(bits4-7)的 PWM 输出,由 gpt2/gpt3_pwm_default state 设 AFSEL |
| GPIO3 | 0x40017000 | 10 | 1<<7 | I2C0/1 SCL/SDA pins(bits4-7,i2c0/i2c1_default state)+ GPTIMER4 CH0-3(bits0-3,gpt4_pwm_default state) |
| GPIO4 | 0x40018000 | 11 | 1<<8 | 407 板 LED1..4(绑 PIN_51..54,见 §11.2) |
| GPIO5 | 0x40019000 | 12 | 1<<9 | 本移植未使用(MAC 外设已从比特流移除,见 §11.2) |
| GPIO6 | 0x4001A000 | 13 | 1<<10 | 板载按键/开关(见下) |
| GPIO7 | 0x4001B000 | 14 | 1<<11 | UART0 TX AFSEL bit6 + CAN0 RX0 AFSEL bit3(均由对应 pinctrl state 设置,PIN_38) |
| GPIO8 | 0x4001C000 | 15 | 1<<12 | CAN0 TX0 AFSEL bit7(由 can0_default state 设置,PIN_39,比特流对其 OE 取反);bit0 = PIN_67 |
| GPIO9 | 0x4001D000 | 16 | 1<<13 | 本移植未使用(比特流未引出) |

- 每 bank 8 bit,寄存器为 Stellaris 风格:`GpioDATA[256]`(masked write)@base、
  `DIR`+0x400、`IS/IBE/IEV/IE/RIS/MIS/IC`+0x404..0x41C、`AFSEL`+0x420
  (SDK `gpio.h`);AFSEL=1 → 该 bit 走外设复用。
- **pin(PIN_xx)↔ bank bit 的映射不是封装固定的**,由片上 FPGA 比特流
  (board.ve)逐板决定;本表 §3 的能力标签只是"可选 mux 菜单"。
- 厂商文档的另一个结论:「在 AG32 中,**必须映射后**,
  代码中操作 gpio 时,才会真正使能到硬件管脚」—— 即 §11.2 的 pin↔bank
  路由是**比特流前置条件**,软件侧只负责 AFSEL/方向/数据。

### 11.2 板级路由(board.ve,103/303/407/test 一致)

| MCU 侧 | pin | 板上角色 | 备注 |
|---|---|---|---|
| GPIO4_1..4 | PIN_51/52/53/54 | LED1..4(低电平亮,后来改绑) | pin 能力 = 通用 IO,比特流作 GPIO 用 |
| GPIO6_2 | PIN_23 | IO_Button1 | pin = WKUP/ADC_IN0/CMP_PA0;按下高 |
| GPIO6_4 | PIN_24 | IO_Button2 | pin = ADC_IN1/CMP_PA1;需弱上拉(见 board.ve 注释) |
| GPIO6_5 | PIN_29 | IO_Switch1 | 需完整 board.ve 比特流;与 DAC0 冲突 |
| GPIO6_6 | PIN_30 | IO_Switch2 | 需完整 board.ve 比特流;与 DAC1 冲突 |
| GPIO7_6 / GPIO6_1 | PIN_68 / PIN_69 | UART0 TX / RX | uart0_default state;uart_agm init 时由 pinctrl 置 AFSEL |
| GPIO3_4/5 | PIN_35/36 | I2C0 SCL/SDA | i2c0_default state(pinctrl 设 AFSEL;开漏,不动 DIR) |
| GPIO7_3 / GPIO8_7 | PIN_38 / PIN_39 | CAN0 RX0 / TX0 | can0_default state(can_agm init 时由 pinctrl 置 AFSEL);`PIN_38_in = CAN0_RX0`、`PIN_39_out_en = !gpio8_io_out_en[7]`(OE 反相);板级 CAN 验证用。来源: `$HOME/logic/example_board.v` |
| (非 GPIO 直连) | PIN_66/67 | UART1 TX/RX | 见 board.ve(MAC RMII+MII 路由已删除) |
| (固定脚) | PIN_70/71 | USB DP/DM | 不进 .ve,见 §4/§5.1 |
| (固定脚) | PIN_78 | USB0_ID(OTG) | .ve 行 `USB0_ID PIN_78` |

### 11.3 Zephyr DT 约定(dts/riscv/agm/agrv2k.dtsi + 板 .dts)

- dtsi 声明全部 GPIO0..9(10 组,见 §11.1;`ti,stellaris-gpio`,`ngpios=<8>`,
  `interrupts=<7+n 1>`),
  默认 `status="disabled"`;板 .dts 只 enable 比特流引出的 bank
  (agrv2k_407:gpio4 + gpio6)。
- 四个板(103/303/407/test)board dts 均暴露 `gpio-keys` 输入设备:
  IO_Button1/2 = GPIO6_2/4(PIN_23/24,`polling-mode` — 规避 AG32 IE 极性
  quirk,不依赖中断);IO_Switch1/2 = GPIO6_5/6(PIN_29/30)默认
  `status="disabled"`,比特流引出后可 overlay 使能(与 DAC0/1 冲突)。
- GPIO3/GPIO7 由外设/soc.c 以 AFSEL 方式使用,不 enable 为普通 GPIO 节点。
- 已知 quirk(均不属主线缺陷,见 STATUS):IE 写 1=enable 与 stellaris IM
  相反(gpio_irq sample 复置);输入态写 DATA 不入输出锁存(先 `GPIO_OUTPUT`
  再 `gpio_pin_set_dt`);无 DEN@0x51C;bit-banding 为 masked write。

### 11.4 电气属性(上下拉 / 驱动能力)= 比特流配置,运行时不可配

Vendor note (AgRV/AG32 silicon team, "pull-up / drive-strength" note,
not redistributed with this repo): the on-die pull resistors and
the per-pin drive-strength selection live in the bitstream (`board.asf`
+ `board.proj`, see `tools/generate_board_ve.py`); about 40 kΩ per
direction, 2 mA steps up to 32 mA. Both are wired by the FPGA bitstream
and changing them at runtime means FCB-side re-configuration of the
CPLD — not in scope here.

结论:

- Zephyr 的 `pinctrl-0` + `bias-pull-up` / `drive-strength` 在本 SoC 上
  **不可能生效**(电气属性是比特流配置的,运行时改需要走 FCB 重配 CPLD —— 厂商那
  边的设计就是"配置进 CPLD,运行时不动")。
- 因此四个板 dts 里的
  `pinctrl { uart0_default { bias-pull-up; drive-strength = "8mA"; } }`
  已**删除**:该 `compatible = "agm,agrv2k-pinctrl"` 节点
  既无 binding 也无 consumer,留着只会让人以为电气属性已经配好。
- pin 的上下拉 / 驱动能力需求落到比特流 ASF(`WEAK_PULL_UP_RESISTOR` /
  `CFG_KEEP` / `CURRENT_STRENGTH`)。
- **后来:这条需求现在可以从 devicetree 出发了** —— pins 节点写
  `agm,pull-ups` / `agm,pull-downs` / `agm,drive-strength-pins` + `-ma`,
  `../tools/generate_board_ve.py` 渲染成 `board.generated.asf`,`build_bitstream.sh`
  折叠进 `logic/board.asf` 交给 Quartus。用法与参数规则见 [DTSI-GUIDE.md](DTSI-GUIDE.md),
  例子 `samples/lcd_40pin`,规则测试 `../tools/tests/test_generate_board_ve_asf.py`。

  **上下拉已经能从 dts 生成**:在 pins 节点里按 pin 号写

  ```dts
  &agrv2k_pins {
          agm,pull-ups   = <69>;        /* 内部上拉 */
          agm,pull-downs = <56>;        /* 内部下拉 */
          agm,drive-strength-pins = <68>;   /* 成对:pin → 驱动电流 */
          agm,drive-strength-ma   = <8>;
  };
  ```

  `../tools/generate_board_ve.py` 会把它渲染成
  `<board_dir>/board.generated.asf`:

  ```
  set_instance_assignment -name WEAK_PULL_UP_RESISTOR -to PIN_69 ON
  set_instance_assignment -name CFG_KEEP -to PIN_56 2'b01 -extension   # 下拉只有这一种写法
  set_instance_assignment -name CURRENT_STRENGTH -to PIN_68 8MA
  ```

  `build_bitstream.sh` 再把这份(以及手写的 `<board_dir>/board.asf`)
  折进 `logic/board.asf` 的 `# pio_begin/# pio_end` 段,Quartus 编译时生效;
  pin list 里删掉那一行,赋值也会在下一次 `-t logic` 时消失。写法/流程见
  [DTSI-GUIDE.md](DTSI-GUIDE.md) §2.1、§3.3;为什么要走 ASF 而不是 DTS 见本文 §11.4。

  注意上电时序:比特流加载完成前(~20 ms)端口是 floating 的,对上电电平敏感的脚**仍然建议外接
  上下拉电阻** —— 内部上下拉要等 CPLD 配置生效。

  **`CFG_KEEP` 的其它取值 / WKUP 配置**仍没有 dts 属性:写在
  `<board_dir>/board.asf`(手写文件)里,它会被一起折叠进去。

---

## 12. 启动模式:BOOT0 / BOOT1(串口下载 / 救砖)

厂商《AG32/AGRV2K 硬件设计注意事项》(单页;该资料不在本仓库分发)原文:

> **5、串口下载只支持 UARTO,不能重新映射。串口下载时,注意 BOOT0(高),BOOT1(低)。**

> **7、BOOT0 建议不要直接接地,而通过电阻接地。调试时,如果烧录了错误程序而导致系统
> 异常的话,可能无法再此下载程序。此时,需要将 boot0 拉高上电,这样不会再运行用户程
> 序,这样就可以重新下载。**

> **9、上电引脚电平**:…需要等 logic 部分配置生效后,用户配置的 IO 上下拉才能生效。
> 这个配置时间一般在 **20~40 mS** 左右。

**开发板实际接法(用户提供)**:

| 脚 | 板级接法 | 结论 |
|---|---|---|
| `BOOT1`(PIN_37) | 下拉到 **GND** | 恒为低 → 满足 §5 的 `BOOT1(低)`,不用管 |
| `BOOT0`(PIN_94) | **默认下拉**(经电阻,非直连 GND),旁边有**跳线可接 3.3V** | 需要串口下载/救砖时插跳线,让 BOOT0 在**上电时**为高 |

所以串口下载(ROM 里的 UART bootloader,`agrv32flash` / `west flash --runner
agrv32flash`)的完整条件是:**BOOT0 高 + BOOT1 低 + 上电或重启**。厂商的
措辞是 "MCU will read and lock the values of Pin BOOT0, BOOT1, **when
powering on or restarting**"(复位后第 4 个 SYSCLK 上升沿锁存)。

实测把这个"重启"钉清楚了:**必须是真 nRESET(或断电重上电)**,不是软件复位 ——

* openocd 的 `reset run`(SDK cfg 没声明 srst,属于软件系统复位)7 次都没让 loader 应答;
* `../tools/probe_reset_target.py`(CMSIS-DAP `DAP_ResetTarget`,真 nRESET 脉冲)一次就进 loader,
  之后 `agrv32flash -r/-w` 全部正常;
* 脉冲之后要**立刻**(≲0.2 s)启动工具 —— loader 的监听窗口很短。

详细步骤与实测结果见 [`FLASH-AND-CAPTURE.md` §10](FLASH-AND-CAPTURE.md)。

写完固件后撤掉 BOOT0 跳线、复位,回到正常启动(跳线插着时每次复位/上电都会进 loader)。

> **一句被撤回的说法**:仓库里曾记过"`BOOT0`/PIN_94 可切到板载 SPI flash 启动"(
> 用户提供),后来用户更正为理解有误 —— 厂商文档(不在本仓库分发)
> 只给出 `BOOT0=1 & BOOT1=0` = 串口下载这一档;SPI flash 能否作启动源、由哪组 BOOT
> 组合选,没有出处,不要写成结论。

两点别混:

* §3 表里 PIN_94 那一行的第 4 列(FPGA 功能)写的是 `GND` —— 那是**比特流对这个 pin 的
  赋值**,不是板级 strap。板级 BOOT0 是"默认下拉 + 跳线到 3.3V"。
* §9 的 20~40 ms 是**比特流生效**的时间:上电初段各 IO 还是 floating,所以对 BOOT0 这类
  上电采样的脚,厂商才建议外接/经电阻的下拉,而不是直接接地(直连 GND 就没法再拉高,
  等于放弃 §7 的救砖通路)。

**本仓库的验证状态**:这条通路**还没被实测过**。当天在未插 BOOT0 跳线时,
`agrv32flash -r` / `west flash --runner agrv32flash` 都是 `Failed to init device`;
另用常驻 openocd 在工具发 INIT 前的 0 / 0.05 / 0.15 / 0.3 / 0.5 s 各发一次真实
`reset run`(共 7 次)也无应答 —— 与"必须 BOOT0 高"一致,但**没有**验证插上跳线后
确实能下载。谁跑过就补一条带日期/命令/输出的记录。
