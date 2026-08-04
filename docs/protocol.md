# Protocols

Both stacks are implemented from the wire up, in `mm_proto` (codecs),
`mm_crypto` (keys) and `mm_net` (behaviour). This is what they do and, where it
is not obvious, why.

- [Radio settings](#radio-settings)
- [Channels](#channels)
- [Identity and signing](#identity-and-signing)
- [Nodes](#nodes)
- [Direct messages](#direct-messages)
- [Routing](#routing)
- [Storage](#storage)
- [Implementation notes](#implementation-notes)

## Radio settings

**MeshCore** modem settings come from the shared `system` NVS namespace
(`lora.freq`, `lora.sf`, `lora.bandwidth`, `lora.codingrate`, `lora.power`,
`lora.preamble`, `lora.sync`, `lora.rxboost`), so the app follows whatever region
preset is configured on the device. Falls back to EU/UK narrow:

```
869.618 MHz  SF8  BW 62.5 kHz  CR 4/8  22 dBm  preamble 8  sync 0x12  rx_boost on
```

**Meshtastic** uses the EdgeFastLow (EFL) profile that is compatible with NarrowSlow-preset (at some level):

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

**MeshCore** derives the key three ways, chosen by what is typed. Tested in this
order, first match wins:

| Condition | Result |
|---|---|
| secret set: base64, 16 or 32 bytes | a private channel — any other length is rejected |
| secret empty, name starts with `#` | a hashtag channel: key = `SHA256(name)[0:16]`, over the name *including* the `#`, so anyone who knows the name can join — that is the point |
| secret empty | the well-known public channel |

The PSK is **base64, not hex** — that is the format MeshCore clients exchange.
The channel hash is `SHA256(key)[0]`. Payloads are AES-ECB with an
HMAC-SHA256 prefix truncated to two bytes.

**Meshtastic** accepts every PSK length upstream does, and each length means
something different:

| Decoded length | Meaning |
|---|---|
| 0 | unencrypted — a valid configuration, not an error |
| 1 | a key *index*, not a key: `AQ==` is `{0x01}` = the default public key, `0x00` disables encryption, and each higher index bumps the default key's last byte |
| 2–16 | the key, zero-padded to 16 bytes (AES-128) |
| 17–32 | the key, zero-padded to 32 bytes (AES-256) |

Channel traffic is AES-CTR with the nonce built from the packet id and sender.
The channel hash mixes the **name as well as the key**, so renaming a Meshtastic
channel changes which traffic it matches. That is upstream behaviour.

Every key that appears in this source tree is a published, well-known network
default — MeshCore's public channel key and Meshtastic's default PSK. Nothing
secret is committed here.

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
node `*` and one whose signature did not check `!`; a failure is re-tested on the
next advert rather than held against the node forever, since a single corrupt
frame can pass a 16-bit CRC.

A MeshCore advert signature covers `pub_key || timestamp || app_data`, and
receivers clamp `app_data` to 32 bytes *before* verifying. The signed region is
taken from the raw payload rather than re-serialised from the parsed struct: a
round trip would drop any field this parser does not understand, and the
signature would then fail against senders that include one — which looks like
broken crypto rather than a lossy parse.

Meshtastic NodeInfo is unsigned — any node may claim any name — so nothing is
reported for it rather than implying a check that could have been made. The
Meshtastic node number is the low four bytes of the factory MAC, rendered as
`!aabbccdd`, and never changes.

## Nodes

An entry is created for **any** packet heard, and enriched when a NodeInfo or a
named advert arrives. Stored fields are what each network actually offers:
Meshtastic gets node number, short and long name, hardware model and the
Curve25519 DM key; MeshCore gets the public key, role, name and learned route.

Announcing is automatic every 24 hours and manual from the nodes view (△), rate
limited to once per 5 minutes — this is a duty-cycle limited band.

Entries expire on last-heard: **7 days** for a node that never told us its name,
**30 days** for one that did. An unnamed entry is little more than evidence that
something transmitted once; a named one is a contact.

MeshCore names carry an optional nickname in the display name; only the first
four characters are shown in a message row by default, and a per-node short name
can be set to override that. Clipping happens on a character boundary, not a byte
boundary — "Härmälänranta" is thirteen characters and sixteen bytes, and cutting
at four bytes would leave a dangling UTF-8 lead byte.

## Direct messages

Both networks encrypt a conversation to the other party, so **neither can carry
one without a key**. The picker marks each contact `e2e`, `...` while the key is
being derived, or `no key`.

**MeshCore** agrees a key from the Ed25519 identity in a node's advert, so a
contact becomes messageable as soon as it has been heard. The identity key is
converted to Montgomery form, X25519 gives a shared secret, and the AES key is
its first 16 bytes — while the HMAC is keyed with all 32. That asymmetry is
upstream's, and it is reproduced deliberately.

Delivery is acknowledged: the reply is a hash of the plaintext and the sender's
key, which proves the recipient decrypted the message rather than merely heard
the frame. An acknowledgement payload also carries a random byte, so the same
acknowledgement is not deduplicated away as it is repeated through the mesh —
which means every retry attempt gets its own expected hash, and all of them stay
live until one matches.

**Meshtastic** has no such shortcut and no channel-key fallback, because current
firmware removed both halves of one: `perhapsEncode` refuses to send a keyless
direct message (`PKI_SEND_FAIL_PUBLIC_KEY`, *"refusing to send legacy DM"*) and
`perhapsDecode` discards one that arrives — it decrypts it, parses it, recognises
it as a direct message and drops it with *"Rejecting legacy DM"*. A fallback
would produce a message that looked sent, reached nobody, and could not even be
acknowledged, since the acknowledgement is generated after a successful decode.

So a Meshtastic contact must publish a key first. **◇ in the node detail
exchanges it** — our NodeInfo addressed to them with `want_response`, which is
what the official app calls "exchange user info". NodeInfo is deliberately exempt
from both rules above, which is exactly what lets it travel before any key
exists. Incoming requests are answered the same way, at most once a day per
asker; asking more often than that resets the window rather than shortening it.

Meshtastic PKI is X25519 → SHA-256 → AES-256-CCM with an 8-byte tag and a
13-byte nonce in which a random extension overwrites the high half of the packet
id. Acknowledgement is a `ROUTING` reply naming the packet it answers, sent only
when the sender set `want_ack`.

## Routing

**MeshCore learns routes.** A message to a contact floods the mesh until that
contact hands back the path it took, in an explicit path-return packet that
carries the acknowledgement along with it. After that the message goes directly
along that path for a fraction of the airtime.

A learned route is used for two attempts; if neither is acknowledged the third
falls back to flooding, and a flooded message draws a fresh path return by
itself. Routes are shown in the node detail and can be forgotten there (☁).
Nothing drops a route automatically — upstream's `onSendTimeout()` is empty, and
a route that fails once is usually a length or propagation problem rather than a
dead path.

Paths are lists of node hashes, and the hash width is **per-path, not global**:
the control byte packs the hop count in its low six bits and the hash size minus
one in its top two. A four-hop path of two-byte hashes and an eight-hop path of
one-byte hashes are the same eight bytes on the wire and mean entirely different
things. This app originates two-byte hashes.

Both networks flood, so every message arrives once directly and again from each
repeater. Dedup keys differ: Meshtastic has an `(from, id)` header pair,
MeshCore has no packet id but an unchanged payload, so that is fingerprinted.
Dedup covers direct messages as well as broadcasts.

### Meshtastic relaying

> **Experimental, and aimed at one situation:** temporarily holding a handful of
> nodes together from somewhere with a view, for as long as you are there. Switch
> it on when you arrive and off when you leave. It is not a substitute for a
> deployed node — a handheld on battery is a poor repeater — and where a repeater
> already covers you, leaving it off is the better neighbour.

At role CLIENT this app forwards other people's packets; at CLIENT_MUTE it does
not, which is what CLIENT_MUTE advertises and why it is the default. The hard
rules are upstream's and hold in every mode: never a packet addressed to us,
never one of ours, never one already at `hop_limit 0`, never one we have relayed
before, never the reserved non-LoRa broadcast address (`to == 1`), and never one
whose `next_hop` names a node that is not us.

A relay rewrites exactly two header bytes — `hop_limit` and `relay_node` — on
the frame as received. It does not re-encode: the payload may be encrypted, on a
channel we do not hold, or carry protobuf fields this build does not know, and a
decode/encode round trip would silently drop all three. `hop_start` is left
alone, because receivers compute distance as `hop_start − hop_limit` and a relay
that moved it would make every node downstream misjudge how far away the sender
is. `relay_node` is the low byte of our node number, which upstream also sets on
its own transmissions and not only when forwarding.

**The backoff is the interesting part.** Every node in earshot hears a packet in
the same instant, so they must be scattered — and Meshtastic scatters them *by
received signal strength, deliberately the wrong way round*. A strong signal
means the sender is close, which means relaying adds little coverage, so a strong
signal buys a **longer** wait. The distant node that would actually extend the
mesh goes first, and everyone nearer hears it and stands down.

The scale is a contention window of `2^CWsize` slots, `CWsize = map(snr, −20,
+10, 3, 8)`, on top of a fixed `2 × CWmax` slot offset that keeps ordinary nodes
clear of the window routers use. A slot is `2.5 symbols + 7.6 ms`, so at SF8 /
62.5 kHz it is 17 ms:

| rx SNR | CWsize | Normal window | Late slot |
|---|---|---|---|
| −20 dB | 3 | 272 – 391 ms | 408 ms |
| −10 dB | 4 | 272 – 527 ms | 544 ms |
| 0 dB | 6 | 272 – 1343 ms | 1360 ms |
| +10 dB | 8 | 272 – 4607 ms | 4624 ms |

Hearing a second copy of something still queued does three things: if the new
copy has *more* hops left, it replaces ours, since the one that has travelled
less far will reach further; then either the relay is cancelled (plain CLIENT) or
moved to the late slot (**always repeat**), which is upstream's ROUTER_LATE
behaviour without the router's early window or its advertised role. An
acknowledgement or reply for somebody else's direct message also cancels a queued
relay of it outright — the reply proves it arrived, so carrying it further is
pure airtime.

**Optimize text** filters on the decoded portnum: only `TEXT_MESSAGE_APP` (1) and
`ROUTING_APP` (5) are carried, and what is carried keeps its hop limit rather
than spending one. Traffic we cannot decrypt — a direct message, or a channel we
do not hold — has no portnum to judge, so it is relayed normally; dropping it
would stop us carrying every DM on the mesh, and upstream's `CORE_PORTNUMS_ONLY`
makes the same choice.

Not decrementing is a deliberate divergence with a cost worth stating: nodes
downstream will underestimate how far away the sender is, and a message's reach
is no longer bounded by the hop count its sender chose. It still terminates —
dedup means each node relays a given `(from, id)` at most once — but it travels
further than a plain mesh would carry it.

### MeshCore off-grid repeat

> **Experimental, and for being outside the MeshCore infrastructure entirely** —
> a field day, a search, a group on a hill, bootstrapping a mesh before anything
> permanent exists. Where deployed repeaters already cover you, they are better
> placed, better powered and better behaved than a handheld; leave this off.

**Off-grid repeat** *(MeshCore)* makes this client forward other nodes' packets
so a handful of ordinary clients can give each other extra hops with no deployed
infrastructure. Flooded packets are re-sent after a random backoff derived from
their airtime; directed ones go out immediately. It differs from the stock
feature in one deliberate way: upstream ties it to one of three preset
frequencies (433.000, 869.000 or 918.000 MHz), which cuts you off from a regional
mesh running anywhere else while it is on. Here it is only a mode — the
configured frequency is left alone. The advertised role does not change.

## Storage

| What | Where | Why |
|---|---|---|
| channels, identity, settings | NVS namespace `multimesh` | small, versioned, rewritten rarely |
| identity seed | NVS key `mc.seed` | 32 bytes; the key pair is derived, never stored |
| node tables | `/locfd/multimesh/nodes-{mc,mt}.bin` | two 48-entry tables would take about a third of the 16 KB NVS partition, which is shared with the launcher and WiFi |
| session log | `/locfd/multimesh/session.log` | only while recording |

Records carry a version and an **unrecognised version is discarded, not
reinterpreted**: losing settings is recoverable, silently misreading a channel key
is not. Node files are written to a temporary file and renamed, so a power cut
leaves the previous table intact. The node struct's field order *is* the on-disk
order — reordering it requires bumping the file version.

MeshCore radio settings are *read* from the shared `system` namespace and never
written there — two apps fighting over one key set is how configuration
mysteriously changes.

> **Channel keys and the identity seed are stored in the clear.** That is what
> every mesh client does: the device is the trust boundary, NVS is not encrypted,
> and the Tanmatsu ships with secure boot permanently disabled. Anyone holding
> the hardware has the keys.

## Implementation notes

- Meshtastic AES-CTR cannot fail on a wrong key — it just yields garbage. The
  strict protobuf parse in `mt_data_parse()` is what rejects foreign traffic, so
  keep it strict.
- `User.public_key` is protobuf field **8**, `bytes`, exactly 32 long. Omitting
  it is invisible locally and shows up on every other radio as "no key provided",
  so `mt_wire_selftest()` checks the encoder against a hand-computed byte vector
  at boot — there is no host compiler on the development machine to run a proper
  unit test.
- Received timestamps are **our own receive clock on both networks**. It is the
  only clock we control, so it is the only one that gives a stable ordering and
  cannot file a message under the wrong week because a sender's clock is adrift.
  MeshCore does carry the sender's claim, and that is kept and shown in the
  message detail — as evidence, not as the ordering key.
- The crypto is checked against published vectors wherever they exist: RFC 8032
  for Ed25519, RFC 7748 for X25519. The Meshtastic PKI nonce layout has no
  published vector, so interoperation there is only provable against a real node.
