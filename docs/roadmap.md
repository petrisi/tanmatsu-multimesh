# Roadmap

What MultiMesh cannot do yet, and what that means in practice.

## Duty cycle

No duty-cycle limit is enforced. The 869.4–869.65 MHz band is capped at 10% in
the EU, and nothing in the app tracks or restricts how much of it you have used.
Staying within the limit is entirely your responsibility, and the app cannot help
you judge it — which is why it is described as lab and experimental use.

## Radio settings and mesh profiles

Meshtastic is configurable: EdgeFastLow EU, LongFast EU, or Custom with the
frequency, spreading factor, bandwidth and coding rate set by hand, plus transmit
power as its own setting.

**MeshCore is not.** It follows whatever region preset the device is configured
with, in the shared storage the stock MeshCore client writes, and cannot be
retuned from here.

Past that, the aim is profiles you can name, add and keep — rather than a fixed
list of two plus one scratch slot — and for MeshCore to have the same. Still one
network at a time, but **△** would become a picker rather than a two-way toggle.

## Message history

Messages are lost at restart. Channels, identity, settings and the node list all
survive; the conversation does not.

## Replies and reactions *(Meshtastic)*

A reply arrives looking like any other message, with nothing to show what it
answers. A reaction arrives as a bare emoji on a line of its own. Neither can be
sent.

## Emoji

Emoji can be neither displayed nor typed. The font carries ASCII and six Finnish
letters, so an emoji in an incoming message does not render, and there is no way
to enter one — which also means reactions will stay unsendable even once replies
work.

## Position and telemetry

Position can only be typed in by hand. There is no way to take it from a phone or
a GPS dongle, Meshtastic position packets are never sent, so other people's maps
will not show you, and no telemetry is sent either — though incoming telemetry is
read.

Note that a position is published rather than merely stored: once set it goes
into every MeshCore advert.

## Traceroute and path discovery

There is no way to ask how traffic reaches a given node. Meshtastic traceroute is
received and ignored; the MeshCore equivalent is absent.

Route learning is passive: a flooded message brings back the path it travelled,
and that path is then reused. It only ever works for nodes you have already
messaged, so there is no way to find a route to a repeater you have never
exchanged traffic with.

## Mesh map

Advert paths, returned routes and traceroute replies all describe the shape of
the mesh, and none of it is kept. A topology view would show what the mesh looks
like from where you are standing.

## Activity view

The message log shows traffic on channels you hold keys for. Everything else
heard on air — other channels, other people's direct messages, adverts, telemetry
— is counted but never shown. The session recorder captures it to a file, but
there is no live view.

## Second radio

One radio means one network at a time. Running MeshCore and Meshtastic together
needs a second one.
[second-radio-investigation.md](second-radio-investigation.md) has the analysis:
a companion radio over serial on the CATT port is the workable option, a bare
SX1262 needs more antenna isolation than a handheld can provide, and the
CH341-based USB sticks contain no processor of their own and are rejected.

## Brightness scale

The two brightness settings run on their own scale: ten levels spaced
geometrically, because evenly spaced duty cycles give one visible step and nine
that do nothing. The launcher's own sliders are a plain percentage of duty.

Two scales for one backlight is a cost, and it is only worth paying while the
perceptual spacing is earning it. **If it turns out not to help in practice, or
to cause confusion of its own, these will move to the launcher's scale instead**
— matching what the device already shows everywhere else is worth more than a
curve nobody notices.

One symptom to watch, since it is the first thing that would tip the balance: on
a fresh install the level adopted from the device can read differently from the
launcher's number, because the level is read back through the hardware rather
than from the launcher's stored percentage. The light is right, the label may not
be. It only happens once, before any level has been chosen here.

## Known limitations

- **RSSI always reads zero.** A bug in the vendor radio driver. Signal quality is
  shown as SNR instead.
- **MeshCore off-grid repeat has no rate limit.** It forwards everything it hears
  for as long as it is switched on.
- **Relay timing is measured from when a packet is processed**, not when it
  arrived, so a burst of traffic can shorten the backoff slightly.
- **`mm_proto` has no host tests.** Correctness rests on self-checks that run at
  every boot against published vectors — RFC 8032 for Ed25519, RFC 7748 for
  X25519, and a fixed byte vector for the NodeInfo encoder.
