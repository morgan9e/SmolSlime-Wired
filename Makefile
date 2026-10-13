# Builds the stock-based tracker and receiver images into dist/ and runs the PC tool tests.
# Run from WSL/Linux; flash from Windows (see README.md).
#
#   make              tracker and receiver
#   make tracker      ProMicro stacked SPI tracker (.uf2)
#   make tracker-i2c  ProMicro stacked I2C tracker (.uf2), for an IMU that only answers on I2C
#   make tracker-xiao Seeed XIAO nRF52840 Sense tracker (.uf2)
#   make receiver     nRF52840 Dongle receiver (.hex for nRF Connect Programmer, .zip for nrfutil)
#   make imu-probe    ProMicro IMU-on-SPI probe (ImuProbe/README.md)
#   make test         PC tool tests
#   make clean        remove the build directories
#
# Toolchain locations can be overridden, e.g. make NCS_TRACKER=/opt/ncs-v3.1 tracker

ROOT          := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
DIST          ?= $(ROOT)/dist
BUILD         ?= $(HOME)/build/slimevr-stock

# nRF Connect SDK workspaces: the tracker builds with v3.1 (as upstream's releases), the receiver
# needs v3.3 (its timer code uses newer nrfx)
NCS_TRACKER   ?= $(HOME)/ncs-tracker
NCS_RECEIVER  ?= $(HOME)/ncs-ws
ZEPHYR_SDK    ?= $(HOME)/zephyr-sdk-0.17.4
WEST_VENV     ?= $(HOME)/ncs-ws/.venv
HOST_TOOLS    ?= $(HOME)/.local/xz/usr/bin:$(HOME)/.espressif/tools/cmake/3.30.2/bin:$(HOME)/.espressif/tools/ninja/1.12.1
RECEIVER_BOARD?= nrf52840dongle/nrf52840
PRISTINE      ?= auto
TRACKER_EXTRA_CONF ?=

ZEPHYR_ENV = PATH="$(HOST_TOOLS):$(WEST_VENV)/bin:$$PATH" ZEPHYR_SDK_INSTALL_DIR="$(ZEPHYR_SDK)" ZEPHYR_TOOLCHAIN_VARIANT=zephyr

# The tracker's version comes from `git describe` (tags like 0.0.3, see tools/bump_version.py),
# handed to its build in FW_GIT_DESCRIBE / FW_GIT_BRANCH. In WSL, Linux git cannot read a worktree
# that Windows git created (its .git file holds a Windows path): git.exe then.
GIT ?= $(shell git -C "$(ROOT)" rev-parse --git-dir >/dev/null 2>&1 && command -v git || command -v git.exe || echo "/mnt/c/Program Files/Git/cmd/git.exe")
FW_GIT_DESCRIBE = $(shell cd "$(ROOT)" && "$(GIT)" describe --dirty --tags --long --match '[0-9]*.[0-9]*.[0-9]*' 2>/dev/null | tr -d '\r')
FW_GIT_BRANCH = $(shell cd "$(ROOT)" && "$(GIT)" branch --show-current 2>/dev/null | tr -d '\r')

RECEIVER_HEX  := $(DIST)/SlimeNRF_Nordic_Dongle_Receiver.hex

.PHONY: all tracker tracker-i2c tracker-xiao receiver imu-probe test clean

all: tracker receiver

# $(call tracker_build,board target,build dir name,output name)
define tracker_build
	@mkdir -p $(DIST)
	cd "$(NCS_TRACKER)" && $(ZEPHYR_ENV) FW_GIT_DESCRIBE="$(FW_GIT_DESCRIBE)" FW_GIT_BRANCH="$(FW_GIT_BRANCH)" \
		west build --board $(1) --pristine=$(PRISTINE) \
		--build-dir "$(BUILD)/$(2)" "$(ROOT)/Tracker" \
		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(ROOT)/Tracker" \
		$(if $(TRACKER_EXTRA_CONF),-DTracker_EXTRA_CONF_FILE="$(TRACKER_EXTRA_CONF)")
	cp "$(BUILD)/$(2)/Tracker/zephyr/zephyr.uf2" "$(DIST)/$(3)"
	@echo "-> $(DIST)/$(3)"
endef

tracker:
	$(call tracker_build,promicro_uf2/nrf52840/stackedspi,tracker,SlimeNRF_ProMicro_StackedSPI_Tracker.uf2)

tracker-i2c:
	$(call tracker_build,promicro_uf2/nrf52840/stackedi2c,tracker-i2c,SlimeNRF_ProMicro_StackedI2C_Tracker.uf2)

tracker-xiao:
	$(call tracker_build,xiao_ble/nrf52840/sense,tracker-xiao,SlimeNRF_XIAO_Sense_Tracker.uf2)

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

# Linked at 0x1000 like the tracker (ImuProbe/pm_static.yml); see ImuProbe/README.md
imu-probe:
	@mkdir -p $(DIST)
	cd "$(NCS_TRACKER)" && $(ZEPHYR_ENV) west build --board promicro_uf2/nrf52840 --pristine=$(PRISTINE) \
		--build-dir "$(BUILD)/imu-probe" "$(ROOT)/ImuProbe" \
		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(ROOT)/Tracker"
	cp "$(BUILD)/imu-probe/ImuProbe/zephyr/zephyr.uf2" "$(DIST)/SlimeNRF_ProMicro_IMU_Probe.uf2"
	@echo "-> $(DIST)/SlimeNRF_ProMicro_IMU_Probe.uf2"

test:
	cd "$(ROOT)/tools" && python3 -m unittest test_slime_link test_quest_bridge

clean:
	rm -rf "$(BUILD)"
