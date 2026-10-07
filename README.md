# ToolKitOSC board firmware

Firmware for an ESP32 that [ToolKitOSC](https://github.com/NekoTiki/ToolKitOSC) can switch outputs
on: a fan, an LED strip, a solenoid, a relay. The board announces itself on your network, you add
it in ToolKitOSC under **Settings › ESP32 boards**, and the app's per-avatar **Automations** decide
which avatar parameter switches which output, how, and for how long.

```
 VRChat ──OSC──▶ ToolKitOSC ──WebSocket (JSON)──▶ ESP32 ──GPIO──▶ MOSFET / relay ──▶ fan, LEDs, …
                     ▲                              │
                     └──────── mDNS _tkosc._tcp ────┘
```

The board keeps time itself: every "on" the app sends carries a deadline, so pulse lengths stay
exact, and nothing stays on if the app or the Wi-Fi goes away. The protocol is in
[PROTOCOL.md](PROTOCOL.md).

## 1. Flash it

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or `pip install platformio`).
2. Pick your board's environment and flash it:

   | Board                              | Environment          | Notes                                       |
   | ---------------------------------- | -------------------- | ------------------------------------------- |
   | Espressif ESP32-S3-DevKitC-1       | `esp32-s3-devkitc-1` | Default. Use the port marked "UART".         |
   | WEMOS LOLIN S3                     | `lolin_s3`           | Use the port marked "USB".                  |
   | Seeed XIAO ESP32-S3                | `seeed_xiao_esp32s3` |                                             |
   | Seeed XIAO ESP32-C3                | `seeed_xiao_esp32c3` |                                             |
   | Other ESP32-C3 boards              | `esp32-c3`           | DevKitM-1, C3 SuperMini, …                  |
   | Any ESP32 (WROOM-32 / WROVER) board | `esp32dev`          |                                             |

   ```
   pio run -e esp32-s3-devkitc-1 -t upload
   pio device monitor
   ```

> Windows: if the build fails with `xtensa-esp32-elf-g++: error: CreateProcess: No such file or
> directory`, your PlatformIO folder path is too long. Set `PLATFORMIO_CORE_DIR` to a short path
> such as `C:\pio`.

## 2. Connect it to Wi-Fi

On first boot the board opens a Wi-Fi access point named `ToolKitOSC-Setup-<id>`. Join it from
your phone or PC; the setup page opens by itself (or go to `http://192.168.4.1`). Choose your
network (2.4 GHz only), give the board a name, and optionally pick a board profile. The board
restarts and joins your network.

## 3. Add it in ToolKitOSC

Open **Settings › ESP32 boards**. The board shows up under **Found on your network**. Click
**Add**, then press the board's **BOOT** button within 30 seconds. On networks where mDNS
doesn't work (VPNs, some guest networks), use **Add by address** with the IP the serial monitor
prints.

## Wiring

An ESP32 pin gives 3.3 V and a few milliamps: enough for one LED, not for a fan, a solenoid or an
LED strip. Switch those through a logic-level MOSFET module or a relay module, powered from their
own supply with a shared ground. Put a flyback diode across motors and solenoids. Relay modules
that switch on when their input is LOW need **active low** turned on for that output in the app.

## Which pins are offered

The firmware only offers pins that are safe to drive, in three layers. Each layer only removes
pins, so a wrong board profile can hide pins but never expose an unsafe one.

1. **Chip** (from the build): strapping, flash, USB, serial and input-only pins are never offered.
2. **Module** (detected at boot): on an ESP32 with PSRAM (WROVER) or in a PICO package,
   GPIO 16 and 17 are taken.
3. **Board profile** (setup page or the `board` serial command): only the pins the board breaks out
   and doesn't already use, with their silkscreen names. `generic` turns it off.

On the ESP32-S3, GPIO 33–37 are never offered: octal flash or PSRAM (as on N16R8 modules) uses
them, and the firmware can't see octal PSRAM it wasn't built for, so it can't tell when they are
free.

The tables are in [src/core/pins.cpp](src/core/pins.cpp). Adding a board profile is a few lines
there. Check new tables against the chip's datasheet and the board's schematic.

## Status LED

On boards with a known LED (DevKitC-1, LOLIN S3, XIAO ESP32-S3):

| LED                 | Meaning                                    |
| ------------------- | ------------------------------------------ |
| Blue, fast blink    | Pairing: press BOOT now                    |
| Green               | ToolKitOSC is connected                    |
| Yellow, slow blink  | Not on Wi-Fi                               |
| Orange, slow blink  | BOOT held 3 s: let go to unpair            |
| Red                 | BOOT held 10 s: let go to factory reset    |

## Resetting without re-flashing

**Unpair** frees the board for another PC and keeps its Wi-Fi. **Factory reset** also forgets the
Wi-Fi, the name and the outputs, and reopens the setup access point.

| How                                          | Unpair | Factory reset |
| -------------------------------------------- | ------ | ------------- |
| **Remove** in ToolKitOSC (board online)      | ✓      |               |
| Hold BOOT while the board is running         | 3 s    | 10 s          |
| Power-cycle 5 times, each under 3 s          |        | ✓             |
| Serial monitor: `unpair` / `reset`           | ✓      | ✓             |

Hold BOOT only once the board is running: held while it powers on, it puts the ESP32 into flashing
mode instead.

If the board loses Wi-Fi for 3 minutes it reopens the setup access point, but keeps its pairing.

## Serial commands

At 115200 baud: `status`, `pins`, `boards`, `name <text>`, `board <id>`, `unpair`, `reset`,
`restart`, `help`.

## Development

```
src/core/            protocol, outputs and pin tables, no Arduino code
src/platform/esp32/  Wi-Fi, mDNS, WebSocket, NVS, button, LED, serial
include/             headers, config.h for ports and timings
test/                Unity tests for src/core, run on the PC
```

The core is tested on the PC, no board needed (requires a MinGW-w64 GCC on `PATH`):

```
pio test -e native
```

Libraries: ArduinoJson, ESPAsyncWebServer and AsyncTCP (ESP32Async), WiFiManager.
