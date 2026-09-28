# Signed-image fixtures (RSA-2048-PSS)

The RSA counterpart of `tests/drivers/misc/boot_agm_ecdsa/fixtures/`: same
payload, same four containers, but signed with RSA keys and therefore carrying
the profile's own TLV type (`0x20`), its raw 256-byte signature and its
PKCS#1-based KEYHASH. Generated once and checked in, so the test needs no
tooling at build time. The private keys are disposable and **not** committed:
regenerating the fixtures means generating new keys, and therefore a new
`pubkey.bin` too.

```sh
# 1. throw-away test keys (only ever used for these fixtures)
python3 -c 'import sys; sys.argv=["imgtool"]; from imgtool import main as m; \
            m.imgtool.main(["keygen","-k","key1.pem","-t","rsa-2048"], standalone_mode=True)'
#    ... and the same for key2.pem

# 2. the payload: 1004 bytes, (i*7+3) & 0xff -- byte-identical to the ECDSA
#    fixtures' image.bin, so the two suites really test the same bytes
python3 -c "open('image.bin','wb').write(bytes(((i*7+3)&0xff) for i in range(1004)))"

# 3. the containers, through the same tool production uses -- so the
#    `ih_load_addr` the loader checks is the one production writes
tools/sign_image.py image.bin key1.pem -o signed_ok.bin --pubkey-out pubkey.bin \
    --slot-size 0x10000 --slot-base 0x80030000
tools/sign_image.py image.bin key2.pem -o signed_other_key.bin --slot-size 0x10000 \
    --slot-base 0x80030000
tools/sign_image.py image.bin key1.pem -o signed_other_slot.bin --slot-size 0x10000 \
    --slot-base 0x80020000
```

`--slot-size 0x10000` matches the native test's application slot, and
`--slot-base 0x80030000` is that slot's address (the tool's default is the
*board's* on-die slot, `0x8007e000`, which the native overlay does not use).
`sign_image.py` takes the key type from the key file: an RSA key makes it emit
the 270-byte PKCS#1 DER and hash that for the KEYHASH check.

| 文件 | 内容 |
|---|---|
| `image.bin` | 1004 B 的裸镜像(没有 MCUboot 头) |
| `signed_ok.bin` | `key1` 签的容器,`ih_load_addr = 0x80030020`(= slot + 0x20) |
| `signed_other_key.bin` | 同样内容、`key2` 签(用于"换了密钥"的负向用例) |
| `signed_other_slot.bin` | `key1` 签,`ih_load_addr = 0x80020020` —— **签名有效、地址不对**(用于"上传到错的槽"的负向用例) |
| `pubkey.bin` | `key1` 的 **PKCS#1 `RSAPublicKey` DER**(270 B),即 loader 编译进去的那把 |
| `ecdsa_signed.bin` | **另一档**的容器(`boot_agm_ecdsa/fixtures/signed_ok.bin` 的逐字节副本):`SIGNED-IMAGES-PLAN.md` §2.1 矩阵"RSA 档拒绝 ECDSA 镜像"那一格的夹具 |

What is inside `signed_ok.bin`, as dumped from the container (this is the
measured form of the conventions [SIGNED-IMAGES-PLAN.md](../../../../../docs/SIGNED-IMAGES-PLAN.md) §2.3 pins down;
the ECDSA fixture's third TLV is `0x22` DER instead, and its KEYHASH covers the
SubjectPublicKeyInfo):

```
magic 0x96f3b83d  ih_load_addr 0x80030020  hdr 32  image 1004
TLV area at 0x40c: magic 0x6907, 336 B
  0x10 len  32  = SHA-256(header + image)
  0x01 len  32  = SHA-256(pubkey.bin)          # SHA-256(PKCS#1 DER), not SPKI
  0x20 len 256  = the raw RSA-2048-PSS signature (no DER to unwrap)
```
