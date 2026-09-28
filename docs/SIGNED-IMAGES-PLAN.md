# 签名镜像设计计划(PLAN-SIGN)

> 启动/DFU 这一整条线的**现状与待办**收在 [`docs/BOOT-DFU-STATUS.md`](BOOT-DFU-STATUS.md)
> —— 其中的 **§0.1 是"当前流水线一览"**(片上有什么、每次启动怎么验、更新怎么过门、
> 各开关默认值、明确没做的事、每次启动的成本)。本文件记录签名这一块的**设计取舍**。

> **状态(2026-09-24 清理)**:本计划里的算法档位、容器格式(CF-3)、比特流签名与**防回滚**都已
> 落地(真机记录在 peripherals §3.26.32/§3.26.34/§3.26.36);仍未做的是 **RDP / 写保护**
> (R2c,见 **§11** 的状态行)。本文是**设计记录**,不是待办清单 —— 要现状看
> BOOT-DFU-STATUS §0.1。
>
> **§11 是生产锁(R2)的设计**:2026-09-24 把原来的 `R2-PRODUCTION-LOCK-PLAN.md` 整份并了进来
> (那份文件已删除),它的内部编号整体平移成 §11.x(§11.6 是 R2b 的路径取舍、§11.7 是已实现的
> 接口契约)。签名与生产锁本就是同一条链:前者回答"谁能装",后者回答"谁能改状态"。
>
> **读法(哪些是历史)**:§2 是算法/后端的实测,§3 是格式选择与"不整体换 MCUboot"的论证,
> §4/§5 是公钥与验签时机的取舍,§9/§10 是比特流签名与防回滚的设计。**§7(落地阶段)与
> §8(决定点)已于 2026-09-24 删除** —— 那里的每一步都已完成,现状与开关进了
> BOOT-DFU-STATUS §0.1,体积数字进了 §3.26.32/§3.26.34;§2.2–§2.6 的实测表保留(它们是
> 选型依据)。编号故意留空档(§5 之后直接到 §6/§9/§10),既有引用不必改。

本文是 `code_review/boot-agm-driver/` (签名 + 生产访问控制)(签名 + 生产访问控制)的**设计计划**:
对比算法/格式/密钥/验证时机的各个选项,给出推荐组合、落地阶段,以及需要拍板的决定点。
本文不实现任何东西;每个阶段的实测数据、命令与证据会在实现时补进
`开发记录（未随本仓库发布）`。

## 0. 结论摘要(推荐组合)

| 维度 | 推荐 | 理由 |
|---|---|---|
| 算法 | **编译期可选三档**(已定,见 §2.1):`ECDSA-P256` / `RSA-2048-PSS` / `NONE`(=仅 CRC,损坏检查) | ECDSA 树内有现成实现且签名/公钥小;RSA 给"已有 RSA 密钥体系"的场合;`NONE` 保持今天的默认开发行为,并在文档里明确它**不是**安全边界 |
| 格式 | **分离式签名**:镜像字节不变,`sha256` + `sig` 存进**启动记录的 slot 条目** | 现有 console/AN3155/SMP/`agm_upload.py` 全链路不需要改;record v3 是 append log,条目变胖不影响磨损结论 |
| 公钥 | **先编译进 loader**(`const uint8_t[]`),将来再考虑片内只读 key 区 | 简单、不可篡改;轮换=刷 loader。option bytes 放不下(见 §4) |
| 验证时机 | **发布时 + 每次启动前** | 发布时拒绝坏签名(不写 record);启动前重算,防"发布后被改写" |
| 落地顺序 | **先在"默认布局"开签名**(外部 NOR),片内 A/B 布局暂时保持关闭 | 实测 loader 64132 B/64 KiB:**只剩 1404 B**;默认布局的 loader 区有 ~192 KiB(§6) |

> 算法一栏已拍板(2026-09-18):**在 ECDSA P-256 / RSA-2048 / CRC(仅损坏检查)三者之间可选**。
> 选择粒度是**每个构建**(Kconfig choice),不是每张镜像 —— 理由与"镜像里为什么还要带算法字段"
> 见 §2.1。

## 1. 威胁模型与目标

**要防的**:能写外部 NOR 或片内 store 的人(物理接触,或通过 console/AN3155/mcumgr 上传)把自己的
镜像变成 TRIAL/active 并被启动。今天 record 里只有 CRC32 —— 它只保证"没有意外损坏",攻击者可以
对任意镜像算出合法 CRC32,所以**现在的 loader 对"恶意镜像"没有任何防线**。

**不防的**(明确写出来,避免"签了名就安全"的错觉):

* 能改 loader 自己、能改 option bytes、能读芯片的攻击者 —— 那要 RDP + 完整的信任链,是另一个议题;
* **回滚攻击**(把旧版本重放回去)—— 需要 anti-rollback 计数器,单独做;
* 侧信道/故障注入;
* 私钥所在主机的安全(本计划只要求"私钥不出签名主机")。

> **厂商对第一条的答案已经有了**(2026-09-19 查《AG32芯片保护与代码加密.pdf》;
> 该资料不在本仓库分发,见 README 的 vendor docs 一节;对照见
> `开发记录（未随本仓库发布）`):
> `lock_flash`(读保护,就在 downloader 里勾)挡住"读芯片"这一半,而**关掉读保护会触发
> 全片擦除** —— 也就是说"能读 loader 里的公钥"这条路不是没有代价。另外两个已知约束:
> ① `board_logic.encrypt` 是 downloader 侧**按芯片 ID** 加密 logic 的,**用了它就不能做
> 远程 logic 升级**,和我们的比特流 A/B 直接冲突;② batch 里带 opt 会 unlock 全片擦除。
> 这些都还没有落到任何实现上,记在这里是为了别在设计"比特流也签名/加密"时踩上去。
>
> 两条的**机制**后来查清了,写在
> `开发记录（未随本仓库发布）`:加密必须配
> 压缩、只有 LZW 流被加密、密钥派生只在上位机工具里(SDK 只给一个回调)、option byte 只有
> AP 通路能写。比"与 A/B 冲突"更强的一句是:**板子一旦用过 `board_logic.encrypt`,本移植
> 连 loader 都跑不起来**(它每次启动都要自己流一遍配置,而我们没有解码通路)。另外注意签名
> 买到的是真伪/回滚,买不到"每芯片绑定"——那是正交的两件事。

**目标性质**:① 只有持私钥者能产生被接受的镜像;② 私钥永不进 CI、不进仓库;③ 任何验证失败路径都
"留在控制台",绝不把镜像变成可启动;④ 验证代码要能在 64 KiB / 192 KiB 两种 loader 区里活下来。

## 2. 算法对比

前提事实(本工作区实测):**tinycrypt 已不在本 Zephyr 里**(`modules/crypto/` 只有 `mbedtls`、
`tf-psa-crypto`、`mldsa-native`;写 `CONFIG_TINYCRYPT*` 会报 undefined symbol),所以老资料里
"tinycrypt ECDSA"那条路不可用;可用的都是 PSA/mbedTLS(以及 MCUboot 自带的验签路径)。

| 方案 | 防伪造 | 公钥 / 签名 | 代码量(本 SoC) | 每次验签耗时 | 树内可用 | 备注 |
|---|---|---|---|---|---|---|
| **现状 CRC32** | ✗ 任何人可构造 | 4 B | 已有 | ~0 | ✓ | 保留为"意外损坏"检查,不再当作安全边界 |
| SHA-256 摘要(only) | ✗ 可重算 | 32 B | ~1–2 KB | 快 | ✓ | 只把"意外"强度提上去 |
| HMAC-SHA256(密钥在 loader 里) | 弱:loader/固件泄露即可伪造 | 32 B | ~2–3 KB | 快 | ✓ | 只适合"防误操作";不建议当安全边界 |
| **ECDSA P-256 + SHA-256** | 强 | 64 B / 64 B | **实测 +5668 B**(MCUboot vendored tinycrypt,rv32imafc `-Os`,见 P1 结果) | 待真机(估计 50–300 ms) | ✓ PSA/mbedTLS 或 tinycrypt | **推荐** |
| Ed25519 | 强;签名/公钥更小 | 64 B / 32 B | 需自带实现(估计 10–20 KB) | 估计 100–400 ms | ✗ 无现成 option | 若坚持 Ed25519,要 vendor 一份(维护成本记在 P1) |
| RSA-2048/3072(**PSS**,MCUboot 只认 PSS) | 强 | 256–384 B / 272 B | **实测 +14.4 KB(+PSA 核心 7.7 KB = +22.1 KB)**,见 §2.2 | **实测 ≈ 99 µs @200 MHz**,见 §2.2 | ✓ | 签名大(PSS 还要 MGF1/salt,比 v1.5 略胖);**公钥是 272 B DER**,不是 64 B |
| ML-DSA-44(`mldsa-native` 在树内) | 后量子 | 1312 B / 2420 B | 估计 50 KB+ | 慢 | ✓ | 记一笔;当前 loader 放不下 |

**为什么推荐 ECDSA P-256**:①树内实现成熟(PSA/mbedTLS),不需要 vendor;②签名/公钥小,record
条目只涨 ~96 B;③验签只需要公钥运算(没有私钥代码);④主机侧 `cryptography`(venv 里已有 49.0.0)
直接支持;⑤rv32imafc 有硬件乘,曲线运算不会离谱地慢。

**为什么不用"只加 SHA-256"**:摘要本身可以被攻击者重算 —— 它防的是"传输/存储意外损坏",
而 CRC32 已经在做这件事了。

> 待实测项(P1):上面"代码量"与"耗时"两列目前是估计。计划里的方法是一个 30 行的 scratch
> app(只引用验签函数,让链接器必须保留它)对比基线 bin 大小与 `k_cycle_get_32()` 计时。
> 本机已经试过的三条路,留给 P1 直接接着走:①`CONFIG_TINYCRYPT*` → undefined symbol(tinycrypt 已不在树内);
> ②`CONFIG_MBEDTLS=y` + `#include <mbedtls/pk.h>` → 头文件不在 include path(4.4 起 mbedTLS 只经
> PSA 暴露);③`CONFIG_PSA_CRYPTO_CLIENT` → undefined symbol(该 Kconfig 在本版本不叫这个名字)。
> 也就是说 P1 的第一步是**找到本版本暴露 PSA/mbedTLS 的正确配置**(`CONFIG_MBEDTLS_PSA_CRYPTO_C`
> 能解析,但配套的 include 路径还没找到),然后再量代码量。
> > **两个算法都要量**:ECDSA P-256(PSA/mbedTLS)与 RSA-2048 验签(只需要公钥指数 `65537`,
> 但需要一个 bignum 实现 —— RSA 档最可能的意外成本就在那里)。
> 本机试跑时卡在"取到 mbedTLS 头文件"这一步(Zephyr 4.4 的 mbedTLS 现在只通过 tf-psa-crypto 的
> PSA 接口暴露),所以**选型确认前必须先把这个度量做出来**,而不是照抄估计值。

### 2.2 RSA-2048-PSS 实测:装配配方与代价(2026-09-19)

当初卡住 P1 的那一步现在有答案了 —— **本树里 mbedtls 的 *经典* API 已经不在**(`modules/crypto/
mbedtls/include/mbedtls/` 只剩 `build_info.h`/`debug.h`/`error.h`/`mbedtls_config.h`/`net_sockets.h`,
**没有 `rsa.h`/`pk.h`**);RSA 实现搬到了 `modules/crypto/tf-psa-crypto/drivers/builtin/src/rsa.c`,
对外只经 **PSA**。装配开关就是:

```conf
CONFIG_PSA_CRYPTO=y
CONFIG_PSA_WANT_ALG_RSA_PSS=y
CONFIG_PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY=y
```

验签调用(`psa_verify_hash` + `PSA_ALG_RSA_PSS(PSA_ALG_SHA_256)`)与 `psa_import_key`
把 MCUboot 用的那个 272 B DER 公钥导进去 —— 也就是说 **CF-3b(自研 TLV 解析 + 直接调原语)在这条路上
是可行的**,不需要借 bootutil。

**实测(`agrv2k_407`,rv32imafc,同一个探针 app 三个配置,`/tmp/rsa_probe`,2026-09-19)**

| 配置 | ROM | 相对基线 |
|---|---|---|
| baseline(不引 PSA) | 22332 B | — |
| PSA 开、不用 RSA | 30060 B | **+7728 B**(PSA 核心) |
| PSA + RSA-2048 PSS 验签 + 公钥导入 | 44448 B | **+14388 B**(RSA+PSS+ASN.1 导入) |

**合计 +22.1 KB** —— 对比 ECDSA P-256 的 **+5668 B**(§P1 结果),**RSA 大约贵 4 倍**。
耗时:真机 `k_cycle_get_32()` 包住"`psa_crypto_init` + `psa_import_key` + `psa_verify_hash`"
= **19755 / 19767 cycles ≈ 99 µs @200 MHz**(探针用的假签名得到 `status=-135`
`PSA_ERROR_INVALID_SIGNATURE`,说明确实走到了验签)。

**对 loader 预算的意义(默认布局,loader 区 96 KiB)**:RSA 档 loader ≈ 64944(未签名基线)+
22.1 KB ≈ **87 KB**,**装得下,但只剩 ~11 KiB**;ECDSA 档是 ~65 KB(剩 ~31 KiB)。两 flash 变体
(192 KiB 区)无所谓。片内 A/B 的两个 200 KiB 槽不受影响(它们装的是 payload,不是 loader)。

**结论/建议**:RSA-2048-PSS 从"只有设计"变成"有配方 + 有数字",**可以按 §2.1 的档位做**,但
1. 它是**编译期选择**,与 ECDSA 互斥,所以两份成本不叠加;
2. 体积上 ECDSA 明显更优(4×),**耗时上 RSA 压倒性更优**(99 µs vs 110 ms,见下表)——
   "RSA 慢"的直觉只对签名成立,验签是反过来的;
3. 公钥从 64 B 变成 272 B DER,`sign_image.py`/loader 侧的公钥注入与比较要按档走(现实现按 64 B 断言,
   要参数化)。

**ECDSA 侧的真机耗时(2026-09-19,补齐对照)**:同一个套路(`/tmp/ec_probe`,直接引用驱动用的
那份 vendored tinycrypt + 仓库里 `tests/drivers/misc/boot_agm_ecdsa/fixtures/` 的夹具,签名
`ok=1` 说明验过):

```
ec_probe run 0 ok=1 cycles=22099763
ec_probe run 1 ok=1 cycles=22073134
ec_probe run 2 ok=1 cycles=22070354
```

即 **≈ 22.07 M cycles ≈ 110 ms @200 MHz**。

**两档对照(同板、同 200 MHz 比特流)**:

