UNAME       := $(shell uname -s)
HOST_ARCH   := $(shell uname -m)

CC          := clang
LD_BOOT     := lld-link
LD_KERNEL   := ld.lld

ifeq ($(UNAME),Darwin)
    OBJCOPY := llvm-objcopy
else
    OBJCOPY := objcopy
endif

QEMU        := qemu-system-x86_64
HOST_CC     := cc
AR          := llvm-ar

ifeq ($(UNAME),Darwin)
    QEMU_PREFIX       ?= $(shell brew --prefix qemu 2>/dev/null)
ifeq ($(HOST_ARCH),arm64)
    QEMU_ACCEL        := tcg
else
    QEMU_ACCEL        := hvf
endif
    QEMU_CPU          := max
    OVMF_CODE_SOURCE  := $(QEMU_PREFIX)/share/qemu/edk2-x86_64-code.fd
    OVMF_VARS_SOURCE  := $(QEMU_PREFIX)/share/qemu/edk2-i386-vars.fd
else
    QEMU_ACCEL        := kvm
    QEMU_CPU          := host
    OVMF_CODE_SOURCE := /usr/share/OVMF/OVMF_CODE_4M.fd
    OVMF_VARS_SOURCE := /usr/share/OVMF/OVMF_VARS_4M.fd
endif

QEMU_PLATFORM_ARGS := -accel $(QEMU_ACCEL) -cpu $(QEMU_CPU)

BUILD_DIR    := build
EFI_DIR      := $(BUILD_DIR)/EFI/BOOT
MANGROVE_DIR := $(BUILD_DIR)/Mangrove
STATE_DIR    := .mangrove
DEV_IMAGE    := $(STATE_DIR)/MangroveDev.img
DEV_ROOT_IMAGE := $(STATE_DIR)/MangroveDevRoot.img
LEGACY_DEV_IMAGE := $(MANGROVE_DIR)/Mangrove.img
FLASH_ROOT_IMAGE := $(MANGROVE_DIR)/MangroveFlash.img
USB_IMAGE    := $(MANGROVE_DIR)/MangroveUSB.img
SPROUT_DIR   := $(BUILD_DIR)/Sprout
HELLO_DIR    := $(BUILD_DIR)/Hello
SHOOT_DIR    := $(BUILD_DIR)/Shoot
CLEAR_DIR    := $(BUILD_DIR)/Clear
CP_DIR       := $(BUILD_DIR)/Cp
LS_DIR       := $(BUILD_DIR)/Ls
LOCATE_DIR   := $(BUILD_DIR)/Locate
MV_DIR       := $(BUILD_DIR)/Mv
PLANT_DIR    := $(BUILD_DIR)/Plant
READ_DIR     := $(BUILD_DIR)/Read
RM_DIR       := $(BUILD_DIR)/Rm
MKDIR_DIR    := $(BUILD_DIR)/Mkdir
RMDIR_DIR    := $(BUILD_DIR)/Rmdir
SAY_DIR      := $(BUILD_DIR)/Say
UPTIME_DIR   := $(BUILD_DIR)/Uptime
VERSION_DIR  := $(BUILD_DIR)/Version
WHERE_DIR    := $(BUILD_DIR)/Where
FSTEST_DIR   := $(BUILD_DIR)/FsTest
NETTEST_DIR  := $(BUILD_DIR)/NetTest
PING_DIR     := $(BUILD_DIR)/Ping
RESOLVE_DIR  := $(BUILD_DIR)/Resolve
FETCH_DIR    := $(BUILD_DIR)/Fetch
NETWORK_DIR  := $(BUILD_DIR)/Network
POWER_DIR    := $(BUILD_DIR)/Power
IDENTITY_DIR := $(BUILD_DIR)/Identity
USER_CMD_DIR := $(BUILD_DIR)/User
SHUTDOWN_DIR := $(BUILD_DIR)/Shutdown
REBOOT_DIR   := $(BUILD_DIR)/Reboot
USER_LIBC_DIR := $(BUILD_DIR)/userspace/libc

