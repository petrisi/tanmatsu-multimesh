# Second radio: options investigated

**Status: parked.** No hardware ordered, no code written. This records what was
established so it does not need re-deriving.

## Why a second radio at all

The app runs both MeshCore and Meshtastic as P4-side stacks over one SX1262, so
switching networks is a config push (F2) rather than a C6 reflash. The one thing
that design cannot do is run both networks **at the same time** — there is a
single radio. A second radio is the only way to get simultaneous operation.

## The physical constraint that applies to every option

Both networks sit in the same band, 187 kHz apart:

| Network | Frequency |
|---|---|
| MeshCore EU/UK narrow | 869.618 MHz |
| Meshtastic EdgeFastLow | 869.43125 MHz |

Two 869 MHz antennas a few centimetres apart give almost no isolation — about
5 dB of free-space loss at 5 cm. So 22 dBm out of one radio arrives at the other
at roughly **+17 dBm, above the SX1262's +10 dBm absolute maximum RF input**.
That is a damage risk, not merely desense. At 50 cm it drops to about −3 dBm:
safe, but still enough to block reception while the other transmits.

Conclusions that hold regardless of which option is chosen:

- **Simultaneous receive on both networks: fine.**
- **Simultaneous TX on one while receiving on the other: not achievable** on a
  handheld at this frequency spacing. It would need ~40 dB isolation, i.e.
  metres of separation.
- Antennas must be physically separated. A companion board on a cable makes this
  easier than two antennas bolted to one device.
- Where both radios are under our control, TX blanking (standby the other radio
  during transmission) is the mitigation. Across independent firmwares there is
  no shared scheduler, so mutual desense during transmissions is expected.

## Option A — bare SX1262 wired to the CATT port

### CATT pinout (external add-on port)

2×7 2.54 mm socket, PMOD + SAO compatible. All 3.3 V outputs together: **max 1 A**.

```
1  +3.3V           8  IO4 (GPIO 5)
2  GND             9  IO5 (GPIO 2)
3  SDA (GPIO 12)  10  IO6 (GPIO 3)
4  SCL (GPIO 13)  11  GND
5  IO1 (GPIO 15)  12  GND
6  IO2 (GPIO 34)  13  P4 reset  <- leave alone
7  IO3 (GPIO 4)   14  +3.3V
```

Six general-purpose IO, plus GPIO 12/13 if the external I²C bus (shared with
QWIIC) is sacrificed.

**QWIIC cannot be used for this** — it is I²C only and cannot carry SPI.

### Pin budget

An SX1262 needs seven signals: SCK, MOSI, MISO, NSS, RESET, BUSY, DIO1. CATT
offers six general IO, so either:

- **Give up the external I²C bus** → 8 pins, everything fits with one spare; or
- **Keep QWIIC** → tie the module's NRESET high through 10 kΩ and rely on
  power-on reset, losing the ability to recover a wedged modem without a power
  cycle.

**Unverified:** whether GPIO 34 is a strapping pin on the ESP32-P4. If it is,
nothing that drives it at boot should be attached — assign it NRST (a module
input, so high-Z from the module side) rather than a module output.

### Module requirements

- **SX1262** — not LLCC68 (see rejected options).
- **TCXO is effectively mandatory.** At 62.5 kHz bandwidth a ±10 ppm crystal
  drifts ±8.7 kHz at 869 MHz — about 14% of the bandwidth, and two nodes at
  opposite extremes are 28% apart. LoRa tolerates roughly ±25% of bandwidth. A
  ±2 ppm TCXO gives ±1.7 kHz, under 3%.
- **RF switch driven by DIO2.** The driver unconditionally calls
  `sx126x_set_dio2_as_rf_switch_ctrl(true)` and `sx126x_set_dio3_as_txco_ctrl(1.8V)`,
  and auto-detects the TCXO.

| Candidate | Verdict |
|---|---|
| **Waveshare Core1262-868M** | Best fit. SX1262, TCXO, DIO2→RXEN internally, 2.54 mm castellated, IPEX + pad |
| Ebyte E22-900M22S | Same module as the onboard radio, but exposes TXEN/RXEN separately — needs an inverter or a spare GPIO plus a driver patch. Antenna is a solder pad |

Also needed: 2×7 2.54 mm header, female–female jumpers, 10 µF + 100 nF at the
module supply, IPEX→SMA pigtail (30 cm+), 868 MHz SMA antenna. ~€30–40.

Practical notes: keep jumper wires under ~10 cm and start SPI at 2 MHz.

### Firmware side

Already supported by the existing component — no new driver:

```c
lora_init_local(&handle2, 16, SPI2_HOST, /*nss*/15, /*reset*/34, /*dio1*/12, /*busy*/3);
```

It logs `LoRa chip initialized (... with TCXO)`, which confirms the module is
alive before any packet arrives.

## Option B — companion radio over serial (preferred)

Boards that carry both an MCU and an SX1262 do not wire to our SPI bus. They run
a complete official firmware and we drive them over serial. This outsources the
protocol stack instead of reimplementing it.

All of the following are first-class targets for **both** networks. Confirmed
against upstream MeshCore's `variants/`: `rak4631`, `xiao_nrf52`, `xiao_s3`,
`xiao_s3_wio`, `xiao_c6`. Meshtastic supports the same set.

### Hardware on hand

| Kit | Notes |
|---|---|
| **RAK19003 + RAK4631** | Preferred. nRF52840 + SX1262, ships pre-flashed with Meshtastic, includes LoRa + BLE antennas, 2.54 mm UART header, low idle power |
| Seeed XIAO nRF52840 + Wio-SX1262 | Works; UART via castellated pads |
| Seeed XIAO ESP32 + Wio-SX1262 | Works; higher idle power than nRF52 |