| 档 | ROM 增量 | 单次验签耗时 |
|---|---|---|
| ECDSA P-256(tinycrypt) | **+5668 B** | **≈ 110 ms**(22.07 M cycles) |
| RSA-2048-PSS(PSA/tf-psa 的 rsa.c) | **+22116 B**(含 PSA 核心) | **≈ 99 µs**(19.8 k cycles) |

**RSA 快约 1100 倍、体积贵约 4 倍** —— 方向与直觉相反,但符合原理:验签只做公钥运算
(e=65537 → 16 次平方 + 1 次乘),而 tinycrypt 的 P-256 验签要做双点乘(曲线运算全在软件里)。
这条也顺带解释了为什么真机上签名档 loader 的每次"发布 + 启动前各验一次"几乎看不出延迟。

**2026-09-23 补测:P-256 没有"更快的后端"可换(实测否掉了这条优化思路)。** 有人问过"把应用侧的
P-256 也换成 PSA/mbedTLS 会不会更快"(PSA 已经在树里,而且只对**应用路径**可行 —— 比特流那半在
PRE_KERNEL_1 没有堆)。做了个探针:同一个 build 里**同时**链 tinycrypt 与 PSA,同一份向量
(一个 32 B 摘要 + 裸 r‖s 签名 + 同一把公钥),`agrv2k_407` + 200 MHz 比特流,`k_cycle_get_32()` 计时:

| P-256 验签后端 | 单次 cycles | @200 MHz |
|---|---|---|
| tinycrypt `uECC_verify()`(现在的 ECDSA 档) | 22.14–22.17 M | **≈ 110.8 ms** |
| PSA / mbedTLS `psa_verify_hash()` | **315.8 M** | **≈ 1579 ms** |

即 **PSA 的 P-256 比 tinycrypt 慢约 14 倍**,换过去是严重退步(顺带量到的:PSA 的
`psa_crypto_init()` 只要 981 cycles,导入公钥 300 k cycles ≈ 1.5 ms;SHA-256 过 22.8 KB 两者接近,
tinycrypt 2.72 M vs PSA 2.98 M cycles)。

**结论:要砍开机延迟只有两条路 —— 应用侧换 RSA(~99 µs),或者让比特流留在 factory(比特流那半不验);
换 P-256 后端解决不了。**(1579 ms 对本芯片偏慢,XIP 取值 + 通用大数路径都可能是原因;但两个后端
跑在完全相同的条件下,这个比较本身是公平的。)

### 2.3 RSA 档的实现配方(约定已实测钉死,2026-09-19)

用 `imgtool keygen -t rsa-2048` + `imgtool sign` 签一张测试镜像,把 TLV 拆开对哈希,得到
**两个与 ECDSA 不同的约定** —— 这两条如果照直觉写会白干:`keygen -t rsa-2048` 签出来的容器里

```
TLV types: {0x10: 32, 0x01: 32, 0x20: 256}     # SHA256 / KEYHASH / RSA2048-PSS
```

1. **签名 TLV 是 `0x20`(`IMAGE_TLV_RSA2048_PSS`),256 B *裸*签名** —— 不是 DER(ECDSA 那边是
   DER 72 B,要自己转 r‖s;RSA 这条不用转,直接喂);
