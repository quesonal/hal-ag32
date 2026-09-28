# Signed-image fixtures

Generated once and checked in, so the test needs no tooling at build time. The
private keys are disposable and **not** committed: regenerating the fixtures
means generating new keys, and therefore a new `pubkey.bin` too.

```sh
# 1. throw-away test keys (only ever used for these fixtures)
python3 -c 'import sys; sys.argv=["imgtool"]; from imgtool import main as m; \
            m.imgtool.main(["keygen","-k","key1.pem","-t","ecdsa-p256"], standalone_mode=True)'
#    ... and the same for key2.pem

# 2. the payload: 1004 bytes, (i*7+3) & 0xff
python3 -c "open('image.bin','wb').write(bytes(((i*7+3)&0xff) for i in range(1004)))"

# 3. the containers, through the same tool production uses -- so the
#    `ih_load_addr` the loader checks is the one production writes
tools/sign_image.py image.bin key1.pem -o signed_ok.bin --pubkey-out pubkey.bin \
    --slot-size 0x10000 --slot-base 0x80030000
tools/sign_image.py image.bin key2.pem -o signed_other_key.bin --slot-size 0x10000 \
    --slot-base 0x80030000
tools/sign_image.py image.bin key1.pem -o signed_other_slot.bin --slot-size 0x10000 \
    --slot-base 0x80020000
#    the anti-rollback floor cases: the same payload at 2.0.0 and at 0.0.0
#    (both signed by key1, so they share pubkey.bin with signed_ok.bin)
tools/sign_image.py image.bin key1.pem -o signed_v2.bin --version 2.0.0 \
    --slot-size 0x10000 --slot-base 0x80030000
tools/sign_image.py image.bin key1.pem -o signed_v0.bin --version 0.0.0 \
    --slot-size 0x10000 --slot-base 0x80030000
```

```sh
# 4. the per-chip binding fixtures (tests/drivers/misc/boot_agm_bind), same
#    throw-away key1 -- they are the containers above with a BIND TLV appended,
#    which is exactly what production does after signing. The UID and the salt
#    are the ones the native suite and tools/tests/test_agm_bind.py assert on:
#    the dev board chip's UID and salt 00..0f.
tools/agm_bind.py embed --container signed_ok.bin \
    --uid 415034363334311200d6b83656060178 --salt-file salt_00_0f.bin \
    -o signed_bound.bin
tools/agm_bind.py embed --container signed_ok.bin \
    --uid 415034363334311200d6b83656060178 --salt-file salt_ff_f0.bin \
    -o signed_bound_other_chip.bin           # the *same* image, another chip
tools/agm_bind.py salt-sector --salt-file salt_00_0f.bin -o bind_salt_sector.bin
# ... and the "re-signed for a new release but not re-bound" case, which needs
# the *other* container's tag (the CLI command above always computes the tag
# for the container it is given):
python3 - <<'PY'
import sys; sys.path.insert(0, "tools")
import agm_bind as b
uid = bytes.fromhex("415034363334311200d6b83656060178")
key = b.bind_key(uid, bytes(range(16)))
ok, v2 = (open(f, "rb").read() for f in ("signed_ok.bin", "signed_v2.bin"))
open("signed_v2_stale_bind.bin", "wb").write(b.attach_bind(v2, b.bind_tag(key, ok)))
PY
```

`signed_v2_stale_bind.bin` is `signed_v2.bin` carrying **1.0.0's tag** -- the
"re-signed for a new release but not re-bound" mistake, which the tag's version
input is there to catch. `salt_00_0f.bin` / `salt_ff_f0.bin` are the two 16-byte
salts (`bytes(range(16))` and its complement, written with `gen-salt` or by
hand); `bind_salt_sector.bin` is the 4 KiB sector image
`tools/agm_bind.py provision` writes for the first one, so the device-side
reader can be asserted against the host tool's own bytes.

`--slot-size 0x10000` matches the native test's application slot; the default
(512 KiB) matches the board. `--slot-base 0x80030000` is that slot's address --
the tool's default is the *board's* on-die slot (`0x8007e000`), which would
produce a container the native overlay refuses as "linked for another address"
(`sign_image.py` writes what the loader compares, so the fixtures have to be
made for the address the test layout runs them at).

