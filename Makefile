.DEFAULT_GOAL := all
CC := gcc
LD := ld
PYTHON ?= python3
# Local build-only dependencies may also be installed with tools/bootstrap.sh.
export PYTHONPATH := $(CURDIR)/.tools/python:$(PYTHONPATH)
CFLAGS := -m32 -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -msoft-float -Wall -Wextra -Werror -Ikernel -Ibuild
UACPI_FLAGS := -Ithird_party/uacpi/include -DUACPI_REDUCED_HARDWARE -DUACPI_DEFAULT_LOG_LEVEL=UACPI_LOG_WARN -DUACPI_DEFAULT_LOOP_TIMEOUT_SECONDS=3 -DUACPI_DEFAULT_MAX_CALL_STACK_DEPTH=64
UACPI_OBJECTS := $(patsubst third_party/uacpi/source/%.c,build/uacpi/%.o,$(wildcard third_party/uacpi/source/*.c))
SOURCES := $(wildcard kernel/*.c drivers/*.c lib/*.c ui/*.c apps/*.c net/*.c net/wireless/*.c)
MUSLINC := -nostdinc -isystem third_party/musl/include -isystem third_party/musl/arch/i386 -isystem third_party/musl/arch/generic -isystem third_party/musl/obj/include -isystem $(shell $(CC) -print-file-name=include)
LIBGCC32 := $(firstword $(wildcard .tools/usr/lib/gcc/*/*/32/libgcc.a /usr/lib/gcc/*/*/32/libgcc.a))
NATIVEFLAGS := -fexcess-precision=standard -m32 -Os -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -ffunction-sections -fdata-sections -mno-sse -mfpmath=387 $(MUSLINC) -Ikernel -Ibuild -Ithird_party/quickjs
BEAR_SOURCES := $(shell find third_party/bearssl/src -name "*.c" | sort)
BEAR_OBJECTS := $(patsubst third_party/bearssl/%.c,build/bearssl/%.o,$(BEAR_SOURCES))
JS_OBJECTS := $(addprefix build/quickjs/,$(addsuffix .o,quickjs libregexp libunicode cutils dtoa))
OBJECTS := $(patsubst %.c,build/%.o,$(SOURCES)) build/kernel/entry.o build/apps/user.o $(JS_OBJECTS) $(UACPI_OBJECTS)
$(OBJECTS) $(BEAR_OBJECTS): Makefile
$(patsubst %.c,build/%.o,$(SOURCES)): $(wildcard kernel/*.h ui/*.h drivers/*.h include/net/*.h)
.PHONY: test-rtw8822be-rx-dma test-wifi-scan all clean run run-iso iso test test-iso-install check test-wifi-port test-wireless-frames test-wifi-runtime test-rtw8822be-diag test-rtw8822b-native test-rtw8822b-firmware
all: dist/LunaOS.img dist/LunaOS.iso
build/font.h: tools/fontgen.py assets/fonts/InterTight.ttf
	$(PYTHON) tools/fontgen.py
build/notices.h: tools/notices.py assets/fonts/OFL.txt assets/icons/lucide/LICENSE $(wildcard assets/licenses/*) third_party/quickjs/LICENSE third_party/musl/COPYRIGHT third_party/bearssl/LICENSE.txt third_party/stb/LICENSE third_party/uacpi/LICENSE
	$(PYTHON) tools/notices.py
build/uacpi/%.o: third_party/uacpi/source/%.c $(shell find third_party/uacpi/include -name '*.h')
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(UACPI_FLAGS) -c $< -o $@
build/kernel/acpi.o build/kernel/acpi_os.o: CFLAGS += $(UACPI_FLAGS)
build/kernel/fs.o: build/notices.h
build/icons.h: tools/icongen.py $(wildcard assets/icons/lucide/*)
	$(PYTHON) tools/icongen.py
build/ui/graphics.o: build/font.h build/icons.h
build/ui/windows.o build/ui/desktop.o: ui/window.h
# Render the source revision actually compiled into the image, not a hardcoded
# release string. FORCE runs the check at every build; unchanged content is
# left untouched so an incremental build does not unnecessarily relink.
.PHONY: FORCE
FORCE:
build/luna_build_version.h: FORCE
	@mkdir -p build
	@$(PYTHON) -c 'from pathlib import Path; import subprocess; p=Path("build/luna_build_version.h"); s=subprocess.run(["git","rev-parse","--short=12","HEAD"],capture_output=True,text=True); v=(s.stdout.strip() if s.returncode==0 else "unknown"); content="#ifndef LUNA_BUILD_VERSION_H\\n#define LUNA_BUILD_VERSION_H\\n#define LUNA_BUILD_COMMIT \\""+v+"\\"\\n#endif\\n"; content=content.replace("\\\\n","\\n").replace("\\\\"","\\""); p.write_text(content) if not p.exists() or p.read_text()!=content else None'
build/ui/desktop.o: build/luna_build_version.h

third_party/musl/config.mak: third_party/musl/configure
	cd third_party/musl && ./configure --target=i386 --prefix=/luna CC='gcc -m32' CFLAGS='-O2 -fno-pie -fno-stack-protector' --disable-shared
third_party/musl/lib/libc.a: third_party/musl/config.mak
	$(MAKE) -C third_party/musl -j4 AR=ar RANLIB=ranlib lib/libc.a
$(JS_OBJECTS) build/lib/runtime.o build/apps/javascript.o build/ui/web_js.o build/drivers/tls.o $(BEAR_OBJECTS): third_party/musl/lib/libc.a
build/kernel/account.o: kernel/account.c third_party/musl/lib/libc.a
	@mkdir -p build/kernel
	$(CC) $(CFLAGS) $(MUSLINC) -Ithird_party/bearssl/inc -c $< -o $@
build/drivers/tls.o: drivers/tls.c assets/tls/trust_anchors.h
	@mkdir -p build/drivers
	$(CC) $(CFLAGS) $(MUSLINC) -Ithird_party/bearssl/inc -Iassets/tls -c $< -o $@
build/bearssl/%.o: third_party/bearssl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(NATIVEFLAGS) -DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0 -DBR_AES_X86NI=0 -DBR_GHASH_PCLMUL=0 -DBR_SSE2=0 -DBR_RDRAND=0 -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -c $< -o $@
build/libbearssl.a: $(BEAR_OBJECTS)
	ar rcs $@ $^
build/apps/javascript.o: apps/javascript.c
	@mkdir -p build/apps
	$(CC) $(NATIVEFLAGS) -c $< -o $@
build/ui/web_js.o: ui/web_js.c
	@mkdir -p build/ui
	$(CC) $(NATIVEFLAGS) -c $< -o $@
build/lib/runtime.o: lib/runtime.c
	@mkdir -p build/lib
	$(CC) $(NATIVEFLAGS) -c $< -o $@
build/quickjs/%.o: third_party/quickjs/%.c
	@mkdir -p build/quickjs
	$(CC) $(NATIVEFLAGS) -fwrapv -DLUNA_FREESTANDING -DCONFIG_VERSION=\"2026-06-04\" -c $< -o $@
build/%.o: %.c kernel/luna.h kernel/task.h kernel/settings.h kernel/web.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
build/apps/user.o: apps/user.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@
build/kernel/entry.o: kernel/entry.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@
build/kernel.elf: $(OBJECTS) build/libbearssl.a kernel/link.ld
	$(LD) -m elf_i386 -T kernel/link.ld --gc-sections -Map build/kernel.map -o $@ $(OBJECTS) build/libbearssl.a third_party/musl/lib/libc.a $(LIBGCC32)
build/kernel.bin: build/kernel.elf
	objcopy -O binary $< $@

EFI_CC ?= clang
EFI_LD ?= $(firstword $(wildcard /usr/bin/lld-link /usr/bin/lld-link-* /usr/lib/llvm-*/bin/lld-link))
build/uefi_trampoline.bin: boot/uefi_trampoline.S
	$(CC) -m64 -c $< -o build/uefi_trampoline.o
	$(LD) -m elf_x86_64 -Ttext 0 --oformat binary -e _start build/uefi_trampoline.o -o $@
