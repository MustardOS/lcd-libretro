# Game & Watch FPGA `.gnw` files

- `.gnw` files come from agg23's Game & Watch core for MiSTer and Analogue Pocket.
- The project is at <https://github.com/agg23/fpga-gameandwatch> under the MIT licence.
- Its generator renders the MAME SVGs and artwork into fixed 720x720 images.
- `src/content/gnw.c` is an independent reader.
- It was written from that project's `docs/format.md` and `support/src/encode_format.rs`.
- No code is copied from that project.

## Layout

| Offset | Content |
|--------|---------|
| `0x000` | Version (1), MPU, screen layout, screen size and reserved bytes |
| `0x008` | Input map: 8 S ports with 4 bits each, then B, BA, ACL and the grounded port (1-based). Each byte is `[active low][code:5]`. `0x7F` means unused |
| `0x079` | Generator commit as 7 ASCII characters |
| `0x100` | 720×720 pixels of 6 bytes each: background R, lit R, background G, lit G, background B, lit B |
| `0x2F7700` | Segment runs of 5 bytes each: a 40-bit little-endian `id:10, x:10, y:10, length:10` field. A zero length ends the list |
| `0x325240` | Program ROM to the end of the file: 0x740 bytes for SM5A, 0x1000 bytes for SM510 and later |

## Reading the file

- The segment id is `x << 6 | y << 2 | z`.
- This matches the MAME output `x.y.z`.
- The field order follows the generator's `parse_title`.
- The format document lists the fields in a different order.
- LCD Lab matches the program ROM against the MAME registry by CRC32.
- A match uses the MAME definition.
- Without a match, LCD Lab builds a definition from the header.
- The MPU codes are:
  - 0 SM510
  - 1 SM511
  - 2 SM512
  - 3 SM530
  - 4 SM5A
  - 5 SM510 Tiger
  - 6 SM511 Tiger 1-bit
  - 7 SM511 Tiger 2-bit
  - 8 KB1013VK1-2 (SM5A)
- A header-only SM530 file is refused.
- The generator does not write the SM511 and SM512 melody ROM.
- LCD Lab reads the melody ROM from the MAME set instead.
- The melody ROM is matched by CRC32 and size.
- LCD Lab looks for `<set>.zip` or `<parent>.zip` beside the `.gnw` file.
- It then looks one folder up.
- It then looks in `<system>/lcd_lab/`.
- Without the MAME set, these games have silent melodies.
- The image is cropped to the area that is not black.
- Background pixels become the static frame.
- Run pixels take the lit colour.
- The segment renderer and the LCD condition option work the same as for SVG artwork.
