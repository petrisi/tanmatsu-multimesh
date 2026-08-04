# MultiMesh

`fi.ps.multimesh`

A mesh communications client for the [Tanmatsu](https://nicolaielectronics.nl/tanmatsu/)
handheld that speaks **MeshCore and Meshtastic** from one app, switching between
them with a single keypress.

Send and receive on broadcast channels of either network, manage channels of
every type both networks support, and keep a persistent list of the nodes you
have heard — with MeshCore adverts signed on the way out and verified on the way
in.

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
| `mm_domain` | the state model: channels, messages, identity, nodes, persistence | — |
| `mm_radio` | modem settings; later the TX queue, LBT and duty-cycle gate | `tanmatsu-lora` |
| `vendor` | third-party and standalone algorithms — currently Ed25519 | `mbedtls` |
| `mm_net` | the per-network stacks behind `mesh_net_t` | `mm_proto` `mm_crypto` `mm_domain` `mm_radio` `vendor` |
| `mm_ui` | framebuffer, message view, overlays, font, LEDs | `mm_domain` |
| `main` | `app_main()` and the event loop, nothing else | all of the above |

The direction that matters: **`mm_net` does not depend on `mm_ui`.** Stacks decode
into the domain model; the UI reads it. An earlier version had receive handlers
calling the UI directly, and that coupling is what forced them out of the build
the moment the UI changed shape.

`mm_proto` is deliberately free of ESP-IDF, FreeRTOS and BSP includes so its
codecs stay host-testable.

The event loop in `main` owns the model and is the only thing that mutates it.
Anything that blocks runs on its own task and reports back through a queue:
`mm_tx` (transmitting, which blocks for the full airtime of a packet) and
`mm_verify` (Ed25519, about a second per signature). Neither touches the model.

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

## Channels

A fresh device starts on the well-known public channel of each network, so it is
on the air without being configured. Deleting them keeps them deleted.

**MeshCore** derives the key three ways, chosen by what is typed:

Tested in this order, first match wins:

| Condition | Result |
|---|---|
| secret set: base64, 16 or 32 bytes | a private channel — any other length is rejected |
| secret empty, name starts with `#` | a hashtag channel: key = `SHA256(name)[0:16]`, over the name *including* the `#`, so anyone who knows the name can join — that is the point |
| secret empty | the well-known public channel |

The PSK is **base64, not hex** — that is the format MeshCore clients exchange.
The channel hash is `SHA256(key)[0]`.

**Meshtastic** accepts every PSK length upstream does, and each length means
something different:

| Decoded length | Meaning |
|---|---|
| 0 | unencrypted — a valid configuration, not an error |
| 1 | a key *index*, not a key: `AQ==` is `{0x01}` = the default public key, `0x00` disables encryption, and each higher index bumps the default key's last byte |
| 2–16 | the key, zero-padded to 16 bytes (AES-128) |
| 17–32 | the key, zero-padded to 32 bytes (AES-256) |

The channel hash mixes the **name as well as the key**, so renaming a Meshtastic
channel changes which traffic it matches. That is upstream behaviour.

## Identity and signing

One name and one 4-character short name serve both networks, and **transmit is
refused until a name is set** — MeshCore carries the sender name inside the
message text and has no other identity field, so an unnamed message is not merely
unfriendly, it is unattributable.

MeshCore has no separate node id: the Ed25519 public key *is* the identity. A
32-byte seed is generated on first run and the key pair derived from it at every
boot, so there is one thing to keep secret rather than three. The seed is
generated only *after* the RFC 8032 self-test passes — the identity is permanent,
and minting one from arithmetic that had not been proved would cost every contact
ever made with it.

Adverts are signed on transmit and verified on receive. Verification is cached
per node and runs on the `mm_verify` task, because one check is two scalar
multiplications and takes about a second here. The nodes list marks a verified
node `*` and a forged one `!`.

Meshtastic NodeInfo is unsigned — any node may claim any name — so nothing is
reported for it rather than implying a check that could have been made.

The Meshtastic node number is the low four bytes of the factory MAC, rendered as
`!aabbccdd`. It never changes.

## Nodes

An entry is created for **any** packet heard, and enriched when a NodeInfo or a
named advert arrives. Stored fields are what each network actually offers:
Meshtastic gets node number, short and long name, hardware model and the
Curve25519 DM key; MeshCore gets the public key, role and name.

Announcing is automatic every 24 hours and manual from the nodes view (△), rate
limited to once per 5 minutes — this is a duty-cycle limited band.

Entries expire on last-heard: **7 days** for a node that never told us its name,
**30 days** for one that did. An unnamed entry is little more than evidence that
something transmitted once; a named one is a contact.

## Storage

| What | Where | Why |
|---|---|---|
| channels, identity, preferences | NVS namespace `multimesh` | small, versioned, rewritten rarely |
| identity seed | NVS key `mc.seed` | 32 bytes; the key pair is derived, never stored |
| node tables | `/locfd/multimesh/nodes-{mc,mt}.bin` | two 48-entry tables would take about a third of the 16 KB NVS partition, which is shared with the launcher and WiFi |

Records carry a version and an **unrecognised version is discarded, not
reinterpreted**: losing settings is recoverable, silently misreading a channel key
is not. Node files are written to a temporary file and renamed, so a power cut
leaves the previous table intact.

MeshCore radio settings are *read* from the shared `system` namespace and never
written there — two apps fighting over one key set is how configuration
mysteriously changes.

> **Channel keys and the identity seed are stored in the clear.** That is what
> every mesh client does: the device is the trust boundary, NVS is not encrypted,
> and the Tanmatsu ships with secure boot permanently disabled. Anyone holding
> the hardware has the keys. Nothing secret is committed to this repository —
> every key that appears in the source is a published, well-known network default.

## Interface

One screen per network, showing every configured channel of that network with a
colour-coded channel column. Switching network swaps the whole log; the channel
selector only retargets the composer.

| Key | |
|---|---|
| ✕ red | clear composer, or exit |
| △ orange | switch network |
| ■ yellow | toggle the radio metadata column |
| ◯ green | nodes — list, details, announce, remove |
| ☁ blue | identity |
| ◇ violet | channels — select, create, edit, delete |
| ←/→ | cursor · **↑/↓** scroll · **ctrl+↑/↓** recall sent messages |
| fn+↑ | page up · **fn+↓** jump to latest |
| alt+↑ | selection mode; **enter** opens message details |

Inside an overlay the same four coloured keys are relabelled on screen, so the
hint bar is always the authority on what they do.

Typing always goes to the composer — there is no edit mode.

An outgoing message occupies the timestamp column with its progress until it
settles. Neither network acknowledges a broadcast, so the delivery signal is
hearing a repeater flood it back: MeshCore shows how many repeats were counted,
Meshtastic the hop budget and last relay. The window is 60 seconds, after which a
message that was never repeated keeps its timestamp in red.

Received timestamps are **our own receive clock on both networks**. It is the
only clock we control, so it is the only one that gives a stable ordering and
cannot file a message under the wrong week because a sender's clock is adrift.
MeshCore does carry the sender's claim, and that is kept and shown in the message
detail — as evidence, not as the ordering key.

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
- ESP-IDF 6.0's mbedtls declares `PSA_ALG_PURE_EDDSA` but ships no implementation
  of it, so `components/vendor/ed25519.c` implements RFC 8032 over `mbedtls_mpi`.
  It is not fast. It is checked against the RFC 8032 §7.1 vectors at every boot,
  including that a flipped bit fails, before anything is allowed to depend on it.
- A MeshCore advert signature covers `pub_key || timestamp || app_data`, and
  receivers clamp `app_data` to 32 bytes *before* verifying. The signed region is
  taken from the raw payload rather than re-serialised from the parsed struct: a
  round trip would drop any field this parser does not understand, and the
  signature would then fail against senders that include one — which looks like
  broken crypto rather than a lossy parse.
- Both networks flood, so every message arrives once directly and again from each
  repeater. Dedup keys differ: Meshtastic has an `(from, id)` header pair,
  MeshCore has no packet id but an unchanged payload, so that is fingerprinted.

## Not implemented yet

Direct messages, message history persistence, position, telemetry, and a
duty-cycle gate. **The last one matters:** 869.4–869.65 MHz is limited to 10%
duty cycle and nothing here enforces it, so this is lab use until it does.

## License

MIT.