EFI          := $(EFI_DIR)/BOOTX64.EFI
KERNEL       := $(MANGROVE_DIR)/kernel.elf
KERNEL_MAP   := $(MANGROVE_DIR)/kernel.map
OVMF_CODE    := $(OVMF_CODE_SOURCE)
OVMF_VARS    := $(BUILD_DIR)/OVMF_VARS.fd
MKMGFS       := $(BUILD_DIR)/mkmgfs
SPROUT       := $(SPROUT_DIR)/sprout.elf
HELLO        := $(HELLO_DIR)/hello.elf
SHOOT        := $(SHOOT_DIR)/shoot.elf
CLEAR        := $(CLEAR_DIR)/clear.elf
CP           := $(CP_DIR)/cp.elf
LS           := $(LS_DIR)/ls.elf
LOCATE       := $(LOCATE_DIR)/locate.elf
MV           := $(MV_DIR)/mv.elf
PLANT        := $(PLANT_DIR)/plant.elf
READ         := $(READ_DIR)/read.elf
RM           := $(RM_DIR)/rm.elf
MKDIR        := $(MKDIR_DIR)/mkdir.elf
RMDIR        := $(RMDIR_DIR)/rmdir.elf
SAY          := $(SAY_DIR)/say.elf
UPTIME       := $(UPTIME_DIR)/uptime.elf
VERSION      := $(VERSION_DIR)/version.elf
WHERE        := $(WHERE_DIR)/where.elf
FSTEST       := $(FSTEST_DIR)/fstest.elf
NETTEST      := $(NETTEST_DIR)/nettest.elf
PING         := $(PING_DIR)/ping.elf
RESOLVE      := $(RESOLVE_DIR)/resolve.elf
FETCH        := $(FETCH_DIR)/fetch.elf
NETWORK      := $(NETWORK_DIR)/network.elf
POWER        := $(POWER_DIR)/power.elf
IDENTITY     := $(IDENTITY_DIR)/identity.elf
USER_CMD     := $(USER_CMD_DIR)/user.elf
SHUTDOWN     := $(SHUTDOWN_DIR)/shutdown.elf
REBOOT       := $(REBOOT_DIR)/reboot.elf
COMMAND_PATH_OBJ := $(BUILD_DIR)/userspace/command_path.o
HELP_OBJ     := $(BUILD_DIR)/userspace/help.o
USER_LIBC    := $(USER_LIBC_DIR)/libc.a
USER_CRT     := $(BUILD_DIR)/userspace/crt0.o

DEPFLAGS     := -MMD -MP

BOOT_CFLAGS  := --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -Iboot/include -Iinclude $(DEPFLAGS)
BOOT_ASFLAGS := --target=x86_64-pc-windows-msvc
BOOT_LDFLAGS := /subsystem:efi_application /entry:efi_main /nodefaultlib /fixed:no

# Interrupt entry preserves GPRs but does not yet save architectural floating
# point/SIMD state.  Keep asynchronous kernel C code strictly general-register
# only until the kernel has a complete FPU/SIMD context-switching design.
KERNEL_CFLAGS  := --target=x86_64-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel -Ikernel/include -Ikernel/include/usb -Ikernel/include/pci -Ikernel/include/storage -Iinclude -Ilibc/include -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float $(DEPFLAGS)
KERNEL_ASFLAGS := --target=x86_64-elf
KERNEL_LDFLAGS := -T kernel/linker.ld -Map=$(KERNEL_MAP)

USER_CFLAGS := --target=x86_64-elf -ffreestanding -fno-stack-protector \
               -fno-builtin -fno-pic -fno-pie -mno-red-zone -nostdinc \
               -I. -Iuserspace/shoot -Ikernel/include -Ilibc/include -Iinclude \
               $(DEPFLAGS)
USER_LINKER_SCRIPT := userspace/linker/userspace.ld

# Boot-time subsystem smoke tests are intentionally excluded from the normal
# Mangrove boot path.  Build with DEBUG_BOOT_TESTS=1 to include them.
ifneq ($(DEBUG_BOOT_TESTS),)
KERNEL_CFLAGS += -DPITH_DEBUG_BOOT_TESTS
endif

# Low-level kernel bring-up details are excluded from normal production boot.
# Use KERNEL_BOOT_DEBUG=1 when auditing the initialization path.
ifeq ($(KERNEL_BOOT_DEBUG),1)
KERNEL_CFLAGS += -DKERNEL_BOOT_DEBUG=1
endif

# Optional host-side TCP echo smoke test.  It is intentionally off in normal
# images and uses the DHCP-learned gateway at runtime.
ifeq ($(TCP_ECHO_TEST),1)
KERNEL_CFLAGS += -DPITH_TCP_ECHO_TEST=1
endif

# Optional one-shot kernel HTTP validation; disabled for normal images.
ifeq ($(HTTP_GET_TEST),1)
KERNEL_CFLAGS += -DPITH_HTTP_GET_TEST=1
endif

# Detailed USB/xHCI investigation traces are excluded from normal builds.
# Use `make -B XHCI_DEBUG=1` when the low-level controller traces are needed.
ifeq ($(XHCI_DEBUG),1)
KERNEL_CFLAGS += -DXHCI_DEBUG=1
endif

# Opt-in DHCP/boot-network milestone diagnostics.  Normal images remain
# silent; the stream is mirrored to QEMU serial output when enabled.
ifeq ($(NETWORK_BOOT_DIAG),1)
KERNEL_CFLAGS += -DNETWORK_BOOT_DIAG=1
endif

