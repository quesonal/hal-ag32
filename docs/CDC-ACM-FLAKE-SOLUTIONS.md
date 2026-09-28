# CDC ACM Flake — 根因修复记录(方案 B 已作废)

> **状态(2026-09-21)**:本文是 2026-09-07 的 USB 层根因记录;**那条"批量场景会复发"的观测
> 已经不复现** —— 见 `开发记录（未随本仓库发布）`
> §3.9(现工具 + 板级 cfg 重跑 4 轮、约 480 次 flash+capture,全 OK、零重枚举)。保留本文是因为
> 它把 CMSIS-DAP / CDC-ACM 在 Linux 下的机制讲清楚了,遇到同类现象仍按这里排查。

**问题(2026-09-07)**: 板上 CMSIS-DAP probe (VID=0xcafe, PID=0x1001, FW 2.1.0)
同时提供 CMSIS-DAP interface(intf 0,debug)+ CDC ACM interface(intf 1+2, UART-bridge)。
旧版 `openocd_warmup.py` 在每个 flash 前 detach 掉 intf 1+2 的 `cdc_acm`,烧录
结束后 host 端不自动重绑,`/dev/ttyACM0` 失踪;多次连续 openocd 调用累积后
看起来像 probe 彻底死掉,需要物理重新插拔才能恢复。

## 根因修复(2026-09-07):warmup 不再 detach cdc_acm

**结论**: detach cdc_acm 是**不必要**的操作,正是它造成了 `/dev/ttyACM0` 失踪。
openocd 只用 CMSIS-DAP intf 0(vendor-specific,无内核驱动绑定);intf 1+2 是
UART 桥,openocd 从不碰它们。EP0 慢速 `GET_DESCRIPTOR` 不 claim / detach 任何 intf。

修复后流程(`../tools/openocd_warmup.py`,已同步 hal_ag32 / zephyr board / module
三处副本):

```
$ west flash --runner agrv_openocd
  ├─ openocd_warmup.py → 仅 EP0 慢速 GET_DESCRIPTOR(不 detach 任何 intf)
  ├─ openocd flash + shutdown
  └─ /dev/ttyACM0 全程保持绑定,无需 rebind

$ cat /dev/ttyACM0 → Zephyr banner OK
```

**验证(2026-09-07)**: 17:35 重插后连续 ~10 轮 `west flash` + 手动 openocd,
内核日志 `journalctl -k` 零 USB disconnect / reset,`/dev/ttyACM0` 节点
(创建于 17:35:18)全程存活。

**方案 B**(`post_flash_rebind.py` + runner `_rebind_cdc_acm()`)已**作废**:
它只存在于 `zephyrproject/modules/hal_ag32` 实验分支
`phase-3.14-cdc-acm-rebind`(commit `c74f844`),west 实际加载的 runner
并不调用它;根因修复后也不再需要。

`agmv2k-cdc-acm-flake-after-openocd.md` 已记录 9 个寄存器 readback
推断 main() 真到的方法,作为读不到 banner 时的兜底(历史参考)。

## 方案对比

| 方案 | 工作量 | 永久性 | 兼容性 | 风险 |
|---|---|---|---|---|
| **A. 升级 probe FW** | 不定(等厂商) | ✅ 永久 | ✅ 不改硬件 | ❌ 不知道有没有新 FW |
| **B. 改 host USB binding 逻辑** | 1d | ⚠️ 缓解 | ✅ 不改硬件 | ⚠️ Linux 版本敏感 |
| **C. 外部 USB-serial 桥** | 30m 硬件 + 1d 软件 | ✅ 永久 | ✅ 不动 probe FW | ⚠️ 需改硬件接线 |
| **D. Semihosting 输出** | 2d | ✅ 永久 | ❌ 改 Zephyr printk | ❌ 完全换输出通道 |
| **E. UART DMA → mem → openocd 读** | 1d | ✅ 永久 | ✅ 不改硬件 | ⚠️ 加 DMA driver |

### 方案 A:升级 probe FW

```
$ ls $HOME/AgRV_pio/packages/tool-agrv_logic/etc/
```

找 FW 文件。**问题**: 不知道有没有新 FW,且 AgRV 厂商可能不维护 CMSIS-DAP
这一面。如果 FW 是开源(LPC4330 / STM32F103 之类),可以自己改 USB 描述符 + CDC ACM
重新绑定逻辑,但工作量大。

**结论**: 不可控,**不推荐**作为主线方案。

