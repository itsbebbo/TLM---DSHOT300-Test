# ESC Tester — M5Stack Core2

Go/no-go bench tester for the iFlight Blitz E80 Single. Drives the ESC over DShot300 on Port A, reads its KISS/BLHeli_32 telemetry, and judges PASS/FAIL. The full design is in [ESC-Tester-Specification.md](ESC-Tester-Specification.md).

## Build

PlatformIO with the **pioarduino** platform (Arduino-ESP32 3.x on ESP-IDF 5.x), pinned in `platformio.ini`. The stock `espressif32` platform will not build this.

```
pio run                 # build
pio run -t upload       # flash
pio device monitor      # serial log, 115200
```

LVGL 9 is configured through `build_flags` (`LV_CONF_SKIP`), so there is no `lv_conf.h`.

Build output and downloaded libraries do not go in this folder. `workspace_dir` in `platformio.ini` puts them in `%USERPROFILE%\PlatformIO\workspaces\esc-tester` on the C: drive, so Google Drive never syncs them. The flashable images are in that folder's `build\m5stack-core2\` subfolder.

## Simulation mode

`ESC_SIM_MODE` in [include/config.h](include/config.h) is **on** by default. The tester then generates synthetic telemetry from a model of a healthy ESC and motor, and never touches the UART. You can run the whole sequence on a bare Core2.

The **SIM** banner on the Home screen — it takes the slot the bench warning occupies in hardware mode — cycles through injected faults on each touch. Each one should produce its reason code:

| SIM setting | Expected result |
| --- | --- |
| healthy | PASS |
| no telem | E01 |
| low volts | E03 |
| no current | E06 |
| overcurrent | E07 |
| no spin | E08 |
| flat rpm | E09 |
| dropouts | E10 |
| hot | E11 |

Pressing STOP during a run should give E13.

Set `ESC_SIM_MODE` to `0` before connecting the real ESC. DShot is still generated on GPIO32 in simulation mode, so you can scope the line while in sim mode.

## Wiring (Port A)

| Grove pin | Core2 signal | To ESC |
| --- | --- | --- |
| 1 | GPIO33 (Port A SCL), telemetry RX | Telemetry wire, through 1 kOhm |
| 2 | GPIO32 (Port A SDA), DShot300 out | Signal wire, through 100 Ohm |
| 3 | 5 V | **Leave unconnected** |
| 4 | GND | Servo-lead ground |

Note the pins differ from the CoreS3 this was ported from, which breaks out GPIO1 and GPIO2 on the same port. Verify the physical pin order with a meter before the first connection.

Power the Core2 first, then the ESC. Firmware also switches off the Grove 5 V output (`cfg.output_power = false`, the AXP192 boost converter on this board).

## Home screen

```
ESC TESTER                       TLM OK      header, with the link indicator
--------------------------------------
              24.68 V                        bus voltage, 48 px
ESC 31 C     bus voltage     runs: 4         the strip under it
+------------------------------------+
| Connect Motor & ESC WITHOUT        |       the bench instruction
| Propeller and Touch Start          |
+------------------------------------+
|              START                 |       70 px, full width
+------------------------------------+
```

The instruction is the last thing between the live reading and the button, and it is not behind an acknowledgement dialog — it is there to be read on the way to START, every run.

Everything above the banner is positioned from the banner's *measured* height rather than from fixed rows, so rewording the message cannot push a live value under it or under START. If you reword it, keep it to two lines at 290 px: `ESC_SIM_MODE` replaces the banner with the fault selector, which is one line.

## Reading the serial log

`pio device monitor` at 115200 now carries a diagnostic line every second, plus one per fault and a per-set-point summary at the end of each run. `CORE_DEBUG_LEVEL=3` in `platformio.ini` is what makes it appear — without it Arduino compiles every `ESP_LOG` call in the firmware out, including the ones reporting RMT failures and the telemetry parser falling back to its sliding window.

```
IDLE               tlm  98/s age     9 ms | rej crc 0 span 0 range 0 held 2 | gap | dshot on req 4021 miss 0
DWELL    E10       tlm  97/s age   258 ms | rej crc 0 span 0 range 0 held 2 | gap | dshot on req 6180 miss 0
SP1  frames 181/200 = 90%  v 25.03  i 0.41  erpm 7100  tmax 31
```

Which is to say, for an `E10`:

