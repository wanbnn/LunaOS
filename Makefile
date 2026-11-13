.DEFAULT_GOAL := all
CC := gcc
LD := ld
PYTHON ?= python3
# Local build-only dependencies may also be installed with tools/bootstrap.sh.
export PYTHONPATH := $(CURDIR)/.tools/python:$(PYTHONPATH)
CFLAGS := -m32 -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -msoft-float -Wall -Wextra -Werror -Ikernel -Ibuild
UACPI_FLAGS := -Ithird_party/uacpi/include -DUACPI_REDUCED_HARDWARE -DUACPI_DEFAULT_LOG_LEVEL=UACPI_LOG_WARN -DUACPI_DEFAULT_LOOP_TIMEOUT_SECONDS=3 -DUACPI_DEFAULT_MAX_CALL_STACK_DEPTH=64
UACPI_OBJECTS := $(patsubst third_party/uacpi/source/%.c,build/uacpi/%.o,$(wildcard third_party/uacpi/source/*.c))
SOURCES := $(filter-out lib/cxx_glibc_compat.c,$(wildcard kernel/*.c drivers/*.c lib/*.c ui/*.c apps/*.c net/*.c net/wireless/*.c))
MUSLINC := -nostdinc -isystem third_party/musl/include -isystem third_party/musl/arch/i386 -isystem third_party/musl/arch/generic -isystem third_party/musl/obj/include -isystem $(shell $(CC) -print-file-name=include)
LIBGCC32 := $(firstword $(wildcard .tools/usr/lib/gcc/*/*/32/libgcc.a /usr/lib/gcc/*/*/32/libgcc.a))
# Lexbor requires a vendored submodule and a separate 32-bit static build.
# Lexbor is the default HTML5 backend in LunaOS images; set LUNA_LEXBOR=0
# explicitly for emergency legacy fallback builds.
LUNA_LEXBOR ?= 1
# litehtml provides layout and painting. The legacy renderer remains available.
LUNA_LITEHTML ?= $(LUNA_LEXBOR)
LITEHTML_CXX ?= g++
LITEHTML_CXX_ARCH := $(shell $(LITEHTML_CXX) -dumpmachine)
LITEHTML_CXX_VERSION := $(shell $(LITEHTML_CXX) -dumpversion)
LITEHTML_LOCAL_LIB := .tools/usr/lib/gcc/$(LITEHTML_CXX_ARCH)/$(LITEHTML_CXX_VERSION)/32
LITEHTML_LOCAL_INCLUDE := $(wildcard .tools/usr/include/$(LITEHTML_CXX_ARCH)/c++/$(LITEHTML_CXX_VERSION)/32)
LITEHTML_CXXFLAGS := -m32 -std=c++17 -Oz -fexceptions -fno-asynchronous-unwind-tables -frtti -fno-pie -fno-pic -fno-stack-protector -fno-threadsafe-statics -ffunction-sections -fdata-sections -mno-sse -mno-mmx -mfpmath=387 -DLITEHTML_NO_THREADS $(addprefix -isystem ,$(abspath $(LITEHTML_LOCAL_INCLUDE)))
LITEHTML_LIBSTDCXX := $(or $(wildcard $(LITEHTML_LOCAL_LIB)/libstdc++.a),$(shell $(LITEHTML_CXX) -m32 -print-file-name=libstdc++.a))
LITEHTML_LIBSUPCXX := $(or $(wildcard $(LITEHTML_LOCAL_LIB)/libsupc++.a),$(shell $(LITEHTML_CXX) -m32 -print-file-name=libsupc++.a))
LITEHTML_LIBGCC_EH := $(shell $(LITEHTML_CXX) -m32 -print-file-name=libgcc_eh.a)
ifeq ($(LUNA_LEXBOR),1)
CFLAGS += -DLUNA_USE_LEXBOR
LEXBOR_NATIVE_LIB := build/lexbor-native/liblexbor_static.a
endif
NATIVEFLAGS := -fexcess-precision=standard -m32 -Os -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -ffunction-sections -fdata-sections -mno-sse -mfpmath=387 $(MUSLINC) -Ikernel -Ibuild -Ithird_party/quickjs
ifeq ($(LUNA_LEXBOR),1)
NATIVEFLAGS += -DLUNA_USE_LEXBOR -Ithird_party/lexbor/source
endif
BEAR_SOURCES := $(shell find third_party/bearssl/src -name "*.c" | sort)
BEAR_OBJECTS := $(patsubst third_party/bearssl/%.c,build/bearssl/%.o,$(BEAR_SOURCES))
JS_OBJECTS := $(addprefix build/quickjs/,$(addsuffix .o,quickjs libregexp libunicode cutils dtoa))
OBJECTS := $(patsubst %.c,build/%.o,$(SOURCES)) build/kernel/entry.o build/apps/user.o $(JS_OBJECTS) $(UACPI_OBJECTS)
WASM3_SRC := $(wildcard third_party/wasm3/*.c)
WASM3_OBJ := $(patsubst third_party/wasm3/%.c,build/wasm3/%.o,$(WASM3_SRC))
CPYTHON_OBJ := build/apps/cpython/port.o build/apps/cpython/wasi.o
OBJECTS += $(WASM3_OBJ) $(CPYTHON_OBJ)
WASM3_FLAGS := $(NATIVEFLAGS) -Ithird_party/wasm3 -O2 -DNDEBUG -Dd_m3MaxLinearMemoryPages=384
$(WASM3_OBJ): $(wildcard third_party/wasm3/*.h) third_party/musl/lib/libc.a
build/wasm3/%.o: third_party/wasm3/%.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(filter-out -mfpmath=387 -fexcess-precision=standard,$(WASM3_FLAGS)) -mno-mmx -mno-sse2 -c $< -o $@
$(CPYTHON_OBJ): apps/cpython/port.h kernel/python.h kernel/image_layout.h third_party/wasm3/wasm3.h third_party/musl/lib/libc.a
build/apps/cpython/%.o: apps/cpython/%.c
	@mkdir -p $(dir $@)
	$(CC) $(WASM3_FLAGS) -Wall -Wextra -Wno-unused-parameter -c $< -o $@

$(OBJECTS) $(BEAR_OBJECTS): Makefile
$(patsubst %.c,build/%.o,$(SOURCES)): $(wildcard kernel/*.h ui/*.h drivers/*.h include/net/*.h)
.PHONY: test-rtw8822be-rx-dma test-wifi-scan all clean run run-iso iso test test-iso-install check test-wifi-port test-wireless-frames test-wifi-runtime test-rtw8822be-diag test-rtw8822b-native test-rtw8822b-firmware
all: dist/LunaOS.img dist/LunaOS.iso
build/font.h: tools/fontgen.py assets/fonts/InterTight.ttf assets/fonts/NotoSans.ttf \
             assets/fonts/Montserrat.ttf assets/fonts/NotoSansSymbols2.ttf
	$(PYTHON) tools/fontgen.py
build/notices.h: tools/notices.py assets/fonts/OFL.txt assets/fonts/NotoSans-OFL.txt \
             assets/fonts/Montserrat-OFL.txt assets/fonts/NotoSansSymbols2-OFL.txt \
             assets/icons/lucide/LICENSE $(wildcard assets/licenses/*) third_party/quickjs/LICENSE third_party/micropython/LICENSE third_party/wasm3/LICENSE third_party/musl/COPYRIGHT third_party/bearssl/LICENSE.txt third_party/stb/LICENSE third_party/uacpi/LICENSE $(wildcard third_party/lexbor/LICENSE third_party/litehtml/LICENSE third_party/litehtml/src/gumbo/LICENSE)
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
build/web-engine-config: FORCE
	@mkdir -p build
	@printf '%s\n' 'Lexbor=$(LUNA_LEXBOR) litehtml=$(LUNA_LITEHTML)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
build/kernel/main.o build/ui/web_dom.o build/ui/web_lexbor.o build/ui/web_view.o: build/web-engine-config
build/luna_build_version.h: FORCE
	@mkdir -p build
	$(PYTHON) tools/build-version.py
build/ui/desktop.o build/kernel/update.o: build/luna_build_version.h
build/kernel/update_format.o: CFLAGS += -Ithird_party/bearssl/inc

third_party/musl/config.mak: third_party/musl/configure
	cd third_party/musl && ./configure --target=i386 --prefix=/luna CC='gcc -m32' CFLAGS='-O2 -fno-pie -fno-stack-protector' --disable-shared
third_party/musl/lib/libc.a: third_party/musl/config.mak
	$(MAKE) -C third_party/musl -j4 AR=ar RANLIB=ranlib lib/libc.a
# Keep MicroPython's qstr/module generation in its upstream build system.
MICROPYTHON_LIB := build/micropython/libmicropython.a
.PHONY: micropython-native
micropython-native: $(MICROPYTHON_LIB)
$(MICROPYTHON_LIB): FORCE third_party/musl/lib/libc.a
	+$(MAKE) -C apps/python PYTHON="$(PYTHON)" LUNA_FLAGS="$(subst third_party/,../../third_party/,$(subst -Ikernel,-I../../kernel,$(subst -Ibuild,-I../../build,$(NATIVEFLAGS))))"

$(JS_OBJECTS) build/lib/runtime.o build/apps/javascript.o build/ui/web_js.o build/drivers/tls.o $(BEAR_OBJECTS): third_party/musl/lib/libc.a
ifeq ($(LUNA_LEXBOR),1)
build/ui/web_lexbor.o: third_party/musl/lib/libc.a
build/ui/web_lexbor.o: CFLAGS += $(MUSLINC) -Ithird_party/lexbor/source -Wno-unused-function
build/ui/web_dom.o: CFLAGS += -Wno-unused-function
# Build freestanding 32-bit Lexbor objects; the static archive does not
# require running Lexbor utilities in the LunaOS target environment.
$(LEXBOR_NATIVE_LIB): third_party/musl/lib/libc.a
	@test -f third_party/lexbor/CMakeLists.txt || (echo "Run git submodule update --init --recursive" && exit 1)
	cmake -S third_party/lexbor -B build/lexbor-native -DLEXBOR_BUILD_SHARED=OFF -DLEXBOR_BUILD_STATIC=ON -DLEXBOR_BUILD_EXAMPLES=OFF -DLEXBOR_BUILD_TESTS=OFF -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_C_FLAGS="$(subst -Ibuild,-I$(CURDIR)/build,$(subst -Ikernel,-I$(CURDIR)/kernel,$(subst third_party/,$(CURDIR)/third_party/,$(NATIVEFLAGS)))) -fno-pic -ffunction-sections -fdata-sections"
	cmake --build build/lexbor-native --target lexbor_static --parallel 2
.PHONY: lexbor-native
lexbor-native: $(LEXBOR_NATIVE_LIB)
endif
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
build/apps/python_index.o: apps/python_index.c apps/python_wheel.h third_party/musl/lib/libc.a
	@mkdir -p build/apps
	$(CC) $(NATIVEFLAGS) -c $< -o $@
build/apps/python_pip.o build/apps/python_packages.o build/apps/python_wheel.o: apps/python_wheel.h
build/apps/python_pip.o build/apps/python_packages.o: third_party/musl/lib/libc.a
build/apps/python_pip.o build/apps/python_packages.o: CFLAGS += $(MUSLINC)
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
# The bridge and source library are built as 32-bit freestanding objects.
# Compile with hosted C++ headers because GCC 13 refuses <cmath> in
# -ffreestanding mode. The resulting objects still use no Linux dynamic
# linker; unresolved host ABI dependencies must fail the kernel link.
LITEHTML_SOURCE := build/litehtml-source
LITEHTML_NATIVE := build/litehtml-native/patched
LITEHTML_PATCHES := tools/litehtml-float-progress.patch tools/litehtml-float-width.patch tools/litehtml-grid.patch
LITEHTML_INC := -I$(LITEHTML_SOURCE)/include -I$(LITEHTML_SOURCE)/src

.PHONY: litehtml-native test-litehtml
litehtml-native: $(LITEHTML_NATIVE)/liblitehtml.a build/ui/litehtml_bridge.o build/lib/cxx_runtime.o
$(LITEHTML_SOURCE)/.patched: $(LITEHTML_PATCHES) $(wildcard third_party/litehtml/src/*.cpp third_party/litehtml/include/litehtml/*.h) third_party/litehtml/CMakeLists.txt
	rm -rf $(LITEHTML_SOURCE).tmp
	mkdir -p $(LITEHTML_SOURCE).tmp
	cp -R third_party/litehtml/CMakeLists.txt third_party/litehtml/cmake third_party/litehtml/src third_party/litehtml/include $(LITEHTML_SOURCE).tmp/
	patch -s -d $(LITEHTML_SOURCE).tmp/src < tools/litehtml-float-progress.patch
	patch -s -p1 -d $(LITEHTML_SOURCE).tmp < tools/litehtml-float-width.patch
	patch -s -p1 -d $(LITEHTML_SOURCE).tmp < tools/litehtml-grid.patch
	rm -rf $(LITEHTML_SOURCE)
	mv $(LITEHTML_SOURCE).tmp $(LITEHTML_SOURCE)
	touch $@
$(LITEHTML_NATIVE)/liblitehtml.a $(LITEHTML_NATIVE)/src/gumbo/libgumbo.a &: Makefile $(LITEHTML_SOURCE)/.patched
	cmake -S $(LITEHTML_SOURCE) -B $(LITEHTML_NATIVE) -DCMAKE_CXX_COMPILER="$(LITEHTML_CXX)" -DCMAKE_C_FLAGS="-m32 -Oz -ffreestanding -fno-pie -fno-pic -fno-stack-protector -ffunction-sections -fdata-sections -mno-sse -mno-mmx -mfpmath=387" -DCMAKE_CXX_FLAGS="$(LITEHTML_CXXFLAGS)" -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DLITEHTML_BUILD_TESTING=OFF -DLITEHTML_ENABLE_LINT=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=OFF
	cmake --build $(LITEHTML_NATIVE) --target litehtml --parallel 2
build/ui/litehtml_bridge.o: ui/litehtml_bridge.cpp ui/litehtml_bridge.h ui/web_dom_types.h Makefile $(LITEHTML_SOURCE)/.patched
	@mkdir -p build/ui
	$(LITEHTML_CXX) $(LITEHTML_CXXFLAGS) -DLUNA_KERNEL_LITEHTML -Iui -Ikernel $(LITEHTML_INC) -c $< -o $@
test-litehtml: $(LITEHTML_SOURCE)/.patched
	cmake -S $(LITEHTML_SOURCE) -B build/litehtml-host-patched -DCMAKE_BUILD_TYPE=Release -DLITEHTML_BUILD_TESTING=OFF
	cmake --build build/litehtml-host-patched --target litehtml --parallel 2
	$(CXX) -std=c++17 -Wall -Wextra -Werror $(LITEHTML_INC) -c ui/litehtml_bridge.cpp -o build/litehtml-bridge.o
	$(CXX) -std=c++17 $(LITEHTML_INC) tests/litehtml_bridge_test.cpp build/litehtml-bridge.o build/litehtml-host-patched/liblitehtml.a build/litehtml-host-patched/src/gumbo/libgumbo.a -o build/litehtml-bridge-test
	./build/litehtml-bridge-test
	$(CXX) -std=c++17 $(LITEHTML_INC) tests/litehtml_float_test.cpp build/litehtml-bridge.o build/litehtml-host-patched/liblitehtml.a build/litehtml-host-patched/src/gumbo/libgumbo.a -o build/litehtml-float-test
	./build/litehtml-float-test
build/lib/cxx_glibc_compat.o: lib/cxx_glibc_compat.c third_party/musl/lib/libc.a
	@mkdir -p build/lib
	$(CC) $(CFLAGS) $(MUSLINC) -c $< -o $@
build/lib/cxx_runtime.o: lib/cxx_runtime.cpp Makefile
	@mkdir -p build/lib
	$(LITEHTML_CXX) $(LITEHTML_CXXFLAGS) -c $< -o $@
ifeq ($(LUNA_LITEHTML),1)
CFLAGS += -DLUNA_USE_LITEHTML
OBJECTS += build/ui/litehtml_bridge.o build/lib/cxx_runtime.o build/lib/cxx_glibc_compat.o
LITEHTML_LIBS := $(LITEHTML_NATIVE)/liblitehtml.a $(LITEHTML_NATIVE)/src/gumbo/libgumbo.a $(LITEHTML_LIBSTDCXX) $(LITEHTML_LIBSUPCXX) $(LITEHTML_LIBGCC_EH)
build/kernel.elf: $(LITEHTML_LIBS)
endif
build/kernel.elf: $(OBJECTS) build/libbearssl.a $(LEXBOR_NATIVE_LIB) $(MICROPYTHON_LIB) kernel/link.ld
	$(LD) -m elf_i386 -T kernel/link.ld --gc-sections -Map build/kernel.map -o $@ $(OBJECTS) build/libbearssl.a $(LEXBOR_NATIVE_LIB) $(LITEHTML_LIBS) $(MICROPYTHON_LIB) third_party/musl/lib/libc.a $(LIBGCC32)
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
build/BOOTX64.EFI: boot/uefi.c boot/uefi.h kernel/image_layout.h build/uefi_trampoline.h build/uefi_layout.h
	$(EFI_CC) --target=x86_64-pc-win32-coff -std=c11 -Os -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone -mno-stack-arg-probe -mno-sse -mno-mmx -Wall -Wextra -Werror -Ibuild -c boot/uefi.c -o build/uefi.obj
	$(EFI_LD) /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 /base:0x10000000 /dynamicbase /timestamp:0 /opt:noref /out:$@ build/uefi.obj

build/stage2.bin: dist/LunaOS.img
	@test -f $@
CPYTHON_BUNDLE := assets/cpython/python313.bundle
dist/LunaOS.img: $(CPYTHON_BUNDLE) kernel/image_layout.h build/kernel.bin build/BOOTX64.EFI tools/efifat.py boot/stage2.S tools/mkimage.py tools/firmwarepack.py assets/driver-sources/linux-wireless.tar.gz assets/driver-sources/manifest.json $(shell find assets/firmware -type f) boot/boot.S
	$(PYTHON) tools/mkimage.py $(CPYTHON_BUNDLE)
dist/LunaOS.iso: dist/LunaOS.img tools/mkiso.py
	$(PYTHON) tools/mkiso.py
iso: dist/LunaOS.iso
run-iso: all
	LUNA_IMAGE=dist/LunaOS.iso tools/run-qemu.sh
run: all
	tools/run-qemu.sh
test: all test-rtw8822be-coex test-rtw8822be-fifo test-storage test-storage-sdhci test-lunafs2 test-lunasdk-hardware test-devkit test-motion test-pmm test-heap test-wpa2 test-wifi-ccmp test-wifi-client test-rtw8822be-tx-dma test-dhcp test-wifi-station test-iwl-firmware test-iwl-pcie test-wifi-intel test-wifi-scan test-wireless-frames test-wifi-port test-wifi-runtime test-rtw8822be-diag test-rtw8822b-native test-rtw8822b-firmware
	$(PYTHON) tests/smoke.py
	$(PYTHON) tests/multitask.py
	$(PYTHON) tests/desktop.py
	$(PYTHON) tests/desktop_next.py
	$(PYTHON) tests/native_resolution.py
	$(PYTHON) tests/https.py
	$(PYTHON) tests/driver_services.py
	$(PYTHON) tests/python_guest.py
	$(PYTHON) tests/cpython_guest.py
	$(PYTHON) tests/python_pip_guest.py
	$(PYTHON) tests/setup.py
	$(PYTHON) tests/iso.py
test-iso-install: all
	LUNA_SETUP_ISO=1 LUNA_TEST_DIR=build/test-iso-install $(PYTHON) tests/setup.py
check: all test-rtw8822be-coex test-rtw8822be-fifo test-storage test-storage-sdhci test-lunafs2 test-lunasdk-hardware test-devkit test-motion test-pmm test-heap test-wpa2 test-dhcp test-wifi-station test-iwl-firmware test-iwl-pcie test-wifi-intel test-wifi-scan test-wireless-frames test-wifi-port test-wifi-runtime test-rtw8822be-diag test-rtw8822b-native test-rtw8822b-firmware
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

.PHONY: test-lunasdk-hardware test-lunapy test-python test-cpython test-pip
test-lunasdk-hardware:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -Ikernel kernel/lunasdk_hardware.c tests/lunasdk_hardware.c -o build/lunasdk-hardware-test
	./build/lunasdk-hardware-test
test-lunapy: test-python
test-python: dist/LunaOS.img
	LUNA_TEST_DIR=build/test-python $(PYTHON) tests/python_guest.py
test-cpython: dist/LunaOS.img
	LUNA_TEST_DIR=build/test-cpython $(PYTHON) tests/cpython_guest.py
test-pip: dist/LunaOS.img
	LUNA_TEST_DIR=build/test-pip $(PYTHON) tests/python_pip_guest.py

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

.PHONY: test-wifi-station test-tcp test-url
test check: test-tcp test-url
test-url:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror lib/url.c tests/url.c -o build/url-test
	./build/url-test

test-tcp:
	@mkdir -p build
	$(CC) -std=c11 -O2 -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-pointer-to-int-cast -Ikernel tests/tcp.c net/time_dns.c -Wl,--gc-sections -o build/tcp-test
	./build/tcp-test

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
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -Itests/fs_mock -Ikernel kernel/fs.c kernel/fs_volume.c tests/lunafs2.c -o build/lunafs2-test
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
.PHONY: test-wifi-qemu
test-wifi-qemu: all
	$(PYTHON) tools/wifi/build-qemu.py
	$(PYTHON) tests/wifi_qtest.py
	$(PYTHON) tests/wifi_qemu.py
	$(PYTHON) tests/wifi_join_qemu.py

test-wifi-client: $(addprefix build/host-bearssl/,$(addsuffix .o,$(HOST_WPA_CRYPTO)))
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Ikernel -Ithird_party/bearssl/inc drivers/wifi_client.c net/wifi_ccmp.c net/wireless/frames.c tests/wifi_client.c $^ -o build/wifi-client-test
	./build/wifi-client-test
test-rtw8822be-tx-dma:
	@mkdir -p build
	$(CC) -std=c11 -O2 -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Ikernel tests/rtw8822be_tx_dma.c -Wl,--gc-sections -o build/rtw8822be-tx-dma-test
	./build/rtw8822be-tx-dma-test

.PHONY: test-rtw8822be-fifo
test-rtw8822be-fifo:
	@mkdir -p build
	$(CC) -std=c11 -O2 -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Ikernel tests/rtw8822be_fifo.c -Wl,--gc-sections -o build/rtw8822be-fifo-test
	./build/rtw8822be-fifo-test

.PHONY: test-rtw8822be-coex
test-rtw8822be-coex:
	@mkdir -p build
	$(CC) -std=c11 -O2 -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Ikernel tests/rtw8822be_coex.c -Wl,--gc-sections -o build/rtw8822be-coex-test
	./build/rtw8822be-coex-test

.PHONY: test-update
test-update: all build/host-bearssl/hash/sha2small.o build/host-bearssl/codec/enc32be.o build/host-bearssl/codec/dec32be.o
	$(PYTHON) tests/updatepack.py
	$(CC) -std=c11 -O2 -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Ikernel -Iui -Itests/update_mock -Ithird_party/bearssl/inc kernel/update.c kernel/update_format.c ui/web_image.c tests/update.c build/host-bearssl/hash/sha2small.o build/host-bearssl/codec/enc32be.o build/host-bearssl/codec/dec32be.o -Wl,--gc-sections -o build/update-test-engine
	./build/update-test-engine

.PHONY: test-fs-streaming
test-fs-streaming:
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Itests/fs_mock -Ikernel kernel/fs.c kernel/fs_volume.c net/http_stream.c tests/fs_streaming.c -o build/fs-streaming-test
	./build/fs-streaming-test

# Pinned upstream Git host baseline; never linked into the LunaOS guest ISO.
# Requires git submodule update --init --recursive third_party/git.
.PHONY: git-upstream-host-build git-upstream-host-test
git-upstream-host-build:
	@test -f third_party/git/Makefile || (echo "Missing upstream Git source. Run git submodule update --init --recursive third_party/git" && exit 1)
	+$(MAKE) -C third_party/git -j2 NO_GETTEXT=YesPlease NO_TCLTK=YesPlease NO_PERL=YesPlease NO_PYTHON=YesPlease all

git-upstream-host-test: git-upstream-host-build
	$(PYTHON) tests/git_upstream_host.py --git third_party/git/git

# ELF32 validation test; host-only parser and guest freestanding i386 object.
.PHONY: test-elf32
test-elf32:
	@mkdir -p build
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Ikernel kernel/elf32.c tests/elf32.c -o build/elf32-test
	./build/elf32-test

# Cross-build a real freestanding C program for the restricted LunaOS i386 ABI.
# This target builds on Linux and runs only inside the LunaOS guest via elfrun.
.PHONY: luna-guest-hello
luna-guest-hello: build/guest/hello.elf
build/guest/hello.elf: sdk/guest/hello.c sdk/guest/posix.c sdk/guest/posix.h sdk/guest/crt0.S sdk/guest/luna32.ld
	@mkdir -p build/guest
	$(CC) -m32 -Os -std=gnu11 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -msoft-float -Wall -Wextra -Werror -Isdk/guest -c sdk/guest/hello.c -o build/guest/hello.o
	$(CC) -m32 -Os -std=gnu11 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -msoft-float -Wall -Wextra -Werror -Isdk/guest -c sdk/guest/posix.c -o build/guest/posix.o
	$(CC) -m32 -c sdk/guest/crt0.S -o build/guest/crt0.o
	$(LD) -m elf_i386 -T sdk/guest/luna32.ld --build-id=none -o $@ build/guest/crt0.o build/guest/hello.o build/guest/posix.o
	@file $@
	@readelf -l $@ | grep -q LOAD