### RAK19003 headers

Datasheet labels differ from some board revisions' silkscreen — `TX1`/`RX1`
(nRF52840 UART1) is what the datasheet calls the pins silkscreened `TX0`/`RX0`.
Same signals.

```
J6:  VDD (3.3V) | GND | SCL (I2C1) | SDA (I2C1)
J7:  RX1/RX0    | TX1/TX0 | GND | BOOT
```

### Wiring to CATT

| CATT pin | P4 GPIO | RAK |
|---|---|---|
| 1 | +3.3 V | VDD — only if not powered over USB-C |
| 2 | GND | GND |
| 5 | 15 | TX (P4 receives) |
| 7 | 4 | RX (P4 transmits) |

Both sides 3.3 V, no level shifting. Do not supply VDD from CATT while USB-C is
also connected. RAK4631 draws ~110 mA peak on TX, well inside the 1 A budget.

### Protocols

Both are framed binary over the same UART.

**Meshtastic StreamAPI** — `0x94 0xC3 <len_hi> <len_lo>` then a protobuf
`ToRadio`/`FromRadio` (verified in `src/mesh/StreamAPI.cpp`). Needs nanopb,
which is already vendored inside the badgelink component; `meshtastic-protobufs`
is already in `reference/`.

**Important:** Meshtastic does **not** expose the client API on the UART pins by
default — `SerialConsole` puts the StreamAPI on USB CDC. To get it on the header:

```
meshtastic --set serial.enabled true
meshtastic --set serial.mode PROTO
meshtastic --set serial.baud BAUD_115200
```

Leaving `serial.rxd`/`serial.txd` at 0 makes the nRF52 path use the variant's
default `Serial1` pins, which should be the header pins — *unverified, confirm
with a loopback*. In PROTO mode the module emits a "rebooted" sequence at
startup, usable as link detection.

**MeshCore companion serial** — `'<'` start byte, length, then a command byte
from the enum in `mc_companion.h`. No protobuf; simpler. A full reference
implementation (server side) exists in
`reference/meshcore-tanmatsu/components/mc_proto/companion-radio-protocol/`.

### Transport trade-off

| | UART header | RAK USB-C into USB-A host port |
|---|---|---|
| Wiring | 4 jumper wires | supplied cable |
| Device config | must enable Serial module in PROTO mode | none, client API is native on USB CDC |
| Tanmatsu code | plain `uart_driver_install` | `usb_host_cdc_acm` component |
| Well-trodden | less so | how every phone/PC client connects |

Start with UART; the protocol layer above is identical either way.

### Suggested split

Run **Meshtastic on the RAK4631** (as shipped) and keep MeshCore native on the
internal radio. Meshtastic's full stack — PKI DMs, ACKs, node database, config
negotiation — is the more expensive one to reimplement, so outsourcing it buys
the most, and the RAK needs no reflashing for that role. F2 then becomes a view
toggle rather than a radio reconfiguration.

## Rejected

**MeshTadpole / Meshstick / MeshToad / PiggyStick USB sticks.** These are not
smart USB devices: no MCU, just a **CH341 USB-to-SPI bridge** (VID `0x1A86`,
PID `0x5512`) wired to an SX1262, with the host doing everything — which is why
they need `meshtasticd`. Confirmed from meshtasticd's own configs
(`bin/config.d/lora-meshstick-1262.yaml` and siblings). Using one from the P4
would need a CH341 USB-host driver written from scratch, a re-targeted SX1262
driver (the existing one is hard-wired to ESP-IDF SPI), and DIO1 polled over USB
instead of a real interrupt line. Days of work for a worse transport than either
option above.

*Note: MeshTadpole's own yaml is not in our (shallow, older) firmware clone —
CH341 is confirmed for its five siblings and inferred for the Tadpole. Plugging
it in and checking for `1A86:5512` would settle it.*

**Ai-Thinker Ra-01SCH-P.** It is an **LLCC68**, not an SX1262. The LLCC68
supports only 125 / 250 / 500 kHz bandwidths — it cannot do the 62.5 kHz both
our networks use. Not a driver limitation; the part cannot tune it. Secondary
problems: it is the PA variant drawing 750 mA at 3.3 V (the CATT port's entire
budget is 1 A shared), and the Ra-01S family has no TCXO.

**Ra-01SH** (the SX1262 sibling) would be capable on bandwidth but lacks a TCXO,
making it marginal at 62.5 kHz.

## Sources

- [Tanmatsu external add-on port](https://docs.tanmatsu.cloud/hardware/connectors/external-add-on-port/)
- [Tanmatsu internal add-on port](https://docs.tanmatsu.cloud/hardware/connectors/internal-add-on-port/) — 36-pin, 9 unrestricted GPIO, but requires a modified back cover
- [LLCC68 datasheet](https://www.mouser.com/pdfDocs/DS_LLCC68_V10-2.pdf)
- [Ra-01SCH-P](https://docs.ai-thinker.com/en/Ra-01SCH-P/)
- [Core1262-868M](https://www.waveshare.com/wiki/Core1262-868M)
- [RAK19003 datasheet](https://docs.rakwireless.com/product-categories/wisblock/rak19003/datasheet/)
- [MeshTadpole](https://www.elecrow.com/meshtadpole-sx1262-usb-stick.html) / [MESHSTICK](https://github.com/markbirss/MESHSTICK)
