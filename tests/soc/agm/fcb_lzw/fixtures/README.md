# Compressed-bitstream fixture

`example_board.compressed.bin` is the compressed form of the canonical bitstream
the boards use (`<your canonical bitstream>`,
md5 `6378549f3a8f82dd386353077f3d4a02`, 99944 B, CRC32 `0x361d90d4`), as
`CONFIG_AGM_FCB_BITSTREAM_COMPRESSED` expects to find it in flash.

```sh
python3 tools/compress_bitstream.py \
    "<your canonical bitstream>" \
    -o tests/soc/agm/fcb_lzw/fixtures/example_board.compressed.bin --check
```

`--check` decodes the result again and compares it with the input, so the
fixture cannot be regenerated into something the decoder would reject. The
test only needs the compressed stream: the expected output is pinned by its
CRC32 (`0x361d90d4`, the same number the bitstream record carries for this file
on the dev board), rather than by checking in another 100 KB.

The format, for reference:

```
bytes 0..7    IDCODE, USERID -- raw, not compressed and not encrypted
bytes 8..     LZW bit stream (MSB first, 9/10/11-bit codes) decoding to the
              remaining 99936 bytes of the config
```
