# Builds the tracker and receiver images into dist/ and runs the PC tool tests.
# Run from WSL/Linux; flash from Windows (see README.md).
#
#   make            tracker and receiver
#   make tracker    ProMicro stacked SPI tracker (.uf2)
#   make tracker-xiao  Seeed XIAO nRF52840 Sense tracker (.uf2)
#   make tracker-ota   ProMicro tracker with MCUboot (docs/OTA.md): _OTA.uf2 (MCUboot + app, USB
#                   flash and recovery) and _OTA_app.bin (the image an update installs)
#   make tracker-ota-xiao  the same for the XIAO nRF52840 Sense
#   make tracker-ota-slot1  that app as a UF2 for the secondary slot, marked for a test swap
#                   (tests MCUboot over USB); with OTA_NOCONFIRM=1 an image that never confirms
#                   itself (rollback test)
#   make receiver   nRF52840 Dongle receiver (.hex for nRF Connect Programmer, .zip for nrfutil)
#   make test       PC tool tests
#   make clean      remove the build directories
#
# Toolchain locations can be overridden, e.g. make NCS_TRACKER=/opt/ncs-v3.1 tracker

ROOT          := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
DIST          ?= $(ROOT)/dist
BUILD         ?= $(HOME)/build/slimevr-clean

# nRF Connect SDK workspaces: the tracker builds with v3.1 (as upstream's releases), the receiver
# needs v3.3 (its timer code uses newer nrfx)
NCS_TRACKER   ?= $(HOME)/ncs-tracker
NCS_RECEIVER  ?= $(HOME)/ncs-ws
ZEPHYR_SDK    ?= $(HOME)/zephyr-sdk-0.17.4
WEST_VENV     ?= $(HOME)/ncs-ws/.venv
HOST_TOOLS    ?= $(HOME)/.local/xz/usr/bin:$(HOME)/.espressif/tools/cmake/3.30.2/bin:$(HOME)/.espressif/tools/ninja/1.12.1
TRACKER_BOARD ?= promicro_uf2/nrf52840/stackedspi
RECEIVER_BOARD?= nrf52840dongle/nrf52840
PRISTINE      ?= auto
TRACKER_EXTRA_CONF ?=
RECEIVER_EXTRA_CONF ?=

ZEPHYR_ENV = PATH="$(HOST_TOOLS):$(WEST_VENV)/bin:$$PATH" ZEPHYR_SDK_INSTALL_DIR="$(ZEPHYR_SDK)" ZEPHYR_TOOLCHAIN_VARIANT=zephyr

# The tracker's version comes from `git describe` (tags like 0.0.3, see tools/bump_version.py),
# handed to its build in FW_GIT_DESCRIBE / FW_GIT_BRANCH. In WSL, Linux git cannot read a worktree
# that Windows git created (its .git file holds a Windows path): git.exe then.
GIT ?= $(shell git -C "$(ROOT)" rev-parse --git-dir >/dev/null 2>&1 && command -v git || command -v git.exe || echo "/mnt/c/Program Files/Git/cmd/git.exe")
FW_GIT_DESCRIBE = $(shell cd "$(ROOT)" && "$(GIT)" describe --dirty --tags --long --match '[0-9]*.[0-9]*.[0-9]*' 2>/dev/null | tr -d '\r')
FW_GIT_BRANCH = $(shell cd "$(ROOT)" && "$(GIT)" branch --show-current 2>/dev/null | tr -d '\r')

