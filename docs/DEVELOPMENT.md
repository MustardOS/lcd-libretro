# Development

## Source layout

- `src/libretro.c` holds the Libretro API with options, input, audio, video and save states.
- `src/cpu/` holds the SM5xx CPUs adapted from MAME.
- `src/hardware/` holds the `hh_sm510` board with the input matrix, wiring, piezo, LCD decay, board variants and clock writes.
- `src/content/` holds the machine registry, ROM set loading, `.gnw` loading, sample packs and clock layouts.
- `src/renderer/` holds the SVG and `.gnw` segment renderer.
- `src/util/` holds the ZIP, inflate, PNG, hash and file reading code.
- `src/diagnostics/` holds the Catch and Carry test game.
- `tests/` holds the unit tests and the Libretro smoke test.
- `tests/reference/` holds the MAME comparison harness.
- `tools/` holds the generators and test tools.

## Tests

```sh
make check
```

- `make check` builds the core and every test program.
- It generates the `.gnw` test fixtures with `tools/selftest_build.py`.
- It then runs every test.
- The self-test ROM set and the other fixtures are original works.
- No ROM data is needed to run the tests.
- Run the sanitiser and leak checks like this:

  ```sh
  make BUILD_DIR=build-ub tests \
      CFLAGS='-std=c11 -O1 -g -fsanitize=undefined -fsanitize-undefined-trap-on-error -Wall -Wextra -Wpedantic -Werror -fPIC'
  valgrind --leak-check=full build-ub/tests/machine_test
  ```

## Comparing with MAME

- `make reference-check MAME_SRC=/path/to/mame` compiles the unmodified MAME SM5xx sources.
- A small framework shim in `tests/reference/emu.h` stands in for the rest of MAME.
- The check runs MAME and `src/cpu/sm5xx.c` side by side on random programs for every CPU variant.
- Registers, RAM and pin events are compared after every oscillator tick.
- `build/reference/compare --rom <dump>` runs the same comparison on a real SM5A program.
- The MAME revision in use is recorded in `third_party/MAME_REVISION`.

## Generated files

- `tools/mame_registry.py /path/to/mame --report` writes `src/content/machines_mame.inc` and `docs/COMPATIBILITY.md`.
- `build/rtc_find sets/*.zip` finds clock layouts for `src/content/rtc_layouts.inc`.
- [RTC.md](RTC.md) describes the clock layouts.
- `tools/lay_to_svg.py elbaskb.lay elbaskb` writes `src/content/builtin_elbaskb.inc`.
- `tools/selftest_build.py` writes `roms/lcdlab_selftest.zip` and `tests/fixtures/`.
- Run the generators from the repository root.
- After a MAME update, regenerate the registry first.
- The `--report` output lists any machine the generator cannot express.
- Such a machine needs a board variant in `src/hardware/machine.c`.
- Board variants use the `LCD_QUIRK_*` flags in `src/content/registry.h`.

## Tools

- `make tools` builds the tools below.
- `build/lcdrun core.so content [frames] [dump_every] [script]` runs the core without a frontend.
- `lcdrun` reports timing and saves frames.
- `lcdrun` reads core options from the environment, for example `lcd_lab_condition=Worn`.
- `build/rtc_find` finds clock layouts.
