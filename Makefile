# ABOUTME: Builds the TT7 probe boot image (the unit's stock kernel + our ramdisk) and checks it.
# ABOUTME: `make` -> build/tt7-probe-boot.img; `make check` runs every host-side verification.
#
# Nothing here talks to the panel. Flashing is scripts/flash-boot.sh, run by hand.

ZIG := $(shell mise which zig 2>/dev/null)
ifeq ($(ZIG),)
$(error zig not found via mise: run `mise install` (mise.toml pins zig 0.16.0))
endif
export ZIG
export PATH := $(CURDIR)/toolchain:$(PATH)

# This unit's own stock boot partition (git-ignored backup, verified byte-exact).
STOCK_BOOT ?= backup/tt7-stock-2026-09-27/03_boot.bin
# The only key allowed to log in as root.
SSH_PUBKEY ?= $(HOME)/.ssh/id_ed25519.pub
export SSH_PUBKEY

B         := build
IMAGE     := $(B)/tt7-probe-boot.img
MKBOOTIMG := third_party/mmkeypad/tools/mkbootimg.py
MKCPIO    := third_party/mmkeypad/tools/mkcpio.py
FONT_DIR  := third_party/mmkeypad/init

CROSS_CC     := arm-linux-musleabihf-cc
CROSS_CFLAGS := -std=c11 -D_GNU_SOURCE -static -Os -Wall -Wextra -Werror
HOST_CFLAGS  := -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined

# tt7d: the network display daemon. Modules shared by the daemon and its unit tests.
# TT7D_ASSETS_C embeds the control panel (tt7d/web/) and the test pattern; see its rule below.
TT7D_ASSETS_C := $(B)/gen/tt7d_assets.c
TT7D_LIB     := tt7d/json.c tt7d/http.c tt7d/render.c tt7d/sha256.c tt7d/ident.c tt7d/sysinfo.c probe/fbdraw.c \
                tt7d/control.c tt7d/hardware.c tt7d/assets.c $(TT7D_ASSETS_C) \
                tt7d/server.c tt7d/mqtt_packet.c tt7d/mqtt_config.c tt7d/mqtt_client.c tt7d/mqtt.c \
                tt7d/sha1.c tt7d/ws.c tt7d/touch.c tt7d/input.c
