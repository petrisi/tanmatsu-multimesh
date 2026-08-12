# MultiMesh

**Mesh communications for the [Tanmatsu](https://nicolaielectronics.nl/tanmatsu/)
handheld — MeshCore and Meshtastic in one app, switched with a single keypress.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: ESP32-P4](https://img.shields.io/badge/platform-ESP32--P4-informational.svg)](#hardware)
[![ESP-IDF v6.0.2](https://img.shields.io/badge/ESP--IDF-v6.0.2-red.svg)](https://docs.espressif.com/projects/esp-idf/)

<img width="1252" height="1663" alt="image" src="https://github.com/user-attachments/assets/7438b4d9-3da7-41f9-be3d-02092a70fa37" />

Both protocol stacks are implemented from the wire up in application code, so the
radio coprocessor keeps its stock firmware and switching between the two networks
takes neither a reboot nor a reflash.

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
- **Relaying on both networks** *(experimental)* — MeshCore off-grid repeat, and
  a Meshtastic CLIENT that forwards for others. Meant for temporarily holding a
  group together from a high spot, not for standing in for a permanent node.
- **Switchable Meshtastic radio profiles** — EdgeFastLow EU, LongFast EU, or
  custom frequency, spreading factor, bandwidth and coding rate, retuned without
  a restart.
- **A persistent node list** with per-node detail, short names, route management
  and Meshtastic key exchange.
- **A Finnish keyboard layer** — å ö ä where a Finnish QWERTY puts them, on a
  keyboard whose caps say US.
- **Session recording** to a file, so a problem on the air can be diagnosed
  afterwards rather than described from memory.

## Status

Working and used on the air, against real MeshCore and Meshtastic nodes on both
sides of every feature listed above.

> [!WARNING]
> **No duty-cycle enforcement yet.** 869.4–869.65 MHz is limited to 10% duty
> cycle in the EU and nothing in this app enforces it. Treat this as lab and
> experimental use until it does.

Also not implemented: message history persistence, position and telemetry
reporting, replies and reactions, and traceroute. See
[docs/roadmap.md](docs/roadmap.md) for the full list and the reasoning behind
each.

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

The scripts hardcode no paths. Everything they need is in
[`tools/config.ps1`](tools/config.ps1): locations inside the repository are
derived from where that file sits, so the checkout works on any drive, and the
few things outside it — where ESP-IDF is installed, chiefly — have defaults you
can override with an environment variable rather than by editing anything. See
[`tools/README.md`](tools/README.md) for the list.

`assets/metadata.json` and the `assets/icon*.png` files are the launcher entry;
regenerate the icons with `tools/make-icon.py` if you change them. The version
and revision live in that metadata, and `deploy.ps1` reads them from there.

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
**←/→** move to the previous or next node without going back to the list, and
**↑/↓** jump to the first or last. The title shows your position.

The list is ordered by when each node was last heard, so it reorders as traffic
arrives. The detail view stays on the node you opened.

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

Fn and Ctrl are the modifier keys rather than AltGr, which the firmware claims
for its own accent layer. AltGr+Q/W/P still produce ä å ö as they do elsewhere on
the device.

The remapping applies only inside MultiMesh. The keycaps and every other app
still say US.

## Configuration

The **bottom side button** opens it. Shared settings apply to both networks; the
rest belong to whichever network was active when the screen was opened, and it
stays on that one while open.

### Shared

| | |
|---|---|
| Location | latitude and longitude, optional. Once set it goes out in **every MeshCore advert** — published to everyone in range and everyone they relay to. Decimal degrees; both coordinates or neither. |
| Brightness | screen backlight, 10–100% in tens. Applied as you step it, and also what waking from dark restores. |
| Screen off | minutes of no input before the backlight goes dark, 0 = never. Backlight only: the panel and the radio keep running and messages keep arriving. The key that wakes it is swallowed. |
| Key light | keyboard backlight, on the same scale. |
| Keyboard off | as Screen off, but for the keys, and one minute by default. Lit keys are worth having for the moment you are typing; a screen is worth reading long after you stopped. |

Both brightness settings are **spaced by eye rather than by duty cycle**. Eyes
respond to ratios, so ten evenly spaced duty steps give one usable step and nine
that do nothing — 20% to 10% halves the light while 100% to 90% changes it by a
ninth. The levels follow a geometric ramp instead, about 1.4× per step, and the
duty actually applied is shown under the row.

On first run both adopt whatever the device is already set to, so an update never
brightens a screen somebody deliberately dimmed.

### Meshtastic

| | |
|---|---|
| Radio | **EdgeFastLow EU**, **LongFast EU** or **Custom**. The note under the row spells out what goes to the modem — `869.525 MHz SF11 BW250 CR4:5 slot 1/1` — and that same string goes to the logs, so what is on screen is what went to the air. |
| Frequency, Spreading, Bandwidth, Coding rate | Custom only. Frequency is typed in MHz; bandwidth steps a fixed set, because the driver takes a nominal label and an unrecognised one falls back to 125 kHz and hears nothing. |
| Power | 2–22 dBm. Separate from the profile: it is a regulatory and battery decision, and applies whichever profile is chosen. The band permits 27 but the module stops at 22. |
| Hop limit | 0–5. The value the active limit **resets to at every start**. |
| Role | CLIENT or CLIENT_MUTE, advertised in NodeInfo. CLIENT forwards other nodes' packets; CLIENT_MUTE listens only, and is the default. |
| Always repeat *(CLIENT only)* | repeat even when another node already has, transmitting last so you only add coverage nobody else provided. |
| Optimize text *(CLIENT only)* | carry only text messages and acknowledgements, and let them keep their hop limit. Traffic that cannot be decrypted is carried normally, since its type is unreadable. |

Changing the profile retunes immediately — no network switch needed. **Channels
are not touched.** The two are deliberately unconnected, so reaching the LongFast
mesh also means adding a channel named `LongFast` with the default PSK; switching
the radio alone gives you carrier and nothing readable.

### MeshCore

| | |
|---|---|
| Off-grid repeat | forward other nodes' packets to extend the mesh. Off by default, because a mesh where everyone repeats everything spends its airtime on itself. |

MeshCore's modem settings are not editable here. They come from the shared
`system` storage the stock MeshCore client writes, so this app follows whatever
region preset is configured on the device — and never writes there, so it cannot
change what other apps rely on.

The two Meshtastic relay settings are shown only at role CLIENT, since at
CLIENT_MUTE they would be controls for something that cannot happen. They are
not mutually exclusive, and with both off a CLIENT behaves like any other
Meshtastic client. `RPT <n>` on the status bar counts what has actually been
forwarded, per network.

> [!IMPORTANT]
> **Both relay features are experimental, and they are aimed at one situation:**
> you and your Tanmatsu are somewhere with a view — an observation tower, a hill,
> a rooftop — and you want to hold a handful of nodes together for the afternoon.
> Switch them on when you arrive and off when you leave.
>
> Off-grid repeat is for being **outside the MeshCore infrastructure entirely**,
> where there is no deployed repeater to defer to. Meshtastic relaying is for the
> same kind of gap on the other network.
>
> Neither is a substitute for a permanent node. A handheld on battery is a poor
> repeater, and a mesh where every client repeats everything spends its airtime
> on itself. If there is already a repeater covering you, leave both off.

**`fn` + `0`…`7`** sets the Meshtastic hop limit for the current session only,
reaching 7 where the stored setting stops at 5. Restarting returns it to the
stored value.

## Documentation

| | |
|---|---|
| [docs/architecture.md](docs/architecture.md) | component layout, threading, and the platform behaviour worth knowing before touching this code |
| [docs/protocol.md](docs/protocol.md) | radio settings, channels, identity, direct messages, routing, storage |
| [docs/roadmap.md](docs/roadmap.md) | what is not built yet, and why it matters |
| [docs/second-radio-investigation.md](docs/second-radio-investigation.md) | what running both networks simultaneously would take, and why the antennas make it harder than it looks |

## Contributing

Issues and pull requests are welcome. Two things worth knowing first:

- `mm_proto` stays free of ESP-IDF, FreeRTOS and BSP includes, so its codecs can
  be tested away from the hardware, and `mm_net` never depends on `mm_ui`, so the
  protocol stacks stay independent of the display.
- Protocol behaviour is checked against upstream source before it is changed.
  Where this app deliberately differs from upstream — the off-grid repeat
  frequency lock, for one — the difference is documented rather than silent.

## Credits and licensing

MultiMesh is licensed under the [MIT License](LICENSE). The compiled firmware
links third-party components under MIT, Apache-2.0 and BSD-3-Clause; their
required notices are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). No
copyleft code is linked or distributed.

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