### 方案 B:改 host USB binding 逻辑(改 warmup + 加 post-flash rebind)

**状态: 已作废(2026-09-07)** — 根因修复(warmup 不再 detach)已使本方案不再
需要,保留下文作为历史分析。

**当前** `openocd_warmup.py`:
```python
for intf in (0, 1, 2):
    if dev.is_kernel_driver_active(intf):
        dev.detach_kernel_driver(intf)
        # ❌ flash 后没有 rebind
```

**修复**(已实现 commit `c74f844`,2026-09-07): flash **完成后** 调用 rebind:

```python
# post_flash_rebind.py — 已 commit 在 hal_ag32
import usb, time

dev = usb.core.find(idVendor=0xCAFE, idProduct=0x1001)
if dev is None: return 0  # probe absent, idempotent

# Normalize device state. Note: USBDEVFS_RESET alone doesn't trigger
# cdc_acm rebind on uhci_hcd; we still need explicit attach_kernel_driver.
dev.reset()
time.sleep(0.5)

# Re-acquire handle (reset may invalidate)
dev = usb.core.find(idVendor=0xCAFE, idProduct=0x1001)
if dev is None: return 1

# CRITICAL: ask kernel to re-bind cdc_acm to CDC ACM interfaces.
# This DOES NOT require root — it goes through libusb USBDEVFS_CONNECT
# ioctl which any user with udev read access can use.
for intf in (1, 2):  # CDC ACM data + notify
    try:
        if not dev.is_kernel_driver_active(intf):
            dev.attach_kernel_driver(intf)
    except usb.core.USBError as e:
        if "Entity not found" not in str(e):
            print(f"intf {intf}: {e}")
```

**改 runner 流程**(已 commit):
```python
# In agrv_openocd.py runner, override do_run:
def do_run(self, command, **kwargs):
    self._run_warmup()
    rc = super().do_run(command, **kwargs)  # flash + shutdown
    self._rebind_cdc_acm()                  # NEW: restore /dev/ttyACM0
    return rc
```

**关键发现**(推翻之前的判断):
- `pyusb dev.attach_kernel_driver()` **不需要 root** — 它走 libusb
  `USBDEVFS_CONNECT` ioctl。之前的"需 root"判断是错的。
- `pyusb dev.reset()` 单用**不**触发 cdc_acm rebind(this kernel 是
  uhci_hcd,USBDEVFS_RESET 只重置 device state,不重新跑 usb_probe_interface)。
- `dev.set_configuration(1)` 也无效(no-op when already at config 1)。
- 真正起作用的只有 `attach_kernel_driver`,reset 只是 normalization。

**验证**(2026-09-07, commit `c74f844`):
- 5 cycles detach → rebind 全绿:`before=None → after=/dev/ttyACM0` 每次都 OK
- end-to-end `west flash --runner agrv_openocd` 后 `cat /dev/ttyACM0` 立刻有 banner

**结论**: 方案 B 落地有效,**永久缓解** CDC ACM flake。

### 方案 C:外部 USB-serial 桥

**原理**: 板上 PL011 UART0 TX/RX/PIN_68/PIN_69 接外部 CP2102 / CH340 / FT232
USB-serial 桥,host 端通过这个独立 USB 设备读串口,与 probe 完全无关。

**接线**:
```
PL011 UART0 TX (PIN_68) ──── USB-serial RX
PL011 UART0 RX (PIN_69) ──── USB-serial TX
GND                       ──── USB-serial GND
```

**Host 端**: `/dev/ttyUSB0` (CH340) 或 `/dev/tty.usbserial` (CP2102) —
独立的 USB 设备,**完全不受 probe USB 操作影响**。

**Zephyr 端**: 不需要改任何代码。PL011 driver 已经把字节发到 PIN_68。

**优点**:
- ✅ 完全独立通道,probe 怎么 reset 都不影响
- ✅ 永久可靠
- ✅ 不改 Zephyr 代码

**缺点**:
- ⚠️ 需要硬件接线(可能需要杜邦线 / 飞线)
- ⚠️ 加一个 USB 设备占用(但 host 一般有多个 USB 端口)
- ⚠️ 不能用板载 USB-CDC 调试(Zephyr 的 printk 必须经 UART0 → 外部桥)

**结论**: 最可靠方案。如果用户愿意改硬件接线,**强烈推荐**。

### 方案 D:Semihosting 输出

**原理**: 移植 AgRV SDK 的 `printstr` magic-memory semihosting 机制 — 写特定
MMIO 地址 → OpenOCD program-buffer 截获 → 打印到 OpenOCD 控制台(host stdout)。