| 文件 | 内容 |
|---|---|
| `image.bin` | 1004 B 的裸镜像(没有 MCUboot 头) |
| `signed_ok.bin` | `key1` 签的容器,`ih_load_addr = 0x80030020`(= slot + 0x20) |
| `signed_other_key.bin` | 同样内容、`key2` 签(用于"换了密钥"的负向用例) |
| `signed_other_slot.bin` | `key1` 签,`ih_load_addr = 0x80020020` —— **签名有效、地址不对**(用于"上传到错的槽"的负向用例) |
| `pubkey.bin` | `key1` 的**原始公钥** X‖Y(64 B),即 loader 编译进去的那把 |
| `bs_signed.bin` | **比特流容器**(`CONFIG_BOOT_AGM_BITSTREAM_SIGNED`):99944 B fabric + MCUboot 头/TLV,戳 `0x800b0020`,由另一把一次性密钥签(100127 B) |
| `bs_pubkey.bin` | 上面那把的 64 B 裸公钥 —— 它是**比特流**的信任锚(`agm_boot_bitstream_pubkey`),故意与应用密钥不同,这样"两条信任链是独立的"这件事才是被测到的 |
| `rsa_signed.bin` | **另一档**的容器(`boot_agm_rsa/fixtures/signed_ok.bin` 的逐字节副本):`SIGNED-IMAGES-PLAN.md` §2.1 矩阵"ECDSA 档拒绝 RSA 镜像"那一格的夹具 |
| `signed_v2.bin` | 同一份镜像、版本 **2.0.0**(`signed_ok.bin` 是 1.0.0):防回滚用例用它把下限抬起来;加这一条时,上面四个应用夹具用新的一次性密钥**重签**过(负载 `image.bin` 字节不变) |
| `signed_v0.bin` | 同一份镜像、版本 **0.0.0**(`imgtool sign -v 0.0.0`):`0.0.0` 是"最旧"的值,防回滚下限必须和别的版本一样拒它 —— 第一版实现里 `cand_ver != 0U` 的短路恰恰把它放过去了(评审 RF-001)。用**同一把** `key1` 签,所以它和 `pubkey.bin` 配对 |
| `signed_bound.bin` | `signed_ok.bin` + **本芯片**的 `BIND` 标签(`tools/agm_bind.py embed`,UID = 开发板那颗、盐 00..0f):`tests/drivers/misc/boot_agm_bind` 的正向夹具 |
| `signed_bound_other_chip.bin` | 同一份镜像,标签用**另一份盐**算 —— "另一颗芯片的副本"就是这一格 |
| `signed_v2_stale_bind.bin` | `signed_v2.bin` 带着 **1.0.0 的标签**:重新签了新版本却没重绑,标签必须对不上 |
| `bind_salt_sector.bin` | `tools/agm_bind.py salt-sector` 为盐 00..0f 产出的 4 KiB 扇区镜像 —— 设备侧读扇区的那段代码拿它当"host 工具到底写了什么字节"的判据 |

## 比特流(fabric)夹具怎么来的

`bs_signed.bin` / `bs_pubkey.bin` 是同一套思路的比特流版,只是负载换成了**整份 99944 B fabric**
(合成图案,不是真的比特流 —— 夹具只要形状对),签名用一个**新的一次性 P-256 密钥**:

```sh
python3 -c 'import sys; sys.argv=["imgtool"]; from imgtool import main as m; \
            m.imgtool.main(["keygen","-k","bs_key.pem","-t","ecdsa-p256"], standalone_mode=True)'
python3 -c "open('bs_image.bin','wb').write(bytes(((i*7+3)&0xff) for i in range(99944)))"
tools/sign_image.py --bitstream bs_image.bin bs_key.pem \
    -o bs_signed.bin --pubkey-out bs_pubkey.bin
```

私钥同样不提交。容器形状(实测):`magic 0x96f3b83d`、`ih_load_addr 0x800b0020`、
`hdr 32`、`img 99944`,共 100127 B;TLV 三个 —— `0x10` SHA-256(头+负载)、
`0x01` KEYHASH(SHA-256(SPKI))、`0x22` ECDSA DER(71 B)。

`--bitstream` 会自己加上 `--overwrite-only`:MCUboot 默认要给 swap 预留
`128 sectors × 3 × 8 = 3 KB` 的 trailer,而 100 KiB 的槽装不下它。我们本来就不 swap
(写非活动槽、记录提交 —— 正是 overwrite-only 的模型),所以那 3 KB 对我们是纯浪费;
它只影响 trailer 的尺寸计算,容器字节一模一样(不 `--pin` 就不写 trailer)。
