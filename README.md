# MeshComms for Tanmatsu

A mesh communications app for the [Tanmatsu](https://nicolaielectronics.nl/tanmatsu/)
handheld, intended to speak **both MeshCore and Meshtastic** with fast switching
between them.

This is currently a receive-only proof of concept. It listens on either network
and shows decoded messages on screen; **F2** switches between them, **F1**
returns to the launcher. Both paths are verified working on air.

## Why this shape

The SX1262 is wired to the ESP32-C6, not to the ESP32-P4 that runs apps. The
stock C6 firmware (`tanmatsu-radio`, an esp-hosted-mcu fork) exposes the radio as
a *dumb modem* over an RPC channel, so the entire protocol stack — framing,
crypto, routing — runs in application code on the P4.

That matters for the end goal. The two networks are integrated very differently
upstream:

- **MeshCore** runs as a P4 app over the raw LoRa RPC.
- **Meshtastic** replaces the C6 firmware outright, which costs the P4 its
  WiFi and BLE and makes switching a reflash.

Implementing both as P4-side stacks over the same `lora_*` API keeps the C6 on
stock firmware and reduces "switch network" to a config change plus one
`lora_set_config()` call — no reboot, no reflash.

One radio means one network at a time. Fast switching, not simultaneous
operation.

## Layout

| Path | Purpose |
|---|---|
| `main/main.c` | Boot sequence, RX loop, network switching |
| `main/mesh_net.h` | The stack interface both networks implement |
| `main/meshcore_wire.[ch]` | MeshCore packet + GRP_TXT framing (parse only) |
| `main/meshcore_crypto.[ch]` | Public-channel HMAC verify + AES-128-ECB decrypt |
| `main/meshcore_net.c` | MeshCore adapter |
| `main/meshtastic_wire.[ch]` | Meshtastic header + minimal protobuf `Data` reader |
| `main/meshtastic_crypto.[ch]` | PSK expansion, channel hash, AES-CTR |
| `main/meshtastic_net.c` | Meshtastic EFL adapter |
| `main/radio_cfg.[ch]` | LoRa modem settings, read from shared NVS |
| `main/ui.[ch]` | PAX-based screen output |
| `tools/` | Build, deploy and ESP-IDF activation scripts |
| `docs/` | Investigations and reference notes |
| `reference/` | Upstream repos, read-only, git-ignored |

See [docs/second-radio-investigation.md](docs/second-radio-investigation.md) for
the parked investigation into running both networks simultaneously via a second
radio.

## Build and deploy

Requires ESP-IDF v6.0.2 and the BadgeLink tools (see `tools/`).

```powershell
K:\tanmatsu\tools\build.ps1                 # -> build/tanmatsu/application.bin
K:\tanmatsu\tools\deploy.ps1 -Start         # AppFS upload over USB, then launch
```

Put the device in **BadgeLink mode** before deploying: press the purple diamond
key (2nd from top-right) on the launcher home screen — the top-right icon
changes from a bug to a USB symbol. In debug mode it enumerates as COM ports
instead and `deploy.ps1` will report "Badge not found".

The two USB modes are mutually exclusive, which is why this app reports status
on screen rather than over serial.

## Radio settings

**MeshCore** settings are read from the shared `system` NVS namespace
(`lora.freq`, `lora.sf`, `lora.bandwidth`, `lora.codingrate`, `lora.power`,
`lora.preamble`, `lora.sync`, `lora.rxboost`) so the app follows whatever region
preset is configured on the device. Falls back to EU/UK narrow:

```
869.618 MHz  SF8  BW 62.5 kHz  CR 4/8  22 dBm  preamble 8  sync 0x12  rx_boost on
```

**Meshtastic** is hardcoded to the Finnish EdgeFastLow (EFL) profile, a custom
modem preset used where 868 MHz interference makes LongFast unreliable:

```
channel "EdgeFastLow", PSK "AQ==" (= {0x01}, the default key)
869.43125 MHz  SF8  BW 62.5 kHz  CR 4/8  sync 0x2B  preamble 16
channel hash 0x55
```

The frequency is derived, not configured: EU_868 spans 869.4–869.65 MHz, which
at 62.5 kHz gives 4 slots, and `channel_num 1` lands at
869.4 + 62.5/2 kHz = 869.43125. EFL and LongFast cannot hear each other.

`bandwidth` is a **nominal label, not a literal**: `62` selects 62.5 kHz.
Passing `63` falls through to the driver's 125 kHz default and you hear nothing.

## Notes

- ESP-IDF 6.0 ships mbedtls 4.x, where `mbedtls/aes.h` and `sha256.h` are
  private. Crypto here uses the PSA Crypto API. Upstream Tanmatsu MeshCore still
  uses the legacy API and pins ESP-IDF 5.5.1.
- `wifi_connection_init_stack()` is what brings up the P4↔C6 SDIO RPC pipeline
  the LoRa component rides on. It is called without associating to any network.
- There is no RTC on the P4; the clock comes from the C6 coprocessor via
  `bsp_rtc_update_time()`. Without it, message timestamps render as 1970.
- Only one app can own the radio, so this contends with any other mesh client
  installed on the device. Run one at a time.
- Meshtastic AES-CTR cannot fail on a wrong key — it just yields garbage. The
  strict protobuf parse in `mt_data_parse()` is what rejects foreign traffic,
  so keep it strict.

## License

MIT.