# Opt-in ACPI battery/adapter discovery diagnostics for real-hardware tests.
# Normal images keep AML evaluation failures silent and report only the
# user-facing unavailable state.
ifeq ($(ACPI_POWER_DEBUG),1)
KERNEL_CFLAGS += -DACPI_POWER_DEBUG=1
endif

# Opt-in platform temperature diagnostics for real-hardware sensor tests.
ifeq ($(PLATFORM_THERMAL_DEBUG),1)
KERNEL_CFLAGS += -DPLATFORM_THERMAL_DEBUG=1
endif

# Automatic Source Discovery
BOOT_C_SRCS    := $(shell find boot/src -name '*.c')
BOOT_S_SRCS    := $(shell find boot/src -name '*.s')
KERNEL_C_SRCS  := $(shell find kernel/src -name '*.c')
KERNEL_S_SRCS  := $(shell find kernel/src -name '*.s')
DRIVERS_C_SRCS := $(shell find drivers -name '*.c')
LIBC_C_SRCS    := $(shell find libc/src -name '*.c' ! -name 'mangrove_syscall.c' ! -name 'allocator.c' ! -name 'stdio.c' ! -name 'native.c' ! -name 'line_editor.c' ! -name 'net.c')

# Object Mappings
BOOT_OBJS := $(patsubst boot/src/%.c,$(BUILD_DIR)/boot/%.o,$(BOOT_C_SRCS))
BOOT_OBJS += $(patsubst boot/src/%.s,$(BUILD_DIR)/boot/%.o,$(BOOT_S_SRCS))

KERNEL_OBJS := $(patsubst kernel/src/%.c,$(BUILD_DIR)/kernel/%.o,$(KERNEL_C_SRCS))
KERNEL_OBJS += $(patsubst kernel/src/%.s,$(BUILD_DIR)/kernel/%.o,$(KERNEL_S_SRCS))
KERNEL_OBJS += $(BUILD_DIR)/kernel/font_blob.o

DRIVERS_OBJS := $(patsubst drivers/%.c,$(BUILD_DIR)/drivers/%.o,$(DRIVERS_C_SRCS))
LIBC_OBJS    := $(patsubst libc/src/%.c,$(BUILD_DIR)/libc/%.o,$(LIBC_C_SRCS))

ALL_KERNEL_OBJS := $(KERNEL_OBJS) $(DRIVERS_OBJS) $(LIBC_OBJS)

DEPS := $(BOOT_OBJS:.o=.d) $(ALL_KERNEL_OBJS:.o=.d)

.PHONY: all help make fresh run fresh-run usb clean test \
        binaries sprout hello shoot clear cp ls locate mv mkdir plant read rm rmdir say shutdown reboot uptime version where fstest nettest ping resolve fetch network power identity user \
        image fresh-image usb-image run-usb mkmgfs mgfsck test-mgfsck test-libc test-net \
        check-image-deps check-usb-deps check-qemu-deps qemu-warning dev-image fresh-dev-image flash-image

# Everyday targets
all: image

help:
	@echo "Everyday commands:"
	@echo "  make             Build/update the persistent development image"
	@echo "  make run         Build and boot the persistent USB/xHCI development image"
	@echo "  make fresh       Reset the persistent development image"
	@echo "  make fresh-run   Reset the development image and boot it"
	@echo "  make usb         Build build/Mangrove/MangroveUSB.img for hardware"
	@echo "  make clean       Remove disposable build artifacts; preserve .mangrove/"
	@echo "  make test        Run the available host-side test suites"
	@echo
	@echo "Specialist targets: binaries image fresh-image usb-image mkmgfs mgfsck"
	@echo "                    test-libc test-net test-mgfsck and individual programs"

make: image

fresh: fresh-dev-image

usb: flash-image

test:
	@status=0; \
	for target in test-libc test-net test-mgfsck; do \
		if $(MAKE) --no-print-directory $$target; then :; else status=1; fi; \
	done; \
	exit $$status

binaries: $(EFI) $(KERNEL) $(SPROUT) $(SHOOT) $(CLEAR) $(CP) $(LS) $(LOCATE) $(MV) $(MKDIR) $(PLANT) $(READ) $(RM) $(RMDIR) $(SAY) $(SHUTDOWN) $(REBOOT) $(UPTIME) $(VERSION) $(WHERE) $(PING) $(RESOLVE) $(FETCH) $(NETWORK) $(POWER) $(IDENTITY) $(USER_CMD)

shoot: $(SHOOT)

clear: $(CLEAR)

cp: $(CP)

ls: $(LS)
locate: $(LOCATE)
mv: $(MV)
mkdir: $(MKDIR)
plant: $(PLANT)
read: $(READ)
rm: $(RM)
rmdir: $(RMDIR)

say: $(SAY)

shutdown: $(SHUTDOWN)

reboot: $(REBOOT)

uptime: $(UPTIME)

version: $(VERSION)
where: $(WHERE)

sprout: $(SPROUT)