TRACKER_UF2   := $(DIST)/SlimeNRF_ProMicro_StackedSPI_Tracker.uf2
TRACKER_XIAO_UF2 := $(DIST)/SlimeNRF_XIAO_Sense_Tracker.uf2
# The OTA build per board: OTA_LAYOUT picks Tracker/ota<layout>.overlay and
# Tracker/sysbuild/mcuboot<layout>.overlay (the pm_static file goes by the board, sysbuild.cmake)
OTA_BOARD     ?= $(TRACKER_BOARD)
OTA_NAME      ?= ProMicro_StackedSPI
OTA_LAYOUT    ?=
TRACKER_OTA_UF2 = $(DIST)/SlimeNRF_$(OTA_NAME)_Tracker_OTA.uf2
TRACKER_OTA_BIN = $(DIST)/SlimeNRF_$(OTA_NAME)_Tracker_OTA_app.bin
TRACKER_OTA_SLOT1_UF2 := $(DIST)/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA_slot1_test.uf2
OTA_CONF      := $(ROOT)/Tracker/ota.conf$(if $(OTA_NOCONFIRM),;$(ROOT)/Tracker/ota_noconfirm.conf)$(if $(TRACKER_EXTRA_CONF),;$(TRACKER_EXTRA_CONF))
IMGTOOL       := $(NCS_TRACKER)/bootloader/mcuboot/scripts/imgtool.py
# Secondary slot of pm_static/mcuboot_nrf52840_uf2.yml
OTA_SLOT1_ADDR := 0x7c000
OTA_SLOT1_SIZE := 0x6e000
UF2CONV       := $(NCS_TRACKER)/zephyr/scripts/build/uf2conv.py
UF2_FAMILY_NRF52840 := 0xADA52840
RECEIVER_HEX  := $(DIST)/SlimeNRF_Nordic_Dongle_Receiver.hex

.PHONY: all tracker tracker-xiao tracker-ota tracker-ota-xiao tracker-ota-slot1 receiver test clean

all: tracker receiver

tracker:
	@mkdir -p $(DIST)
	cd "$(NCS_TRACKER)" && $(ZEPHYR_ENV) FW_GIT_DESCRIBE="$(FW_GIT_DESCRIBE)" FW_GIT_BRANCH="$(FW_GIT_BRANCH)" \
		west build --board $(TRACKER_BOARD) --pristine=$(PRISTINE) \
		--build-dir "$(BUILD)/tracker" "$(ROOT)/Tracker" \
		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(ROOT)/Tracker"
	cp "$(BUILD)/tracker/Tracker/zephyr/zephyr.uf2" "$(TRACKER_UF2)"
	@echo "-> $(TRACKER_UF2)"

tracker-xiao:
	@mkdir -p $(DIST)
	cd "$(NCS_TRACKER)" && $(ZEPHYR_ENV) FW_GIT_DESCRIBE="$(FW_GIT_DESCRIBE)" FW_GIT_BRANCH="$(FW_GIT_BRANCH)" \
		west build --board xiao_ble/nrf52840/sense --pristine=$(PRISTINE) \
		--build-dir "$(BUILD)/tracker-xiao" "$(ROOT)/Tracker" \
		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(ROOT)/Tracker"
	cp "$(BUILD)/tracker-xiao/Tracker/zephyr/zephyr.uf2" "$(TRACKER_XIAO_UF2)"
	@echo "-> $(TRACKER_XIAO_UF2)"

# MCUboot without signature (the image's SHA-256 is still checked), swap with rollback. Always
# pristine: the image version in MCUboot's header is only taken when the build is configured.
tracker-ota:
	@mkdir -p $(DIST)
	cd "$(NCS_TRACKER)" && $(ZEPHYR_ENV) FW_GIT_DESCRIBE="$(FW_GIT_DESCRIBE)" FW_GIT_BRANCH="$(FW_GIT_BRANCH)" \
		west build --board $(OTA_BOARD) --pristine=always \
		--build-dir "$(BUILD)/tracker-ota$(OTA_LAYOUT)" "$(ROOT)/Tracker" \
		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(ROOT)/Tracker" \
		-DSB_CONFIG_BOOTLOADER_MCUBOOT=y -DSB_CONFIG_MCUBOOT_MODE_SWAP_USING_MOVE=y \
		-DSB_CONFIG_BOOT_SIGNATURE_TYPE_NONE=y -Dmcuboot_BOARD=nrf52840dk/nrf52840 \
		-Dmcuboot_DTC_OVERLAY_FILE="$(ROOT)/Tracker/sysbuild/mcuboot$(OTA_LAYOUT).overlay" \
		-DTracker_EXTRA_CONF_FILE="$(OTA_CONF)" -DTracker_EXTRA_DTC_OVERLAY_FILE="$(ROOT)/Tracker/ota$(OTA_LAYOUT).overlay"
	python3 "$(UF2CONV)" -c -f $(UF2_FAMILY_NRF52840) -o "$(TRACKER_OTA_UF2)" "$(BUILD)/tracker-ota$(OTA_LAYOUT)/merged.hex"
	cp "$(BUILD)/tracker-ota$(OTA_LAYOUT)/Tracker/zephyr/zephyr.signed.bin" "$(TRACKER_OTA_BIN)"
	@echo "-> $(TRACKER_OTA_UF2), $(TRACKER_OTA_BIN)"

