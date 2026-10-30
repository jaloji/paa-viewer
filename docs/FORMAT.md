# PAA format notes

What the decoder in `src/core/paa.cpp` implements. All values are little-endian.

References: the [Bohemia Interactive community wiki](https://community.bistudio.com/wiki/PAA_File_Format) and the [ImHex PAA pattern](https://github.com/WerWolv/ImHex-Patterns/blob/master/patterns/a3/a3_paa.hexpat).

## Layout

```
u16          type            pixel format (absent in old .pac files)
TAGG[]       tags            while the next 4 bytes are "GGAT"
u16          palette count   followed by count x BGR triplets (0 for PAA)
MIPMAP[]     mipmaps         until a 0 x 0 entry
u16          0               end marker
```

## Pixel formats

| type | name | storage |
|---|---|---|
| `0xFF01`–`0xFF05` | DXT1–DXT5 | S3TC blocks, optionally LZO1X compressed |
| `0x4444` | ARGB4444 | `AAAA RRRR GGGG BBBB`, LZSS |
| `0x1555` | ARGB1555 | `A RRRRR GGGGG BBBBB`, LZSS |
| `0x8888` | ARGB8888 | bytes B, G, R, A, LZSS |
| `0x8080` | AI88 | intensity byte then alpha byte, LZSS |

## Tags

Each tag is `char[8] name`, `u32 length`, then `length` bytes. Names are stored reversed.

| name | meaning |
|---|---|
| `GGATCGVA` | average color, BGRA |
| `GGATCXAM` | max color, BGRA |
| `GGATGALF` | alpha flag: 0 none, 1 interpolated, 2 binary |
| `GGATSFFO` | 16 x u32 file offsets of the mipmaps |
| `GGATZIWS` | channel swizzle, 4 bytes in A, R, G, B order |

### Swizzle

TexConvert can store channels shuffled (`channelSwizzleR = "1-A"` etc. in `TexConvert.cfg`, documented as *file (physical) = memory (logical)*). Each tag byte says which source channel a stored channel holds:

| value | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| source | A | R | G | B | 1-A | 1-R | 1-G | 1-B | 1 | 0 |

To display the original image, each stored channel is copied back to its source channel (inverted for 4–7). Constant channels (8, 9) carry no data and are left as stored. The game ignores this tag; TexView uses it.

## Mipmap entry

```
u16   width       for DXT types, bit 15 set = data is LZO1X compressed
u16   height
u24   data size
u8[]  data
```

Unpacked sizes: DXT1 `ceil(w/4) * ceil(h/4) * 8`, DXT2–5 `* 16`, ARGB8888 `w * h * 4`, other formats `w * h * 2`.

## LZSS (non-DXT types)

Repeat until the output is full: read a flag byte, then for each bit from the lowest:

- bit 1: copy one literal byte.
- bit 0: read two bytes `b0 b1`; `distance = b0 | (b1 & 0xF0) << 4`, `length = (b1 & 0x0F) + 3`; copy `length` bytes starting `distance` bytes back (positions before the start read as `0x20`).

A 4-byte checksum follows: the sum of all output bytes taken as **signed** 8-bit values.

## LZO1X (DXT types)

Standard LZO1X-1 stream ending with the `0x11 0x00 0x00` end marker. The unpacked size is known from the mipmap dimensions.