hello: $(HELLO)

fstest: $(FSTEST)
nettest: $(NETTEST)
ping: $(PING)
resolve: $(RESOLVE)
fetch: $(FETCH)
network: $(NETWORK)

power: $(POWER)

identity: $(IDENTITY)

user: $(USER_CMD)

mkmgfs: $(MKMGFS)

mgfsck: $(BUILD_DIR)/mgfsck

test-mgfsck: $(BUILD_DIR)/mgfsck $(MKMGFS)
	@if [ ! -f tests/test_mgfsck.sh ]; then \
		echo "UNAVAILABLE: test-mgfsck (tests/test_mgfsck.sh is missing)" >&2; \
		exit 2; \
	fi
	./tests/test_mgfsck.sh

test-libc:
	@if [ ! -f tests/libc_string_test.c ]; then \
		echo "UNAVAILABLE: test-libc (tests/ directory is missing)" >&2; \
		exit 2; \
	fi
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude tests/libc_string_test.c libc/src/string.c \
		-o /tmp/mangrove-libc-string-test
	/tmp/mangrove-libc-string-test
	@echo libc string tests passed
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude tests/libc_allocator_test.c \
		libc/src/allocator.c libc/src/string.c \
		-Dmalloc=mg_test_malloc -Dcalloc=mg_test_calloc \
		-Drealloc=mg_test_realloc -Dfree=mg_test_free \
		-o /tmp/mangrove-libc-allocator-test
	/tmp/mangrove-libc-allocator-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude tests/libc_stdio_test.c \
		libc/src/stdio.c libc/src/string.c \
		-o /tmp/mangrove-libc-stdio-test
	/tmp/mangrove-libc-stdio-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude tests/libc_native_test.c \
		libc/src/native.c libc/src/string.c \
		-o /tmp/mangrove-libc-native-test
	/tmp/mangrove-libc-native-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude tests/libc_net_test.c libc/src/net.c \
		-o /tmp/mangrove-libc-net-test
	/tmp/mangrove-libc-net-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-I. -Ilibc/include -Iinclude tests/ping_args_test.c userspace/ping/ping_args.c \
		-o /tmp/mangrove-ping-args-test
	/tmp/mangrove-ping-args-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-I. -Ilibc/include -Iinclude tests/fetch_url_test.c userspace/fetch/fetch_url.c \
		-o /tmp/mangrove-fetch-url-test
	/tmp/mangrove-fetch-url-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin -ffunction-sections \
		-I. -Ilibc/include -Iinclude tests/fetch_http_test.c \
		-Wl,--gc-sections -o /tmp/mangrove-fetch-http-test
	/tmp/mangrove-fetch-http-test

test-net:
	@if [ ! -f tests/net_checksum_test.c ]; then \
		echo "UNAVAILABLE: test-net (tests/ directory is missing)" >&2; \
		exit 2; \
	fi
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Iinclude -Ikernel/include tests/net_checksum_test.c \
		kernel/src/net/checksum.c -o /tmp/mangrove-net-checksum-test
	/tmp/mangrove-net-checksum-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror tests/net_udp_checksum_test.c \
		-o /tmp/mangrove-net-udp-test
	/tmp/mangrove-net-udp-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Iinclude -Ikernel/include tests/net_dns_test.c kernel/src/net/dns_wire.c \
		-o /tmp/mangrove-net-dns-test
	/tmp/mangrove-net-dns-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Iinclude -Ikernel/include tests/net_tcp_test.c kernel/src/net/tcp_wire.c \
		kernel/src/net/checksum.c -o /tmp/mangrove-net-tcp-test
	/tmp/mangrove-net-tcp-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Iinclude -Ikernel/include tests/net_tcp_state_test.c kernel/src/net/tcp.c \
		kernel/src/net/tcp_wire.c kernel/src/net/checksum.c \
		-o /tmp/mangrove-net-tcp-state-test
	/tmp/mangrove-net-tcp-state-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Iinclude -Ikernel/include tests/net_http_test.c kernel/src/net/http_wire.c \
		-o /tmp/mangrove-net-http-test
	/tmp/mangrove-net-http-test

check-image-deps:
	@missing=""; \
	for tool in mkfs.fat mmd mcopy python3; do \
		command -v "$$tool" >/dev/null 2>&1 || missing="$$missing $$tool"; \
	done; \
	if [ -n "$$missing" ]; then \
		echo "Missing image-build tools:$$missing" >&2; \
		if [ "$(UNAME)" = Darwin ]; then \
			echo "Install them with: brew install dosfstools mtools" >&2; \
		else \
			echo "On Debian/Ubuntu, install them with: sudo apt-get install dosfstools mtools python3" >&2; \
		fi; \
		exit 1; \
	fi; \
	if [ ! -f "$(OVMF_VARS_SOURCE)" ]; then \
		echo "Missing UEFI firmware: $(OVMF_VARS_SOURCE)" >&2; \
		if [ "$(UNAME)" = Darwin ]; then \
			echo "Install it with: brew install qemu" >&2; \
		else \
			echo "On Debian/Ubuntu, install it with: sudo apt-get install ovmf" >&2; \
		fi; \
		exit 1; \
	fi

