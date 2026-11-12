# Real-time clock

- The SM5xx games keep time in their own RAM.
- MAME always starts them at 12:00.
- LCD Lab writes the system time into the game's clock RAM when content starts.
- It presses Time first, the same as a person would.
- It also writes the time when a save state is loaded in the first five seconds.
- Frontends load a state this way when they resume an autosave.

## Layouts

- Each game stores its clock in its own way.
- Every game needs a layout in `src/content/rtc_layouts.inc`, for example:

  ```c
  {"gnw_helmet", {1, 0x36, 0x37, 0x38, 0x39, 0x0, 0x0, 0x8, 0xff, 0, 0, 0}},
  ```

- The first field marks the layout as present.
- The next four fields are the RAM addresses of the hour tens, hour units, minute tens and minute units.
- The next three fields are the base, AM and PM bits.
- The hour tens nibble is `base | digit | (pm ? PM : AM)`.
- The next three fields give a separate AM/PM nibble and its AM and PM values.
- An AM/PM address of `0xff` means there is no separate nibble.
- The last field marks a clock with hours from 0 to 23.

## Finding layouts

```sh
make tools
build/rtc_find sets/*.zip >> src/content/rtc_layouts.inc
```

- `rtc_find` only looks at sets with a Time button.
- It boots each game the same way the core does.
- It reads RAM once a minute for 70 emulated minutes to find the minute nibbles.
- It writes 59 into the minute nibbles and lets the hour roll over 24 times.
- The nibbles that change with each hour give the hour digits and the AM/PM storage.
- It writes test times with the core's own code.
- It checks the game keeps each test time:
  - 3:47 PM moves to 3:48 PM.
  - 9:59 moves to 10:00.
  - 11:59 AM moves to 12:00 PM.
  - 11:59 PM moves to 12:00 AM.
  - 12:59 moves to 1:00.
- It checks two different times give two different LCD pictures.
- It prints a layout only when every check passes.
