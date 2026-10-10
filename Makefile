# SlimeVR-Hub: builds the tracker, receiver and ESP32 hub firmware, and runs the host tests.
# Run from WSL/Linux. Flash the images in dist/ from Windows (see README.md).
#
#   make            build everything into dist/
#   make tracker    tracker image (wired mode / relay mode)
#   make tracker-nosleep  same, with power saving off (debug, see DEBUG_NO_POWER_SAVING)
#   make receiver   receiver image (UART mirror for the hub)
#   make receiver-xiao  same, for a Seeed XIAO nRF52840 (Sense), mirror on D6
#   make receiver-dongle-diag  stock receiver + link diagnostics ("diag on"), nRF52840 Dongle .hex
#                   (upstream at RECEIVER_STOCK_REV plus patches/receiver-stock/*.patch)
#   make hub        ESP32-C6 hub image      make hub-s3   ESP32-S3 hub image
#   make test       host tests (hub decoder/framing, tracker wired link)
#   make clean      remove the build directories
#
# Toolchain locations can be overridden, e.g. make NCS_TRACKER=/opt/ncs-v3.1 tracker

ROOT          := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
DIST          := $(ROOT)/dist
BUILD         ?= $(HOME)/build/slimevr-hub

# nRF Connect SDK workspaces (west.yml of each repo pins a different NCS version)
NCS_TRACKER   ?= $(HOME)/ncs-tracker
NCS_RECEIVER  ?= $(HOME)/ncs-ws
ZEPHYR_SDK    ?= $(HOME)/zephyr-sdk-0.17.4
WEST_VENV     ?= $(HOME)/ncs-ws/.venv
HOST_TOOLS    ?= $(HOME)/.local/xz/usr/bin:$(HOME)/.espressif/tools/cmake/3.30.2/bin:$(HOME)/.espressif/tools/ninja/1.12.1
TRACKER_BOARD ?= promicro_uf2/nrf52840/stackedspiwired
RECEIVER_BOARD?= promicro_uf2/nrf52840/stacked
PRISTINE      ?= auto

# ESP-IDF
IDF_PATH      ?= $(HOME)/esp/esp-idf
IDF_LIBS      ?= $(HOME)/.local/libusb/usr/lib/x86_64-linux-gnu

# Host C compiler for the tests: cc/gcc, or the zig compiler installed with uv
ZIG           ?= $(firstword $(wildcard $(HOME)/.local/share/uv/tools/ziglang/lib/python3*/site-packages/ziglang/zig))
HOST_CC       ?= $(or $(shell command -v cc 2>/dev/null),$(shell command -v gcc 2>/dev/null),$(if $(ZIG),$(ZIG) cc))

ZEPHYR_ENV = PATH="$(HOST_TOOLS):$(WEST_VENV)/bin:$$PATH" ZEPHYR_SDK_INSTALL_DIR="$(ZEPHYR_SDK)" ZEPHYR_TOOLCHAIN_VARIANT=zephyr
IDF_RUN    = LD_LIBRARY_PATH="$(IDF_LIBS)" bash -c '. "$(IDF_PATH)/export.sh" >/dev/null && cd "$(ROOT)/src/Hub" && $(1)'

TRACKER_UF2  := $(DIST)/SlimeNRF_ProMicro_StackedSPI_Wired_Tracker.uf2
TRACKER_NOSLEEP_UF2 := $(DIST)/SlimeNRF_ProMicro_StackedSPI_Wired_Tracker_NoPowerSave.uf2
RECEIVER_UF2 := $(DIST)/SlimeNRF_ProMicro_HubReceiver.uf2
RECEIVER_XIAO_UF2 := $(DIST)/SlimeNRF_XIAO_nRF52840_HubReceiver.uf2
RECEIVER_DONGLE_DIAG_HEX := $(DIST)/SlimeNRF_Nordic_Dongle_Receiver_Diag.hex

# Stock receiver source for receiver-dongle-diag
RECEIVER_STOCK_URL ?= https://github.com/SlimeVR/SlimeVR-Tracker-nRF-Receiver.git
RECEIVER_STOCK_REV ?= e3d66d5
RECEIVER_STOCK_SRC := $(BUILD)/receiver-stock-src
HUB_C6_BIN   := $(DIST)/slime_hub-xiao-esp32c6.bin
HUB_S3_BIN   := $(DIST)/slime_hub-xiao-esp32s3.bin

.PHONY: all tracker tracker-nosleep receiver receiver-xiao receiver-dongle-diag hub hub-s3 test test-hub test-tracker clean help

all: tracker receiver hub

help:
	@sed -n '2,13p' $(lastword $(MAKEFILE_LIST)) | sed 's/^# \{0,1\}//'

tracker:
	@mkdir -p $(DIST)
	cd "$(NCS_TRACKER)" && $(ZEPHYR_ENV) west build --board $(TRACKER_BOARD) --pristine=$(PRISTINE) \
		--build-dir "$(BUILD)/tracker" "$(ROOT)/src/Tracker" \
		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(ROOT)/src/Tracker"
	cp "$(BUILD)/tracker/Tracker/zephyr/zephyr.uf2" "$(TRACKER_UF2)"
	@echo "-> $(TRACKER_UF2)"