check-usb-deps: check-image-deps
	@if [ "$(UNAME)" = Darwin ]; then \
		if ! command -v sgdisk >/dev/null 2>&1; then \
			echo "Missing macOS USB-image tool: sgdisk" >&2; \
			echo "Install it with: brew install gptfdisk" >&2; \
			exit 1; \
		fi; \
	elif [ "$(UNAME)" = Linux ]; then \
		if ! command -v parted >/dev/null 2>&1; then \
			echo "Missing Linux USB-image tool: parted" >&2; \
			echo "On Debian/Ubuntu, install it with: sudo apt-get install parted" >&2; \
			exit 1; \
		fi; \
	else \
		echo "Unsupported host OS for usb-image: $(UNAME)" >&2; \
		exit 1; \
	fi

check-qemu-deps:
	@if ! command -v "$(QEMU)" >/dev/null 2>&1; then \
		echo "Missing QEMU executable: $(QEMU)" >&2; \
		if [ "$(UNAME)" = Darwin ]; then \
			echo "Install it with: brew install qemu" >&2; \
		else \
			echo "On Debian/Ubuntu, install it with: sudo apt-get install qemu-system-x86 ovmf" >&2; \
		fi; \
		exit 1; \
	fi; \
	if ! "$(QEMU)" -accel help 2>/dev/null | grep -q "^[[:space:]]*$(QEMU_ACCEL)[[:space:]]*$$"; then \
		echo "$(QEMU) does not provide the required $(QEMU_ACCEL) accelerator on $(UNAME)." >&2; \
		echo "Available accelerators:" >&2; \
		"$(QEMU)" -accel help >&2; \
		exit 1; \
	fi; \
	if [ "$(UNAME)" = Linux ] && [ ! -r /dev/kvm -o ! -w /dev/kvm ]; then \
		echo "KVM is unavailable: /dev/kvm must exist and be readable and writable." >&2; \
		echo "Load the KVM module and add your user to the kvm group, then log in again." >&2; \
		exit 1; \
	fi; \
	if [ "$(UNAME)" = Darwin ] && [ "$(QEMU_ACCEL)" = hvf ] && [ "$$(sysctl -n kern.hv_support 2>/dev/null)" != 1 ]; then \
		echo "HVF is unavailable: this Mac does not report Hypervisor Framework support." >&2; \
		exit 1; \
	fi; \
	if [ ! -f "$(OVMF_CODE_SOURCE)" ]; then \
		echo "Missing UEFI firmware: $(OVMF_CODE_SOURCE)" >&2; \
		exit 1; \
	fi

ifeq ($(QEMU_ACCEL),tcg)
qemu-warning:
	@echo "Warning: Running x86_64 Mangrove under TCG software emulation on Apple Silicon; performance will be slower." >&2
else
qemu-warning:
endif

dev-image: check-usb-deps binaries $(MKMGFS) $(OVMF_VARS)
	@mkdir -p $(STATE_DIR)
	./scripts/update_dev_image.sh --disk "$(DEV_IMAGE)" --root "$(DEV_ROOT_IMAGE)"

fresh-dev-image: check-usb-deps binaries $(MKMGFS) $(OVMF_VARS)
	@mkdir -p $(STATE_DIR)
	@echo "[FRESH] Factory-resetting persistent development image $(DEV_IMAGE)"
	./scripts/update_dev_image.sh --fresh --disk "$(DEV_IMAGE)" --root "$(DEV_ROOT_IMAGE)"

flash-image: check-usb-deps binaries $(MKMGFS) $(OVMF_VARS)
	./scripts/make_image.sh --fresh --root "$(FLASH_ROOT_IMAGE)" --autologin developer
	@mkdir -p $(MANGROVE_DIR)
	@rm -f $(USB_IMAGE)
	@dd if=/dev/zero of=$(USB_IMAGE) bs=1 count=0 seek=135283200 2>/dev/null
ifeq ($(UNAME),Darwin)
	@sgdisk --zap-all \
		--new=1:2048:133119 --typecode=1:EF00 --change-name=1:ESP \
		--new=2:133120:264191 --typecode=2:8300 --change-name=2:primary \
		$(USB_IMAGE) >/dev/null 2>&1 || { \
			echo "sgdisk failed while creating the GPT in $(USB_IMAGE)" >&2; \
			exit 1; \
		}
else
	@parted -s -a minimal $(USB_IMAGE) mklabel gpt
	@parted -s -a minimal $(USB_IMAGE) mkpart ESP fat32 2048s 133119s
	@parted -s -a minimal $(USB_IMAGE) set 1 esp on
	@parted -s -a minimal $(USB_IMAGE) mkpart primary 133120s 264191s
