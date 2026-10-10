# Builds the tracker and receiver images into dist/ and runs the PC tool tests.
# Run from WSL/Linux; flash from Windows (see README.md).
#
#   make            tracker and receiver
#   make tracker    ProMicro stacked SPI tracker (.uf2)
#   make tracker-xiao  Seeed XIAO nRF52840 Sense tracker (.uf2)
#   make receiver   nRF52840 Dongle receiver (.hex for nRF Connect Programmer, .zip for nrfutil)
#   make test       PC tool tests
#   make clean      remove the build directories
#
# Toolchain locations can be overridden, e.g. make NCS_TRACKER=/opt/ncs-v3.1 tracker

ROOT          := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
DIST          := $(ROOT)/dist
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

ZEPHYR_ENV = PATH="$(HOST_TOOLS):$(WEST_VENV)/bin:$$PATH" ZEPHYR_SDK_INSTALL_DIR="$(ZEPHYR_SDK)" ZEPHYR_TOOLCHAIN_VARIANT=zephyr

# The tracker's version comes from `git describe` (tags like 0.0.3, see tools/bump_version.py),
# handed to its build in FW_GIT_DESCRIBE / FW_GIT_BRANCH. In WSL, Linux git cannot read a worktree
# that Windows git created (its .git file holds a Windows path): git.exe then.
GIT ?= $(shell git -C "$(ROOT)" rev-parse --git-dir >/dev/null 2>&1 && command -v git || command -v git.exe || echo "/mnt/c/Program Files/Git/cmd/git.exe")
FW_GIT_DESCRIBE = $(shell cd "$(ROOT)" && "$(GIT)" describe --dirty --tags --long --match '[0-9]*.[0-9]*.[0-9]*' 2>/dev/null | tr -d '\r')
FW_GIT_BRANCH = $(shell cd "$(ROOT)" && "$(GIT)" branch --show-current 2>/dev/null | tr -d '\r')

TRACKER_UF2   := $(DIST)/SlimeNRF_ProMicro_StackedSPI_Tracker.uf2
TRACKER_XIAO_UF2 := $(DIST)/SlimeNRF_XIAO_Sense_Tracker.uf2
RECEIVER_HEX  := $(DIST)/SlimeNRF_Nordic_Dongle_Receiver.hex

.PHONY: all tracker tracker-xiao receiver test clean

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

# The dongle board's own USB console (new USB stack) clashes with the receiver's legacy one in
# this SDK: BOARD_SERIAL_BACKEND_CDC_ACM off.
receiver:
	@mkdir -p $(DIST)
	cd "$(NCS_RECEIVER)" && $(ZEPHYR_ENV) west build --board $(RECEIVER_BOARD) --pristine=$(PRISTINE) \
		--build-dir "$(BUILD)/receiver" "$(ROOT)/Receiver" \
		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(ROOT)/Receiver" -DCONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=n
	cp "$(BUILD)/receiver/Receiver/zephyr/zephyr.hex" "$(RECEIVER_HEX)"
	python3 "$(ROOT)/tools/dfu_zip.py" "$(RECEIVER_HEX)" "$(RECEIVER_HEX:.hex=.zip)"
	@echo "-> $(RECEIVER_HEX), $(RECEIVER_HEX:.hex=.zip)"

test:
	cd "$(ROOT)/tools" && python3 -m unittest test_slime_link

clean:
	rm -rf "$(BUILD)"
