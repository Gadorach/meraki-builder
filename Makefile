SHELL := /usr/bin/env bash
.DEFAULT_GOAL := help

JOBS ?= $(shell nproc 2>/dev/null || echo 1)
export JOBS

.PHONY: help all base web doctor deps sources kernel loader donor ui prepare rootfs image \
        validate verify-inputs test-fwupdate test-image test-modules test-hardware test-docs test-ui-contract test-loader-contract verify-modules test-all menuconfig distrobox clean distclean print-config \
        mx80 mx80-prepare mx80-validate mx80-check mx80-menuconfig mx80-clean mx80-distclean mx84-check

help:
	@printf '%s\n' \
	  'MS42P/postmerkOS build targets' \
	  '' \
	  '  make all          Interactive complete build; prompts for optional UI' \
	  '  make base         Complete build without the web UI' \
	  '  make web          Complete build with Gadorach/postmerkos-ui ms42p-dev' \
	  '  make doctor       Check host tools and local build state' \
	  '  make deps         Install dependencies for the current distribution' \
	  '  make sources      Clone/select kernel/OpenWrt and latest meraki-redboot main' \
	  '  make kernel       Build the OpenWrt toolchain and Linux 3.18 kernel' \
	  '  make loader       Build meraki-redboot, its embedded recovery stages, and payload packer from source' \
	  '  make donor        Download/extract the donor switch module tree' \
	  '  make ui           Clone/update and compile postmerkos-ui' \
	  '  make verify-inputs Validate and hash all local binary build inputs' \
	  '  make prepare      Download/configure Buildroot and generated overlays' \
	  '  make rootfs       Build SquashFS and the complete 16 MiB NOR image' \
	  '  make validate     Validate the most recent image and rootfs contents' \
	  '  make test-fwupdate Run updater, UART, flasher, and packaging tests' \
	  '  make test-image    Verify full-capacity SquashFS image construction' \
	  '  make test-hardware Verify board identity and hardware capability policy' \
	  '  make test-docs     Check repository Markdown links' \
	  '  make test-ui-contract UI_DIR=../postmerkos-ui checks browser/configd methods' \
	  '  make test-loader-contract Check authoritative upstream source policy' \
	  '  make test-all      Run all builder host-side validation targets' \
	  '  make test-modules  Test complete multi-platform module staging and boot selection' \
	  '  make verify-modules Verify all platform modules in ROOTFS=artifacts/rootfs.squashfs' \
	  '  make menuconfig   Open Buildroot menuconfig after preparation' \
	  '  make distrobox    Run the complete build in Ubuntu 22.04 distrobox' \
	  '' \
	  'MX appliance targets:' \
	  '  make mx80         Prepare, build, validate, and publish the MX80 image' \
	  '  make mx80-prepare Prepare the Buildroot 2020.02.8 MX80 workspace' \
	  '  make mx80-check   Check MX80 source/build prerequisites' \
	  '  make mx80-validate Validate an existing MX80 UBI image' \
	  '  make mx80-menuconfig Open the MX80 Buildroot menuconfig' \
	  '  make mx80-clean   Remove MX80 build output but keep downloads' \
	  '  make mx80-distclean Remove the complete MX80 work tree' \
	  '  make mx84-check   Report available and missing MX84 build inputs' \
	  '' \
	  '  make clean        Remove generated Buildroot output, keep downloads' \
	  '  make distclean    Remove .work and artifacts completely' \
	  '' \
	  'Useful variables:' \
	  '  JOBS=8 INCLUDE_UI=1 DONOR_IMAGE=/path/file.bin LOADER_REF=main UI_REF=ms42p-dev' \
	  '  USE_DISTROBOX=1 NONINTERACTIVE=1 AUTO_DOWNLOAD_DONOR=1 LOADER_BUILD_MODE=auto'

all:
	@./scripts/build-all.sh

base:
	@INCLUDE_UI=0 ./scripts/build-all.sh