**SDK 已有实现** (`framework-agrv_sdk/src/semihosting.c`):
```c
void printstr(const char *s, int len) {
    volatile uint32_t *shmem = (volatile uint32_t *)0xF0000000;
    shmem[0] = (uint32_t)s;  // magic: pointer to string
    shmem[1] = len;          // length
    asm volatile ("ecall");  // or specific CSR write
}
```

OpenOCD 已经知道怎么截获这个,见 SDK 的 openocd 配置。

**Zephyr 端改**: 把 `printk` 重定向到 semihosting:
```c
// subsys/debug/printk.c (or new file)
void printk(const char *fmt, ...) {
    va_list args;
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    agrv_semihost_printstr(buf, n);
}
```

**优点**:
- ✅ 完全独立通道(走 OpenOCD,不走 UART)
- ✅ 即使 PL011/UART 全坏了也能用
- ✅ 不需要额外硬件
- ✅ 永久可靠

**缺点**:
- ❌ 必须有 OpenOCD session 在跑(但反正烧录+调试时都有)
- ❌ 改了 Zephyr printk 路径,需要测试所有 printk 还能用
- ❌ 不能直接看 boot 早期 banner(printk 之前),需要 OpenOCD 在 vector.S
  设 mtvec 之前就 attach,但 OpenOCD 启动慢,banner 会丢
- ❌ Zephyr 内核的很多 printk 路径依赖 `stdout`/`stderr` flush 语义,semihosting
  没有 stdout/stderr 区分
- ⚠️ 输出格式是 raw bytes,没有 line-ending 自动 flush

**结论**: 有用,但不是首选。**保留作为 fallback**(PL011 全坏时用)。

### 方案 E:UART DMA → mem → openocd 读

**原理**: PL011 TX FIFO(16 字节)很容易溢出。配一个 DMA channel 把
PL011 TX 自动搬到一个大的 ring buffer,然后 OpenOCD 通过 halt + memory
read 把 ring buffer dump 出来。

**Zephyr 端改**:
- 在 soc.c 配 DMA channel(AgRV2K 有 DMAC)
- soc.c 注册一个 `sys_read_printk_ring()` 给 OpenOCD
- OpenOCD 端用 `agrv read_printk_ring` 命令 dump ring buffer

**优点**:
- ✅ 不丢任何 printk byte
- ✅ 不依赖 USB CDC

**缺点**:
- ❌ 复杂(加 DMA driver + OpenOCD 端工具)
- ❌ 不是"永久解决",只是缓解 — PL011 还是 UART,理论上还有丢失风险
  (但实际 ring buffer 够大就 OK)

**结论**: 工程量大,**不推荐**作为主线方案。

---

## 推荐组合(修复后更新)

| 优先级 | 方案 | 何时做 | 状态 |
|---|---|---|---|
| **P1.a** | **根因修复(warmup 不再 detach)** | 已完成 | ✅ 2026-09-07(见文首) |
| **P1.b** | **B (host USB binding / rebind)** | — | ❌ 已作废(不再需要) |
| P2 | C (外部 USB-serial 桥) | 根因修复不够稳时 | ⏸ 暂不需要 |
| P3 | D (semihosting) | PL011/UART 全坏时作为 fallback | ⏸ |
| P4 | E (UART DMA) | 不推荐 | ❌ |

**当前** ✅ = warmup 不再 detach cdc_acm,`/dev/ttyACM0` 在 flash 全程保持
绑定,日常不再需要 rebind / replug。

---

## 推荐:方案 B 实现细节

### 改动清单

**1. 新文件**: `tools/post_flash_rebind.py`

