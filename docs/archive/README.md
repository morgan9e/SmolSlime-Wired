# Historical documents

[Current documentation](../README.md)

These files preserve earlier evidence and designs. They are **not current flashing
or setup instructions**. Branch names, repository paths, remotes, device assignments
and statements such as "now" refer to the time each file was written.

| File | Status |
| --- | --- |
| [Original memory, 2026-10-10](MEMORY-2026-10-10.md) | Exact copy before documentation cleanup; full timeline, wired history, measurements, decisions, pitfalls and source inventory |
| [Measurements](MEASUREMENTS.md) | Extract of the original measurement section for quick comparison; no new test results |
| [Quest receiver v1](quest-receiver-v1.md) | Superseded October 8 draft; based on the old ProMicro receiver |
| [Quest receiver v2](quest-receiver-v2.md) | October 10 proposal; replaced for current planning by [receiver placement](../RECEIVER_PLACEMENT.md) |

The three original documents are retained byte-for-byte. Their old internal paths
and cross-references have intentionally not been rewritten. Use this index to
navigate. The original memory's numbered sections remain useful for historical
hardware/pins, radio experiments, branch reconstruction and the retired hub.

## Corrections to carry forward

- The checkout is now `SmolSlime-mod`, origin `morgan9e/SmolSlime-mod`; `clean`
  became `main`. Wired hub source is in the `wired` history, not current `main`.
- Old claims about what is pushed, missing tags or pending local files require a
  fresh Git check. The archive is not authorization to force-push or delete backups.
- Quest v1's type-7 offsets are wrong: sleep is u16 at bytes 3–4, compact quaternion
  at 5–8, acceleration at 9–14. Type 255 carries address announcements and must not
  be discarded as generic padding. See [protocol](../PROTOCOL.md).
- IDs 0 and 5 were suspected to be the antenna-damaged trackers, not confirmed.
- The ESP32-S3 USB-host path was proposed, not implemented. The working historical
  hub used UART input; its removed gap smoothing should not be treated as available.
- Earlier power, sensitivity and charger estimates are not measurements of the
  current setup. Revalidate external platform/API assumptions before implementation.