build/uefi_trampoline.h: build/uefi_trampoline.bin
	$(PYTHON) -c "from pathlib import Path; d=Path('$<').read_bytes(); Path('$@').write_text('static const unsigned char uefi_trampoline[]={'+','.join(map(str,d))+'};\n')"
build/uefi_layout.h: build/kernel.elf
	$(PYTHON) tools/uefi-layout.py
build/BOOTX64.EFI: boot/uefi.c boot/uefi.h build/uefi_trampoline.h build/uefi_layout.h
	$(EFI_CC) --target=x86_64-pc-win32-coff -std=c11 -Os -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone -mno-stack-arg-probe -mno-sse -mno-mmx -Wall -Wextra -Werror -Ibuild -c boot/uefi.c -o build/uefi.obj
	$(EFI_LD) /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 /base:0x10000000 /dynamicbase /timestamp:0 /opt:noref /out:$@ build/uefi.obj

build/stage2.bin: dist/LunaOS.img
	@test -f $@
dist/LunaOS.img: build/kernel.bin build/BOOTX64.EFI tools/efifat.py boot/stage2.S tools/mkimage.py tools/firmwarepack.py assets/driver-sources/linux-wireless.tar.gz assets/driver-sources/manifest.json $(shell find assets/firmware -type f) boot/boot.S
	$(PYTHON) tools/mkimage.py
