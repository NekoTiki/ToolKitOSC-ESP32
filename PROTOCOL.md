# ToolKitOSC board protocol, version 1

How the ToolKitOSC app talks to a board running this firmware. This file is the contract between
the app and the firmware: any change to it bumps the protocol version (`TKOSC_PROTO` in
`include/config.h`).

## Discovery

The board announces `_tkosc._tcp` over mDNS on port `8787`, host name `tkosc-<id>.local`.

| TXT key  | Example      | Meaning                                                              |
| -------- | ------------ | -------------------------------------------------------------------- |
| `id`     | `a1b2c3`     | Last 3 bytes of the Wi-Fi MAC. Stable across IP changes.             |
| `name`   | `Desk board` | Name set on the setup page or with the `name` serial command.        |
| `proto`  | `1`          | Protocol version. The app refuses a version it doesn't know.         |
| `fw`     | `0.1.0`      | Firmware version.                                                    |
| `paired` | `0` / `1`    | Whether the board already has a pairing token. Updated live.         |

## Connection

WebSocket at `ws://<host>:8787/tkosc`, opened by the app. One JSON object per text frame; `t` is
the message type. Fragmented and binary frames, and frames over 1024 bytes, are ignored.

Only one app session at a time: a newer authenticated `hello` closes the older socket.

## Messages

`→` app to board, `←` board to app. Fields marked `?` are optional.

### Pairing

| Message   | Dir | Fields              | Meaning                                                                          |
| --------- | --- | ------------------- | -------------------------------------------------------------------------------- |
| `pair`    | →   |                     | Start pairing. Opens a 30 s window for a short press of the board's BOOT button. |
| `pairing` | ←   | `timeoutMs`         | Waiting for the press. The status LED blinks blue.                               |
| `paired`  | ←   | `token`             | Button pressed. 32 random bytes in base64. Replaces any previous token.          |
| `unpair`  | →   |                     | Clears the token. The board closes the socket with 1000.                         |

`pair` works with or without a session, and on a board that is already paired: it needs the button
anyway. Pairing again drops the app that used the old token. If nobody presses the button in time,
the board sends `err` with code `pair-timeout`.

### Session

| Message   | Dir | Fields                                                  | Meaning                                         |
| --------- | --- | ------------------------------------------------------- | ----------------------------------------------- |
| `hello`   | →   | `proto`, `token`                                        | Must come first. Starts the session.            |
| `welcome` | ←   | `proto`, `id`, `fw`, `name`, `chip`, `board`, `outputs` | Reply to `hello`.                               |
| `config`  | →   | `outputs: {pin, activeLow?, maxOnMs?}[]`                | Which pins to use. Saved on the board.          |
| `ping`    | →   |                                                         | Every second.                                   |
| `pong`    | ←   |                                                         | Reply to `ping`.                                |

`welcome.outputs` is `{pin, pwm, label?}[]`: the pins this board allows as outputs, after the chip,
module and board profile rules (see the README). `label` is the silkscreen name when the board
profile knows it. `chip` is e.g. `ESP32-S3`, `board` the profile name or `Generic`.

`config` replaces the whole list and switches every output off. Pins left out stay unused. A pin
that isn't in `welcome.outputs` makes the board reply `err` `bad-pin` and change nothing.
`maxOnMs` defaults to 10000.

Any message from the app counts as a sign of life. **After 3 s without one, the board switches
everything off and closes the socket with 4408.**

### Outputs

| Message | Dir | Fields                                 | Meaning                                                          |
| ------- | --- | -------------------------------------- | ---------------------------------------------------------------- |
| `set`   | →   | `pin`, `on`, `ms` (required when on)   | On until now + `ms`, or off now.                                 |
| `pulse` | →   | `pin`, `onMs`, `offMs?`, `count?`      | `count` pulses (default 1) of `onMs`, `offMs` apart (default 0). |
| `pwm`   | →   | `pin`, `duty` (0–1), `ms`              | Dim level until now + `ms`. `duty: 0` switches off.              |
| `stop`  | →   |                                        | Every pin off, every pulse train cancelled, trips cleared.       |
| `out`   | ←   | `pin`, `on`, `duty`                    | Sent on every real change of a pin.                              |

Rules:

- **Deadlines only move later.** A `set` or `pwm` "on" sets the deadline to the later of the
  current one and now + `ms`. A short pulse can't cut a longer one short.
- **Holds are leases.** To keep a pin on, resend `set` (or `pwm`) before its `ms` runs out. The
  app uses 1500 ms renewed every 500 ms.
- **`maxOnMs` caps one continuous on-time.** When a `set` or `pwm` lease reaches it, the pin
  switches off and *trips*: the board sends `err` `max-on` with the `pin`, and refuses further
  "on" commands for that pin (`err` `max-on`) until it gets `set` with `on: false`, or `stop`.
  Pulses longer than `maxOnMs` are shortened instead, without tripping.
- **`pulse` replaces** whatever the pin was doing, and a `set`/`pwm` "on" ends a running train.
- PWM uses 5 kHz, 10-bit. The number of pins that can dim at once is limited by the chip's LEDC
  channels (8 on ESP32, ESP32-S2 and ESP32-S3, 6 on ESP32-C3); past that the board replies
  `not-pwm`.

### Errors

`err` (←) has `code`, plus `ref` (the type of the message it refers to) and `pin` when they apply.

| Code           | Meaning                                                               |
| -------------- | --------------------------------------------------------------------- |
| `bad-msg`      | Not JSON, unknown type, or a field missing or of the wrong type.      |
| `bad-pin`      | Pin not configured (`set`, `pulse`, `pwm`) or not offered (`config`). |
| `not-pwm`      | No PWM channel left for this pin.                                     |
| `max-on`       | The pin tripped, or is still tripped. See `maxOnMs` above.            |
| `bad-proto`    | `hello` with another protocol version. Followed by close 4400.        |
| `pair-timeout` | Nobody pressed BOOT within the pairing window.                        |

### Close codes

| Code | Meaning                                                             |
| ---- | ------------------------------------------------------------------- |
| 1000 | Unpaired by the app.                                                |
| 1013 | Too many sockets open.                                              |
| 4000 | Unpaired on the board (button or serial).                           |
| 4400 | Wrong protocol version.                                             |
| 4401 | Not authenticated: no `hello` yet, wrong token, or the token was replaced by a new pairing. |
| 4408 | No message from the app for 3 s.                                    |
| 4409 | A newer app session took over.                                      |

Whenever the authenticated socket closes, for any reason, the board switches everything off.

## Example

```
→ {"t":"hello","proto":1,"token":"AQgPFh0kKzI5QEdOVVxjanF4f4aNlJuiqbC3vsXM09o="}
← {"t":"welcome","proto":1,"id":"a1b2c3","fw":"0.1.0","name":"Desk board","chip":"ESP32","board":"Generic","outputs":[{"pin":4,"pwm":true},…]}
→ {"t":"config","outputs":[{"pin":4,"maxOnMs":10000}]}
→ {"t":"set","pin":4,"on":true,"ms":2000}
← {"t":"out","pin":4,"on":true,"duty":1}
   … 2 s later, on the board …
← {"t":"out","pin":4,"on":false,"duty":0}
```
