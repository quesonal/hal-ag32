# Host-side tests for the tools

These are plain `pytest` unit tests for the *host* side of the bootloader work:
the wire formats the tools speak to the loader. They need no board, no probe
and no west workspace -- just the tool's own dependencies (`pyserial`, `cbor2`),
which the repo's virtual environment already has:

```sh
source <your-venv>/bin/activate
pytest tools/tests -q
```

What they pin, and why it matters:

| File | Covers |
|---|---|
| `test_agm_upload.py` | the console upload protocol's 16-byte frame layout (`0xA5`, cmd, target, offset/len/CRC32 little-endian), the FINISH payload (`total, crc, load, entry`), and the "resend once on a frame CRC NACK, give up on anything else" retry rule |
| `test_smp_cli.py` | `../tools/smp_cli.py`'s SMP over console framing: the `0x0609`/`0x0414` base64 lines (and the 127-byte line limit the loader's buffer assumes), the CRC16, the SMP header, and the CBOR bodies for `image state`/`upload`/`erase` |

The *device* side of both protocols is tested on the target:
`tests/drivers/misc/boot_agm/` runs the real AN3155 server on native_sim, and the
mcumgr group is exercised on
the dev board with `smpmgr` / `../tools/smp_cli.py` (3.26.15, 3.26.20).