dist/LunaOS.iso: dist/LunaOS.img tools/mkiso.py
	$(PYTHON) tools/mkiso.py
iso: dist/LunaOS.iso
run-iso: all
	LUNA_IMAGE=dist/LunaOS.iso tools/run-qemu.sh
run: all
	tools/run-qemu.sh
test: all test-storage test-storage-sdhci test-lunafs2 test-lunasdk-hardware test-lunapy test-devkit test-motion test-pmm test-heap test-wpa2 test-wifi-ccmp test-wifi-client test-rtw8822be-tx-dma test-dhcp test-wifi-station test-iwl-firmware test-iwl-pcie test-wifi-intel test-wifi-scan test-wireless-frames test-wifi-port test-wifi-runtime test-rtw8822be-diag test-rtw8822b-native test-rtw8822b-firmware
	$(PYTHON) tests/smoke.py
	$(PYTHON) tests/multitask.py
	$(PYTHON) tests/desktop.py
	$(PYTHON) tests/desktop_next.py
	$(PYTHON) tests/native_resolution.py
	$(PYTHON) tests/https.py
	$(PYTHON) tests/driver_services.py
	$(PYTHON) tests/setup.py
	$(PYTHON) tests/iso.py
test-iso-install: all
	LUNA_SETUP_ISO=1 LUNA_TEST_DIR=build/test-iso-install $(PYTHON) tests/setup.py
check: all test-storage test-storage-sdhci test-lunafs2 test-lunasdk-hardware test-lunapy test-devkit test-motion test-pmm test-heap test-wpa2 test-dhcp test-wifi-station test-iwl-firmware test-iwl-pcie test-wifi-intel test-wifi-scan test-wireless-frames test-wifi-port test-wifi-runtime test-rtw8822be-diag test-rtw8822b-native test-rtw8822b-firmware
	$(PYTHON) tools/check_image.py
	$(PYTHON) tools/check_iso.py
clean:
	rm -rf build dist

# Host tests cover frame handling only (not radio firmware or PCI hardware).
test-wireless-frames:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -I. net/wireless/frames.c tests/wireless_frames.c -o build/wireless-frames-test
	./build/wireless-frames-test

# Preflight mocks only; never interpreted as proof of radio functionality.
test-wifi-port:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Ikernel drivers/wifi_port.c tests/wifi_port.c -o build/wifi-port-test
	./build/wifi-port-test

# Host simulations validate the public API and read-only device diagnostics.
test-wifi-runtime:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Ikernel drivers/wifi.c tests/wifi_runtime.c -o build/wifi-runtime-test
	./build/wifi-runtime-test
test-rtw8822be-diag:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Ikernel drivers/rtw8822be_diag.c drivers/rtw8822b_native.c tests/rtw8822be_diag.c -o build/rtw8822be-diag-test
	./build/rtw8822be-diag-test

# Real firmware parsing and upstream PCIe power flow; register I/O is simulated.
test-rtw8822b-native:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror drivers/rtw8822b_native.c tests/rtw8822b_native.c -o build/rtw8822b-native-test
	./build/rtw8822b-native-test

