# Builds the stock-based tracker and receiver images into dist/ and runs the PC tool tests.
# Run from WSL/Linux; flash from Windows (see README.md).
#
#   make              tracker and receiver
#   make tracker      ProMicro stacked SPI tracker (.uf2)
#   make tracker-i2c  ProMicro stacked I2C tracker (.uf2), for an IMU that only answers on I2C
#   make tracker-xiao Seeed XIAO nRF52840 Sense tracker (.uf2)
#   make tracker-ota  ProMicro stacked SPI tracker with MCUboot (docs/OTA.md): _OTA.uf2 (MCUboot +
#                     app, USB flash and recovery) and _OTA_app.bin (the image an update installs)
#   make tracker-ota-i2c   the same for the stacked I2C tracker
#   make tracker-ota-xiao  the same for the XIAO nRF52840 Sense
#   make tracker-ota-slot1 the SPI OTA app as a UF2 for the secondary slot, marked for a test swap
#                     (tests MCUboot over USB); with OTA_NOCONFIRM=1 an image that never confirms
#   make receiver     nRF52840 Dongle receiver (.hex for nRF Connect Programmer, .zip for nrfutil)
#   make ble-receiver ESP32-C6 BLE receiver for trackers in BLE mode (BleReceiver/, docs/BLE.md):
#                     one .bin from 0x0, flash with esptool (ESP-IDF v5.5 in ~/esp/esp-idf)
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

# The OTA build per board: OTA_LAYOUT picks Tracker/ota<layout>.overlay and
# Tracker/sysbuild/mcuboot<layout>.overlay (the pm_static file goes by the board, sysbuild.cmake)
OTA_BOARD     ?= promicro_uf2/nrf52840/stackedspi
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

.PHONY: all tracker tracker-i2c tracker-xiao receiver test clean
.PHONY: tracker-ota tracker-ota-i2c tracker-ota-xiao tracker-ota-slot1 ble-receiver

# ESP-IDF v5.5 for the ESP32-C6 BLE receiver. Its export.sh is not needed (and fails without
# openocd): the build only takes these.
IDF_PATH_DIR  ?= $(HOME)/esp/esp-idf
IDF_TOOLS     ?= $(HOME)/.espressif/tools
IDF_PYENV     ?= $(firstword $(wildcard $(HOME)/.espressif/python_env/idf5.5_py*_env))
IDF_BIN = $(firstword $(wildcard $(IDF_TOOLS)/riscv32-esp-elf/*/riscv32-esp-elf/bin)):$(firstword $(wildcard $(IDF_TOOLS)/cmake/*/bin)):$(firstword $(wildcard $(IDF_TOOLS)/ninja/*))
IDF_ENV = IDF_PATH="$(IDF_PATH_DIR)" IDF_PYTHON_ENV_PATH="$(IDF_PYENV)" PATH="$(IDF_BIN):$(IDF_PYENV)/bin:$(IDF_PATH_DIR)/tools:$$PATH"
BLE_RECEIVER_BIN := $(DIST)/SlimeNRF_ESP32C6_BleReceiver.bin

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

# MCUboot without signature (the image's SHA-256 is still checked), swap with rollback. Always
# pristine: the image version in MCUboot's header is only taken when the build is configured.
tracker-ota:
	@mkdir -p $(DIST)
	cd "$(NCS_TRACKER)" && $(ZEPHYR_ENV) FW_GIT_DESCRIBE="$(FW_GIT_DESCRIBE)" FW_GIT_BRANCH="$(FW_GIT_BRANCH)" 		west build --board $(OTA_BOARD) --pristine=always 		--build-dir "$(BUILD)/tracker-ota-$(OTA_NAME)" "$(ROOT)/Tracker" 		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(ROOT)/Tracker" 		-DSB_CONFIG_BOOTLOADER_MCUBOOT=y -DSB_CONFIG_MCUBOOT_MODE_SWAP_USING_MOVE=y 		-DSB_CONFIG_BOOT_SIGNATURE_TYPE_NONE=y -Dmcuboot_BOARD=nrf52840dk/nrf52840 		-Dmcuboot_DTC_OVERLAY_FILE="$(ROOT)/Tracker/sysbuild/mcuboot$(OTA_LAYOUT).overlay" 		-DTracker_EXTRA_CONF_FILE="$(OTA_CONF)" -DTracker_EXTRA_DTC_OVERLAY_FILE="$(ROOT)/Tracker/ota$(OTA_LAYOUT).overlay"
	python3 "$(UF2CONV)" -c -f $(UF2_FAMILY_NRF52840) -o "$(TRACKER_OTA_UF2)" "$(BUILD)/tracker-ota-$(OTA_NAME)/merged.hex"
	cp "$(BUILD)/tracker-ota-$(OTA_NAME)/Tracker/zephyr/zephyr.signed.bin" "$(TRACKER_OTA_BIN)"
	@echo "-> $(TRACKER_OTA_UF2), $(TRACKER_OTA_BIN)"

# Same flash layout as the SPI ProMicro
tracker-ota-i2c:
	$(MAKE) tracker-ota OTA_BOARD=promicro_uf2/nrf52840/stackedi2c OTA_NAME=ProMicro_StackedI2C

# The XIAO keeps its bootloader's SoftDevice area: MCUboot at 0x27000 (pm_static/mcuboot_xiao_ble.yml)
tracker-ota-xiao:
	$(MAKE) tracker-ota OTA_BOARD=xiao_ble/nrf52840/sense OTA_NAME=XIAO_Sense OTA_LAYOUT=_xiao

# The tracker-ota app padded to the secondary slot, with the trailer that asks MCUboot for a test
# swap on the next boot, as a UF2 for that slot (same imgtool arguments as the build, plus --pad)
tracker-ota-slot1: tracker-ota
	version=$$(grep -o -m1 -- '--version [^ ]*' "$(BUILD)/tracker-ota-$(OTA_NAME)/Tracker/build.ninja" | head -1 | cut -d' ' -f2); 	"$(WEST_VENV)/bin/python" "$(IMGTOOL)" sign --version "$$version" --align 4 --slot-size $(OTA_SLOT1_SIZE) 		--pad-header --header-size 0x200 --pad 		"$(BUILD)/tracker-ota-$(OTA_NAME)/Tracker/zephyr/zephyr.bin" "$(BUILD)/tracker-ota-$(OTA_NAME)/slot1_test.bin"
	python3 "$(UF2CONV)" -c -b $(OTA_SLOT1_ADDR) -f $(UF2_FAMILY_NRF52840) -o "$(TRACKER_OTA_SLOT1_UF2)" 		"$(BUILD)/tracker-ota-$(OTA_NAME)/slot1_test.bin"
	@echo "-> $(TRACKER_OTA_SLOT1_UF2)"

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

ble-receiver:
	@mkdir -p $(DIST)
	$(IDF_ENV) idf.py -C "$(ROOT)/BleReceiver" -B "$(BUILD)/ble-receiver" \
		-DSDKCONFIG="$(BUILD)/ble-receiver/sdkconfig" -DIDF_TARGET=esp32c6 build
	cd "$(BUILD)/ble-receiver" && $(IDF_ENV) python -m esptool --chip esp32c6 merge_bin -o "$(BLE_RECEIVER_BIN)" @flash_args
	@echo "-> $(BLE_RECEIVER_BIN) (python -m esptool --chip esp32c6 -p COMx write-flash 0x0 <it>)"

test:
	cd "$(ROOT)/tools" && python3 -m unittest test_slime_link test_quest_bridge test_remote_config test_ble_bridge

clean:
	rm -rf "$(BUILD)"
