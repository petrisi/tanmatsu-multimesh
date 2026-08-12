# Architecture

Why the app is shaped the way it is, how the components fit together, and the
platform constraints worth knowing before changing any of it.

## Why both stacks run on the P4

The SX1262 is wired to the ESP32-C6, not to the ESP32-P4 that runs apps. The
stock C6 firmware (`tanmatsu-radio`, an esp-hosted-mcu fork) exposes the radio as
a *dumb modem* over an RPC channel, so the entire protocol stack — framing,
crypto, routing — runs in application code on the P4.

That matters, because upstream the two networks are integrated very differently:
MeshCore runs as a P4 app over the raw LoRa RPC, while Meshtastic replaces the C6
firmware outright, costing the P4 its WiFi and BLE and making a switch a reflash.

Implementing every network as a P4-side stack behind one `mesh_net_t` interface
keeps the C6 on stock firmware and reduces "switch network" to a config change
plus one `lora_set_config()` call — no reboot, no reflash.

One radio means one network at a time: fast switching, not simultaneous
operation. See [second-radio-investigation.md](second-radio-investigation.md)
for what a second radio would take, and why the antennas make it harder than it
looks.

## Components

Dependency direction is enforced by CMake `REQUIRES`:

| Component | Contents | Depends on |
|---|---|---|
| `mm_proto` | wire codecs for both networks — pure C, no ESP-IDF | — |
| `mm_crypto` | channel and direct-message crypto (PSA), key expansion, channel hashes | `mm_proto` |
| `mm_domain` | the state model: channels, messages, identity, nodes, settings, persistence | — |
| `mm_radio` | modem settings and the region/profile presets | `tanmatsu-lora` |
| `vendor` | third-party and standalone algorithms — currently Ed25519 | `mbedtls` |
| `mm_net` | the per-network stacks behind `mesh_net_t` | `mm_proto` `mm_crypto` `mm_domain` `mm_radio` `vendor` |
| `mm_ui` | framebuffer, message view, overlays, font, LEDs | `mm_domain` |
| `mm_log` | the optional session recorder | — |
| `main` | `app_main()` and the event loop, nothing else | all of the above |

The direction that matters: **`mm_net` does not depend on `mm_ui`.** Stacks decode
into the domain model and the UI reads it, so a change to the display cannot
reach the protocol code.

`mm_proto` is deliberately free of ESP-IDF, FreeRTOS and BSP includes so its
codecs stay testable away from the hardware.

## Threading

The event loop in `main` owns the model and is the only thing that mutates it.
Anything that blocks runs on its own task and reports back through a queue:

- `mm_tx` — transmitting, which blocks for the full airtime of a packet.
- `mm_verify` — Ed25519 and X25519, about a second per signature on this part.

Neither touches the model. Results are drained by the loop and applied there.

## Session logging

`fn` + **■** starts and stops a recording to `/locfd/multimesh/session.log`:
raw frames in and out, decode decisions, and path and acknowledgement handling.
It is meant to be short-lived — record a session, leave the app, put the device
in BadgeLink mode, and pull the file with `tools/fetch-log.ps1`. The receive path
is logged in enough detail to diagnose a delivery failure from the file alone.

## Platform notes

Constraints of this hardware and toolchain, each of which shapes code above it:

- ESP-IDF 6.0 ships mbedtls 4.x, where `mbedtls/aes.h` and `sha256.h` are
  private. Crypto here uses the PSA Crypto API. Upstream Tanmatsu MeshCore still
  uses the legacy API and pins ESP-IDF 5.5.1.
- ESP-IDF 6.0's mbedtls declares `PSA_ALG_PURE_EDDSA` but ships no implementation
  of it, so `components/vendor/ed25519.c` implements RFC 8032 over `mbedtls_mpi`.
  It is not fast. It is checked against the RFC 8032 §7.1 vectors at every boot,
  including that a flipped bit fails, before anything is allowed to depend on it.
- The panel is **480×800 rotated 270°**. Lay out against
  `pax_buf_get_width/height()` *after* setting orientation, not against what the
  BSP reports — those are the native dimensions and will silently confine
  everything to the wrong two-thirds of the screen.
- `pax_draw_thick_line()` skips PAX's orientation transform and draws at
  transposed coordinates. Use `pax_draw_line()`.
- PAX ships exactly one monospace font and it is also the only one without
  Latin-1. `pax_font_mono_fi` bolts the six Finnish glyphs onto `sky_mono`'s own
  letterforms; regenerate with `tools/gen_fi_glyphs.py`. There are no
  box-drawing or block glyphs at all, which is why the boot art is built from
  brackets and slashes.
- `wifi_connection_init_stack()` is what brings up the P4↔C6 SDIO RPC pipeline
  the LoRa component rides on. It is called without associating to any network.
- There is no RTC on the P4; the clock comes from the C6 coprocessor via
  `bsp_rtc_update_time()`. Without it, message timestamps render as 1970.
- The two USB modes are mutually exclusive — BadgeLink *or* serial debug, never
  both — which is why this app reports status on screen and to a log file rather
  than over serial.
- Packet RSSI from `tanmatsu-lora` is always reported as 0; the driver's own
  conversion contradicts its documented scale. SNR is shown instead, and all
  three raw status bytes go to the session log.
- **Never read a backlight level back after setting one.** Both the display and
  keyboard APIs take a percentage and convert to the coprocessor's 0–255 scale
  with `(pct * 255) / 100`, converting back with `(raw * 100) / 255` — both
  truncating, so a round trip loses about a point each time. Code that reads the
  level before dimming, to restore it afterwards, walks a screen from 10% to 2%
  in eight blank-and-wake cycles and looks exactly like a crash. Both levels here
  are read once at startup and never again.
