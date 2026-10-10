# Documentation

Start with the [project README](../README.md) for everyday use or the
[handoff](MEMORY.md) when resuming development.

| Document | Purpose |
| --- | --- |
| [Hardware](HARDWARE.md) | Supported boards, identity lookup and last recorded fleet |
| [Development](DEVELOPMENT.md) | Build targets, SDK setup, versioning and maintenance |
| [Experimental TDMA](TDMA.md) | Beacon/slot protocol, isolated builds, diagnostics and acceptance tests |
| [OTA](OTA.md) | USB installation/recovery, radio updates, protocol and failure cases |
| [PC tools](TOOLS.md) | Monitoring, viewer, console and utility usage |
| [Protocol reference](PROTOCOL.md) | Local packet layouts and where to verify them |
| [Testing](TESTING.md) | Reproducible link tests and outstanding hardware validation |
| [Receiver placement](RECEIVER_PLACEMENT.md) | Conditional plan; no Quest/USB-host implementation is claimed |
| [Archive](archive/README.md) | Original notes, measurements and superseded Quest drafts |

## How to keep these useful

Current instructions take precedence over archived drafts. Code and the Makefile
define actual behavior; report disagreements instead of silently treating an old
note as proof. Hardware tables are dated observations and require a fresh check
before flashing.

Keep procedures in the focused guides and decisions/open work in `MEMORY.md`.
Record tests with the date (KST), commit/tag, board address, power source, placement,
duration and log location. Label estimates and untested behavior explicitly.
Keep raw historical evidence in the archive instead of rewriting old observations
as if they were new measurements.

The READMEs under `Tracker/`, `Receiver/` and `Tracker/vqf-c/` contain upstream
project/library documentation. Local setup is documented here; preserve upstream
license information when updating those files.