tracker-nosleep:
	@mkdir -p $(DIST)
	cd "$(NCS_TRACKER)" && $(ZEPHYR_ENV) west build --board $(TRACKER_BOARD) --pristine=$(PRISTINE) \
		--build-dir "$(BUILD)/tracker-nosleep" "$(ROOT)/src/Tracker" \
		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(ROOT)/src/Tracker" -DCONFIG_DEBUG_NO_POWER_SAVING=y
	cp "$(BUILD)/tracker-nosleep/Tracker/zephyr/zephyr.uf2" "$(TRACKER_NOSLEEP_UF2)"
	@echo "-> $(TRACKER_NOSLEEP_UF2)"

receiver:
	@mkdir -p $(DIST)
	cd "$(NCS_RECEIVER)" && $(ZEPHYR_ENV) west build --board $(RECEIVER_BOARD) --pristine=$(PRISTINE) \
		--build-dir "$(BUILD)/receiver" "$(ROOT)/src/Receiver" \
		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(ROOT)/src/Receiver"
	cp "$(BUILD)/receiver/Receiver/zephyr/zephyr.uf2" "$(RECEIVER_UF2)"
	@echo "-> $(RECEIVER_UF2)"

receiver-xiao:
	@mkdir -p $(DIST)
	cd "$(NCS_RECEIVER)" && $(ZEPHYR_ENV) west build --board xiao_ble/nrf52840 --pristine=$(PRISTINE) \
		--build-dir "$(BUILD)/receiver-xiao" "$(ROOT)/src/Receiver" \
		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(ROOT)/src/Receiver" \
		-DEXTRA_DTC_OVERLAY_FILE="$(ROOT)/src/Receiver/boards/xiao_ble_hub.overlay" -DCONFIG_HUB_UART_MIRROR=y
	cp "$(BUILD)/receiver-xiao/Receiver/zephyr/zephyr.uf2" "$(RECEIVER_XIAO_UF2)"
	@echo "-> $(RECEIVER_XIAO_UF2)"

# The dongle board's own USB console (new USB stack) clashes with the receiver's legacy one in
# this SDK: BOARD_SERIAL_BACKEND_CDC_ACM off. Flash the .hex with nRF Connect Programmer, or the
# .zip with nrfutil: "dfu" on the receiver's console (bootloader), then
#   nrfutil device program --firmware <zip> --serial-number <bootloader's serial number>
receiver-dongle-diag:
	@mkdir -p $(DIST)
	test -d "$(RECEIVER_STOCK_SRC)/.git" || git clone -q "$(RECEIVER_STOCK_URL)" "$(RECEIVER_STOCK_SRC)"
	git -C "$(RECEIVER_STOCK_SRC)" checkout -q -f $(RECEIVER_STOCK_REV)
	git -C "$(RECEIVER_STOCK_SRC)" clean -qfdx
	git -C "$(RECEIVER_STOCK_SRC)" -c user.name=build -c user.email=build@localhost am -q "$(ROOT)"/patches/receiver-stock/*.patch
	cd "$(NCS_RECEIVER)" && $(ZEPHYR_ENV) west build --board nrf52840dongle/nrf52840 --pristine=always 		--build-dir "$(BUILD)/receiver-dongle-diag" "$(RECEIVER_STOCK_SRC)" 		-- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT="$(RECEIVER_STOCK_SRC)" -DCONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=n
	cp "$(BUILD)/receiver-dongle-diag/receiver-stock-src/zephyr/zephyr.hex" "$(RECEIVER_DONGLE_DIAG_HEX)"
	python3 "$(ROOT)/tools/dfu_zip.py" "$(RECEIVER_DONGLE_DIAG_HEX)" "$(RECEIVER_DONGLE_DIAG_HEX:.hex=.zip)"
	@echo "-> $(RECEIVER_DONGLE_DIAG_HEX) (nRF Connect Programmer), $(RECEIVER_DONGLE_DIAG_HEX:.hex=.zip) (nrfutil device program)"

hub:
	@mkdir -p $(DIST)
	$(call IDF_RUN,idf.py -B "$(BUILD)/hub-c6" build && idf.py -B "$(BUILD)/hub-c6" merge-bin -o merged.bin)
	cp "$(BUILD)/hub-c6/merged.bin" "$(HUB_C6_BIN)"
	@echo "-> $(HUB_C6_BIN)"

hub-s3:
	@mkdir -p $(DIST)
	$(call IDF_RUN,idf.py -B "$(BUILD)/hub-s3" -D SDKCONFIG="$(BUILD)/hub-s3/sdkconfig" -D IDF_TARGET=esp32s3 build \
		&& idf.py -B "$(BUILD)/hub-s3" -D SDKCONFIG="$(BUILD)/hub-s3/sdkconfig" -D IDF_TARGET=esp32s3 merge-bin -o merged.bin)
	cp "$(BUILD)/hub-s3/merged.bin" "$(HUB_S3_BIN)"
	@echo "-> $(HUB_S3_BIN)"

test: test-hub test-tracker

test-hub:
	CC="$(HOST_CC)" "$(ROOT)/src/Hub/test/run.sh"

test-tracker:
	CC="$(HOST_CC)" "$(ROOT)/src/Tracker/tools/wired_test/run.sh" "$(NCS_TRACKER)/zephyr"

clean:
	rm -rf "$(BUILD)"