TT7D_SRCS    := $(TT7D_LIB) tt7d/display.c tt7d/frame.c tt7d/panel.c tt7d/events.c tt7d/main.c
TT7D_WEB     := tt7d/web/index.html tt7d/web/panel.css tt7d/web/panel.js
TT7D_HDRS    := $(wildcard tt7d/*.h) probe/fbdraw.h $(FONT_DIR)/font8x8_basic.h third_party/lodepng/lodepng.h
TT7D_INC     := -Itt7d -Iprobe -I$(FONT_DIR) -Ithird_party/lodepng
LODEPNG      := third_party/lodepng/lodepng.cpp
LODEPNG_DEFS := -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK -DLODEPNG_NO_COMPILE_CPP \
                -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
TT7D_VERSION := $(shell git describe --always --dirty 2>/dev/null || echo unknown)
TT7D_UNITS   := render json http util sysinfo control hardware assets mqtt ws input

.PHONY: all image busybox dropbear wifi tt7d test-host test-e2e test-mqtt test-input check clean FORCE
.DELETE_ON_ERROR:

all: image
image: $(IMAGE)
busybox: $(B)/busybox/busybox
# Also usable alone: copy build/tt7d to /data/tt7/bin on a running panel (tt7d/README.md).
tt7d: $(B)/tt7d
dropbear: $(B)/dropbear/dropbearmulti
# Also usable alone: copy build/wifi/* to /data/tt7/bin on a running panel.
wifi: $(B)/wifi/wpa_supplicant

$(B)/busybox/busybox: config/busybox.config scripts/build-busybox.sh scripts/fetch-sources.sh
	scripts/build-busybox.sh

$(B)/dropbear/dropbearmulti: config/dropbear-localoptions.h scripts/build-dropbear.sh scripts/fetch-sources.sh
	scripts/build-dropbear.sh

$(B)/wifi/wpa_supplicant: scripts/build-wpa.sh scripts/fetch-sources.sh
	scripts/build-wpa.sh

$(B)/init: third_party/mmkeypad/init/init.c init/usb_stall.c init/usb_stall.h
	@mkdir -p $(B)
	$(CROSS_CC) -static -Os -Wall -Werror -Iinit -o $@ third_party/mmkeypad/init/init.c init/usb_stall.c

# Rewritten only when `git describe` changes, so both tt7d builds relink with
# the build string that /api/v1/info reports.
$(B)/tt7d.version: FORCE
	@mkdir -p $(B)
	@echo '$(TT7D_VERSION)' | cmp -s - $@ || echo '$(TT7D_VERSION)' > $@
FORCE:

# The built-in test pattern: the same orientation/colour frame as tools/make-test-frame.py,
# with fixed text in place of the time so the same source gives the same binary.
$(B)/gen/test-pattern.png: tools/make-test-frame.py $(FONT_DIR)/font8x8_basic.h
	@mkdir -p $(B)/gen
	python3 tools/make-test-frame.py $@ --label "tt7d test pattern" --stamp "BUILT-IN" > /dev/null

# Files compiled into tt7d: URL paths start with '/', internal names do not.
$(TT7D_ASSETS_C): tt7d/embed.py $(TT7D_WEB) $(B)/gen/test-pattern.png
	@mkdir -p $(B)/gen
	python3 tt7d/embed.py $@ /=tt7d/web/index.html /panel.css=tt7d/web/panel.css /panel.js=tt7d/web/panel.js \
		test-pattern.png=$(B)/gen/test-pattern.png

# The display daemon for the panel. lodepng is compiled as C (third_party/lodepng/PROVENANCE).
$(B)/tt7d: $(TT7D_SRCS) $(TT7D_HDRS) $(LODEPNG) $(B)/tt7d.version
	@mkdir -p $(B)
	$(CROSS_CC) $(CROSS_CFLAGS) $(TT7D_INC) $(LODEPNG_DEFS) -DTT7D_VERSION='"$(TT7D_VERSION)"' -o $@ \
		$(TT7D_SRCS) -x c $(LODEPNG) -x none

$(B)/tt7probe: probe/tt7probe.c probe/fbdraw.c probe/fbdraw.h $(FONT_DIR)/font8x8_basic.h
	@mkdir -p $(B)
	$(CROSS_CC) $(CROSS_CFLAGS) -Iprobe -I$(FONT_DIR) -o $@ probe/tt7probe.c probe/fbdraw.c

# Stock kernel + the NAND module from the stock ramdisk. Vendor binaries: they
# live only under build/ and are never committed.
$(B)/stock/kernel.img: $(STOCK_BOOT) scripts/bootimg.py $(MKBOOTIMG)
	python3 scripts/bootimg.py verify $(STOCK_BOOT)
	rm -rf $(B)/stock && mkdir -p $(B)/stock/ramdisk
	python3 $(MKBOOTIMG) unpack $(STOCK_BOOT) $(B)/stock
	cd $(B)/stock/ramdisk && gzip -dc ../ramdisk.cpio.gz | cpio -id --quiet 'rk30xxnand_ko.ko.3.0.36+'

ROOTFS_INPUTS := $(B)/init $(B)/tt7probe $(B)/tt7d $(B)/busybox/busybox $(B)/dropbear/dropbearmulti \
                 $(B)/wifi/wpa_supplicant $(B)/stock/kernel.img \
                 probe/tt7-app.sh probe/tt7-discover.sh probe/tt7-wifi-start.sh \
                 scripts/stage-rootfs.sh $(SSH_PUBKEY) $(shell find third_party/mmkeypad/rootfs -type f)

$(B)/ramdisk.cpio.gz: $(ROOTFS_INPUTS) $(MKCPIO)
	scripts/stage-rootfs.sh
	python3 $(MKCPIO) $(B)/rootfs $(B)/ramdisk.cpio
	gzip -9 -n -c $(B)/ramdisk.cpio > $@

$(IMAGE): $(B)/ramdisk.cpio.gz $(B)/stock/kernel.img $(MKBOOTIMG) scripts/bootimg.py
	python3 $(MKBOOTIMG) pack --kernel $(B)/stock/kernel.img --ramdisk $(B)/ramdisk.cpio.gz \
		--header-from $(STOCK_BOOT) -o $@
	python3 scripts/bootimg.py verify $@
	sha256sum $@

$(B)/host/test_fbdraw: probe/test_fbdraw.c probe/fbdraw.c probe/fbdraw.h $(FONT_DIR)/font8x8_basic.h
	@mkdir -p $(B)/host
	gcc $(HOST_CFLAGS) -Iprobe -I$(FONT_DIR) -o $@ probe/test_fbdraw.c probe/fbdraw.c

$(B)/host/test_usb_stall: init/test_usb_stall.c init/usb_stall.c init/usb_stall.h
	@mkdir -p $(B)/host
	gcc $(HOST_CFLAGS) -Iinit -o $@ init/test_usb_stall.c init/usb_stall.c

$(B)/host/test_%: tt7d/test_%.c tt7d/test_common.h $(TT7D_LIB) $(TT7D_HDRS)
	@mkdir -p $(B)/host
	gcc $(HOST_CFLAGS) -D_GNU_SOURCE $(TT7D_INC) -DFIXTURE='"tt7d/test/fixtures/sysfs-tt7"' -o $@ $< $(TT7D_LIB)

# Host build of the real daemon for the end-to-end test. lodepng is compiled
# as C (see third_party/lodepng/PROVENANCE).
$(B)/host/lodepng.o: $(LODEPNG) third_party/lodepng/lodepng.h
	@mkdir -p $(B)/host
	gcc $(HOST_CFLAGS) $(LODEPNG_DEFS) -x c -c -o $@ $<

$(B)/host/tt7d: $(TT7D_SRCS) $(TT7D_HDRS) $(B)/host/lodepng.o $(B)/tt7d.version
	gcc $(HOST_CFLAGS) -D_GNU_SOURCE $(TT7D_INC) $(LODEPNG_DEFS) -DTT7D_VERSION='"$(TT7D_VERSION)"' \
		-o $@ $(TT7D_SRCS) $(B)/host/lodepng.o

test-host: $(B)/host/test_fbdraw $(B)/host/test_usb_stall $(TT7D_UNITS:%=$(B)/host/test_%)
	$(B)/host/test_fbdraw
	$(B)/host/test_usb_stall
	@for t in $(TT7D_UNITS); do $(B)/host/test_$$t || exit 1; done

# Runs the real host-built daemon on a file-backed framebuffer (tt7d/test_e2e.py).
test-e2e: $(B)/host/tt7d
	python3 tt7d/test_e2e.py --daemon $(B)/host/tt7d

# The real daemon against a real MQTT broker (amqtt) and client (paho-mqtt),
# both pinned and run through uv (tt7d/test_mqtt_e2e.py).
MQTT_TEST_DEPS := --with amqtt==0.12.1 --with paho-mqtt==2.1.0
test-mqtt: $(B)/host/tt7d
	uv run --no-project --quiet $(MQTT_TEST_DEPS) python tt7d/test_mqtt_e2e.py --daemon $(B)/host/tt7d

# Input: real input_event records through FIFOs into the real daemon, events
# out over its WebSocket (and tools/events.py), buttons to a real amqtt broker.
test-input: $(B)/host/tt7d
	uv run --no-project --quiet $(MQTT_TEST_DEPS) python tt7d/test_input_e2e.py --daemon $(B)/host/tt7d

SHELL_SCRIPTS := scripts/flash-boot.sh scripts/backup-flash.sh scripts/build-busybox.sh \
                 scripts/build-dropbear.sh scripts/fetch-sources.sh scripts/stage-rootfs.sh \
                 scripts/build-wpa.sh scripts/wifi-setup.sh scripts/test-wifi-setup.sh \
                 tools/push-frame.sh tools/mqtt-setup.sh
DEVICE_SCRIPTS := probe/tt7-app.sh probe/tt7-discover.sh probe/tt7-wifi-start.sh
# A system shellcheck if there is one, else the pinned PyPI build through uv.
SHELLCHECK := $(shell command -v shellcheck 2>/dev/null || echo "uvx --from shellcheck-py==0.11.0.1 shellcheck")

check: test-host test-e2e test-mqtt test-input $(IMAGE)
	python3 scripts/check-image.py --image $(IMAGE) --stock $(STOCK_BOOT) --pubkey $(SSH_PUBKEY)
	@for s in $(SHELL_SCRIPTS); do bash -n $$s || exit 1; done; echo "  ok   bash -n: $(SHELL_SCRIPTS)"
	@for s in $(DEVICE_SCRIPTS); do sh -n $$s || exit 1; done; echo "  ok   sh -n: $(DEVICE_SCRIPTS)"
	@$(SHELLCHECK) $(SHELL_SCRIPTS) $(DEVICE_SCRIPTS) toolchain/* && echo "  ok   shellcheck"
	scripts/flash-boot.sh --self-test
	scripts/test-wifi-setup.sh
	@echo "make check: all passed"

# Keeps build/known_hosts (the panel's pinned host key) and build/flash-*/
# (read-backs from real flashes); everything else under build/ is regenerated.
clean:
	@mkdir -p $(B)
	find $(B) -mindepth 1 -maxdepth 1 ! -name known_hosts ! -name 'flash-*' -exec rm -rf {} +