2. **`KEYHASH` = SHA-256(*PKCS#1* 公钥 DER,270 B)**,**不是** SPKI(294 B)的哈希 —— 而 ECDSA
   档我们实测的是 **SPKI** 的哈希。两个档的 KEYHASH 覆盖的编码**不一样**,不能共用一段代码。

实现落点(照 ECDSA 档的骨架改):

| 层 | 改动 |
|---|---|
| Kconfig | `choice BOOT_AGM_SIGNATURE` 加 `BOOT_AGM_SIG_RSA2048_PSS`,select `PSA_CRYPTO` + `PSA_WANT_ALG_RSA_PSS` + `PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY` |
| 公钥 | 嵌入 **270 B 的 PKCS#1 DER**(不是 64 B 裸点):它同时满足两件事 —— `KEYHASH = SHA-256(这份字节)`,而 PSA 的 `psa_import_key(PSA_KEY_TYPE_RSA_PUBLIC_KEY, …)` 要的也正是这个编码。头文件里按档定长(`AGM_BOOT_PUBKEY_LEN` = 64 / 270),样例的 `-DSPI_BOOT_PUBKEY` 长度断言同步放宽 |
| 验签 | `psa_crypto_init()` → `psa_import_key()` → `psa_verify_hash(key, PSA_ALG_RSA_PSS(PSA_ALG_SHA_256), hash, 32, sig, 256)`;失败返回 `PSA_ERROR_INVALID_SIGNATURE`(探针实测 -135) |
| 工具 | `sign_image.py` 的公钥导出与 KEYHASH 自检按档走(ECDSA = 64 B 裸点 + SPKI 哈希 / RSA = 270 B DER + PKCS#1 哈希);档位**从 key 文件类型推断**,不额外加参数 —— 也就没有"用 RSA 钥匙却按 ECDSA 编码"的空间 |
| 夹具/用例 | 一组 RSA 夹具(镜像 + 容器 + 270 B 公钥)与 native 正向/负向用例(篡改载荷、换密钥、裸镜像、错槽) |

### 2.4 RSA 档落地结果(2026-09-19,提交 `509c9e8` / `07de379`)

**已做**:上表全部落地。两处计划里没写、实现时才现形的:

1. **`PSA_WANT_ALG_SHA_256` 是必需的**,不是顺手加的 —— loader 自己用 tinycrypt 算 SHA-256 再把
   摘要交给 `psa_verify_hash()`,但 PSS 的 MGF1 还要一份 SHA-256;少了它 tf-psa 的 `extras/md.c`
   编成空壳,ZTEST 默认的 `-Werror=unused-parameter` 直接打断构建(native 首次构建即触发)。
2. **两个档的签名缓冲不能共用**:ECDSA 的 TLV 是 ~72 B DER(裸签名只有 64),RSA 是 256 B 裸的。
   共用 `MCUBOOT_SIG_LEN` 长度的缓冲会把 DER 截断 —— ECDSA 的 native 用例当场红了 4 例。

**loader 里的实测体积**(`agrv2k_407`,默认全片内布局,同一棵树):未签名 **57588 B** /
ECDSA **65708 B** / RSA **81976 B**(96 KiB 区的 58.6 % / 66.8 % / **83.4 %**)。也就是 §2.2 说的
+22.1 KB 是"从零引入 PSA+RSA"的量;在本来就带 SHA-256 与 SPI/NOR 栈的 loader 上,边际差
16.3 KB。真机序列(签名上传 → 试启动 → 确认 → confirmed 启动 → 篡改被拒)在
`开发记录（未随本仓库发布）`。

### 2.5 树里还有哪些密码实现(2026-09-23 盘点)

回答"还能不能用别的算法"之前,先把**这棵树里真实存在**的实现列清楚(四个来源):

| 来源 | 有什么 | 对我们意味着什么 |
|---|---|---|
| **mcuboot 自带的 tinycrypt**(`bootloader/mcuboot/ext/tinycrypt`) | ECC P-256(ECDSA 验签 + ECDH)、SHA-256、AES-128(CBC/CCM/CMAC/CTR)、HMAC-SHA256、两个 PRNG(**全部无动态分配**) | 就是现在 ECDSA 档和比特流档用的那套;唯一能在 **PRE_KERNEL_1** 里跑的 |
| **tf-psa-crypto**(经 PSA 接口,`modules/crypto/tf-psa-crypto`) | 签名:ECDSA(P-256/384/521、secp256k1、brainpool)、确定性 ECDSA、RSA(PSS / PKCS1v15 / OAEP);密钥交换:ECDH/FFDH/JPAKE(曲线含 Montgomery 255/448 = X25519/X448);对称:AES(CBC/CTR/CMAC/GCM/CCM/CFB/OFB)、ARIA、Camellia、ChaCha20-Poly1305;KDF/HMAC:HMAC、HKDF、PBKDF2、TLS1.2 PRF;哈希:SHA-1/224/256/384/512、SHA3、SHAKE、RIPEMD160、MD5 | 菜单很大,但**都要堆**(PSA 的 RSA/ECDSA 都会 malloc)→ 只能跑在运行时(应用路径);RSA 档就是这么接的 |
| **tf-psa 的替代/加速驱动** | `p256-m`(紧凑 P-256,`CONFIG_MBEDTLS_PSA_P256M_DRIVER_ENABLED`)、`everest`(X25519)、`pqcp` = **ML-DSA-44/87**(`CONFIG_TF_PSA_CRYPTO_PQCP_MLDSA_ENABLED`,EXPERIMENTAL,底层是 `mldsa-native`,核心无 malloc) | 见下面 §2.6 的实测:它们改变体积/速度,但不改变"PSA 要堆"这条 |
| **Zephyr 的通用 crypto 驱动 API**(`drivers/crypto/*`) | 全是**别家**的硬件加速(STM32/ESP32/nRF/mchp/Camellia…),外加一个 mbedTLS 软件 shim(只有 hash/cipher) | **AGM 没有任何硬件加速** → 我们全软件;这个 API 也不做验签 |

**明确没有的东西**:

* **Ed25519 / EdDSA**:PSA 的头文件里有算法*名字*,但 tf-psa 这份**没有实现**(`drivers/builtin/src/` 里没有 eddsa.c,测试套件里它在 "not supported" 名单上),tinycrypt 也没有 → 要用得自己 vendor 一份;
* **设备侧熵源**:`dts/riscv/agm/agrv2k.dtsi` 里**没有** entropy/RNG 设备 → `psa_generate_random()`/`psa_generate_key()` 没有可信种子。这对我们现在没影响(只验签、不产生密钥),但意味着"设备自己生成密钥"这条路要先解决熵。(R2b 的 nonce 不需要密码学强度,文档一直这么写。)

### 2.6 P-256 三个后端的实测(2026-09-23):tinycrypt 最快也最小

同一块 `agrv2k_407` + 200 MHz 比特流,`k_cycle_get_32()` 计时;体积是**同一份探针 app**在四个配置下的 ROM
差(基线 = 不链任何密码库 = **22,388 B**;PSA 核心 = **+7,884 B**,与本文 §2.2 说的 "+7.7 KB" 对得上):

| 验签后端 | 实现来源 | ROM 增量 | 单次验签 | 需要堆 |
|---|---|---|---|---|
| **ECDSA P-256,tinycrypt**(现在的档) | mcuboot vendored | **+4,352 B** | **110.8 ms**(22.14–22.17 M cycles) | ✗ |
| ECDSA P-256,PSA + **p256-m** | tf-psa driver | +14,600 B(含 PSA 核心 7,884 + p256-m 6,716) | 557 ms(111.4 M cycles) | ✓ |
| ECDSA P-256,PSA 内置大数 | tf-psa builtin | +28,164 B(含 PSA 核心) | 1579 ms(315.8 M cycles) | ✓ |
| RSA-2048-PSS | tf-psa builtin | +22,116 B(§2.2,含 PSA 核心) | **99 µs**(19.8 k cycles) | ✓ |

读法:**P-256 换后端全是退步** —— p256-m 比内置快 3 倍、小 13.6 KB,但仍比 tinycrypt 慢 5 倍、大一倍多;
"tinycrypt 小是因为没优化"这个直觉在这颗芯片上是反的(它的 P-256 是固定曲线的精简实现,而 mbedTLS
那套带通用大数/ECP)。所以 P-256 继续用 tinycrypt;真要砍开机时间就换 **RSA**(只对应用路径可行)。

> ML-DSA 只列不测:签名 2420 B / 公钥 1312 B、`mldsa-native` 核心无 malloc,但代码量级(估 50 KB+)与
> 验签速度都超出 96 KiB loader 的预算;要评估得单独开一轮(先量 ROM,再定能不能进)。

### 2.7 "给 AGM 做密码硬件加速"能做什么(2026-09-23 调研 + 两个实测)

**结论先写**:这颗芯片上**唯一的密码学硬件是 CRC 单元**,而且它真正值钱的地方不是安全,是**启动路径的
CRC 耗时**;AES/SHA/ECC/RSA/TRNG 都没有硬件,放进 fabric 也放不下。

**① 有什么硬件**

* **CRC0**(手册 §16,p.213 起):可编程多项式(7/8/16/32 位)、可编程初值、8/16/32 位喂数据、
  32 位 IO 寄存器 + 输入缓冲(**4 AHB 周期/32 位字**)、`REV_IN[1:0]`/`REV_OUT` 位反转、
  默认多项式就是 CRC-32/Ethernet。寄存器块 `0x4100_2000`(`DR/IDR/CR/INIT/POL`),**AHB 门控 =
  `SYS.AHB_CLKENABLE` bit 2**(SDK 的 `crc.h` 有整套内联助手;厂商 `example_crc.c` 用
  `SYS_EnableAHBClock(AHB_MASK_CRC0)` —— 开成 APB 的 bit 2 时寄存器读回 `0xffffffff`/`0x0`,
  一个值都算不出来,这是本轮卡最久的一步)。
* **没有的东西**:AES / SHA / ECC / RSA / TRNG 一律没有(手册特性表与全文只有 CRC 与 128-bit unique ID),
  dtsi 里也没有 entropy 设备。
* **fabric(CPLD)里造**:厂商自己的教程写着"**逻辑单元不能超过 2K,严格讲是 2112 个**";SHA-256 核约
  1.5–3 K LE、AES-128 约 2–3 K LE、P-256 验签要几万 LE → 放不下。而且**比特流就是被验的对象**,
  用它来验自己是循环论证(唯一不循环的用法是"工厂比特流里跑加速器去验更新槽",那只是把信任根从
  tinycrypt 换成出厂比特流,并不更安全)。

**② 驱动已落地并实测(2026-09-23,`agrv2k_407`,200 MHz)** —— 见
`开发记录（未随本仓库发布）`(`drivers/crc/crc_agm.c` + `crc0` 节点 + `samples/crc_agm`):

| 项 | 值 |
|---|---|
| 标准校验值 `crc32_ieee("123456789")` | **0xCBF43926**(硬件 == 软件)|
| 99944 B(fabric 槽大小) | 软件 **13.39 M cycles ≈ 67 ms** → 硬件 **0.70 M ≈ 3.5 ms**(**约 19×**)|
| 22784 B(应用镜像大小) | 软件 3.05 M ≈ 15 ms → 硬件 ~0.16 M ≈ 0.8 ms |

即"比特流来自更新槽"的板子每次启动少付 **~63 ms**。**③ 调用点零改动**:Zephyr 的
`crc32_ieee_update()` 是 **`__weak`**,`subsys/crc/crc_hardware.c` 在有 CRC 驱动时提供**强符号**,
而 `CRC_HW_HANDLER` 只要 `zephyr,crc` chosen 存在就默认开(`select CRC_DRIVER`)—— 一个节点 + 一个
chosen,loader 里现有的 `crc32_ieee*`(记录/镜像/bitstream)就全部走硬件。代价是顺带的三处顺序调整:
CRC 设备注册在 `PRE_KERNEL_1` priority 0、`soc.c` 的 SYS_INIT 挪到 priority 1、AHB 门控提到 FCB
之前(理由见 §3.29)—— 这三处都**只 gate 在 `CONFIG_CRC_AGM`** 上,没开 CRC 的 build 走原来的
顺序,所以 `hello_world` 这类镜像一行都受影响不到。

**④ 和"验签慢"没关系**:110 ms 是 P-256 曲线运算,芯片里没有对应硬件;能砍它的只有 RSA(应用侧,
99 µs)或"比特流留在 factory 不验"。

### 2.1 可选的三个算法档(已拍板的设计)

**选择粒度:每个构建一个档**(Kconfig choice),不是每张镜像。板子一旦烧成 `ECDSA-P256` 档,
它就只接受"用 ECDSA P-256 签过的镜像";换档要重烧 loader(顺带也就换了公钥)。

```kconfig
choice BOOT_AGM_SIGNATURE
	prompt "Image signature"
	default BOOT_AGM_SIG_NONE

config BOOT_AGM_SIG_NONE        # 只有 CRC32:防意外损坏,不防伪造(开发板默认)
config BOOT_AGM_SIG_ECDSA_P256  # SHA-256 + ECDSA P-256(推荐给生产)
config BOOT_AGM_SIG_RSA2048     # SHA-256 + RSA-2048(PKCS#1 v1.5,e = 65537)
endchoice
```

**镜像里仍要带算法字段(record slot 条目的 `sig_alg`)**,哪怕选择粒度是"每构建":`ECDSA`/`RSA`
档的板子必须能区分"这是 RSA 签的""这张根本没签名",并把两者都**拒绝**;只靠"签名字段全 0"猜会
把合法的全 0 签名误判。条目里存:

| 字段 | 大小 | 说明 |
|---|---|---|
| `sig_alg` | 4 B | `0 = NONE`(仅 CRC)、`1 = ECDSA-P256`、`2 = RSA-2048`;其它值一律拒 |
| `sha256` | 32 B | `sig_alg != 0` 时:镜像的 SHA-256 |
| `sig` | 0 / 64 / 256 B | 由本构建选定的档决定 —— **条目大小因此是构建期常量** |

**验收矩阵(每个档的行为)**

| 构建档 \ 镜像带的 `sig_alg` | NONE | ECDSA-P256 | RSA-2048 |
|---|---|---|---|
| **NONE**(仅 CRC) | 接受(CRC 过即发布) | 拒绝(本构建没编验签器) | 拒绝 |
| **ECDSA-P256** | 拒绝(缺签名) | 验签通过才发布/启动 | 拒绝 |
| **RSA-2048** | 拒绝 | 拒绝 | 验签通过才发布/启动 |

> 这张表就是**防降级**的定义:`ECDSA`/`RSA` 档不会因为"镜像没签名"而放行 —— 缺字段、字段为 0、
> 算法不匹配、签名长度不对,全部走"拒绝"这条路径(与签名验证失败同一条失败路径)。
>
> **每一格都有 native 用例(2026-09-20 补齐矩阵里最后两格)**:两档各自覆盖篡改载荷、换密钥、
> 裸镜像、错槽,**另一档签的容器**(`test_rejects_a_container_of_the_other_profile`),以及
> **把签名 TLV 的类型标签改成另一档**(`test_rejects_a_relabelled_signature_tlv` —— 摘要、KEYHASH、
> 签名都不动,只改那一个 selector 字节,所以它钉的正是"profile 由 TLV 类型决定")。
> 见 `tests/drivers/misc/boot_agm_{ecdsa,rsa}/src/main.c`。

> **CF-3 下的修正(2026-09-18)**:下面这两张表(条目带 `sig_alg`/`sha256`/`sig`、record 升 v4)
> 是 **CF-2(签名进 record)专属**的写法。选定的 **CF-3** 用 TLV 承载签名与摘要,算法由 TLV 类型
> 标识,record 继续用 v3、不升级 —— 见 §3 的"已定:CF-3"与 §3.1 的逐字段表。
> 另外 §2 上表里的 "RSA-2048" 在 CF-3 下精确化为 **RSA-2048-PSS**(MCUboot 只有 PSS,没有 v1.5)。

**条目大小与记录容量**(CF-2 专属;record v3 条目 88 B,append log 每 4 KiB 扇区写满才擦):

| 档 | 条目大小 | 每扇区条目数 | 对现有结论的影响 |
|---|---|---|---|
| NONE | 88 B | 46 | 与今天一致 |
| ECDSA-P256 | 88+4+32+64 = **188 B** | 21 | 磨损结论不变(每扇区 21 次提交才擦一次) |
| RSA-2048 | 88+4+32+256 = **380 B** | 10 | 同上;RSA 档条目明显更胖,仍远好于"每次提交擦一次" |

条目大小由构建期常量决定 ⇒ **换档就是条目布局变化**,所以 v4 的约定要覆盖它:见 §3 的 v4 说明
(v3→v4 迁移把老条目导入成 `sig_alg = 0`;签名档读回无签名条目按上表拒绝,不会发生降级)。

## 3. 签名格式:四个正式候选

先亮出**决定性的实现约束**(它把候选表重新排了序):三条上传路径里 **AN3155 是厂商工具驱动的,
GO 帧不带任何载荷** —— 任何"需要 host 在上传时报一个签名/算法给 loader"的方案,在 AN3155 上都
无处安放。于是候选表按"要不要动 Host 协议"这一刀重排:

| # | 候选 | 镜像字节 | 上传协议 | payload 链接 | record |
|---|---|---|---|---|---|
| **CF-1** | **签名尾随镜像**(footer) | 原样,尾部追加固定 footer | **三条全不动** | **不动** | **不动(v3 继续用)** |
| CF-2 | 签名进 record(v4) | 原样 | console/SMP 要加字段;**AN3155 无解** | 不动 | 升 v4 + 迁移 |
| CF-3 | MCUboot 格式(header + TLV) | 头部 + TLV 前置 | **三条全不动** | **要平移**(`slot + hdr_size`) | 不动(只用格式) |
| CF-4 | 自定义头部 | 头部前置 | 三条全不动 | 要平移 | 不动 |

### 落选的三个(CF-1 / CF-2 / CF-4,存档一句话)

* **CF-1 签名尾随镜像(footer)**:把 `magic|alg|sig|sha256` 定长追加在镜像尾部。它其实满足
  "三条协议不动 + payload 不重链接 + record 不升级",当年是推荐的备选;输给 CF-3 的原因只有
  生态(见下表:`imgtool` 现成、TLV 里的 `SEC_CNT` 现成)。**如果哪天要一个不依赖 MCUboot 生态的
  最小改动方案,它仍是第一备选。**
* **CF-2 签名进 record**:致命伤是 **AN3155 的 GO 帧无处放签名**(三条上传路径里唯一由厂商工具
  驱动的那个),record 还要升版本 + 迁移(条目 88 → 188/380 B)。
* **CF-4 自定义头部**:和 CF-3 同样有"payload 平移"的代价,却没有生态工具 —— 被 CF-1 支配,
  只列在候选表里保持完整性。

### CF-3:MCUboot 格式(header + TLV)

镜像 = `image_header`(含 `load_addr`/`img_size`/版本)+ 镜像后面的 TLV(`SHA256`/`KEYHASH`/签名,
未来还能带 `SEC_CNT` 做反回滚)。已有生态:**`imgtool` 本机 venv 里就能 import**(实测 ✓),
将来真要换 MCUboot 也平滑。

代价:**payload 必须按 `slot + hdr_size` 链接**(Zephyr 侧就是 MCUboot 集成那套
`ROM_START_OFFSET` 类开关),于是 AN3155 的 `-g <slot>`、文档里的窗口/入口地址、
`SPI_BOOT_IMAGE_LOAD/ENTRY` 全都要平移一个 `hdr_size`;另外 record 里将来会和 TLV 有少量
元数据重叠(版本/哈希),但**不需要重构**(见 §3.1)。

### CF-4:自定义头部(magic+len+sha256+sig 前置)

和 CF-3 同样的"前置头部"代价(payload 平移),却没有生态工具。它被 CF-1 支配:**尾部追加同样能
让签名跟着镜像走,而且不用动链接地址**。列在这里只是为了完整性。

### 成本矩阵(选型当时的对照,留档)

| 维度 | CF-1 footer | CF-2 record | **CF-3 MCUboot(选中)** | CF-4 自定义头 |
|---|---|---|---|---|
| console/AN3155/SMP 协议改动 | 无 | console/SMP 加字段,AN3155 无解 | 无 | 无 |
| payload 链接 / `-g` 地址 | 不动 | 不动 | 平移 `hdr_size`(主要风险点) | 平移 `hdr_size` |
| record 改动 | 无 | v4 + 迁移 + 档标识 | 无 | 无 |
| 生态/互操作 | 自研 | 自研 | **MCUboot 生态(`imgtool` 现成)** | 自研 |
| 反回滚扩展 | 自研 | 自研 | **TLV 现成(`SEC_CNT` 留作将来)** | 自研 |

### 已定(2026-09-18):**CF-3 MCUboot 格式**

选定 CF-3 后,本节其余候选(C F-2/CF-4)降为"未选方案",下面的补充条款生效:

**① RSA 档按 MCUboot 的 TLV 走,就是 RSA-2048 **PSS**[^pss]。** MCUboot 的 TLV 类型里只有
`IMAGE_TLV_RSA2048_PSS`(0x20)/`IMAGE_TLV_RSA3072_PSS`(0x23),**没有 PKCS#1 v1.5**;`imgtool`
的密钥类型也就 `rsa-2048` / `ecdsa-p256` 两种(已核实)。所以 §2.1 的 "RSA-2048" 精确化为
**RSA-2048-PSS**;要 v1.5 就得离开 MCUboot 格式,与本次选择冲突,不做。

**② 算法由 TLV 类型标识,record 不需要 `sig_alg` 字段。** `IMAGE_TLV_ECDSA256` 与
`IMAGE_TLV_RSA2048_PSS` 本身就是"这是哪个算法",`IMAGE_TLV_KEYHASH` 指明用哪把公钥
(将来多密钥时直接用它选)。所以 **§2.1 的"条目带 `sig_alg`/`sha256`/`sig`"与 §3 的 "record v4"
都是 CF-2 专属的写法,CF-3 下不采用** —— record 继续用 v3。

**③ CF-3 内部还有一个子选择:验签代码从哪来。**

> **已定(2026-09-19,实现时):走 CF-3b。** P1 的实测把 CF-3a 的成本翻了出来(那个 `.a` 里
> 只有 `bootutil_public.c`,`bootutil_img_validate()` 属于 bootloader 构建,还要把 MCUboot 的
> crypto 配置一起搬进来),而实现又必须自己写 TLV 遍历才能让**比特流档**用另一套 profile/密钥
> (§9.3);于是验签器是自己那 300 行:解析 header/TLV + 直接调 tinycrypt(ECDSA)或 PSA(RSA)。
> 代价是"失去 bootutil 那层经年测试的解析逻辑",换来的是零构建依赖、两档共用一段遍历、
> 以及 native 用例能把两档的每一格都钉住(§2.3/§2.4、peripherals §3.26.32/§3.26.34)。

| | CF-3a:借 `bootutil_img_validate()`(推荐) | CF-3b:自己解析 header/TLV + 自己调 PSA/mbedTLS |
|---|---|---|
| 代码量/风险 | 用 MCUboot 现成、上游维护、已被大量设备跑的验证器 | 自己写解析与比对,SHA-256/签名验证已经踩过的坑要重踩一遍 |
| 依赖 | 需要 `flash_area`(Zephyr flash map + `slot0/slot1` 分区,**已验证**),而且**不是一句 Kconfig** —— 见 P1 的实测备注 | 只依赖 PSA/mbedTLS 与自己的 flash 读 |
| 适用 | 想尽快把"能验签"装上、以后想真换 MCUboot | 想完全掌控验证逻辑、不想引入 bootutil 的构建依赖 |
| 与 CF-3 的关系 | 天然一致(格式就是它的格式) | 也可以,但要自己保证与 `imgtool` 产物的字节级兼容 |

当时的推荐是 **CF-3a**(省掉"自研验签器"这一整块风险,而且验证器与其 crypto 后端 —— mcuboot
自带的 tinycrypt,体积小 —— 可以一起量进 P1;注意 bootutil 并不要求我们把 loader 换成 MCUboot,
它是一套库)。**实现时改成了 CF-3b**,理由见上面那条"已定"注:CF-3a 的构建边界比预期大,而比特流档
本来就需要一个 profile/密钥可换的遍历器。

**④ 容器化会牵动哪些地址(一次性、可验证)**

| 对象 | 变化 |
|---|---|
| `samples/spi_boot_app`(RAM 与 internal 两个变体) | 按 `slot + hdr_size` 链接(MCUboot 集成那套 `ROM_START_OFFSET` 类开关) |
| `samples/spi_boot_loader` 的 `SPI_BOOT_IMAGE_LOAD/ENTRY` 默认值 | 加上 `hdr_size`(或改成从 header 读 `load_addr`) |
| AN3155 的 `-g <addr>`、文档里的窗口/入口地址、真机清单里的地址 | 全部平移 `hdr_size` |
| loader 的 `entry` 计算 | 实现成 `slot + hdr_size`,同时**要求 header 的 `ih_load_addr` 就是它**(不符即拒收,见 peripherals §3.26.28)——等价于「从 header 读 `load_addr`」,但顺手把"传错槽/传错板"的镜像挡在发布之前 |
| 映像长度 | record 的 `ext_len` = `hdr_size + img_size`;TLV 区在它后面(仍在同一个 store 区内) |

**⑤ 还有一个子选择:NONE 档要不要也强制容器?**

| | CF-3-S1:所有档都要求 MCUboot 容器 | CF-3-S2(推荐):NONE 档继续收裸镜像 |
|---|---|---|
| 含义 | 连开发板也走同一个容器(只校验 SHA-256 TLV) | NONE 档行为与今天完全一致(裸 `.bin` + CRC);ECDSA/RSA 档要求容器且必须验签 |
| 优点 | loader 只有一条路径 | 开发板/现有工具链零改动;迁移平滑 |
| 缺点 | 开发板/文档/样例全要跟着重链接 | loader 里多一个"这是不是容器"的分支 |

推荐 **CF-3-S2**:把"容器"限制在需要签名的档里,`NONE` 档保持今天的裸镜像语义(也就保住了
"CRC 只做损坏检查"这条已定的语义)。

[^pss]: PSS 的验签比 v1.5 多一层 MGF1 + salt 处理,代码量比 v1.5 略大;这正是 P1 要量的一部分。

### 3.1 如果改用 MCUboot:**格式**和**状态模型**要分开看

"用 MCUboot" 有两层意思,对 record 的影响完全不同:

| | 只借它的**镜像格式**(header + TLV) | 借它的**状态模型**(bootutil + trailer + swap) |
|---|---|---|
| 是什么 | 镜像 = `image_header`(32 B,含 `load_addr`/`img_size`/版本)+ 镜像后面一段 TLV(含 `SHA256`、`KEYHASH`、签名、可选 `SEC_CNT`) | 再加:每个 slot 末尾的 trailer(`image_ok` 标志)+ swap 状态区/scratch,由 bootutil 驱动 swap / overwrite / direct-XIP |
| record 要重构吗 | **不需要**。record 继续当 A/B 状态的主人,只是它的 `ext_crc` 可以被 TLV 里的 SHA-256 取代(甚至可以退休),签名/摘要不再进 record | **需要**。能被推导的字段全部搬走,只剩策略字段;见下表 |
| 代价落在哪 | payload 必须以 `slot + hdr_size` 为运行地址(Zephyr: `CONFIG_ROM_START_OFFSET`)、host 侧换成 `imgtool`、loader 从 header 推 `entry` | 还要接受"两个同尺寸 slot + scratch"的硬约束(见下),以及 MCUboot 对"什么算有效镜像"的定义 |

**逐字段对照**(record v3 的每个字段在两种方案里的归宿):

| record v3 字段 | V1:只用镜像格式 | V2:再借状态模型 |
|---|---|---|
| `ext_off` | 保留(record 说了算) | 推导:flash_area / DT partition |
| `ext_len` | 保留(可与 `img_size` 交叉校验) | 推导:`hdr_size + img_size`,并和 slot 大小比对 |
| `ext_crc` | 保留(快速自检),或退休让位给 TLV 的 SHA-256 | 退休(SHA-256 接手) |
| `state` = EMPTY / TRIAL | 保留 | 推导:slot 里有没有合法 header;"TRIAL" = 未 `image_ok` 的 pending |
| `state` = CONFIRMED | 保留 | 推导:trailer 的 `image_ok` |
| `state` = **BAD** | 保留 | **没有对应物** —— MCUboot 只有"下次启动 swap 回来",没有"这块坏了别再试" |
| `attempts` | 保留 | **没有对应物**(所以我们的"3 次尝试后判 BAD"这条策略无处可放) |
| `load` / `entry` | 保留(可从 `load_addr` 推) | 推导:`load_addr` / `load_addr + hdr_size` |
| `src`(store / on-die) | 保留 | 推导:slot 就是 flash_area |
| `mode`(auto/internal/external) | 保留 | **没有对应物** —— 始终是我们的策略 |
| `once`(RTC 一次性覆盖) | 保留 | **没有对应物** |

**结论**:如果只是想要"MCUboot 那套签名容器",**record 不用重构**,改的是 payload 的链接布局 + host 侧工具 +
loader 从 header 推导入口;如果要连它的**状态模型**一起用,record 会瘦成"只剩策略字段"的小块
(`mode`、可选的一次性覆盖、以及要不要保留尝试计数),因为 offsets/lengths/hash/KEYHASH/state 都能从
镜像 + flash map + trailer 推出来。

**但 V2 有一个绕不过去的硬约束:我们的 A/B 不是 MCUboot 的 A/B。**

* MCUboot 的 swap(以及 overwrite-only)都假设 **两个同尺寸 slot 在同一块 flash 上**(swap 还要一个
  scratch 区);而我们的布局是"外部 NOR 里两个 store(默认布局 508 KiB,片内布局 60 KiB)+ 片内一个
  512 KiB 执行槽",**跨两颗 flash、尺寸也不同**;
* 于是只有 MCUboot 的 overwrite-only / direct-XIP 类模式勉强能映射,而 overwrite-only 是
  **直接覆盖 primary** —— 我们辛苦做的"坏镜像能回滚到另一侧"就没了(那正是 §3.26.13 的 A/B 策略);
* 想让 MCUboot 的 swap 生效,就得把布局改成"同尺寸双 slot + scratch"(等于重排片内/NOR 分区),
  这是比 record 重构大得多的改动。

所以计划里的建议保持不变:**格式可以用 MCUboot 的(甚至可以直接用 TLV 当签名容器),状态模型继续用我们
自己的 record** —— 这也正好呼应 §2.1 的"分离式签名"设计:签名/摘要放哪是**容器**问题,A/B 状态放哪是
**策略**问题,两者可以独立选。

### 3.2 如果**整体换成 MCUboot**(把 loader 整个换掉)会怎样

§3.1 只回答"借它的格式 / 借它的状态模型"。这里回答更激进的一档:**把我们的 loader
(`samples/spi_boot_loader` + `drivers/misc/boot_agm*.c`)整个换成 MCUboot**(
`bootloader/mcuboot/boot/zephyr` 那个 bootloader,`west.yml` 里钉的是 `aa32eaaa`)。

**结论:不做。** 前三条是读码得到的硬约束,第四条是实测的体积对照 —— 而这条恰好说明它**换不来**
片内 A/B 布局真正想要的空间。

**① 没有 RISC-V / AgRV2K 的板级移植**

* `boot/zephyr/boards/` 有 **59 个**板级 `.conf`,**没有一个 RISC-V**;
* `boot/zephyr/arch/` 只有 `arm.c` / `esp32.c` / `xtensa.c` / `arc.c` 加一个通用兜底 `default.c`
  (那个兜底就是"锁中断 + 跳到 `flash_base + off + hdr_size`");
* 树里唯一带 riscv 字样的是 `boot/espressif/` —— Espressif 自己的 IDF 移植,不走 `boot/zephyr` 这条路。

所以"整体迁移"的第一步是**给 MCUboot 写一个 AgRV2K 端口**,量级比我们现在的 loader 大。

而且不只是"加一个 board conf"这种工作量:MCUboot 的 Zephyr 集成假设 app 是
"slot 基址 + `hdr_size`"链接的,靠 `USE_DT_CODE_PARTITION` / `ROM_START_OFFSET` 那一套;
本仓库 **2026-09-17 实测过**:这套机制在 RISC-V 上**不移动链接地址**
(见 `samples/spi_boot_app/boards/agrv2k_407.overlay` 的注释 —— 它在 2026-09-24 之前叫
`agrv2k_407_internal.overlay`,随 RAM 变体删除而改名为该板唯一的 overlay),真正起作用的是
`/chosen/zephyr,flash`。也就是说连"app 怎么按 slot 链接"这一环,在这颗芯片上都得按它的脾性重做
(可行 —— 再定义一个基址为 `slot + hdr_size` 的 fixed-partition 指过去 —— 但与 MCUboot 文档里写的
操作步骤不同,每一步都得自己验)。

**② 状态模型对不上(§3.1 的 V2 那一栏)**

MCUboot 的 slot 是"**同一块 flash 上两个等尺寸分区**(+ 可选 scratch)",我们的模型是
"**两颗不同 flash** 上的两个 store(外部 NOR 508 KiB / 片内 60 KiB)+ **一个** 512 KiB 片内执行槽",
并且 loader 每次启动是**把选中的 store 拷进执行槽**再跳(`store A -> on-die slot`,真机日志)。

这三个区域**角色不同**,不是三份等价副本:store A/B 是**上传目标**(上传只写当前没在跑的那一侧),
执行槽才是**唯一有链接地址、能跑的那块**(payload 链接在 0x80030000);默认布局里前两者在外部 NOR、
只有执行槽在片内,片内 A/B 布局时三者才在同一颗 flash 上(on-die DFU 模式下直接写执行槽,不拷贝)。
MCUboot 是**两个**区域就够:primary 自己就是执行区,secondary 是上传目标,回滚靠 swap 状态机 +
trailer —— 下面三条不能被它表达,正是因为这个"上传区/执行区分离 + 第三块地址"的形状。

* `BOOT_SWAP_USING_SCRATCH/MOVE/OFFSET` 都表达不了"从两个 store 挑一个装进第三个地址";
* `BOOT_UPGRADE_ONLY`(overwrite-only)是**直接覆盖 primary** —— 我们用 A/B 换来的"坏镜像退回另一侧"
  就没了;
* `BOOT_DIRECT_XIP` 要求两个 slot 都能**原地执行**,而我们的 store 在外部 NOR 里(不是执行地址)。

想让它成立就得把布局重排成"同尺寸双 slot + scratch" —— 比 record 重构大得多,还要放弃外部 NOR
store 的容量优势。

**③ loader 今天不是"只挑镜像"**

`drivers/misc/boot_agm*.c` 里有五件 MCUboot 不提供的事:

| 能力 | 现状 | MCUboot 侧 |
|---|---|---|
| AN3155 服务端(让 `agrv32flash` 可用) | `boot_agm_an3155.c`,820 行 | **没有**,而且 AN3155 不是 CBOR/SMP 帧 |
| mcumgr/SMP 服务端 | `boot_agm_smp.c`,423 行 | serial recovery 有同类能力(mcumgr 帧) |
| 控制台命令(`install` / `once` / …) | `boot_agm.c` + `main.c` | 没有(只有 serial recovery 模式) |
| 比特流(fabric)收发与暂存(staging + BSB1 + CRC) | `boot_agm.c` | **没有概念** |
| record v3 A/B 策略(EMPTY/TRIAL/CONFIRMED/BAD、3 次尝试、`mode`、RTC `once`) | `boot_agm.c` | 只有 trailer + swap 状态机 |

MCUboot 对外留的扩展点只有一个:`mcuboot_bs_custom_handlers`(给 serial recovery 的 CBOR 协议挂
自定义 group,`boot/zephyr/boot_serial_extension_zephyr_basic.c` 就是这么写的)。它能挂我们的 SMP
group,**挂不了 AN3155** —— 那不是 CBOR,得改到 `serial_adapter.c` 那一层。

**④ 它换不来空间(实测,2026-09-18,`-Os`)**

MCUboot 一栏在 `nrf52840dk/nrf52840` 上量(MCUboot 有移植的那一档,ARM 对 rv32imafc 只当同量级看),
loader 一栏在 `agrv2k_407` 上量,同一台机器、同一天、仓库 HEAD 与工作区镜像同为 `12d2341`:

| 构建 | FLASH(`.bin`) | RAM(`.bss`) |
|---|---|---|
| MCUboot,ECDSA-P256 + tinycrypt | **30104 B** | 15978 B |
| MCUboot + serial recovery(`CONFIG_MCUBOOT_SERIAL=y`,关掉 UART console) | **37104 B** | 19944 B |
| 我们的 loader,默认布局,NONE 档 | 64944 B | 18500 B |
| 我们的 loader,默认布局,ECDSA 档 | **72768 B** | 18500 B |
| 我们的 loader,片内 A/B 布局,ECDSA 档 | **72548 B** | 18500 B |
| 我们的 loader,ECDSA 档,**关 mcumgr/zcbor/net_buf** | 59472 B | 12652 B |
| 我们的 loader,ECDSA 档,关 UART console/printk/stdout | 72532 B | 18500 B |

读法:MCUboot 小,是因为它**不做**③里那五件事。把那些搬回去(哪怕 SMP 走它的扩展点、AN3155 自己
再写一遍),体积只会回到同一量级 —— 我们这 72 KB 的构成大致是
`boot_agm.c` 14.6 KB、mcumgr 生态(zcbor/net_buf/heap/rb/work …)≈15 KB、tinycrypt 7.4 KB、
SPI/NOR 栈 8.1 KB、app 侧 `main.c` 5.2 KB、AN3155 3.6 KB、SMP 1.8 KB。

顺带量出来三条,直接关系到"loader 区要多大"这个老问题:

* **关掉 mcumgr 那一坨省 13.3 KB**(72768 → 59472),是目前最大的一块可裁剪空间;
* **关掉 console/printk 只省 236 B** —— printf 机制被驱动侧的 `LOG` 共用,"砍 console 省空间"是
  个直觉陷阱(量过才算数);
* **`CONFIG_BOOT_AGM_AN3155=n` 现在编不过**:`samples/spi_boot_loader/src/main.c` 无条件调用
  `agm_boot_an3155_run()` / `agm_boot_an3155_status_print()`(实测报 `implicit declaration`)。
  要让"瘦身档"成立,得先把这两处按 `IS_ENABLED()` 保护起来(SMP 那侧已经是这么写的)。

**结论与重估条件**

* **不整体迁移**:§3.1 的结论保持 —— 格式拿 MCUboot 的(CF-3),策略留我们的(record v3);
* 片内 A/B 布局装不下签名 loader 的问题按原方案解决 —— 扩 loader 区,或该布局不开签名。
  如果在意那个布局的空间,该做的是**片内瘦身档**(见上面的 13.3 KB),不是换 bootloader;
* MCUboot 真正值钱、我们也确实该拿的是**格式层**的东西:`imgtool`(已在用)、`SEC_CNT` 反回滚、
  图像加密(X25519)/multi-image 的 TLV 定义 —— 它们都不需要换 loader;
* 值得重新评估"整体迁移"的触发条件:①需要 MCUboot 独有的 image encryption 或 multi-image;
  ②"两个等尺寸、能原地执行或能 swap 的 slot"变成架构本身(例如放弃外部 NOR store);
  ③上游/社区已经有人做了 `boot/zephyr` 的 RV32 端口,移植成本从"写一个端口"降成"加一个 board conf"。

## 4. 公钥放哪

> **已定(2026-09-18)**:**第一步把公钥编译进 loader**(`const uint8_t agm_boot_pubkey[]`,长度与格式随 §2.1 的档:
> ECDSA P-256 = 64 B 裸公钥;RSA-2048-PSS = MCUboot/mbedTLS 的 DER 或 `imgtool` 导出的形式)。片内只读 key 区
> (轮换)不在本计划里,作为**单独的计划**。`IMAGE_TLV_KEYHASH` 仍然要写进签名镜像 —— 它是"这份镜像用哪把
> 公钥签的",将来多密钥/轮换时直接用它选择,不用改镜像格式。

| 位置 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| **编译进 loader**(`const uint8_t agm_boot_pubkey[64]`) | 最简单;改公钥必须改 loader(物理写保护之外还多一层) | 轮换要刷 loader | **起点** |
| 片内 flash 只读 key 区(新 DT 属性 `pubkey-offset`/`pubkey-size`) | 可轮换(用旧 key 签新 key) | 需要"key 更新"协议 + 写保护策略;loader 要防"key 区被改" | 第二阶段(与生产锁一起设计) |
| option bytes | 硬件级、和 RDP 同级 | AgRV2K 的 option bytes 只有 2 字节 user data(实测:`option byte register` 里 `user data = 0xffff`)——**放不下任何摘要**;它现有用途是 RDP 与 FPGA 配置指针 | ✗ 不可行(除可选的 2 字节"key 代号") |
| 从 staging/外部 NOR 读 | 灵活 | 攻击者也能改它 → 等于没有根 | ✗ |

## 5. 验证时机与失败语义(**已实现**,2026-09-19)

1. **发布时**(`publish_upload()` 的所有入口:console FINISH、AN3155 GO、SMP upload 完成、
   `install`——它们都汇到这一个函数):
   * 先按容器里的 SHA-256 TLV 对照"头 + 负载"复算,再验 KEYHASH 与签名;
   * 失败 → console/AN3155 **NAK code 4**(`UP_ERR_REJECT`)、SMP `EACCESSDENIED`,
     **record 一个字节都不写**;裸镜像在签名档直接 `-EINVAL`(防降级);
   * 成功 → record 只记 `len` + CRC-32(见下面第 3 条),**摘要与签名不落 record** ——
     它们是容器自带的一部分(CF-3,§3),启动时从槽里现读。
2. **每次启动前**:
   * 应用镜像:`boot_ab_policy()` 先按已有规则记一次试验额度(为了"掉电也计数"),`store_boot()`
     再在拷贝/跳转之前调同一个验证器;失败 → **不拷贝、不跳转**,控制台报
     `store A is no longer acceptable (-13) -- not booting it`,然后落在 loader 控制台。
     试验镜像走已有的 A/B 规则(连续 3 次起不来 → BAD → 另一侧可用就回退),**不是"一次就判死"**;
     CONFIRMED 槽验不过则直接停在控制台(那说明 flash 被改过,而不是"新镜像没跑起来");
   * 比特流:**`fcb.c` 在碰 FCB 之前**先按记录里的 `len`/`crc` 回读比对,再验容器;失败 →
     不流它,改流 factory 区,原因经 `agm_fcb_bitstream_refused` 报到控制台(§9.1、§3.26.34)。
3. **CRC32 保留**,但只在 record 里当"记录与槽是否一致"的快检:签名与摘要在容器里,
   CRC-32 负责的是"先校验再拆 fabric"这种便宜的一步(比特流档),以及 SMP/记录自身的损坏检查。

## 6. RSA 档的 22 KB 花在哪、能不能瘦(实测,2026-09-19 复核)

同一棵树、同一个样例(`samples/spi_boot_loader`,默认全片内布局,**loader 区 96 KiB**):

| 档 | ROM | 占 96 KiB | 余量 | RAM |
|---|---|---|---|---|
| 未签名 | 57652 B | 58.7 % | 39.7 KB | 16420 B |
| ECDSA-P256 | 65772 B | 66.9 % | 31.8 KB | 16420 B |
| RSA-2048-PSS | **81544 B** | **83.0 %** | **16.4 KB** | 25668 B |

也就是 §2.2 那个 "+22.1 KB" 是"从零引入 PSA+RSA"的量;相对 ECDSA 的边际原来 +16.3 KB,
瘦身后 **+15.7 KB**。拆开看:`tf-psa-crypto` 一项 **18.6 KB**(PSA 核心约 7.7 KB +
RSA/bignum/ASN.1 约 11 KB),再加一点胶水(下表第二行之前还有一份 tinycrypt SHA-256)。

**两处可以立刻减,都不是砍功能**:

| 手段 | 实测效果 | 说明 |
|---|---|---|
| `CONFIG_MBEDTLS_PSA_KEY_SLOT_COUNT=2` | **RAM 25668 → 17764 B(−7.9 KB)**,ROM +8 B | 默认 16 个静态 key slot(每槽还得按最大密钥类型留空间),而我们只 import 一个公钥 |
| 摘要也走 PSA 的 SHA-256,RSA 档不再链 tinycrypt | **ROM 82040 → 81544 B(−496 B)** | RSA 档里 SHA-256 原本有**两份**(tinycrypt 算镜像摘要 + PSA 算 PSS 的 MGF1;`rom_report` 里 tinycrypt 1426 B 全在 sha256.c)。去掉的是那 1426 B,PSA 的 hash API 又加回约 930 B,所以净 −496 B |

瘦身后的边际变成 **+15.7 KB**(81544 vs 65772)。顺带一个好处:RSA 档现在**不需要 mcuboot
模块**(tinycrypt 只在 ECDSA 档里链),`drivers/CMakeLists.txt` 的那个 FATAL_ERROR 只在
ECDSA 档触发。这两处都做了,并在真机上复验过:RSA loader 上传签名容器 → `image verified
(MCUboot container, 22856 B, RSA-2048-PSS)`,篡改一个字节 → `NAK (code 4)`。

**更大的旋钮**(都不是"砍功能",而是布局):`loader-size` 默认 96 KiB,但片内布局里 staging 与
fabric 预留之间**并没有**富余 —— 那一带早在 SoC 固定两个比特流 update 槽、比特流 record 和
bind-salt 扇区时就用掉了(2026-09-24 校正,见 [`FLASH-LAYOUT.md`](FLASH-LAYOUT.md) §8;
原文写"116 KiB 富余、提到 128 KiB 可行",那是比特流 A/B 几何定下来之前的算法)。要加
`loader-size` 得从别的区域让出来,`boot_agm.c` 的断言会把撞上的一方报成构建错误;
两 flash 变体
(`agrv2k_407_ext_nor.overlay`)的 loader 区是 **192 KiB**,RSA 只占 43 %。

**真正决定档位的不是 ROM,而是"每次启动都要付的那一次验签"**:`store_boot()` 每次启动都重新验
(注释就写着"防发布后被改写"),于是 ECDSA 的 **~110 ms**(§2.2 实测 22.07 M cycles @200 MHz)
是**每次开机**的延迟,而 RSA 是 ~99 µs。所以取舍是"~16 KB flash ↔ 110 ms 开机",不是"16 KB 值不值"。

**开机延迟预算(2026-09-23 汇总;回答"到底哪几次验签、各付多少")** —— 一次完整启动最多付**两**次
验签,而且只有一处是"每次都付":

| 验签对象 | 在哪 | 频率 | ECDSA P-256 |
|---|---|---|---|
| **应用镜像** | loader 的 `store_boot()`,跳转之前 | **每次启动**(注释:防发布后被改写) | ~110 ms |
| **比特流(fabric)** | `fcb.c::agrv2k_fcb_program()`,在 `#if AGM_BITSTREAM_SIGNED` 的 `if (sel.from_record)` 里面 | **只在 fabric 来自更新槽时**;记录空或指向 factory → 走 ROM 已经流过的那份,**不验** | ~110 ms |

所以"签名档 + 比特流也从槽起来"最坏约 **220 ms**;RSA 档只能省应用那一半(99 µs)—— 比特流那半跑在
PRE_KERNEL_1、**没有堆**,PSA 的 RSA 用不了(§9.3),这也是比特流档固定 ECDSA 的原因。要再省只能改
策略(例如"记录一直空着、只用 factory fabric"),换算法解决不了。

顺带两点容易误解的:①**签名不是加密**,P-256 这条路没有任何"解密"步骤 —— 比特流在 flash 里一直是
明文,验签只回答"这份是不是我们签的";②载荷应用默认**不**开 `BOOT_AGM_BITSTREAM_SIGNED`
(默认 n),它启动时也会按记录重流一遍 fabric,但**不验**(设计上假定 loader 已经验过、且两段之间
没人写 flash);要让两段口径一致,就给载荷也打开这个选项(代价同上)。

## 9. 比特流签名:已实现(2026-09-19)

> **状态**:已落地、已真机验证。下面是设计与当初的缺口对照。**与 §9.1 的唯一偏差**:比特流档不是 RSA 而是固定的
> ECDSA P-256 —— 不是偏好,是启动路径的硬约束,见 §9.3(实测)。
> 开关:`CONFIG_BOOT_AGM_BITSTREAM_SIGNED` + `CONFIG_ISR_STACK_SIZE >= 3072`。

**当初的缺口**(2026-09-19 记录,同月补上):签名的范围只覆盖应用镜像,查一遍代码就能看到
比特流这一半更空 ——

* `publish_upload()`(drivers/misc/boot_agm.c)对 `AGM_BOOT_TARGET_BITSTREAM` 先
  `bitstream_commit()` 再 `return`,**根本没走到** `agm_boot_image_verify()`(现在先验后提交);
* 启动路径 `fcb.c::agrv2k_fcb_program()` 按记录选地址直接流,连记录里的 payload `crc`
  都不复核(`agm_bitstream_slot_in_use()` 只校验记录自身的 `rec_crc`)(现在先回读比对、
  再验容器,失败就退回 factory)。

也就是说,当时比特流的完整性只有"上传时回读比对"这一道 —— 是签名范围本身的缺口。补法是
**同一个模式重复一遍**:提交前验一次、启动前验一次(§9.1 的五条,已按 §9.3 的实测约束落地)。

**算法在这一档当初打算反过来选(RSA)** —— 理由是比特流的验证发生在**每次启动**的路径上,
ECDSA P-256 的 ~110 ms 会直接变成开机延迟,而 RSA-2048-PSS 只要 ~99 µs(§2.2 的实测)。
**实测否掉了这一条**:RSA 验签要过 PSA,PSA 要 `malloc`,而启动路径(`PRE_KERNEL_1`)没有堆
(§9.3)。最终选的是 ECDSA,并因此给出那条 `ISR_STACK_SIZE >= 3072` 的编译期断言。

三件事要分清,**签名买不到其中两件**:

> **2026-09-24 补:这一档的真实启动代价量出来了 —— 每次启动 +3.13 s**,因为验签发生在
> `agrv2k_clk_switch_pll()` 之前(CPU 还在 10 MHz 的 HSI 上)。应用档同一轮实测是
> +101 ms。也就是说 §9.3 里"ECDSA 的 110 ms 会变成开机延迟"那个量级判断低估了一个数量级:
> 在 HSI 上真正贵的是"扫 99944 B 的 SHA-256",换 RSA 也压不到 ms 级。数字、对照与
> "把验证挪到 PLL 之后"的选项见 [`BOOT-DFU-STATUS.md`](BOOT-DFU-STATUS.md) §5.5 / §4 第 5 条,
> 真机记录在 `开发记录（未随本仓库发布）`。

| 想要的性质 | 谁给 | 备注 |
|---|---|---|
| 只有持私钥者能装/启动应用镜像 | ✅ 本文已实现(ECDSA/RSA,均已真机验证) | 且**不阻碍 DFU** —— 这正是它和厂商 logic 加密的分水岭 |
| 比特流也不能被换 | ✅ 已实现(`CONFIG_BOOT_AGM_BITSTREAM_SIGNED`,§9.1 + peripherals §3.26.34) | factory 那份例外:ROM 只认 raw/压缩形态,任何容器它都读不了 |
| 别人读不到比特流/固件 | `lock_flash`(读保护;unlock 会全片擦) | 与签名正交,签名不提供机密性 |
| 每芯片绑定(防克隆) | 签名**买不到**:签名是可移植的,同一份签名镜像在任何嵌入同一公钥的板子上都能跑 | 要这个性质只能走厂商 `logic.encrypt`(代价:放弃设备侧比特流更新,见 `开发记录（未随本仓库发布）`),或者自己设计"设备侧持有密钥"的封装 |

### 9.1 用它替换厂商的 UID 加密:能换掉哪一半(2026-09-19)

厂商那套(UID 派生 + 无签名)只买"防复制",而缺的恰好是"防替换";我们自己的 ECDSA/RSA 只买
"防替换",天生不买"防复制"。两者是**正交**的,所以"替换"要按性质拆开:

| 想要的性质 | 谁给 | 现状 |
|---|---|---|
| **只有我们能产出被接受的镜像**(防替换/防篡改) | 我们自己的签名 | 应用镜像 ✅ 已实现;比特流 ❌ 缺口(§9 上半) |
| 把 flash 原样复制到另一颗芯片不工作(防克隆) | 厂商 `logic.encrypt`(UID 派生) | 可选,且与签名不冲突。**2026-09-23**:这套密钥流已完整读出(`开发记录（未随本仓库发布）` ),所以"可选"里现在多了一条"设备侧自己也能封"的实现路径 —— 但它是可计算的混淆,不是密钥 |
| 别人读不到比特流/固件 | `lock_flash`(RDP) | 未做(代价:unlock 全擦) |
| 旧版本不能重放 | anti-rollback 下限(record v4 的 `sec_ver`) | ✅ 已实现(2026-09-20,§10 + peripherals §3.26.36:安装时拒旧版,NAK code 5 / SMP `EBADSTATE`;限度=仍能用原始 flash 写绕过,那要 R2c) |

**具体怎么接比特流签名**(补 §9 上半那个缺口)——**以下五条已落地**,
与计划的差异逐条标注:

1. **槽里存容器** ✅:`tools/sign_image.py --bitstream`(`--slot-base` 固定成
   `0x800b0000`,并强制输入就是 99944 B);`fcb.c` 从 `slot + hdr_size` 开始流 —— 和应用载荷
   同一条约定。**差异**:两个槽互换,所以容器只有一个戳(`ih_load_addr = 0x800b0020`),
   不是"一槽一戳";应用容器因此在上传时就被拒(戳/尺寸都不对);
2. **两处验签** ✅:提交前(`bitstream_commit` 之前)+ 每次流之前(`fcb.c`);§7.7 第 3 条的
   payload CRC 预检**做在验证函数里**(先按记录的 `len/crc` 回读比对,再验容器);
   失败路径是"退回 factory 区"(板子活着),不是"流一份坏 fabric";
3. **factory 那份保持"生产时写入即受信"**:ROM 只认 raw / `[算法][压缩]` 两种形态,任何容器它
   都读不了 —— 所以**签名覆盖不了 factory**,只能覆盖设备侧会去流的槽。想连 factory 也防替换,
   得靠 RDP/写保护(或厂商那层);
4. **算法** ⚠️ **改成 ECDSA P-256,而且是固定的**(不看应用档):RSA 的验签要过 PSA,
   PSA 要 `malloc`,而 `malloc` 在 PRE_KERNEL_1 **根本不存在** —— 实测见 §9.3。
   代价是每次启动多 ~110 ms;RSA+fabric 的组合仍可编(两个后端都链,89496 B / 96 KiB);
5. **密钥** ✅:`agm_boot_bitstream_pubkey[64]`(`-DSPI_BOOT_BITSTREAM_PUBKEY=`),
   全 0 且应用档是 ECDSA 时回落到应用那把 —— 一把钥匙签两处就不用第二个 `-D`。

**结论**:厂商的 UID 加密换不掉"防读",也换不来"防替换";我们自己的签名换得来"防替换"
(而且不阻碍 DFU)。两者叠加时,合理分工是 **factory 用厂商密文防复制、槽用我们的签名防替换**。

### 9.2 签名 + UID 绑定:两种做法,代价都在"每芯片一份"

"ECDSA/RSA + UID"这个概念上是**厂商那套思路的正规版**:他们用 UID 派生密钥流拿到"每芯片绑定"
却丢了真伪;我们用签名拿到真伪,再把 UID 拉进来就补齐绑定。四种组合挡什么:

| 组合 | 通过 DFU 换镜像 | dump 一颗搬到另一颗 | 每芯片一份签名? | 备注 |
|---|---|---|---|---|
| 纯签名(现状的一半) | ✅ 拒 | ❌ 照跑(签名天生可移植) | 否 | 最现实的攻击面(上传口)已经挡住 |
| 纯厂商 UID 密文(现状的另一半) | ❌ 放行 | ✅ 拒 | 否 | 无真伪:谁都能为这颗芯片封一份**自己的**镜像 |
| **签名 + UID(本节的题)** | ✅ 拒 | ✅ 拒 | **是** | 性质最全,代价是产线 |
| 签名 + 设备侧每板密钥 | ✅ 拒 | ✅ 拒 | **否** | 见下面 (b) |

**做法 (a):UID 进被签名的载荷**。容器里放一个带 UID 的字段(或 `UID` 的哈希),验签时再
`FLASH_GetUniqueID()` 比一次(设备读自己是现成的,§7.1 E9)。简单直接,但**私钥(或签名服务)
必须进 provisioning 环节** —— 这正是厂商用"UID 派生密钥流"想避开的东西:他们的产线只发一份
镜像,由**烧录器**按芯片做变换。我们走 (a) 就等于接受"每颗一份签名"(私钥不出签名主机这一条
仍然能守,§1,但要有一个在线的签名服务)。

**做法 (b):签一次 + 设备侧用"每板密钥"重新绑定**。镜像只签一次(产线复杂度不变),设备在
**写槽时**用自己的密钥对镜像做一次封装/MAC;克隆到别的板子因密钥不同而失败。前提是那把密钥
**不能由 UID 算出来**(否则退回厂商那套的弱点:任何拿到算法+UID 的人都能为目标芯片重封),
所以它必须 provisioning 写入、并且放在 RDP/写保护后面。

> **2026-09-23,这条前提不再是假设(§7.12)**:厂商那套就是"由 UID 算出来"的活样本,而且已经
> 完整读出并落成工具(`../tools/agm_logic_crypt.py`)—— 唯一的每芯片输入是**片内 flash 的 128-bit
> 唯一 ID**(16 B,四个字都在,经 3 个 XOR 组合进状态),套在**两轮** Salsa20 变体上。所以做法 (b) 的
> 密钥**必须**是我们自己 provisioning 写入的随机量(由 RDP/写保护兜住),把它绑到 UID 上等于
> 重做一个厂商那套。

**三种组合都改不了的约束**:

* **factory 那份永远签不了**:ROM 只认 raw 与 `[算法][压缩]`,不会验签 —— UID 绑定也一样只能
  覆盖"设备侧会去流的槽"(§9.1 第 3 条);
* **机密性只能靠 RDP**,而"比特流能被读出去"这扇窗**已经关上**(2026-09-19,
  peripherals §3.26.35):AN3155 的读命令现在只回答**本次会话写过**的比特流窗口范围
  (厂商工具写完回读校验照常),越过水位就 NACK,`CONFIG_BOOT_AGM_AN3155_READ_FABRIC=y`
  是显式的开发板开关;剩下的读暴露是我们**自己的**固件窗口(store A 与 loader 同居一片),
  那半要靠 RDP/写保护;
* **回滚**要计数器;能重刷 loader 的攻击者要写保护/RDP 才挡得住。

**建议顺序**:先把"防替换"补齐(比特流签名 + payload CRC 预检)+ 关掉 AN3155 比特流读窗口;
在这之后再决定要不要"每芯片绑定",以及选 (a)(产线每颗一份签名,简单)还是 (b)(设备侧密钥,
产线不变但要做密钥生命周期)。

### 9.3 比特流验签必须跑在 PRE_KERNEL_1 —— 这条把算法选择锁成了 ECDSA(2026-09-19 实测)

§9 上半说"启动路径在 `fcb.c` 真正流之前验一次"。写实现时才发现这条约束的强度:
`agrv2k_fcb_program()` 跑在 **PRE_KERNEL_1**(`soc.c` 的 SYS_INIT),那时**一个驱动都没起来,
也没有堆**。

堆这一点是硬的:Zephyr 的 `malloc_prepare()` 是 **POST_KERNEL** 的 SYS_INIT
(`lib/libc/common/source/stdlib/malloc.c`),在那之前 `z_malloc_heap` 还是零值。实测三轮:

1. 在 PRE_KERNEL_1 调 `psa_crypto_init()` + `psa_hash_compute()` + `psa_import_key()` +
   `psa_verify_hash()` → 板上**一个字都没有**(串口静默);
2. 换成只调 `malloc(16)` 的探针 → **同样静默**;SWD 里 `mcause=5`(load access fault)、
   `mtval=0x8`(NULL+8 的引用);
3. 把探针删掉、其余不动 → 同一份构建立刻正常启动。

而 PSA 的 RSA 验签内部走 `mbedtls_calloc`(`tf-psa-crypto/drivers/builtin/src/rsa.c`:
PSS 的 `sig_try`/`encoded` 两块缓冲),`psa_import_key()` 还要为大数分配 ——
所以 **RSA 档在启动路径上不可用**,除非给 mbedtls 单独架一套 pre-kernel 可用的分配器
(`MBEDTLS_MEMORY_BUFFER_ALLOC_C` + 自己在 EARLY 初始化),而那是"改另一个模块的初始化语义",
换来的只是 ~110 ms 的启动时间。

tinycrypt 的 ECDSA P-256 是纯计算(§2.1 已量:验签 + SHA-256 共 +5.7 KB、22.07 M cycles),
没有任何分配,所以比特流档固定用它 —— **与应用档解耦**:RSA 应用照旧走 PSA(运行时,堆在),
比特流走 tinycrypt。顺带一条:`PRE_KERNEL_1` 到切主线程之前跑在**中断栈**上,所以驱动里/
`fcb.c` 里有 `BUILD_ASSERT(CONFIG_ISR_STACK_SIZE >= 3072)` —— 栈不够要在编译期炸,
不能变成一块开不了机的板。

## 10. 防回滚(anti-rollback,R1)— 已实现(2026-09-20)

> 实现与真机实录在 `开发记录（未随本仓库发布）`;
> 这里是设计与取舍。开关:`CONFIG_BOOT_AGM_ANTI_ROLLBACK`(签名档默认开)。

**为什么它是签名之后的下一个洞**:签名保证"只有持私钥者能产出被接受的镜像",但一份**旧的真
签名镜像**照样是真签名镜像 —— 谁留着旧 release,谁就能把已知有洞的版装回去。

**四个取舍,都写成了实现里的注释**:

1. **计数器放哪**。三种候选:①RTC 备份域 —— 板上没有电池,掉电即失,它不是防回滚计数器;
   ②option bytes —— CPU **读不到**(只有 AP/调试通路能碰,§3.26.31 的老问题),设备侧没法比较;
   ③flash(记录里)。选了 ③,并把"原始 flash 写能连记录一起清掉"这件事明确写进限度里 ——
   真正挡住那一步的是 RDP/写保护(R2)。
2. **版本从哪来**。用容器自带的 `ih_ver`(imgtool `--version` 已经写它),不引入新字段;
   build 号不参与排序(同 release 重建不该显得更新)。`SEC_CNT` TLV(MCUboot 的标准反回滚
   载体)留作将来精化 —— 现在一套版本序就够,两套机制反而容易不一致。
3. **什么时候拦**:**只在安装时**。启动时拦会把 A/B 回退这条安全网一起拦掉(回退到另一侧本来
   就是 A/B 的目的),而能改记录的对手也能抹 floor —— 所以在启动路径加检查只损失安全性。
4. **erase 怎么办**。控制台/SMP 的 `erase` 走的是"改记录 + 重写",天然保住 floor(实测:
   `store A : empty` 但 `floor : v2.0.0`,再装旧版仍然 NAK 5)。整片擦(SWD/`erase-all`)
   才能清掉它 —— 与第 1 条的限度同源。

**记录格式 v4**:`boot_cfg` 加一个 `sec_ver`(整板一个)。升级到带 R1 的固件那一刻不追溯
历史,否则一块正跑着旧 release 的板会被自己的首次上传拒掉。2026-09-20 起**不再有 v3/v2
记录的导入**(没有发布过那些格式;评审 RF-013):旧格式记录按 invalid 处理,第一次上传重写
一条新记录,floor 因此也是从零开始 —— 与当年"导入时 floor = 0"的结果一致。

## 11. 生产锁与访问控制(R2)

> 本节是 **2026-09-24 从 `R2-PRODUCTION-LOCK-PLAN.md` 并进来的**(那份文件已删除):
> 它与前面的签名计划是同一条链 —— 签名回答"谁能装",R2 回答"谁能改状态",生产档
> (`CONFIG_BOOT_AGM_PRODUCTION_PROFILE`)正是把两者一起拉齐的预设。
> 内部编号整体平移:旧 §N → 本节 §11.N(旧 §7.1 → §11.7.1,依此类推);里面对
> §0.1 / §10.1 的引用指 BOOT-DFU-STATUS 的对应小节;§3.26.x / §7.x 那类编号只在维护者
> 本地开发记录里有对应章节(未随本仓库发布)。
> 状态:R2a/R2b 已落地并开发板验收,**R2c(RDP/写保护)未做**。

### 11.1 事实前提(先钉住,免得设计跑偏)

| 事实 | 出处 / 证据 |
|---|---|
| **CPU 读不到真实 option bytes** —— 它们只有 AP/flash-controller 通路能碰 | `fcb.c` 的注释、[FLASH-AND-CAPTURE.md](FLASH-AND-CAPTURE.md) §10.1(`tools/agm_oo.sh info` 走 SDK 的 AP 通路才能读 RDP/WRPR/FPGA 指针) |
| 所以我们能做的"回真实状态"= **回"未知"**,而不是回一个真值 | 同上;`0x00` 当作 unknown,工具会停在写入之前(这正是想要的效果) |
| option byte **写**也只有 AP 通路能做 | §10.1;现成通路:`../tools/agm_oo.sh`(`oo -o` / `raw`)、`../tools/rom_opt.py`(ROM bootloader 侧的 option 区读写,已开发板验证) |
| `lock_flash`(读保护)+ 关掉它会**全片擦除** | `开发记录（未随本仓库发布）`;`agm_oo.sh unlock` = `-L` |
| 我们自己要的 DFU **必须**在锁之后仍然可用 | 这是与厂商 `board_logic.encrypt` 的分水岭(它和设备侧更新互斥,`fabric-compression.md` §7) |

### 11.2 决定点(四个都已拍板,2026-09-20)

这一节原来是"拍板前的问题清单",2026-09-24 整理时压成结论;展开的取舍在 §11.6。

1. **生产档保留哪条上传路径** → AN3155 整条编掉(`CONFIG_BOOT_AGM_AN3155=n`,顺带省 3.6 KB),
   **SMP 留作唯一带内通路**(它是唯一能签发/接收授权命令的服务端,§11.6.4/§11.7)。
2. **认证强度** → 不用 HMAC token,用**非对称签名**:`publish`/`erase`/开上传会话各要一条
   ECDSA P-256 签名命令(§11.6.2 把 mcumgr 现成认证实测排除了;§11.7 是落地的契约)。
3. **RDP 什么时候上** → **还没上(R2c)**。烧完 RDP 后唯一的恢复是 BOOT0 + UART ROM bootloader
   (全片擦),所以这条必须先在可牺牲的板上演练,并把步骤写进 [FLASH-AND-CAPTURE.md](FLASH-AND-CAPTURE.md)。
4. **`once`(RTC 一次性 override)在锁之后** → `CONFIG_BOOT_AGM_LOCK_PRODUCTION` 直接拒
   (`-EPERM`);`erase` 保留(它保留下限,不算上传口)。

### 11.3 与现有机制的关系

* **R1(防回滚)**:下限在记录里,能挡住走 DFU 路径的降级;挡住"原始 flash 写"要靠 RDP/写保护
  —— 也就是本文的 ②。
* **R3(AN3155 比特流读窗口)**:已经关掉(按会话水位只放行刚写的那段)。**loader 自己那片**
  (store A 窗口与 loader 同居一片)仍然可读 —— 与"SWD 在 RDP 关闭时可读"同一件事,随 RDP 一起关。
* **R5(每芯片绑定)**:做法 (b)"设备侧每板密钥"**依赖**本文的 RDP/写保护;做法 (a)"UID 进被签
  载体"需要在线签名服务。两者都在 R2 之后才有意义。

### 11.4 分批(状态)

* **R2a ✅**(2026-09-20,`eff24de` + 评审 RF-003):AN3155 不再假报"未保护"
  (`CONFIG_BOOT_AGM_AN3155_CLAIM_UNPROTECTED`),`LOCK_PRODUCTION` 拒掉 `once` 这类逃生口。
* **R2b ✅**(2026-09-20,`5252a91`+`0fcf4ab`+`b3a663d`):状态变更要一条签名命令 ——
  契约见,端到端验收见 `开发记录（未随本仓库发布）`,
  今天还有 `../tools/verify_flow.py` 的第 2–5 步在做同一件事。
* **R2c ⬜ 未做**:产线烧 RDP/写保护 + 恢复流程演练 + 文档(`agm_oo.sh info` 应显示
  read protection 已开;解锁全擦后可恢复)。依赖 §11.2 第 3 条与一块可牺牲的板。

### 11.5 必须先澄清的坑

* **"回真实 RDP"做不到**:CPU 读不到 option bytes(§11.1),所以 R2a-① 的目标是"不再撒谎",
  而不是"报真值"。谁需要真值,用 `agm_oo.sh info`(AP 通路)或产线工具看。
* **默认档不能改行为**:`agrv32flash -w` 依赖 `0xA5` 才肯写;R2a-① 默认保持 y,只有生产档
  主动关掉 —— 否则今天的 DFU 流程会当场断。
* **锁了之后 `erase`/`rollback` 仍然要能用**(它们是 A/B 策略的一部分,不是"上传口");
  别把"生产锁"做成"只要锁了就不能升级"。
* **RDP 是不可逆的体验变化**:一旦开了,SWD 调试、`probe_state.sh`、`agm_oo.sh fw` 都会换一种
  工作方式(或需要 unlock = 全擦)。开发板纪律里要把"哪些板是锁的"写清楚。

### 11.6 R2b 的路径取舍(2026-09-20 分析 + 选项 5 实测)

#### 11.6.1 三条路径不一样,不能一刀切

| 路径 | 能否加认证 | 生产上是否必需 | 结论 |
|---|---|---|---|
| **AN3155**(厂商 `agrv32flash`) | **不能**:帧格式里没有认证字段,我们改不了那个二进制 | 不必须:SMP 已覆盖上传;现场恢复另有 **ROM bootloader**(BOOT0 高 + 上电),它不依赖我们 loader 的服务端 | **生产档砍掉**(`CONFIG_BOOT_AGM_AN3155=n`,顺带 ~−3.6 KB) |
| **console** | 能(自己的帧) | 诊断/状态有用,**上传与 SMP 重复** | **保留 console,砍上传相**(`upload` + 二进制相),或生产档要求 R2b 签名 |
| **SMP**(mcumgr) | 串口传输没有现成钩子,要自己加 | 是(上传 + 状态) | **保留,作为 R2b 认证落点**;状态变更命令走签名 |

#### 11.6.2 选项 5(mcumgr 现成认证)已实测排除

在钉住的 Zephyr 里枚举 transport 并搜索整个 `subsys/mgmt/mcumgr/`:

```
transports: bluetooth dummy lorawan raw_dummy raw_uart shell spi uart udp
grep -riE "pairing|authenticat|challenge|handshake" → 只有两处:
  MCUMGR_TRANSPORT_BT_PERM_RW_ENCRYPT / _AUTHEN   (蓝牙 GATT 权限位)
  MCUMGR_TRANSPORT_UDP_DTLS                       (UDP + DTLS,要网络栈 + mbedTLS)
```

也就是说 **serial / uart / raw_uart / shell / spi 上没有任何认证设施**(我们要用的正是 serial
帧),BT 那条板上用不上、UDP+DTLS 的体积对 96 KiB 的 loader 直接出局。结论:没有"白拿一套",
R2b 只能自己做 —— 认证层落在 SMP 的命令语义上(见 6.4)。

#### 11.6.3 为什么砍 AN3155 不心疼

1. **它救不了砖**:现场恢复的正确姿势本来就是 BOOT0 跳线 + ROM bootloader;那套 AN3155 是芯片
   自带的,与我们 loader 里编的服务端无关。砍掉服务端是**把物理门槛从"插一根 UART"抬到"拆机 +
   跳线"**,不是取消恢复能力 —— 这正是生产锁假设的访问级别。
2. **它冗余**:上传走 SMP 一套就够;三条路径并存 = 三处要认证、三处要回归。
3. **它省 ROM**:`AN3155=n` ≈ −3.6 KB(§3.26.33 的瘦身档 `AN3155=n`+`SMP=n` 实测 −11.2 KB)。
4. **它是最脏的一条**:R2a 刚修掉"假绿灯";而那个假绿灯恰恰是厂商工具**要求**的(它读不到真
   option bytes 就不肯写),留着它就等于留一个无法在其上做认证、只能靠策略窗口兜的入口。

#### 11.6.4 生产档规则(拍板:2026-09-20)

* `AN3155=n`;
* **console 只读**:help/info/status/reboot + A/B 策略里的 `confirm`/`rollback`(它们是策略,不是
  上传口;`erase` 归入"需要认证的状态变更");
* **上传只走 SMP**,且**上传完成 + 所有状态变更命令需要签名**:host 用签镜像的那把私钥对
  `nonce ‖ 命令 ‖ 参数` 签名,设备用**已编译进去的公钥**(tinycrypt,已链接)验证 —— 设备端无秘密,
  因此**不依赖 RDP 保密性**;一次验签 ≈110 ms,命令级无感;
* **默认(开发板)档三条全留**,工具与回归流程不变;
* 若现场要求"没有私钥也能恢复",只能加**服务窗口**(跳线在位才接受未签名命令,或一次性
  provisioning 标志)—— 这不是漏洞,但必须在文档里写成明确的物理边界。

> **2026-09-20 落地(F)**:上面这套形状现在有一个 Kconfig 名字 ——
> `CONFIG_BOOT_AGM_PRODUCTION_PROFILE`。它 `select` 掉那些"必须一起成立"的东西:
> `BOOT_AGM_LOCK_PRODUCTION`(门控)、`BOOT_AGM_SMP`(**唯一**能签发命令的路径)、
> `BOOT_AGM_BITSTREAM_SIGNED`(比特流内容;RSA 档也靠它拿到那把 P-256 授权密钥)、以及
> mcumgr/zcbor/net_buf/base64/crc 这条链;`AN3155` 默认关、签名档默认 ECDSA P-256。
> 这样两条最贵的错误被**结构上排除**:`SMP=n`(没人能授权)与"RSA 档没有 P-256 密钥"。
> 剩下的"没给公钥"由 sample 的 CMake 兜:`CONFIG_BOOT_AGM_PRODUCTION_PROFILE=y` 而
> `-DSPI_BOOT_PUBKEY=` 缺失时**直接 FATAL_ERROR**(一块锁了却没密钥的板子只能物理恢复,
> 那是构建错误,不是上了板才发现的意外)。它**不管**两件事:console 的命令集(上面"console
> 只读"仍由应用决定)、以及 RDP/写保护(R2c 的烧写步骤);"故意不给自己留升级口"仍然可以
> 直接设 `LOCK_PRODUCTION` 但不给密钥 —— 那时启动日志会说明这块板子不可升级。
> CI 形状:`sample.spi_boot_loader.production`(用 `samples/spi_boot_loader/fixtures/` 里那把
> **公开**的 CI 密钥),以及保留的 `locked`(bare LOCK、无密钥)作为 fail-closed 回归。

### 11.7 R2b 接口契约(实现从这一节开始)

> **2026-09-24 整理**:这一节就是**已实现**的契约(设备侧 + SMP group + host 工具 + native
> 用例,提交 `5252a91`/`0fcf4ab`/`b3a663d`),不再有"落点清单"那种实施顺序表 —— 每一步的
> 位置在下面各条里就地写明。开发板与 native 的验收记录在
> `开发记录（未随本仓库发布）`。

最小闭环:**只读命令不签,状态变更命令要签**。先把 `erase` 和"上传完成后的 publish"这两条
装上,`once` 已由 R2a 的 `LOCK_PRODUCTION` 直接拒掉。

> **2026-09-20:已按本节实现**(设备侧 + SMP group + host 工具 + native 用例;开发板验收见
> `开发记录（未随本仓库发布）`)。下面几处与最初的写法有出入,以实测/实现为准,
> 已就地改掉:①console 相的 NAK 码从 4 改成**新的 6**;②"置 `session_authorized`"收紧成
> **一次授权一个命令、用过即失效**;③明确了 RSA 档用哪把 P-256 公钥。

#### 11.7.1 受控集合(锁定档)

| 命令 | 锁定档行为 |
|---|---|
| info / status / `image state` 只读 / `reboot` | **不签**,照常(只读) |
| `confirm` / `rollback` / `mode` / `install-slot` | **不签**:A/B 策略与"摆位",只在**已发布**(已授权)的镜像之间做选择,改不了内容 |
| 开一次上传会话(`upload_begin`)/ publish(上传完成)/ `erase` | **要签**,否则回错误码、不执行 |

> **2026-09-20 补(评审 RF-004)**:最初只把 publish 与 `erase` 装上门,但"未授权的上传"仍然
> 能把 store 的字节写掉 —— 第一次写就会从零擦目标扇区,而对 store 来说那就是 record 还指着的
> 那份**回退镜像**:`erase` 有门控、它的孪生动作却没有,等于绕开。现在 `upload_begin()` 也
> 要 **ERASE** 授权(拒绝时会话不打开,后续 write 直接失败,flash 一字未动),代价是 host 侧
> 一次上传变成两次授权:先 erase 开会话、传完再 publish。`tools/smp_cli.py upload
> --authorize <key.pem>` 把这两步包在一次调用里(它自己会在这中间取新 nonce)。
>
> **2026-09-20 补(评审 RF-011)**:上面第二行是这一节的**完整**集合 —— 原来的接口清单里列的
> `mode` 与 `install*` 最后没有进门控,这不是漏掉而是收窄:它们和 `confirm`/`rollback` 同属
> "在已发布镜像之间选择/摆位",不引入任何未经授权的内容(内容由镜像签名 + 这里的
> publish 门控保证)。反过来,真要按这个粒度分权限,做法是给它们各自的 gated 命令,而不是
> 把"上传完成"这扇门再拧紧。产线档如果连这条余地都不想留,`§11.6.4` 的"console 只读"是
> 更合适的落点(把 `mode`/`install-slot` 从命令表里去掉)。

#### 11.7.2 线格式(mcumgr 自定义 group `0x40`)

```
cmd 0  NONCE   : 设备回 nonce[16](来源:记录 seq + k_cycle_get_32 + 每次会话计数器;
                 只要求"每次会话唯一",不要求密码学强度 —— 签名绑定的是命令本身)
cmd 1  AUTHORIZE: payload = nonce[16] ‖ cmd_code[1] ‖ args_len[2](LE) ‖ args
                  sig     = ECDSA P-256 over SHA-256(payload),DER 或裸 r‖s
                  设备:验签(复用 ecdsa_der_to_raw() + uECC_verify() 那条 tinycrypt 路径)
                  → 消费 nonce、记下"这个命令被授权了"→ 回 {"rc": 0}
```

线格式的细节(CBOR 字段而不是一整块 blob):请求是
`{"nonce": bstr16, "cmd": uint, "args": bstr?, "sig": bstr}`;设备把前三个字段**重组成**
上面那条 `payload` 再哈希,所以签名覆盖的是那个字节串、不是 CBOR 编码本身。

* **防重放**:nonce 一次性(用掉即失效)+ 每次 AUTH 重新取 nonce;不接受"只签 nonce"或
  "签了命令不签参数"的形态。
* **一次授权一个命令**:成功验签后设备记的是"`cmd` 这个命令被授权过一次",不是"这条会话可信",
  而且**被授权的那个调用会把它用掉**。授权了 `erase` 不等于授权了 publish;同一个 `erase`
  想再来一次要重新取 nonce 再签。
* **参数签了但不做绑定**:`args` 在签名覆盖范围内(操作员的意图可审计),但设备只按"这个命令
  一次"放行 —— 一次被授权的 `erase` 可以擦主机接下来指定的任意目标,一次被授权的 publish
  可以发布任意长度。要绑到具体参数,得让每个调用方都按 host 的序列化方式交出参数,这是
  §11.7.4 里明确不做的那一类;真要做,先改契约再改实现。
* **拒绝语义**:未授权 → mcumgr 侧回 `MGMT_ERR_EACCESSDENIED`(与 §3.26.33 的"被拒镜像"保持
  同一套观感);console 相回 **NAK code 6**(`UP_ERR_UNAUTHORIZED`)—— 初稿写的是 4,但 4 是
  "镜像被拒",让操作员去看镜像,和"你没授权"是两回事,所以按 §3.26.33 给旧 `-EACCES` 分家的
  同一个理由再分一个码。两条路径都**不落记录、不动 flash**。
* **密钥**:默认复用应用签名公钥(`agm_boot_pubkey`)。**RSA 档例外**:它的应用公钥是 270 B 的
  PKCS#1 DER,不是曲线上的点,那条 build 里唯一现成的 P-256 公钥是比特流公钥
  (`agm_boot_bitstream_pubkey`,前提是 `BOOT_AGM_BITSTREAM_SIGNED`);两者都没有的 build
  (RSA + 无签名比特流)拿不到任何 P-256 公钥,`agm_boot_authorize()` 回 `-ENOTSUP`,于是所有
  状态变更**全部被拒**(fail closed)。`CONFIG_BOOT_AGM_LOCK_PRODUCTION` 因此会自己把
  tinycrypt 的 P-256 验签链进来:锁了却验不了签的 build 是一块再也升级不了的板子。
  **密钥与目标无关**(2026-09-20 评审 RF-006 定案):grant 不写目标、不写长度,所以只有一把
  密钥能覆盖整个受控命令集 —— 即使应用与 fabric 配了**两把**不同的 P-256 密钥,命令授权也
  一律走应用密钥(比特流**内容**的验签仍然用 fabric 密钥,两件事互不影响)。配了双密钥的
  操作员如果想让 fabric 密钥同时当管理密钥,那是 §11.7.4 里"另起管理密钥"那一步的事。
* **授权是设备级的**:grant 存在驱动里,不绑定协议服务器 —— SMP 侧签发的授权,AN3155 会话的
  GO、或者 console 相的 `upload` 一样能用(反过来说,谁先用掉就是谁的)。

#### 11.7.4 明确不做(在这一层不解决)

* 不引入设备侧秘密(HMAC token):那要等 RDP(R2c)才成立;选项 4 的非对称路子天然不需要它。
* 不做"每帧签名":上传内容由镜像签名 + R1 下限保证,会话层只签**命令**。
* 不做"上传内容先落暂存区":锁定档里一次上传要两次授权(开会话 + publish)是刻意换来的
  简单性 —— 代价是 host 侧多一次 nonce/签名往返,收益是未授权主机连"擦掉回退镜像"这条
  可用性路子也一起关掉(§11.7.1 的补注)。
* 不做"按目标区分授权密钥":那需要 grant 绑目标(见 §11.7.2 最后一条),而这一层刻意不绑 ——
  真要按目标分权限,应该做成两条 gated 命令(例如 future `AGM_BOOT_AUTH_CMD_*`),而不是让
  同一把命令的签名在不同目标下换密钥。
* 不动开发板默认档:三条路径 + 无签名的状态变更照旧,回归流程不变。

## 12. R2c(上 RDP)与每芯片绑定 — 施工记录(2026-09-24 起)

> 这一节是 **§11.4 那条 "R2c 未做" 的施工清单**,以及它依赖的"每芯片绑定(加盐)"的设计与
> 进度。两者是一体的:没有 RDP,盐就是可读的;没有盐,UID 就是可算的(§7.11/§7.12)。
>
> **2026-09-24 状态**:绑定这一半**已实现并真机验证**(12.2–12.4:盐区、provisioning、
> 容器 `BIND` 标签、publish 与启动两处强制、native 负例、`verify_flow` 断言);
> **RDP(R2c)的 SWD 侧也已实现并演练**(12.5:加锁不擦、外部读全零、解锁全片擦、恢复后
> 重烧重绑)—— 只剩"ROM loader 侧那条兜底通路"没在板上走过。两半合起来,R2c 的
> **物理边界**已经在手边,但它不是默认开着的:哪块板要锁,是产线的显式动作。

### 12.1 为什么是 `KDF(UID, salt)` 而不是 `KDF(UID)`

UID 不是秘密 —— 把两把**公开常量**写进 flash 控制器的 `KEYR` 就能读它(§7.11),而且设备自己
也要读它(下一条)。所以 `KDF(UID)` 只挡"顺手复制",不挡"有芯片 + 有算法"的人。**盐**是产线
写入、由 RDP 保护的**秘密**那一半;`KDF(UID, salt)` 才是"即使读到了 flash 也算不出"的那一步。
两者缺一不可,这也是"防复制"的全部内容:

| 组合 | 挡住"原样复制 flash" | 挡住"有芯片 + 会读" |
|---|---|---|
| 只有签名 | ❌(签名可移植,§9.1) | ❌ |
| 只有 RDP | ⚠️(前提是 RDP 打不穿) | ⚠️ |
| RDP + `KDF(UID, salt)` | ✅ | ⚠️(仍挡不住"能在芯片上跑自己代码"的对手 —— 那要靠 RDP 的"解锁=全擦") |

### 12.2 KDF 与绑定标签(✅ 已实现)

两侧实现都在:`../tools/agm_bind.py`(主机)与 `drivers/misc/boot_agm_bind.c`(设备),
已知答案向量把两边钉在同一串字节上(`../tools/tests/test_agm_bind.py`、
`tests/drivers/misc/boot_agm_bind`;后者用的是**同一份夹具容器**,不是同一段配方的第二份抄写)。

* `key = HKDF-SHA256(ikm = UID[16], salt = salt[16], info = "agm-bind-v1", L = 32)`
  —— RFC 5869,只有 hashlib/hmac(设备侧是 tinycrypt 的 SHA-256);单测里有 RFC 5869 A.1 的向量。
* `tag = HMAC-SHA256(key, ih_ver ‖ SHA-256(header ‖ image))`,32 B。
  **版本取自容器头自己的 8 字节 `ih_ver`,不是操作员敲的字符串**:`--version 2.0` 被 imgtool
  补成 `2.0.0` 时,字符串会做出一个设备必然拒收的镜像,而这个理由在两边输出里都看不见
  (第一版就是字符串形式,改成头字段后这类错误在构造上不可能发生)。头字段一变标签就变,
  所以"改个版本重新签"不会顺带把绑定带过去。
* **标签的落点:容器里的 `BIND` TLV(type `0x00a0`)**,由 `agm_bind.py embed` 在签名之后追加。
  这一步之所以成立,是因为 MCUboot 的签名覆盖 `SHA-256(header ‖ image)` 而**不覆盖 TLV 区**:
  追加一个 TLV 不动 SHA256/KEYHASH/签名三个字段,标签本身又通过它吃的那个 digest 绑在签名
  覆盖的字节上。`0x00a0` 取的是 MCUboot 明文留给厂商的段(`IMAGE_TLV_*` 的注释:
  "vendor reserved TLVs at xxA0-xxFF");显而易见的 `0x30` 已经被 `IMAGE_TLV_ENC_RSA2048` 占了。
* 命令行:`gen-salt`、`key`、`tag`、`embed`(产出绑定版容器)、`check`(复核)、
  `salt-sector`(离线产扇区镜像)、`provision`(见 12.3)、`uid-from-words`。

### 12.3 盐放哪(布局 + provisioning + 实测)

> **2026-09-24 位置已改**:那 16 KiB"两槽之间的间隙"是手挑地址撞出来的、不是设计;
> 现在比特流区从 factory 锚点往下紧排(无间隙),salt 成了**应用链的最后一节**、与 boot
> record 连续成 12 KiB 元数据块(`0x800ae000..0x800b1000`,salt 在 `0x800b0000`),
> 位置由 `loader-size`/`app-size`/`record-size` 推导,不再有 Kconfig 地址。下面这段
> 描述的是改前的形态,保留作记录;权威地图见 [`FLASH-LAYOUT.md`](FLASH-LAYOUT.md)。

* 位置:默认布局里两个比特流槽之间空着 16 KiB(`0x800c9000..0x800cd000`),取**一个 4 KiB 扇区**
  做 `bind-salt`。已加进 §3.26.30 的布局表与 `drivers/misc/boot_agm.c` 的 `BUILD_ASSERT`
  (必须落在两槽之间、扇区对齐)。位置由 `CONFIG_BOOT_AGM_BIND_SALT_OFFSET` 决定(默认
  `0x800c9000`),扇区格式是 `"AGMB" + version + salt[16] + CRC-32`,其余 `0xff`。
* 为什么在片内 flash:它必须**比固件活得久**。实测(2026-09-24,`agrv2k_407`):写好的盐在
  `west flash --skip-bitstream` 与"固件 + canonical 比特流"的完整 `west flash` 之后都原样还在
  (厂商 flash 驱动的 auto-erase 只吃它写的那段扇区),而 loader 的**带内**更新更是只动
  自己那几个扇区。
* 写入:`tools/agm_bind.py provision`(设备在环:SWD 读回扇区 → 写 → 再读回逐字节校验)。
  二次写入默认拒绝:扇区里已经有一份**不同的**盐就报错退出,要覆盖得显式 `--force`
  (覆盖会让此前对这颗芯片签出的所有镜像作废)。同一份盐再写一次是 no-op。
  首次烧录的完整顺序是"固件 + 比特流 → 写盐 → 对这台签绑定版固件 → 上 RDP"。
* **指纹**:设备把 `SHA-256(key)` 的前 4 B 打在 loader 的 `info` 里
  (`bind     : salt v1 present, key fp bcfe1706`),provisioning 工具打印同一串 ——
  这是"板子读到的盐就是刚写进去的那份"的可核对证据,而密钥本身从不出芯片
  (128-bit 秘密的哈希不泄露密钥)。真机实录见 §3.26.39。
* 保护:盐的机密性**完全等价于 RDP 的强度** —— 这是为什么 §12.5 的演练是这条链的前置条件。

### 12.4 设备侧:UID 读取与两处强制(✅ 2026-09-24)

`agm_boot_unique_id()`(`drivers/misc/boot_agm.c`,公开 API 在 `boot_agm.h`)走 flash 控制器的
flex-read(命令 `0x4B`,四次读,偏移 4/8/12/16,4 个 dummy 字节 —— 与 SDK `FLASH_GetUniqueID()`
同一序列),读前用两把公开常量解锁 `KEYR`、读完锁回;等待用有上限的自旋(它在 PRE_KERNEL_1
就可能被调用,那里没有内核定时器),超时返回 `-ETIMEDOUT` 而不是把启动挂死。

实测(2026-09-24,`agrv2k_407`,canonical 200 MHz 比特流,loader 控制台 `info`):

```
uid      : 41503436 33343112 00d6b836 56060178
```

与 §7.12 经 SWD flex-read 记录的**同一颗芯片**的 16 B(`41 50 34 36 33 34 31 12 00 d6 b8 36
56 06 01 78`)逐字节一致 —— 也就是说主机侧 KDF 的 UID 输入可以直接取这条日志
(`agm_bind.py uid-from-words 0x36345041 0x12313433 0x36b8d600 0x78010656`)。

**校验只有一处,两条路径共用**:`img_check_tlvs()`(`boot_agm_verify.c`)在走 TLV 时,
遇到 `BIND` 就用**已经为签名算过的那个 digest** 重算标签并比对
(`boot_agm_bind_tag_digest()`,所以容器不会被哈希第二遍)。`agm_boot_image_verify()` 是
publish 与启动路径都调的那个函数,所以"发布时拒、启动时再拒一次"是同一份代码,不会走偏。
三种拒绝都回 `AGM_BOOT_E_UNBOUND`(-1002):

| 情形 | 结果 |
|---|---|
| 容器里没有 `BIND` TLV | 拒(签名档 + 绑定档不接受未绑定镜像) |
| 标签与本芯片/本版本算出来的不一致 | 拒(另一颗芯片的副本、或换了版本没重绑) |
| 本芯片没有盐(`-ENOENT`) | 拒(fail closed:绝不退化成"零密钥等于通过") |

错误码是独立的(`-1002`,console 相 **NAK 7**),理由与 §11.4 的 `AGM_BOOT_E_OLD_VERSION`
分家一样:"这份镜像不是给这颗芯片的"与"这份镜像不是我们签的"是两条不同的产线动作。

**native 覆盖**(`tests/drivers/misc/boot_agm_bind`,8 例全绿):正向、无标签、另一颗芯片、
换版本、无盐、publish 拒绝后记录不动,加上"设备读到的扇区就是 host 工具产出的字节"与
`key`/`tag`/指纹三组已知答案。

**真机覆盖**:`../tools/verify_flow.py` 的第 2、3 步(2026-09-24,§3.26.39)。

**这条链要的一份额外预算(实测)**:绑定检查跑在调用者自己的栈上,而上传路径的栈是
mcumgr 传输的 work queue —— 在它之前已经压着 CBOR/分帧、容器哈希与 ECDSA 验签。
Zephyr 的默认 2048 B **不够**:第一次真机上传时,拒绝信息正确打印,随后溢出把保存的
返回地址冲掉,CPU 在 `mepc` 指向 RAM(栈底之下)时以非法指令停机
(`mcause 2`,"ZEPHYR FATAL ERROR 0");`samples/spi_boot_loader` 现在是 4096 B
(`CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE`),`boot_agm_bind.c` 里有一条
`BUILD_ASSERT` 把这个下限变成构建期错误 —— 这类损坏是静默的,让它在编译期响。

### 12.5 R2c:上 RDP 的施工清单(SWD 侧与 ROM 侧都已做并演练)

> **2026-09-24 更新:SWD 侧的加锁/解锁已实现并在板上演练完**(实现记录
> `开发记录（未随本仓库发布）`,操作步骤与现象速查在
> [`FLASH-AND-CAPTURE.md`](FLASH-AND-CAPTURE.md) §11)。下面保留原始清单,逐条标注状态。

* **动作只能在 AP 通路**:设备侧读不到 option bytes(§11.1),所以"加锁"要新增——
  **已加**:`tools/agm_oo.sh lock`(`oo -L` → `agrv lock 0`,另有 `read-protect` 走 `-p`);
  `unlock`(`oo -u`)本来就有。另一条通路是 ROM loader(`agrv32flash -j/-k/-L`,需 BOOT0),
  以及只写 option 镜像的 `../tools/rom_opt.py`(注意它写的镜像第一半字固定是 `0x5AA5` =
  **unprotected**,所以它不能用来加锁)。
* **顺序**:firmware + 比特流 + 盐都烧完、验完 → 才加锁;锁上之后设备侧更新 = 解锁(全片擦)重烧。
  **实测补充**:加锁这一步**不擦**片内镜像(控制台照跑),擦的是**解锁**那一步。
* **必须演练的四条**(在一块可牺牲的板上,步骤写进 [FLASH-AND-CAPTURE.md](FLASH-AND-CAPTURE.md)):
  ①加锁后外部读必须失败 —— ✅ 实测:SWD 读整片全 `0x00` + `Failed to read memory via system bus`,
  盐扇区读出为空;(设备自己仍能读 UID —— 锁挡的是外部读,这条在文档里写清楚了)
  ②`agm_oo.sh info` 显示 `read protection: on`,ORB `0x3fffffd → 0x3ffffff` —— ✅ 实测;
  ③解锁触发全片擦、板子如预期变砖 → 恢复 —— ✅ 实测:解锁后整片 `0xFF`、串口 0 字节,
  恢复走 **SWD**(`west flash` firmware + canonical 比特流),**没有**走到 ROM 路径(见下一条);
  ④恢复后重烧 firmware + 比特流 + 盐,再验一遍启动与绑定 —— ✅ 实测:UID 不变、盐重写后
  指纹仍是 `bcfe1706`,`verify_flow --only 2,3` 两步 PASS。
* **风险与兜底**:写 option 期间出错时,唯一入口是 BOOT0 + ROM bootloader ——
  **2026-09-24 也演练过了**(插上 BOOT0 跳线):`agrv32flash -j` 加锁(`Flash : Protected`,
  接着 SWD 侧同样变成 `read protection: on` + 读全 `0x00`)、`agrv32flash -k` 解锁并整片擦、
  之后用 SWD 重烧 firmware + 比特流 + 盐并逐字节核对。**两条通路的设备状态完全一致**,所以
  "无探针也能上锁/解锁"这条现在有实测背书。两个记下来的坑:`-L` 单独用**不解锁**
  (它只是写操作的修饰,解锁要用 `-k`);ROM 路径加锁后**要拔掉跳线再上电**,否则复位后
  进的是 ROM loader(§10 的"BOOT0 陷阱")。
  **写 option 中途被打断这一格也已演练**(同日,`../tools/agm_rdp_tear_test.sh`):
  连续写窗口里 sigkill 掉会话 → option 停在"已擦未写回"(RDP 读出 on、FPGA 指针无效) →
  **板子起不来**(FCB STAT=0、时钟门控全关),但 **SWD 仍然活着**,
  阶梯 `unlock` → `bitstream <canonical>` → `west flash` → `provision` 全部走通并逐字节核对。
  所以这句话要修正成:**SWD 通常就是兜底,BOOT0 + ROM loader 是 SWD 也进不去时的第二道**;
  实测与步骤见 [`FLASH-AND-CAPTURE.md`](FLASH-AND-CAPTURE.md) §11.5。
  > **2026-09-24 复核(§11.9)**:那次"sigkill 拿到撕裂态"要打折看 —— 同一"已擦未写回"签名
  > 用一条**无复位的 `agrv32flash -O`** 就能造出来,而 host 侧打断在 8 个延迟点上都被证明
  > **是原子的**(§11.8),所以那一刀到底是不是 sigkill 打出来的没有独立证据。要演练恢复,
  > 现在用确定性路径 `--rom-erase-tear`(canonical → `-O` → 撕裂态,判据是转移)。

### 12.6 明确不在这条链里的

* **不是机密性方案**:绑定只回答"这份镜像能不能在这颗芯片上跑";flash 里仍是明文,固件加密是
  另一件事(设备侧要能解密 → 明文必然出现过,见 BOOT-DFU-STATUS §0.1 ⑤)。
* **不替代 RDP**:见 §12.1 的表;也不替代签名(绑定挡不住"换成攻击者自己的镜像",那要靠签名)。
* **不解决外部监测**:ASIL 论证仍要器件外的监测器件(§5.4)。