endif
	@dd if=$(MANGROVE_DIR)/Boot.img of=$(USB_IMAGE) bs=512 seek=2048 conv=notrunc 2>/dev/null
	@dd if=$(FLASH_ROOT_IMAGE) of=$(USB_IMAGE) bs=512 seek=133120 conv=notrunc 2>/dev/null
	@echo "Created $(USB_IMAGE)"

# Specialist compatibility names.
image: dev-image
fresh-image: fresh-dev-image
usb-image: flash-image

QEMU_RUN_ARGS = \
	-machine q35 \
	$(QEMU_PLATFORM_ARGS) \
	-m 512M \
	-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	-drive if=pflash,format=raw,file=$(OVMF_VARS) \
	-drive id=usb,file=$(DEV_IMAGE),format=raw,if=none \
	-netdev user,id=net0 \
	-device e1000,netdev=net0,mac=52:54:00:18:01:01 \
	-device qemu-xhci,id=xhci \
	-device usb-storage,bus=xhci.0,port=2,drive=usb,bootindex=1 \
	-device usb-kbd,bus=xhci.0,port=1

run: check-qemu-deps qemu-warning dev-image
	$(QEMU) $(QEMU_RUN_ARGS)

run-usb: run

fresh-run: check-qemu-deps qemu-warning fresh-dev-image
	$(QEMU) $(QEMU_RUN_ARGS)

clean:
	@mkdir -p $(STATE_DIR)
	@if [ ! -f "$(DEV_IMAGE)" ] && [ ! -f "$(DEV_ROOT_IMAGE)" ] && [ -f "$(LEGACY_DEV_IMAGE)" ]; then \
		echo "[CLEAN] Preserving legacy MGFS state as $(DEV_ROOT_IMAGE)"; \
		cp "$(LEGACY_DEV_IMAGE)" "$(DEV_ROOT_IMAGE)"; \
	fi
	rm -rf $(BUILD_DIR)
	@echo "[CLEAN] Removed $(BUILD_DIR); preserved $(STATE_DIR)/"

# OVMF Variable
$(OVMF_VARS):
	@mkdir -p $(dir $@)
	cp $(OVMF_VARS_SOURCE) $@

# Bootloader Link
$(EFI): $(BOOT_OBJS)
	@mkdir -p $(dir $@)
	$(LD_BOOT) $(BOOT_LDFLAGS) /out:$@ $^

# Kernel Link
$(KERNEL): $(ALL_KERNEL_OBJS) kernel/linker.ld
	@mkdir -p $(dir $@)
	$(LD_KERNEL) $(KERNEL_LDFLAGS) -o $@ $(ALL_KERNEL_OBJS)

$(MKMGFS): tools/mkmgfs.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 $< -o $@

SHOOT_C_SRCS := userspace/shoot/main.c \
                userspace/shoot/shell.c \
                userspace/shoot/builtin.c \
                userspace/shoot/help.c \
                userspace/shoot/commands/exit.c \
                userspace/shoot/commands/help.c \
                userspace/shoot/commands/cd.c

SHOOT_OBJS := $(patsubst userspace/shoot/%.c,$(SHOOT_DIR)/%.o,$(SHOOT_C_SRCS))

USER_C_OBJS := $(BUILD_DIR)/Sprout/sprout.o \
               $(CLEAR_DIR)/clear.o \
               $(CP_DIR)/main.o \
               $(LS_DIR)/main.o \
               $(LOCATE_DIR)/locate.o \
               $(MV_DIR)/main.o \
               $(MKDIR_DIR)/main.o \
               $(PLANT_DIR)/plant.o \
               $(READ_DIR)/read.o \
               $(RM_DIR)/main.o \
               $(RMDIR_DIR)/main.o \
               $(SAY_DIR)/say.o \
               $(SHUTDOWN_DIR)/shutdown.o \
               $(REBOOT_DIR)/reboot.o \
               $(UPTIME_DIR)/uptime.o \
               $(VERSION_DIR)/version.o \
               $(WHERE_DIR)/where.o \
               $(PING_DIR)/main.o \
               $(PING_DIR)/ping_args.o \
               $(RESOLVE_DIR)/main.o \
               $(FETCH_DIR)/main.o \
               $(FETCH_DIR)/fetch_url.o \
               $(NETWORK_DIR)/main.o \
               $(POWER_DIR)/power.o \
               $(IDENTITY_DIR)/identity.o \
               $(USER_CMD_DIR)/user.o \
               $(USER_LIBC_DIR)/syscall_c.o \
               $(USER_LIBC_DIR)/string.o \
               $(USER_LIBC_DIR)/allocator.o \
               $(USER_LIBC_DIR)/stdio.o \
               $(USER_LIBC_DIR)/native.o \
               $(USER_LIBC_DIR)/line_editor.o \
               $(USER_LIBC_DIR)/net.o \
               $(COMMAND_PATH_OBJ) \
               $(HELP_OBJ) \
               $(BUILD_DIR)/userspace/secret_input.o \
               $(SHOOT_OBJS)