web:
	@INCLUDE_UI=1 ./scripts/build-all.sh

doctor:
	@./scripts/doctor.sh

deps:
	@./scripts/install-deps.sh

sources:
	@./scripts/prepare-sources.sh

kernel:
	@./scripts/build-kernel.sh

loader:
	@./scripts/build-loader.sh

donor:
	@./scripts/prepare-donor.sh

ui:
	@./scripts/build-ui.sh

verify-inputs:
	@./scripts/verify-inputs.sh

prepare:
	@./scripts/prepare-buildroot.sh

rootfs: prepare
	@./scripts/build-rootfs.sh

image: prepare
	@./scripts/build-rootfs.sh
	@./scripts/validate-image.sh

validate:
	@./scripts/validate-image.sh

test-fwupdate:
	@./buildroot/packages/fwupdate/tests/test-host.sh
	@./buildroot/packages/fwupdate/tests/test-uart.sh
	@./tools/fwupdate-smoke-test/host-smoke-test.sh
	@python3 -m unittest discover -s tools/firmware-flasher/tests -p 'test_*.py' -v
	@./tools/firmware-flasher/tests/test_flasher_control_flow.sh

test-image:
	@./buildroot/board/meraki/ms220/tests/test-post-image.sh
	@python3 ./scripts/tests/test-artifact-manifest.py
	@python3 ./scripts/tests/test-build-cache-contract.py
	@python3 ./scripts/tests/test-pipefail-feature-probe.py

test-modules:
	@./scripts/tests/test-vendor-modules.sh

verify-modules:
	@./scripts/verify-vendor-modules-in-rootfs.sh "$${ROOTFS:-artifacts/rootfs.squashfs}"


test-hardware:
	@./buildroot/board/meraki/ms220/tests/test-board-identity.sh
	@./buildroot/board/meraki/ms220/tests/test-poe-init.sh
	@./buildroot/board/meraki/ms220/tests/test-configd-supervisor.sh
	@./buildroot/board/meraki/ms220/tests/test-configd-init.sh
	@./buildroot/packages/postmerkos-hardware/tests/test-host.sh

test-docs:
	@./scripts/tests/test-doc-links.py

test-ui-contract:
	@./scripts/tests/test-ui-configd-contract.py "$${UI_DIR:-../postmerkos-ui}"

test-loader-contract:
	@python3 ./scripts/tests/test-loader-source-contract.py

test-all: test-fwupdate test-image test-modules test-hardware test-docs test-ui-contract test-loader-contract
	@./buildroot/packages/postmerkos-console/tests/test-host.sh
	@$(MAKE) -C buildroot/packages/configd test-host

menuconfig: prepare
	@bash -c 'source ./scripts/common.sh; $(MAKE) -C "$$BUILDROOT_DIR" menuconfig'

distrobox:
	@./scripts/distrobox-run.sh env INCLUDE_UI="$${INCLUDE_UI:-ask}" CLEAN_BUILDROOT="$${CLEAN_BUILDROOT:-0}" ./scripts/build-all.sh

clean:
	@./scripts/clean.sh build

distclean:
	@./scripts/clean.sh all

print-config:
	@./scripts/doctor.sh || true
	@printf '\nBuildroot config: %s\n' '.work/build/buildroot-2023.02.4/.config'

mx80:
	@./scripts/mx80/build.sh

mx80-prepare:
	@./scripts/mx80/prepare.sh

mx80-validate:
	@./scripts/mx80/validate.py "$${MX80_IMAGE:-.work/mx80/buildroot-2020.02.8/output/images/ubi_image.bin}"

mx80-check:
	@./scripts/mx80/check.sh

mx80-menuconfig:
	@./scripts/mx80/menuconfig.sh

mx80-clean:
	@./scripts/mx80/clean.sh build

mx80-distclean:
	@./scripts/mx80/clean.sh all

mx84-check:
	@./scripts/mx84-check.sh