```python
#!/usr/bin/env python3
"""post-flash CDC ACM rebind — runs after openocd shutdown.

Why: openocd_warmup.py detaches intf 1+2 (cdc_acm) so openocd has raw
bulk EP access. After openocd shutdown, those interfaces don't rebind
automatically — host cdc_acm driver sees detached state and won't
re-claim them until USB port reset / physical replug.

This script does the rebind via sysfs bind/authorized.
"""

import subprocess
import sys

def main():
    # Find the probe's USB device path
    try:
        out = subprocess.check_output(
            ["ls", "/sys/bus/usb/drivers/cdc_acm/"],
            text=True
        ).strip().split("\n")
    except subprocess.CalledProcessError:
        print("no cdc_acm bindings found", flush=True)
        return 0

    # Filter for our VID/PID (cafe:1001)
    for entry in out:
        path = f"/sys/bus/usb/drivers/cdc_acm/{entry}"
        try:
            with open(f"{path}/../../../../../idVendor") as f:
                vid = f.read().strip()
            if vid == "cafe":
                # Rebind: write to unbind then bind
                print(f"rebinding {entry} to cdc_acm", flush=True)
                with open("/sys/bus/usb/drivers/cdc_acm/unbind", "w") as f:
                    f.write(entry + "\n")
                with open("/sys/bus/usb/drivers/cdc_acm/bind", "w") as f:
                    f.write(entry + "\n")
        except (FileNotFoundError, PermissionError) as e:
            print(f"{entry}: {e}", flush=True)
            print("ERROR: requires root or usbdev group membership",
                  file=sys.stderr, flush=True)
            return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
```

**问题**: sysfs bind 需要 sudo。我们 User 不是 root。

**变体**: 不用 sysfs,直接 USB port reset:
```python
# Use pyusb to issue USB port reset (hub-style)
import usb
dev = usb.core.find(idVendor=0xCAFE, idProduct=0x1001)
if dev:
    dev.reset()  # 触发 host 重新枚举,cdc_acm 自动 rebind
```

`dev.reset()` 不需要 root(只要有 udev 规则放行)。这是更好的选择。

**问题**: USB port reset 会杀掉所有在跑的 openocd session。所以必须
**先** `openocd shutdown`,**再** `dev.reset()`。

### 2. 改 agrv_openocd.py runner

```python
def do_run(self, **kwargs):
    # existing logic: warmup + flash
    super().do_run(**kwargs)
    # post: rebind CDC ACM
    self._rebind_cdc_acm()

def _rebind_cdc_acm(self):
    try:
        import usb
        dev = usb.core.find(idVendor=0xCAFE, idProduct=0x1001)
        if dev is None:
            return  # probe not present, ignore
        dev.reset()  # USB port reset
        # Wait for cdc_acm to re-enumerate
        time.sleep(1.0)
        # Verify /dev/ttyACM0 is back
        if os.path.exists("/dev/ttyACM0"):
            log.info("CDC ACM rebound OK, /dev/ttyACM0 ready")
        else:
            log.warn("CDC ACM rebound but /dev/ttyACM0 not found")
    except Exception as e:
        log.warn(f"CDC ACM rebind failed: {e}")
```

### 3. 验证

```bash
# 测试: 多次 flash + 读 banner
for i in 1 2 3; do
    west flash --runner agrv_openocd
    timeout 5 cat /dev/ttyACM0 | head -1
done
# 期望: 3 次都能读到 banner(目前只有第 1 次能)
```

### 4. 风险评估

| 风险 | 影响 | 缓解 |
|---|---|---|
| `dev.reset()` 不工作 | CDC ACM 仍死 | 退回到方案 C(外部桥) |
| `dev.reset()` 杀掉 openocd | 测试失败 | 必须先 `openocd shutdown` 再 reset |
| 用户没装 pyusb | rebind 步骤 fail | `pip install pyusb` (warmup 已要求) |
| 其他 USB 设备被影响 | 同事的设备掉线 | `dev.reset()` 只 reset 该 USB 设备,不影响 hub 下其他设备 |

---

## 不推荐的方案总结

| 方案 | 不推荐原因 |
|---|---|
| A. 升级 probe FW | 不可控,等厂商 |
| D. Semihosting | 改 printk 路径,工程量大,只作为 fallback |
| E. UART DMA | 工程量最大,得不偿失 |

---

## 推荐:分两阶段

**阶段 1 (✅ 已完成,2026-09-07)**: 根因修复 — `openocd_warmup.py` 移除
`detach_kernel_driver`,不再 detach cdc_acm(见文首)。此前实验性的方案 B
(`post_flash_rebind.py` + runner rebind hook,commit `c74f844` 于
`modules/hal_ag32` 实验分支)随之作废。
- 验证:17:35 重插后 ~10 轮 flash + openocd,内核零 USB 事件,`/dev/ttyACM0` 全程在

**阶段 2 (⏸ 暂不需要)**: C(外部 USB-serial 桥)。仅当某些 host 上仍有
问题(例如老 kernel 或 xhci 不同行为)时启用。30m 接线 + 1d 软件验证。

阶段 2 完成后,probe 的 CMSIS-DAP 仍然是 debug 通道,USB-serial 是 UART
通道,两者完全独立。