# The XIAO keeps its bootloader's SoftDevice area: MCUboot at 0x27000 (pm_static/mcuboot_xiao_ble.yml)
tracker-ota-xiao:
	$(MAKE) tracker-ota OTA_BOARD=xiao_ble/nrf52840/sense OTA_NAME=XIAO_Sense OTA_LAYOUT=_xiao

# The tracker-ota app padded to the secondary slot, with the trailer that asks MCUboot for a test
# swap on the next boot, as a UF2 for that slot (same imgtool arguments as the build, plus --pad)
tracker-ota-slot1: tracker-ota
	version=$$(grep -o -m1 -- '--version [^ ]*' "$(BUILD)/tracker-ota/Tracker/build.ninja" | head -1 | cut -d' ' -f2); \
	"$(WEST_VENV)/bin/python" "$(IMGTOOL)" sign --version "$$version" --align 4 --slot-size $(OTA_SLOT1_SIZE) \
		--pad-header --header-size 0x200 --pad \
		"$(BUILD)/tracker-ota/Tracker/zephyr/zephyr.bin" "$(BUILD)/tracker-ota/slot1_test.bin"
	python3 "$(UF2CONV)" -c -b $(OTA_SLOT1_ADDR) -f $(UF2_FAMILY_NRF52840) -o "$(TRACKER_OTA_SLOT1_UF2)" \
		"$(BUILD)/tracker-ota/slot1_test.bin"
	@echo "-> $(TRACKER_OTA_SLOT1_UF2)"

# The dongle board's own USB console (new USB stack) clashes with the receiver's legacy one in
# this SDK: BOARD_SERIAL_BACKEND_CDC_ACM off.
receiver:
	@mkdir -p $(DIST)
	cd "$(NCS_RECEIVER)" && $(ZEPHYR_ENV) west build --board $(RECEIVER_BOARD) --pristine=$(PRISTINE) \
		--build-dir "$(BUILD)/receiver" "$(ROOT)/Receiver" \
		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(ROOT)/Receiver" -DCONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=n $(if $(RECEIVER_EXTRA_CONF),-DReceiver_EXTRA_CONF_FILE="$(RECEIVER_EXTRA_CONF)")
	cp "$(BUILD)/receiver/Receiver/zephyr/zephyr.hex" "$(RECEIVER_HEX)"
	python3 "$(ROOT)/tools/dfu_zip.py" "$(RECEIVER_HEX)" "$(RECEIVER_HEX:.hex=.zip)"
	@echo "-> $(RECEIVER_HEX), $(RECEIVER_HEX:.hex=.zip)"

# Keep experimental binaries/builds away from the known-good images.
.PHONY: tdma tdma-tracker tdma-tracker-xiao tdma-receiver test-tdma
tdma: tdma-tracker tdma-tracker-xiao tdma-receiver

tdma-tracker:
	$(MAKE) tracker-ota BUILD="$(BUILD)/tdma" DIST="$(DIST)/tdma" TRACKER_EXTRA_CONF="$(ROOT)/Tracker/tdma.conf"

tdma-tracker-xiao:
	$(MAKE) tracker-ota-xiao BUILD="$(BUILD)/tdma" DIST="$(DIST)/tdma" TRACKER_EXTRA_CONF="$(ROOT)/Tracker/tdma.conf"

tdma-receiver:
	$(MAKE) receiver PRISTINE=always BUILD="$(BUILD)/tdma" DIST="$(DIST)/tdma" RECEIVER_EXTRA_CONF="$(ROOT)/Receiver/tdma.conf"

test-tdma:
	@mkdir -p "$(BUILD)/tests"
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -I"$(ROOT)/shared" "$(ROOT)/tests/test_radio_tdma.c" -o "$(BUILD)/tests/test_radio_tdma"
	"$(BUILD)/tests/test_radio_tdma"

test: test-tdma
	cd "$(ROOT)/tools" && python3 -m unittest test_slime_link

clean:
	rm -rf "$(BUILD)"
