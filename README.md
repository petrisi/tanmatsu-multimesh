# MultiMesh

**Mesh communications for the [Tanmatsu](https://nicolaielectronics.nl/tanmatsu/)
handheld — MeshCore and Meshtastic in one app, switched with a single keypress.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: ESP32-P4](https://img.shields.io/badge/platform-ESP32--P4-informational.svg)](#hardware)
[![ESP-IDF v6.0.2](https://img.shields.io/badge/ESP--IDF-v6.0.2-red.svg)](https://docs.espressif.com/projects/esp-idf/)

```
          ((  o  ))
       ((     |     ))
    ((        |        ))
             /|\
            / | \
      _____/__|__\_____

#   # #   # #     ##### #####
## ## #   # #       #     #
# # # #   # #       #     #
#   # #   # #       #     #
#   #  ###  #####   #   #####
   #   # #####  #### #   #
   ## ## #     #     #   #
   # # # ####   ###  #####
   #   # #         # #   #
   #   # ##### ####  #   #

   MeshCore  +  Meshtastic
```

Both protocol stacks are implemented from the wire up in application code on the
ESP32-P4, so the C6 radio coprocessor keeps its stock firmware and changing
network is a config change and one `lora_set_config()` call — no reboot, no
reflash.

---

## What it does

- **Two networks, one app.** MeshCore and Meshtastic, switched with **△**. Each
  keeps its own channels, nodes and message log.
- **Channels of every type both networks support** — MeshCore public, hashtag and
  private (base64 PSK); Meshtastic at every PSK length upstream accepts,
  including unencrypted and the one-byte key index.
- **Signed identity.** One Ed25519 key pair is the MeshCore identity. Adverts are
  signed going out and verified coming in, and the nodes list says which is which.
- **End-to-end direct messages on both networks**, with delivery acknowledgement
  that proves the other side *decrypted* the message rather than merely heard the
  frame.
- **MeshCore route learning** — flood until the contact returns a path, then send
  directly along it, with a retry ladder back to flooding when it stops working.
- **Off-grid repeat** *(MeshCore)* — optionally forward other nodes' packets so a
  handful of clients can extend each other's range with no infrastructure.
- **A persistent node list** with per-node detail, short names, route management
  and Meshtastic key exchange.
- **A Finnish keyboard layer** — å ö ä where a Finnish QWERTY puts them, on a
  keyboard whose caps say US.
- **Session recording** to a file for diagnosing what actually happened on the
  air, since the USB debug port is unavailable whenever the flashing port is.

## Status

Working and used on the air, against real MeshCore and Meshtastic nodes on both
sides of every feature listed above.

> [!WARNING]
> **No duty-cycle enforcement yet.** 869.4–869.65 MHz is limited to 10% duty
> cycle in the EU and nothing in this app enforces it. Treat this as lab and
> experimental use until it does.

Also not implemented: message history persistence, position and telemetry
reporting, and Meshtastic relaying — the app advertises `CLIENT_MUTE` by default
precisely because that is the truthful role while it does not relay.

## Hardware

[Tanmatsu](https://nicolaielectronics.nl/tanmatsu/) — ESP32-P4 application
processor, ESP32-C6 radio coprocessor over SDIO, SX1262 LoRa modem, 480×800 MIPI
DSI panel used rotated to 800×480, QWERTY keyboard via a CH32 coprocessor.

The C6 stays on stock `tanmatsu-radio` firmware, which exposes the SX1262 as a
dumb modem over RPC. Nothing here reflashes it.

## Build and install

Requires **ESP-IDF v6.0.2** and the BadgeLink tools (see [`tools/`](tools/)).

```powershell
tools\build.ps1                 # -> build/tanmatsu/application.bin
tools\deploy.ps1 -Start         # AppFS upload over USB, then launch
```

Put the device in **BadgeLink mode** first: press the violet ◇ key on the
launcher home screen — the top-right icon changes from a bug to a USB symbol. In
debug mode it enumerates as COM ports instead and `deploy.ps1` reports "Badge not
found". The two USB modes are mutually exclusive.

`assets/metadata.json` and `assets/icon32.png` are the launcher entry; regenerate
the icon with `tools/make-icon.py` if you change it.

## Using it

One screen per network, showing every configured channel of that network with a
colour-coded channel column. Switching network swaps the whole log; the channel
selector only retargets the composer. Typing always goes to the composer — there
is no edit mode.

| Key | |
|---|---|
| ✕ red | clear composer, or exit |
| △ orange | switch network |
| ■ yellow | toggle the radio metadata column |
| ◯ green | nodes — list, details, announce, remove |
| ☁ blue | identity |
| ◇ violet | channels — select, create, edit, delete |
| bottom side button | configuration |
| ←/→ | cursor · **↑/↓** scroll · **ctrl+↑/↓** recall sent messages |
| fn+↑ | page up · **fn+↓** jump to latest |
| alt+↑ | selection mode; **enter** opens message details |
| fn+■ | start/stop session recording |

Inside an overlay the same four coloured keys are relabelled on screen, so the
hint bar is always the authority on what they do.

Per-node actions from the detail view: **△** message, **◯** set a short name,
**■** remove, **☁** forget the route (MeshCore), **◇** exchange info (Meshtastic).

### Message status

An outgoing message occupies the timestamp column with its progress until it
settles. A broadcast shows repeats as `x2`; a direct message adds the attempt,
`x1a2` being one repeat heard on the second try. Neither network acknowledges a
broadcast, so the delivery signal is hearing a repeater flood it back: MeshCore
shows how many repeats were counted, Meshtastic the hop budget and last relay.
The window is 60 seconds, after which a message that was never repeated keeps its
timestamp in red.

### Finnish letters

The Tanmatsu keyboard is US QWERTY. On a Finnish one the three letters this app
most needs sit exactly where US punctuation is — Å right of P, Ö and Ä right of
L — so those keys are remapped and the punctuation moves to a modifier layer:

| Key | Alone | Shift | **Fn** or **Ctrl** | Fn/Ctrl + Shift |
|---|---|---|---|---|
| `[` | å | Å | `[` | `{` |
| `;` | ö | Ö | `;` | `:` |
| `'` | ä | Ä | `'` | `"` |

Not AltGr: the BSP substitutes its own third-level table before the event reaches
the app, so AltGr+`;` never arrives as a semicolon — it arrives as a combining
ogonek. Fn and Ctrl leave the character alone and only set a modifier bit. The
BSP's own AltGr+Q/W/P still produce ä å ö and are left working.

This is app-local. The keycaps and every other app still say US.

## Configuration

The **bottom side button** opens it. Shared settings apply to both networks; the
rest belong to whichever network was active when the screen was opened, and it
stays on that one while open.

| | |
|---|---|
| Location | latitude and longitude, optional. Once set it goes out in **every MeshCore advert** — published to everyone in range and everyone they relay to. Decimal degrees; both coordinates or neither. |
| Screen off | minutes of no input before the backlight goes dark, 0 = never. Backlight only: the panel and the radio keep running and messages keep arriving. The key that wakes it is swallowed. |
| Hop limit *(MT)* | 0–5. The value the active limit **resets to at every start**. |
| Role *(MT)* | CLIENT or CLIENT_MUTE, advertised in NodeInfo. Defaults to CLIENT_MUTE. |
| Off-grid repeat *(MC)* | forward other nodes' packets to extend the mesh. Off by default, because a mesh where everyone repeats everything spends its airtime on itself. |

**`fn` + `0`…`7`** sets the Meshtastic hop limit for the current session only. It
reaches 7 where the stored setting stops at 5, on the grounds that a limit worth
raising to get one message out is not one worth making permanent. Restarting
returns it to the stored value.

## Documentation

| | |
|---|---|
| [docs/architecture.md](docs/architecture.md) | component layout, threading, and the platform behaviour worth knowing before touching this code |
| [docs/protocol.md](docs/protocol.md) | radio settings, channels, identity, direct messages, routing, storage |
| [docs/second-radio-investigation.md](docs/second-radio-investigation.md) | what running both networks simultaneously would take, and why the antennas make it harder than it looks |

## Contributing

Issues and pull requests are welcome. Two things worth knowing first:

- `mm_proto` stays free of ESP-IDF, FreeRTOS and BSP includes, and `mm_net` never
  depends on `mm_ui`. Both rules exist because breaking them has already cost a
  rewrite once.
- Protocol behaviour is checked against upstream source before it is changed.
  Where this app deliberately differs from upstream — the off-grid repeat
  frequency lock, for one — the difference is documented rather than silent.

## Credits and licensing

MultiMesh is licensed under the [MIT License](LICENSE).

The MeshCore and Meshtastic support here is an **independent implementation from
protocol behaviour and published documentation**. No code is derived from the
Meshtastic firmware or any other GPL-licensed project; upstream repositories are
consulted as references only and are excluded from this tree.

Every cryptographic key that appears in this source is a published, well-known
network default. Nothing secret is committed here.

MeshCore and Meshtastic are the work and the trademarks of their respective
projects. This is an unofficial third-party client, not affiliated with, endorsed
by, or supported by either.

Built on [BadgeTeam](https://badge.team/)'s Tanmatsu BSP, PAX graphics and
BadgeLink tooling.
