# Development and builds

[Documentation index](README.md)

## Build environment

Build in WSL/Linux and flash from Windows. The [Makefile](../Makefile) resolves the
repository root itself; run it from this checkout, not the archived `SlimeVR-Hub`
folder. This checkout's WSL path is `/mnt/c/Users/Morgan/Lab/SmolSlime-mod`.

The configured environment below comes from the Makefile and the last successful
build notes. The documentation cleanup did not rebuild firmware or reinstall SDKs.

| Variable | Default | Recorded environment |
| --- | --- | --- |
| `NCS_TRACKER` | `~/ncs-tracker` | NCS v3.1-branch; Zephyr 4.1.99 |
| `NCS_RECEIVER` | `~/ncs-ws` | NCS v3.3.0-preview3-branch |
| `ZEPHYR_SDK` | `~/zephyr-sdk-0.17.4` | Zephyr SDK |
| `WEST_VENV` | `~/ncs-ws/.venv` | west/Python environment |
| `BUILD` | `~/build/slimevr-clean` | External build output; old name retained by Makefile |
| `HOST_TOOLS` | See Makefile | xz, CMake 3.30.2 and Ninja 1.12.1 paths |

These SDK workspaces must already contain their dependencies. The root Makefile
does not install them. Override paths explicitly when using another environment:

```bash
cd /mnt/c/Users/Morgan/Lab/SmolSlime-mod
make NCS_TRACKER=/path/to/ncs-tracker tracker-ota
```

The receiver's imported base uses newer nrfx APIs and did not build on the
tracker's NCS v3.1 workspace. Its build disables `BOARD_SERIAL_BACKEND_CDC_ACM`
to avoid a conflict between the dongle board's USB console and the receiver's
legacy USB stack.

## Targets and image selection

All outputs below are under `dist/`, which Git ignores.

| Target | Output | Intended use |
| --- | --- | --- |
| `tracker-ota` | `SlimeNRF_ProMicro_StackedSPI_Tracker_OTA.uf2`, `_OTA_app.bin` | ProMicro USB install/recovery and radio updates |
| `tracker-ota-xiao` | `SlimeNRF_XIAO_Sense_Tracker_OTA.uf2`, `_OTA_app.bin` | XIAO USB install/recovery and radio updates |
| `receiver` | `SlimeNRF_Nordic_Dongle_Receiver.hex`, `.zip` | Dongle USB flashing |
| `tracker` | `SlimeNRF_ProMicro_StackedSPI_Tracker.uf2` | Plain app without MCUboot |
| `tracker-xiao` | `SlimeNRF_XIAO_Sense_Tracker.uf2` | Plain XIAO app without MCUboot |
| `tracker-ota-slot1` | `SlimeNRF_ProMicro_StackedSPI_Tracker_OTA_slot1_test.uf2` | ProMicro rollback experiment only |
| `test` | Runs `tools/test_slime_link.py` | PC link-monitor tests |
| `clean` | Removes the configured `BUILD` directory | Does not clear `dist/` |

Bare `make` selects `tracker receiver`: **it does not build an OTA tracker**.
The installed MCUboot trackers need the explicit OTA targets.
`OTA_NOCONFIRM=1` deliberately builds an image that never confirms; keep that
option out of normal releases.

## Experimental TDMA builds

`make tdma` builds the ProMicro and XIAO OTA-layout trackers and Nordic receiver
with TDMA enabled, isolated under `dist/tdma/`. `make test-tdma` runs host C tests;
`make test` also includes them and requires a C11 compiler. See [TDMA](TDMA.md)
for protocol, limitations, diagnostics and hardware acceptance.

## Version and release workflow

1. Review the working tree and commit the intended source changes.
2. Run `python tools/bump_version.py`. It creates an annotated next-patch tag on
   HEAD. It refuses tracked changes but does not reject untracked files.
3. Build the appropriate OTA target. OTA builds are always pristine so the
   MCUboot image header receives the new version.
4. Test the image on the identified test tracker first; verify boot, tracking and
   confirmation. See [OTA](OTA.md).
5. Roll out only the validated image. When both sides changed, flash the receiver
   last. Do not push unless requested.

The app gets `FW_GIT_DESCRIBE` and `FW_GIT_BRANCH` from the Makefile. Its generated
`Tracker/VERSION` is ignored. Tags follow `0.0.N`; no tag is needed for this
documentation-only cleanup. Inspect actual tags instead of assuming the versions
listed in historical notes are still the latest.

## Source map

| Feature | Source |
| --- | --- |
| Moving-loop period, OTA confirmation, charge estimate options | [Tracker Kconfig](../Tracker/Kconfig) |
| Per-device retry delay, flush behavior, HF clock handling | [Tracker ESB](../Tracker/src/connection/esb.c) |
| Tracker packet encoding and send loop | [Tracker connection](../Tracker/src/connection/connection.c) |
| Receiver diagnostics | [packet_log.c](../Receiver/src/packet_log.c) |
| OTA receiver/tracker | [Receiver OTA](../Receiver/src/ota.c), [Tracker OTA](../Tracker/src/ota.c) |
| Flash partitions and overlays | [OTA layouts](OTA.md) |
| Host utilities | [PC tools](TOOLS.md) |

## Checks and pitfalls

- For PC monitor changes: `make test`, or from Windows at the root,
  `python -m unittest discover -s tools -p test_slime_link.py`. This is not a
  firmware or radio acceptance test.
- Test power/radio changes on battery. USB power masked the historical HF-clock
  bug, and the USB cable changes antenna behavior.
- Check actual stack configuration before adding init/workqueue work. Earlier
  512-byte main and 256-byte workqueue stacks overflowed with printing/NVS work.
  Use `CONFIG_STACK_USAGE` for new init code before flashing.
- Keep each board's partition-manager YAML, app overlay and MCUboot overlay in
  agreement. See [OTA](OTA.md). The XIAO preserves its SoftDevice region.
- Use explicit UTF-8 when reading/writing text in Python on Windows. Preserve line
  endings of existing source files; WSL shell scripts need LF.
- Windows Git worktree metadata may not be readable by Linux Git. The Makefile
  falls back to `git.exe` for version discovery.
- COM-port access denied usually means another program owns the console. Close
  that connection before flashing; one flashing watcher at a time.
- Generated/ignored build files are not reset by restoring tracked source.
  Inspect them when diagnosing a configuration mismatch.

## Upstream maintenance

There are no submodules or nested repositories. Tracker and receiver histories
were imported as Git subtrees. Review and commit local work before an integration:

```bash
git subtree pull --prefix=Receiver https://github.com/SlimeVR/SlimeVR-Tracker-nRF-Receiver main
git subtree pull --prefix=Tracker https://github.com/SlimeVR/SlimeVR-Tracker-nRF main
```

These commands modify history and files; they are maintenance instructions, not
part of an ordinary build. Review each merge and validate both firmware and tools
before flashing. Preserve upstream READMEs and licenses.

The locally known remote-tracking ref `origin/wired` contains the old `src/Hub`,
`src/Tracker` and `src/Receiver` tree. Inspect it without switching the current
checkout, for example `git show origin/wired:README.md`. Local remote-tracking
refs are not proof of the remote's latest state; no fetch or push was performed
for the documentation cleanup.