| What the line shows | What it means |
| --- | --- |
| `tlm` near 100/s, `age` small | The link is fine; the `E10` came from a dwell's frame count, so read the `SP` lines |
| `tlm` drops to 0, `rej crc` climbing | Frames are arriving corrupted — wiring, ground, or motor noise |
| `tlm` drops to 0, every `rej` flat | The ESC stopped answering; check `dshot on` and `req` still rising |
| `rej range` climbing | Frames pass CRC but decode to nonsense — misframing, or the wrong baud |
| `held` climbing alone | Frames are valid but too far apart to hold the link lock |
| `sliding` instead of `gap` | The parser gave up on gap framing; the ESC may have Auto Telemetry on |

Set `CFG_DIAG_LOG` to 0 in [include/config.h](include/config.h) to silence it.

## Idle behaviour

DShot is transmitted continuously, Idle included, with the telemetry bit set on every tenth frame. BLHeli_32 answers requests rather than streaming on its own, so that Idle polling is the only reason the Home screen has live voltage and a link indicator at all. The ESC consequently stays armed at throttle 0 for as long as the tester is on; the supply switch remains the emergency stop.

Set `CFG_IDLE_DISARM_MS` non-zero (60000 was the old default) to stop transmitting after that long in Idle and let the ESC disarm and beep instead. Telemetry stops with it until the next touch.

## Layout

| File | Job |
| --- | --- |
| [include/config.h](include/config.h) | Every tunable constant: pins, timings, set points, thresholds, trips |
| [src/dshot.cpp](src/dshot.cpp) | RMT DShot300 generator, 1 kHz frame timer, command guard |
| [src/esc_telem.cpp](src/esc_telem.cpp) | UART framing and CRC8, or the simulator |
| [src/verdict.cpp](src/verdict.cpp) | Medians and the pass/fail decision. No hardware dependencies |
| [src/test_seq.cpp](src/test_seq.cpp) | State machine, ramps, trips, watchdogs |
| [src/ui.cpp](src/ui.cpp) | LVGL screens, touch, tones |
| [src/main.cpp](src/main.cpp) | Boot and task creation |

## Things to check on first boot

1. **Screen colours.** If colours look wrong, flip the `swap` argument of `writePixels()` in `flush_cb` ([src/ui.cpp](src/ui.cpp)).
2. **DShot waveform, with no ESC attached.** Scope GPIO32 and check for a 3.34 us bit period, 2.5 us / 1.25 us high times, low idle, a frame every 1 ms, and the telemetry bit set on every 10th frame.
3. **Telemetry wire.** Check it against iFlight's wiring diagram before building the harness.
4. **Telemetry with no ESC connected.** Leave GPIO33 unconnected and watch the Home screen for a few minutes. The link indicator must stay dark. If it flickers, the line is not being held and the noise is beating the frame validation — check the pull-up reached the pin before blaming the parser.
5. **The button row below the screen.** The Core2's digitiser is 40 px taller than its display and covers the BtnA/B/C strip. Pressing there must do nothing — if it fires START, the `t.y >= SCR_H` guard in `touch_cb` ([src/ui.cpp](src/ui.cpp)) is not doing its job.

## Where the code differs from the spec

- **LVGL tick** comes from `lv_tick_set_cb(millis)`, not a separate 1 ms `esp_timer`. The effect is the same, with one less timer.
- **The DShot timer** runs on the ESP-IDF `esp_timer` task, which runs at high priority on the core IDF assigns, not pinned to core 1. Because it outranks the UI task, a redraw cannot delay it.
- **A dwell with zero samples** reports `E10` (telemetry) before `E03`. Otherwise a missing dwell would read as low bus voltage.
- **Precheck** reports `E10` when 1 to 29 frames arrive, and keeps `E01` for zero frames.
- **Ramps between set points** start from the previous set point rather than from 48, so throttle never dips between points.
- **A motor still spinning** 2 s after spindown reports `E12`.
- **LVGL's pool is 32 kB and the draw buffers are 24 lines**, against the 48 kB and 40 lines the spec's CoreS3 sizing implies. The classic ESP32's DRAM segment will not link the larger pair.

## Not yet built

This first version leaves out several spec items: SD card logging, the NVS run counter (the counter resets on reboot), the Settings screen, ESC serial entry, and host-side unit tests for `verdict.cpp`.
