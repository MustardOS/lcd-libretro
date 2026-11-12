# LCD Lab

A Libretro core for Sharp SM5xx LCD handhelds.

- It plays Nintendo Game & Watch, Tiger Electronics, Konami, Elektronika and Nelsonic games.
- It runs the original program ROMs on a C11 port of the MAME SM5xx CPUs and `hh_sm510` board logic.
- It draws the LCD from the SVG artwork in each MAME ROM set.
- It needs only libc and libm.
- It supports all 175 machines in the MAME 0.289 `hh_sm510` driver.
- The full list is in [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).
- No ROMs or artwork are included.
- Use ROM sets you are entitled to use.

## Content

- **MAME ROM sets (`.zip`)**
  - A set holds the program ROM and the SVG screens.
  - SM511, SM512 and SM530 sets also hold a melody ROM.
  - Sets are identified by CRC32 and size, then checked with SHA-1.
  - Split clones read shared files from `<parent>.zip` in the same folder.
  - Merged sets use the revision with the same name as the archive.
- **`background.png`**
  - Place it at the root of a set's archive or its parent.
  - It is multiplied under the LCD in place of the tint.
- **Game & Watch FPGA `.gnw` files**
  - These come from agg23's MiSTer and Analogue Pocket core.
  - The format is described in [docs/GNW_FORMAT.md](docs/GNW_FORMAT.md).
  - Melody ROMs are read from the MAME set beside the file, one folder up, or in `system/lcd_lab`.
- **MAME sample packs**
  - Place them in `samples/<set>.zip` beside the ROM sets, or in `system/lcd_lab/samples`.
  - They provide the voice clips for Konami's TMNT II, Star Trek and Top Gun II.
- **`.lcd` files or no content**
  - These start Catch and Carry, a small built-in test game.

## Controls

- Buttons are assigned from the MAME input types of each machine.
- Frontends show the machine's own button names.
- The D-pad is for movement and for the left player on two-player games.
- A, B, Y and X are buttons 1 to 4 or the right player's controls.
- Start is usually Game A or Power On.
- Select is usually Game B.
- L is usually Time or Pause.
- R is usually Alarm.
- L2 and R2 are for Power Off, Sound and extra buttons.
- R3 is the ACL reset button.
- Keypad machines put their named keys on the pad.
- Every key on a keypad machine also works from a keyboard with MAME's key assignments.

## Clock

- Games with a known clock layout start at the system time.
- The core powers the game on, presses Time and writes the time into the game's clock memory.
- The same happens when a save state is loaded in the first five seconds.
- Frontends load a state this way when they resume an autosave.
- More detail is in [docs/RTC.md](docs/RTC.md).

## Core options

- **Real-time clock** turns the clock setting on or off.
- **Background image** turns `background.png` on or off.
- **Display size** sets the box the SVG artwork fits into, from 320x240 up to 1920x1440.
- `.gnw` content always keeps its own 720x720 size.
- **LCD condition** chooses Perfect, Good, Worn or Bad.
- A worse condition lowers the contrast, shows more of the unlit segments and leaves trails behind moving segments.
- **LCD tint** chooses MAME grey, Neutral, Green or Amber.
- **Dual screen gap** chooses the MAME gap or no gap between screens.
- **Piezo sound** turns the sound on or off.
- **Test game speed** sets the speed of Catch and Carry.
- Display options never change the emulated state.

## Building

```sh

make                # build/lcd_libretro.so
make check          # tests, needs python3 for the test fixtures
make CROSS_COMPILE=aarch64-buildroot-linux-gnu- \
    CFLAGS="-std=c11 -O2 -mcpu=cortex-a53 -Wall -Wextra -Wpedantic -fPIC"

```

- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) covers the source layout, the MAME comparison and the generators.

## Installing on MustardOS

- Copy `lcd_libretro.so` to `/opt/muos/share/core/`.
- Copy `info/lcd_libretro.info` to `/opt/muos/share/emulator/retroarch/info/`.
- Add the core to the Game & Watch system definition.
- One place for this is `MUOS/info/manifest/core/Nintendo/Handheld Electronic - Game and Watch.json`:

  ```json

  "lcd lab": { "name": "LCD Lab", "core": "lcd_libretro.so", "config": "LCD Lab" }
  ```

- Assign *LCD Lab* (Pickles) to the folder with the ROM sets.

## Known limitations

- Konami's voice chip has never been dumped.
- Konami voice clips are silent without a sample pack.
- The Star Fox sound effect chip is not emulated.
- MAME has the same two limits.
- The two text labels in Basketbol are not drawn.
- The SVG renderer has no fonts.
- Holding L2 steps the Altair dosimeter dial at a fixed rate.
- The clocks in `vesarif` and `trsgkeep` do not have a verified layout yet.

## Licence

- Original code uses the MIT licence.
- Code adapted from MAME uses the BSD-3-Clause licence.
- Full details are in [docs/PROVENANCE.md](docs/PROVENANCE.md).

