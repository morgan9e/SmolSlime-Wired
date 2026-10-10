# Receiver placement and optional hosts

[Documentation index](README.md) · Conditional proposal, updated 2026-10-10.

Keep the Nordic dongle on the PC unless measured worn-pose failures justify moving
it. The old ProMicro receiver was the main reception problem; replacing it helped
substantially. Historical six-tracker load tests also showed collision loss, which
moving the receiver alone cannot be assumed to fix.

## Measure first

Follow [testing](TESTING.md): six moving trackers on battery, a normal worn session,
then comparable tests with the dongle raised or on an extension near the body.

| Observation | Next step |
| --- | --- |
| Desk/raised dongle meets tracking targets in all poses | Keep the direct USB setup |
| Particular poses fail and the extension placement reliably improves them | Choose the measured position, then evaluate a host |
| Loss remains high at both placements | Investigate load, timing and firmware before building a new host |

Do not discard visible gaps just because collisions caused them. Retain the loss,
gap and pose evidence behind the decision.

## Candidate hosts — not implemented or validated here

| Option | Proposed path | Work and unresolved checks |
| --- | --- | --- |
| A: Android server on Quest | Dongle → Quest USB host → Android SlimeVR server → PC VRChat OSC | Confirm current APK, USB access, resets, OSC behavior, charging and survival during Steam Link |
| C: ESP32-S3 host on the body | Dongle → new USB HID input in historical hub → PC SlimeVR server | Implement/test USB host input, 5 V VBUS supply, reconnects and RF coexistence |
| B: Quest forwarder | Dongle → Android raw-HID forwarder → PC UDP translator → PC server | New Android and PC programs; validate lifecycle, decoding, identity, reconnects and latency |

The earlier preference was A for a VRChat experiment, then C if PC/SteamVR
integration is needed, with B reserved for a headset-mounted receiver that still
needs the PC server. This is a planning preference, not evidence that any option
works on the Quest.

## Constraints carried forward

- Use the Nordic dongle and keep all trackers wireless.
- The old working ESP32 hub used a UART input. It lives in the `wired` history
  under `src/Hub`; current `main` has no hub target or USB-host module.
- Option C would reuse the existing relay/UDP behavior, not add smoothing or
  unrelated hub features. Removed gap-hold code is not an available component.
- Powering the USB dongle and running Wi-Fi beside it require bench tests. The old
  router-channel observation does not prove a new colocated host is interference-free.
- Preserve device addresses so assignments match direct USB. Type 255 HID records
  carry those addresses; type 7 uses quaternion bytes 5–8 and acceleration 9–14.
  See [protocol](PROTOCOL.md).
- The old forwarder code was reported absent from disk. Recovery from other chat
  transcripts is not authorized by this plan.
- Revalidate external SDK/platform/API assumptions before implementing an option.

## Acceptance if a host is built

First meet the link targets in [testing](TESTING.md), then verify the intended
server/game sees all six trackers with correct identities and motion. Check reset
controls, battery/status handling where supported, unplug/replug recovery and a
two-hour session. The earlier PC bridge target was under 15 ms displayed PingPong;
it remains a target, not a measured result.

The original [v1](archive/quest-receiver-v1.md) and
[v2](archive/quest-receiver-v2.md) designs remain available for context. Their
implementation details contain stale paths and unverified assumptions; this guide
supersedes their project status and decision order.
