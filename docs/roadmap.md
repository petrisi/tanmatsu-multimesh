# Roadmap

What is not built yet, and why it matters. Ordered roughly by how much it holds
the project back rather than by how interesting it is.

Nothing here is a promise of a date. Items move when the reason they were
deferred stops applying.

## Compliance

**Duty-cycle setting and enforcement.** 869.4–869.65 MHz is limited to 10% duty
cycle in the EU and nothing in this app enforces it. That needs a configurable
limit, airtime accounting per band, and a gate on the transmit path that defers
or refuses a send that would breach it.

This is the single item that makes the README describe the app as lab and
experimental use. Everything else on this list is a feature; this one is the
difference between a toy and something that can be left running.

## Configuration and mesh profiles

Two steps, in order.

**Editable radio settings.** Frequency, spreading factor, bandwidth, coding
rate, power, preamble length and sync word, editable per network. Today MeshCore
reads the shared `system` NVS namespace so it follows whatever region preset the
device is configured for, and the Meshtastic EdgeFastLow profile is compiled in.
Neither can be changed from the app.

**User-definable mesh profiles.** Named sets of those settings that can be
added, edited and switched between, with LongFast shipping as a preset alongside
EdgeFastLow and MeshCore. Still one radio and one profile at a time — switching
stays a configuration change plus a single `lora_set_config()` call, exactly as
the network switch works today.

The interface consequence is worth stating up front: **△** is a two-way toggle
because there are exactly two networks. With user-defined profiles it becomes a
picker.

## Messaging

**Message history persistence.** Messages live in RAM and are gone at restart.
Channels, identity, settings and the node tables all persist; this does not.
The node tables already live on `/locfd` rather than NVS for size reasons, and
message history would go the same way.

**Replies and reactions** *(Meshtastic)*. The `Data` submessage carries
`reply_id` (field 7, `fixed32`) naming the message being answered, and `emoji`
(field 8) marking a payload as a reaction rather than text. This parser reads
fields 1, 2 and 6, so both are currently skipped: an incoming reply shows as an
ordinary message with no indication of what it answers, and a reaction shows as
a stray emoji on its own line. Sending either is not possible at all.

Worth doing together, since they are the same field pair and the same threading
model in the message view.

**Emoji and special character input.** There is no picker. The Finnish layer
covers å ä ö and nothing else, so anything outside the keyboard's own repertoire
cannot be typed — which also blocks sending reactions.

## Position and telemetry

**Capture position** from a phone or a USB GPS dongle, instead of typing
coordinates into the configuration screen.

**Send Meshtastic position updates** (`POSITION_APP`), honouring the precision
settings so a shared location can be deliberately coarse.

**Telemetry reporting.** Battery, channel utilisation and air time. Received
telemetry is already parsed and counted; none is sent.

Note that location is published, not merely stored: once set it goes out in
every MeshCore advert, to everyone in range and everyone they relay to.

## Network visibility

**Traceroute on both networks.** Meshtastic's `TRACEROUTE_APP` (port 70) is
currently received and skipped. MeshCore has an equivalent.

This is also where **actively probing a path to a repeater we have never
exchanged messages with** belongs — it is the same mechanism. Route learning
today is passive: a flooded message draws a path return, which is stored and
used directly afterwards. That works only for nodes we have already talked to.

**MeshCore topology builder.** Assemble a picture of the mesh from advert paths,
path returns and traceroute results. The data mostly arrives already; nothing
keeps it.

**Activity view.** A live on-screen list of everything heard on air, on both
networks, rather than only the traffic addressed to a channel we hold. The
session recorder already captures exactly this — the work is presenting it as it
happens instead of only in a file collected afterwards.

## Radio capacity

**Second radio support.** Genuine concurrency, as distinct from the profile
switching above: two networks live at once rather than one at a time.

[second-radio-investigation.md](second-radio-investigation.md) has the analysis
already. In short: a companion radio over serial on the CATT port is the
preferred option, a bare SX1262 on the same port is constrained by the ~40 dB of
antenna isolation a handheld cannot provide at this frequency spacing, and the
CH341-based USB sticks are rejected — they contain no MCU, so the host would
have to do everything through a USB-to-SPI bridge.

## Known issues and housekeeping

**`mm_proto` has no host tests.** The component is deliberately free of ESP-IDF,
FreeRTOS and BSP includes precisely so its codecs can be tested on a host, but
there is no host compiler on the development machine. The gap is currently
covered by boot-time self-tests against published vectors — RFC 8032 for
Ed25519, RFC 7748 for X25519, and a hand-computed byte vector for the NodeInfo
encoder.

**RSSI is always reported as zero.** A bug in the vendor `tanmatsu-lora` driver,
whose conversion contradicts its own documented scale. SNR is shown instead.
Needs an upstream fix or a local workaround.

**Relay slot timing is measured from processing, not reception.** `radio_poll()`
drains up to eight frames per loop iteration, so a burst can compress the
backoff a relayed packet was scheduled with. Small, real, and visible in a
session log as two receive lines sharing a timestamp.

**MeshCore off-grid repeat has no rate cap.** Deferred deliberately rather than
overlooked.

**`.gitattributes`.** Line endings produce a warning on every commit. Fixing it
renormalises the whole tree, so it wants its own commit rather than riding along
with unrelated work.

**`deploy.ps1` error message.** When the BadgeLink virtual environment is
missing it says so but does not say where to get it. That is the first thing a
fresh clone hits, and [tools/README.md](../tools/README.md) has the answer.