USER_DEPS := $(USER_C_OBJS:.o=.d)

$(BUILD_DIR)/Sprout/sprout.o: userspace/sprout/main.c \
                              include/mangrove_version.h \
                              $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/sprout -c $< -o $@

$(BUILD_DIR)/userspace/secret_input.o: userspace/common/secret_input.c \
                                      userspace/common/secret_input.h \
                                      $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(HELP_OBJ): userspace/common/help.c userspace/common/help.h \
             libc/include/mg/filesystem.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(SPROUT): $(BUILD_DIR)/Sprout/sprout.o \
           $(BUILD_DIR)/userspace/secret_input.o $(USER_CRT) $(USER_LIBC) \
           $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(BUILD_DIR)/Sprout/sprout.o \
		$(BUILD_DIR)/userspace/secret_input.o $(USER_LIBC)

$(COMMAND_PATH_OBJ): userspace/common/path.c userspace/common/path.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(CLEAR_DIR)/clear.o: userspace/clear/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(CLEAR): $(CLEAR_DIR)/clear.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(CLEAR_DIR)/clear.o $(HELP_OBJ) $(USER_LIBC)

$(CP_DIR)/main.o: userspace/cp/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(CP): $(CP_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(CP_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(LS_DIR)/main.o: userspace/ls/main.c userspace/common/path.h \
                  userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(LS): $(LS_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(LS_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(LOCATE_DIR)/locate.o: userspace/locate/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(LOCATE): $(LOCATE_DIR)/locate.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(LOCATE_DIR)/locate.o $(HELP_OBJ) $(USER_LIBC)

$(MV_DIR)/main.o: userspace/mv/main.c userspace/common/path.h \
                  userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(MV): $(MV_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(MV_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(MKDIR_DIR)/main.o: userspace/mkdir/main.c userspace/common/path.h \
                     userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(MKDIR): $(MKDIR_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(MKDIR_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(PLANT_DIR)/plant.o: userspace/plant/main.c userspace/common/path.h \
                      userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(PLANT): $(PLANT_DIR)/plant.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(PLANT_DIR)/plant.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(READ_DIR)/read.o: userspace/read/main.c userspace/common/path.h \
                    userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(READ): $(READ_DIR)/read.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(READ_DIR)/read.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(RM_DIR)/main.o: userspace/rm/main.c userspace/common/path.h \
                  userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(RM): $(RM_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(RM_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(RMDIR_DIR)/main.o: userspace/rmdir/main.c userspace/common/path.h \
                     userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(RMDIR): $(RMDIR_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(RMDIR_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(SAY_DIR)/say.o: userspace/say/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SAY): $(SAY_DIR)/say.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(SAY_DIR)/say.o $(HELP_OBJ) $(USER_LIBC)

$(SHUTDOWN_DIR)/shutdown.o: userspace/shutdown/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SHUTDOWN): $(SHUTDOWN_DIR)/shutdown.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(SHUTDOWN_DIR)/shutdown.o $(HELP_OBJ) $(USER_LIBC)

$(REBOOT_DIR)/reboot.o: userspace/reboot/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(REBOOT): $(REBOOT_DIR)/reboot.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(REBOOT_DIR)/reboot.o $(HELP_OBJ) $(USER_LIBC)

$(POWER_DIR)/power.o: userspace/power/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(POWER): $(POWER_DIR)/power.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(POWER_DIR)/power.o $(HELP_OBJ) $(USER_LIBC)

$(IDENTITY_DIR)/identity.o: userspace/identity/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(IDENTITY): $(IDENTITY_DIR)/identity.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(IDENTITY_DIR)/identity.o $(HELP_OBJ) $(USER_LIBC)

$(USER_CMD_DIR)/user.o: userspace/user/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_CMD): $(USER_CMD_DIR)/user.o $(BUILD_DIR)/userspace/secret_input.o \
             $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(USER_CMD_DIR)/user.o \
		$(BUILD_DIR)/userspace/secret_input.o $(HELP_OBJ) $(USER_LIBC)

$(UPTIME_DIR)/uptime.o: userspace/uptime/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(UPTIME): $(UPTIME_DIR)/uptime.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(UPTIME_DIR)/uptime.o $(HELP_OBJ) $(USER_LIBC)

$(VERSION_DIR)/version.o: userspace/version/main.c include/mangrove_version.h userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -I. -c $< -o $@

$(VERSION): $(VERSION_DIR)/version.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(VERSION_DIR)/version.o $(HELP_OBJ) $(USER_LIBC)

$(WHERE_DIR)/where.o: userspace/where/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(WHERE): $(WHERE_DIR)/where.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(WHERE_DIR)/where.o $(HELP_OBJ) $(USER_LIBC)

$(BUILD_DIR)/Hello/hello.o: userspace/hello/main.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(HELLO): $(BUILD_DIR)/Hello/hello.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(BUILD_DIR)/Hello/hello.o $(USER_LIBC)

$(BUILD_DIR)/FsTest/fstest.o: userspace/fstest/main.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(FSTEST): $(BUILD_DIR)/FsTest/fstest.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(BUILD_DIR)/FsTest/fstest.o $(USER_LIBC)

$(NETTEST_DIR)/nettest.o: userspace/nettest/main.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(NETTEST): $(NETTEST_DIR)/nettest.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(NETTEST_DIR)/nettest.o $(USER_LIBC)

$(PING_DIR)/main.o: userspace/ping/main.c userspace/ping/ping_args.h \
                    userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/ping -c $< -o $@

$(PING_DIR)/ping_args.o: userspace/ping/ping_args.c userspace/ping/ping_args.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/ping -c $< -o $@

$(PING): $(PING_DIR)/main.o $(PING_DIR)/ping_args.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(PING_DIR)/main.o $(PING_DIR)/ping_args.o $(HELP_OBJ) $(USER_LIBC)

$(RESOLVE_DIR)/main.o: userspace/resolve/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(RESOLVE): $(RESOLVE_DIR)/main.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(RESOLVE_DIR)/main.o $(HELP_OBJ) $(USER_LIBC)

$(FETCH_DIR)/main.o: userspace/fetch/main.c userspace/fetch/fetch_url.h \
                    userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/fetch -Wframe-larger-than=16384 -Werror -c $< -o $@

$(FETCH_DIR)/fetch_url.o: userspace/fetch/fetch_url.c userspace/fetch/fetch_url.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/fetch -c $< -o $@

$(FETCH): $(FETCH_DIR)/main.o $(FETCH_DIR)/fetch_url.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(FETCH_DIR)/main.o $(FETCH_DIR)/fetch_url.o $(HELP_OBJ) $(USER_LIBC)

$(NETWORK_DIR)/main.o: userspace/network/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(NETWORK): $(NETWORK_DIR)/main.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(NETWORK_DIR)/main.o $(HELP_OBJ) $(USER_LIBC)

$(USER_LIBC_DIR)/syscall.o: libc/src/mangrove_syscall.s
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-elf -mno-red-zone -c $< -o $@

$(USER_LIBC_DIR)/syscall_c.o: libc/src/mangrove_syscall.c libc/include/mangrove.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/string.o: libc/src/string.c libc/include/string.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC): $(USER_LIBC_DIR)/syscall.o $(USER_LIBC_DIR)/syscall_c.o $(USER_LIBC_DIR)/string.o $(USER_LIBC_DIR)/allocator.o $(USER_LIBC_DIR)/stdio.o $(USER_LIBC_DIR)/native.o $(USER_LIBC_DIR)/line_editor.o $(USER_LIBC_DIR)/net.o
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(USER_LIBC_DIR)/allocator.o: libc/src/allocator.c libc/include/stdlib.h libc/include/mangrove.h libc/include/string.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/stdio.o: libc/src/stdio.c libc/include/stdio.h libc/include/mangrove.h libc/include/string.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/native.o: libc/src/native.c libc/include/mangrove.h libc/include/string.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/line_editor.o: libc/src/line_editor.c libc/include/mg/line_editor.h libc/include/mg/object.h libc/include/string.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/net.o: libc/src/net.c libc/include/mg/net.h libc/include/mangrove_errors.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_CRT): libc/crt/crt0.s
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-elf -mno-red-zone -c $< -o $@

$(SHOOT_DIR)/%.o: userspace/shoot/%.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SHOOT): $(SHOOT_OBJS) $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(SHOOT_OBJS) $(HELP_OBJ) $(USER_LIBC)

$(BUILD_DIR)/mgfsck: tools/mgfsck.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Wno-parentheses -Wno-unused-parameter -O2 $< -o $@

# Bootloader Compilation
$(BUILD_DIR)/boot/%.o: boot/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/boot/%.o: boot/src/%.s
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_ASFLAGS) -c $< -o $@

# Kernel Compilation
$(BUILD_DIR)/kernel/%.o: kernel/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/%.o: kernel/src/%.s
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_ASFLAGS) -c $< -o $@

# Font Blob Compilation
$(BUILD_DIR)/kernel/font_blob.o: kernel/assets/font.psf
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386 $< $@

# Drivers Compilation
$(BUILD_DIR)/drivers/%.o: drivers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# Libc Compilation
$(BUILD_DIR)/libc/%.o: libc/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

-include $(DEPS) $(USER_DEPS)