# Firmware loader device-memory model (not a physical radio test).
test-rtw8822b-firmware:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror drivers/rtw8822b_native.c drivers/rtw8822b_firmware.c tests/rtw8822b_firmware.c -o build/rtw8822b-firmware-test
	./build/rtw8822b-firmware-test

# Focused passive-scanner test with a simulated radio; not part of release runtime.
test-wifi-scan:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Ikernel drivers/wifi.c drivers/wifi_scan.c drivers/rtw8822b_native.c net/wireless/frames.c tests/wifi_scan.c -o build/wifi-scan-test
	./build/wifi-scan-test

build/drivers/rtw8822be_phy.o: $(wildcard drivers/rtw8822b/*.h)

# Focused model of the production RX DMA consumer, not radio validation.
test-rtw8822be-rx-dma:
	@mkdir -p build
	$(CC) -std=c11 -O2 -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Ikernel drivers/rtw8822b_native.c tests/rtw8822be_rx_dma.c -Wl,--gc-sections -o build/rtw8822be-rx-dma-test
	./build/rtw8822be-rx-dma-test

# PNG decoder uses freestanding musl declarations; all I/O remains in Luna.
build/ui/web_image.o: ui/web_image.c ui/webengine.h third_party/stb/stb_image.h third_party/musl/lib/libc.a
	@mkdir -p build/ui
	$(CC) $(NATIVEFLAGS) -c $< -o $@

.PHONY: test-lunasdk-hardware test-lunapy
test-lunasdk-hardware:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -Ikernel kernel/lunasdk_hardware.c tests/lunasdk_hardware.c -o build/lunasdk-hardware-test
	./build/lunasdk-hardware-test
test-lunapy:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -Ikernel apps/lunapy.c tests/lunapy.c -o build/lunapy-test
	./build/lunapy-test

.PHONY: test-devkit
test-devkit:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -Ikernel kernel/devkit.c tests/devkit.c -o build/devkit-test
	./build/devkit-test

.PHONY: test-motion
test-motion:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -Iui ui/motion.c tests/motion.c -o build/motion-test
	./build/motion-test

.PHONY: test-webengine
test-webengine:
	@mkdir -p build
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Ikernel -Ibuild tests/webengine_test.c ui/web_dom.c ui/web_css.c ui/web_layout.c ui/web_image.c -o build/webengine-test
	./build/webengine-test

.PHONY: test-wifi-intel
test-wifi-intel:
	@mkdir -p build
	$(CC) -std=c11 -O2 -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Ikernel drivers/wifi_intel.c tests/wifi_intel.c -Wl,--gc-sections -o build/wifi-intel-test
	./build/wifi-intel-test

build/net/wifi_ccmp.o: net/wifi_ccmp.c third_party/musl/lib/libc.a
	@mkdir -p build/net
	$(CC) $(CFLAGS) $(MUSLINC) -Ithird_party/bearssl/inc -c $< -o $@

build/net/wpa2.o: net/wpa2.c third_party/musl/lib/libc.a
	@mkdir -p build/net
	$(CC) $(CFLAGS) $(MUSLINC) -Ithird_party/bearssl/inc -c $< -o $@

.PHONY: test-iwl-firmware test-wpa2 test-dhcp
test-iwl-firmware:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror drivers/iwl_firmware.c tests/iwl_firmware.c -o build/iwl-firmware-test
	./build/iwl-firmware-test assets/firmware/iwlwifi-so-a0-gf-a0-89.ucode

build/host-bearssl/%.o: third_party/bearssl/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -c $< -o $@
HOST_WPA_CRYPTO := codec/dec32be codec/enc32be codec/ccopy hash/sha1 mac/hmac mac/hmac_ct hash/dig_size symcipher/aes_ct symcipher/aes_ct_dec symcipher/aes_ct_cbcdec symcipher/aes_ct_enc symcipher/aes_ct_cbcenc
test-wifi-ccmp: $(addprefix build/host-bearssl/,$(addsuffix .o,$(HOST_WPA_CRYPTO)))
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Ithird_party/bearssl/inc net/wifi_ccmp.c tests/wifi_ccmp.c $^ -o build/wifi-ccmp-test
	./build/wifi-ccmp-test

test-wpa2: $(addprefix build/host-bearssl/,$(addsuffix .o,$(HOST_WPA_CRYPTO)))
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Ithird_party/bearssl/inc net/wpa2.c tests/wpa2.c $^ -o build/wpa2-test
	./build/wpa2-test

test-dhcp:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror net/dhcp.c tests/dhcp.c -o build/dhcp-test
	./build/dhcp-test

.PHONY: test-wifi-station
test-wifi-station: $(addprefix build/host-bearssl/,$(addsuffix .o,$(HOST_WPA_CRYPTO)))
	$(CC) -std=c11 -O2 -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-pointer-to-int-cast -Ikernel -Ithird_party/bearssl/inc drivers/net.c drivers/wifi_station.c net/wpa2.c net/netif.c net/dhcp.c net/time_dns.c tests/wifi_station.c $^ -Wl,--gc-sections -o build/wifi-station-test
	./build/wifi-station-test

.PHONY: test-iwl-pcie
test-iwl-pcie:
	@mkdir -p build
	$(CC) -std=c11 -O2 -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Ikernel drivers/iwl_pcie.c drivers/iwl_firmware.c tests/iwl_pcie.c -Wl,--gc-sections -o build/iwl-pcie-test
	./build/iwl-pcie-test

.PHONY: test-pmm test-heap
test-pmm:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror kernel/pmm.c tests/pmm.c -o build/pmm-test
	./build/pmm-test
test-heap:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Ikernel kernel/pmm.c kernel/heap.c tests/heap.c -o build/heap-test
	./build/heap-test

# LunaFS2: binary-safe variable-size files, v1 migration and snapshot recovery.
.PHONY: test-lunafs2
test-lunafs2:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -Itests/fs_mock -Ikernel kernel/fs.c tests/lunafs2.c -o build/lunafs2-test
	./build/lunafs2-test

.PHONY: test-storage test-storage-guest test-storage-interfaces test-storage-install
test-storage:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -Ikernel kernel/storage.c tests/storage.c -o build/storage-test
	./build/storage-test

test-storage-guest: all
	$(PYTHON) tests/storage_guest.py

test-storage-interfaces: all
	$(PYTHON) tests/storage_interfaces.py

test-storage-install: all
	$(PYTHON) tests/storage_install.py

.PHONY: test-storage-sdhci
test-storage-sdhci:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -Ikernel tests/storage_sdhci.c -o build/storage-sdhci-test
	./build/storage-sdhci-test

.PHONY: test-storage-emmc-iso
test-storage-emmc-iso: all
	LUNA_TEST_DIR=build/test-emmc-iso $(PYTHON) tests/storage_emmc_iso.py
	LUNA_UEFI=1 LUNA_TEST_DIR=build/test-emmc-iso-uefi $(PYTHON) tests/storage_emmc_iso.py

.PHONY: storage-sim-setup test-storage-matrix
storage-sim-setup:
	$(PYTHON) tools/storage/build-qemu.py
build/emmc-acpi.aml: tests/fixtures/storage/emmc-acpi.asl
	@mkdir -p build
	$(if $(wildcard .tools/usr/bin/iasl),.tools/usr/bin/iasl,iasl) -p build/emmc-acpi $<
test-storage-matrix: all test-storage test-storage-sdhci
	$(PYTHON) tools/storage/build-qemu.py
	$(MAKE) build/emmc-acpi.aml
	$(PYTHON) tests/storage_matrix.py

.PHONY: test-wifi-client test-rtw8822be-tx-dma
test-wifi-client: $(addprefix build/host-bearssl/,$(addsuffix .o,$(HOST_WPA_CRYPTO)))
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Ikernel -Ithird_party/bearssl/inc drivers/wifi_client.c net/wifi_ccmp.c net/wireless/frames.c tests/wifi_client.c $^ -o build/wifi-client-test
	./build/wifi-client-test
test-rtw8822be-tx-dma:
	@mkdir -p build
	$(CC) -std=c11 -O2 -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Ikernel tests/rtw8822be_tx_dma.c -Wl,--gc-sections -o build/rtw8822be-tx-dma-test
	./build/rtw8822be-tx-dma-test
