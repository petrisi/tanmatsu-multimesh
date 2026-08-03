# MultiMesh

`fi.ps.multimesh`

A mesh communications client for the [Tanmatsu](https://nicolaielectronics.nl/tanmatsu/)
handheld that speaks **MeshCore and Meshtastic** from one app, switching between
them with a single keypress.

## Why this shape

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
operation. See [docs/second-radio-investigation.md](docs/second-radio-investigation.md)
for what a second radio would take, and why the antennas make it harder than it
looks.

## Layout

Components, with the dependency direction enforced by CMake `REQUIRES`:

| Component | Contents | Depends on |
|---|---|---|
| `mm_proto` | wire codecs for both networks — pure C, no ESP-IDF | — |
| `mm_crypto` | channel crypto (PSA), key expansion, channel hashes | `mm_proto` |
| `mm_domain` | the state model: channels, messages, identity | — |
| `mm_radio` | modem settings; later the TX queue, LBT and duty-cycle gate | `tanmatsu-lora` |
| `mm_net` | the per-network stacks behind `mesh_net_t` | `mm_proto` `mm_crypto` `mm_domain` `mm_radio` |
| `mm_ui` | framebuffer, message view, overlays, font, LEDs | `mm_domain` |
| `main` | `app_main()` and the event loop, nothing else | all of the above |

The direction that matters: **`mm_net` does not depend on `mm_ui`.** Stacks decode
into the domain model; the UI reads it. An earlier version had receive handlers
calling the UI directly, and that coupling is what forced them out of the build
the moment the UI changed shape.

`mm_proto` is deliberately free of ESP-IDF, FreeRTOS and BSP includes so its
codecs stay host-testable.

## Build and deploy

Requires ESP-IDF v6.0.2 and the BadgeLink tools (see `tools/`).

```powershell
K:\tanmatsu\tools\build.ps1                 # -> build/tanmatsu/application.bin
K:\tanmatsu\tools\deploy.ps1 -Start         # AppFS upload over USB, then launch
```

Put the device in **BadgeLink mode** before deploying: press the violet ◇ key on
the launcher home screen — the top-right icon changes from a bug to a USB symbol.
In debug mode it enumerates as COM ports instead and `deploy.ps1` reports
"Badge not found". The two USB modes are mutually exclusive, which is why this
app reports status on screen rather than over serial.

## Radio settings

**MeshCore** modem settings come from the shared `system` NVS namespace
(`lora.freq`, `lora.sf`, `lora.bandwidth`, `lora.codingrate`, `lora.power`,
`lora.preamble`, `lora.sync`, `lora.rxboost`), so the app follows whatever region
preset is configured on the device. Falls back to EU/UK narrow:

```
869.618 MHz  SF8  BW 62.5 kHz  CR 4/8  22 dBm  preamble 8  sync 0x12  rx_boost on
```

**Meshtastic** uses the Finnish EdgeFastLow (EFL) profile:

```
channel "EdgeFastLow", PSK "AQ==" (= {0x01}, the default key)
869.43125 MHz  SF8  BW 62.5 kHz  CR 4/8  sync 0x2B  preamble 16
channel hash 0x55
```

The frequency is derived, not configured: EU_868 spans 869.4–869.65 MHz, which at
62.5 kHz gives four slots, and `channel_num 1` lands at 869.4 + 62.5/2 kHz. EFL
and LongFast cannot hear each other.

`bandwidth` is a **nominal label, not a literal**: `62` selects 62.5 kHz. Passing
`63` falls through to the driver's 125 kHz default and you hear nothing.

## Interface

One screen per network, showing every configured channel of that network with a
colour-coded channel column. Switching network swaps the whole log; the channel
selector only retargets the composer.

| Key | |
|---|---|
| ✕ red | clear composer, or exit |
| △ orange | switch network |
| ■ yellow | toggle the radio metadata column |
| ☁ blue | identity |
| ◇ violet | channels — select, create, edit, delete |
| ←/→ | cursor · **↑/↓** scroll · **ctrl+↑/↓** recall sent messages |
| fn+↑ | page up · **fn+↓** jump to latest |
| alt+↑ | selection mode; **enter** opens message details |

Typing always goes to the composer — there is no edit mode.

## Notes

- ESP-IDF 6.0 ships mbedtls 4.x, where `mbedtls/aes.h` and `sha256.h` are
  private. Crypto here uses the PSA Crypto API. Upstream Tanmatsu MeshCore still
  uses the legacy API and pins ESP-IDF 5.5.1.
- The panel is **480×800 rotated 270°**. Lay out against
  `pax_buf_get_width/height()` *after* setting orientation, not against what the
  BSP reports — those are the native dimensions and will silently confine
  everything to the wrong two-thirds of the screen.
- `pax_draw_thick_line()` skips PAX's orientation transform and draws at
  transposed coordinates. Use `pax_draw_line()`.
- `wifi_connection_init_stack()` is what brings up the P4↔C6 SDIO RPC pipeline
  the LoRa component rides on. It is called without associating to any network.
- There is no RTC on the P4; the clock comes from the C6 coprocessor via
  `bsp_rtc_update_time()`. Without it, message timestamps render as 1970.
- Meshtastic AES-CTR cannot fail on a wrong key — it just yields garbage. The
  strict protobuf parse in `mt_data_parse()` is what rejects foreign traffic, so
  keep it strict.
- PAX ships exactly one monospace font and it is also the only one without
  Latin-1. `pax_font_mono_fi` bolts the six Finnish glyphs onto `sky_mono`'s own
  letterforms; regenerate with `tools/gen_fi_glyphs.py`.

## License

MIT.
