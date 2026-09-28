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

.PHONY: all image busybox dropbear wifi test-host check clean
.DELETE_ON_ERROR:

all: image
image: $(IMAGE)
busybox: $(B)/busybox/busybox
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

ROOTFS_INPUTS := $(B)/init $(B)/tt7probe $(B)/busybox/busybox $(B)/dropbear/dropbearmulti \
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

test-host: $(B)/host/test_fbdraw $(B)/host/test_usb_stall
	$(B)/host/test_fbdraw
	$(B)/host/test_usb_stall

SHELL_SCRIPTS := scripts/flash-boot.sh scripts/backup-flash.sh scripts/build-busybox.sh \
                 scripts/build-dropbear.sh scripts/fetch-sources.sh scripts/stage-rootfs.sh \
                 scripts/build-wpa.sh scripts/wifi-setup.sh scripts/test-wifi-setup.sh
DEVICE_SCRIPTS := probe/tt7-app.sh probe/tt7-discover.sh probe/tt7-wifi-start.sh
# A system shellcheck if there is one, else the pinned PyPI build through uv.
SHELLCHECK := $(shell command -v shellcheck 2>/dev/null || echo "uvx --from shellcheck-py==0.11.0.1 shellcheck")

check: test-host $(IMAGE)
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
