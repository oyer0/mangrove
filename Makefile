CC          := clang
LD_BOOT     := lld-link
LD_KERNEL   := ld.lld

ifeq ($(UNAME),Darwin)
    OBJCOPY := gobjcopy
else
    OBJCOPY := objcopy
endif

QEMU        := qemu-system-x86_64
HOST_CC     := cc
AR          := llvm-ar

UNAME := $(shell uname)

ifeq ($(UNAME),Darwin)
    OVMF_CODE_SOURCE := /opt/homebrew/opt/qemu/share/qemu/edk2-x86_64-code.fd
    OVMF_VARS_SOURCE := /opt/homebrew/opt/qemu/share/qemu/edk2-i386-vars.fd
else
    OVMF_CODE_SOURCE := /usr/share/OVMF/OVMF_CODE_4M.fd
    OVMF_VARS_SOURCE := /usr/share/OVMF/OVMF_VARS_4M.fd
endif

BUILD_DIR    := build
EFI_DIR      := $(BUILD_DIR)/EFI/BOOT
MANGROVE_DIR := $(BUILD_DIR)/Mangrove
USB_IMAGE    := $(MANGROVE_DIR)/MangroveUSB.img
SPROUT_DIR   := $(BUILD_DIR)/Sprout
HELLO_DIR    := $(BUILD_DIR)/Hello
SHOOT_DIR    := $(BUILD_DIR)/Shoot
COPY_DIR     := $(BUILD_DIR)/Copy
SAY_DIR      := $(BUILD_DIR)/Say
UPTIME_DIR   := $(BUILD_DIR)/Uptime
FSTEST_DIR   := $(BUILD_DIR)/FsTest
NETTEST_DIR  := $(BUILD_DIR)/NetTest
PING_DIR     := $(BUILD_DIR)/Ping
RESOLVE_DIR  := $(BUILD_DIR)/Resolve
FETCH_DIR    := $(BUILD_DIR)/Fetch
NETWORK_DIR  := $(BUILD_DIR)/Network
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
COPY         := $(COPY_DIR)/copy.elf
SAY          := $(SAY_DIR)/say.elf
UPTIME       := $(UPTIME_DIR)/uptime.elf
FSTEST       := $(FSTEST_DIR)/fstest.elf
NETTEST      := $(NETTEST_DIR)/nettest.elf
PING         := $(PING_DIR)/ping.elf
RESOLVE      := $(RESOLVE_DIR)/resolve.elf
FETCH        := $(FETCH_DIR)/fetch.elf
NETWORK      := $(NETWORK_DIR)/network.elf
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
# Rhizome boot path.  Build with DEBUG_BOOT_TESTS=1 to include them.
ifneq ($(DEBUG_BOOT_TESTS),)
KERNEL_CFLAGS += -DRHIZOME_DEBUG_BOOT_TESTS
endif

# Optional host-side TCP echo smoke test.  It is intentionally off in normal
# images and uses the DHCP-learned gateway at runtime.
ifeq ($(TCP_ECHO_TEST),1)
KERNEL_CFLAGS += -DRHIZOME_TCP_ECHO_TEST=1
endif

# Optional one-shot kernel HTTP validation; disabled for normal images.
ifeq ($(HTTP_GET_TEST),1)
KERNEL_CFLAGS += -DRHIZOME_HTTP_GET_TEST=1
endif

# Detailed USB/xHCI investigation traces are excluded from normal builds.
# Use `make -B XHCI_DEBUG=1` when the low-level controller traces are needed.
ifeq ($(XHCI_DEBUG),1)
KERNEL_CFLAGS += -DXHCI_DEBUG=1
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

.PHONY: all binaries sprout hello shoot copy say uptime fstest nettest ping resolve fetch network image fresh-image usb-image run run-usb fresh-run clean mkmgfs mgfsck test-mgfsck test-libc test-net

# Targets
all: image

binaries: $(EFI) $(KERNEL) $(SPROUT) $(SHOOT) $(COPY) $(SAY) $(UPTIME) $(HELLO) $(FSTEST) $(NETTEST) $(PING) $(RESOLVE) $(FETCH) $(NETWORK)

shoot: $(SHOOT)

copy: $(COPY)

say: $(SAY)

uptime: $(UPTIME)

sprout: $(SPROUT)

hello: $(HELLO)

fstest: $(FSTEST)
nettest: $(NETTEST)
ping: $(PING)
resolve: $(RESOLVE)
fetch: $(FETCH)
network: $(NETWORK)

mkmgfs: $(MKMGFS)

mgfsck: $(BUILD_DIR)/mgfsck

test-mgfsck: $(BUILD_DIR)/mgfsck $(MKMGFS)
	./tests/test_mgfsck.sh

test-libc: tests/libc_string_test.c libc/src/string.c libc/src/stdio.c
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

test-net: tests/net_checksum_test.c tests/net_udp_checksum_test.c tests/net_http_test.c kernel/src/net/checksum.c kernel/src/net/http_wire.c
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

image: binaries $(MKMGFS) $(OVMF_VARS)
	./scripts/make_image.sh

fresh-image: binaries $(MKMGFS) $(OVMF_VARS)
	./scripts/make_image.sh --fresh

usb-image: image
	@mkdir -p $(MANGROVE_DIR)
	@rm -f $(USB_IMAGE)
	@truncate -s 1141916160 $(USB_IMAGE)
	@parted -s -a minimal $(USB_IMAGE) mklabel gpt
	@parted -s -a minimal $(USB_IMAGE) mkpart ESP fat32 2048s 133119s
	@parted -s -a minimal $(USB_IMAGE) set 1 esp on
	@parted -s -a minimal $(USB_IMAGE) mkpart primary 133120s 2230271s
	@dd if=$(MANGROVE_DIR)/Boot.img of=$(USB_IMAGE) bs=512 seek=2048 conv=notrunc status=none
	@dd if=$(MANGROVE_DIR)/Mangrove.img of=$(USB_IMAGE) bs=512 seek=133120 conv=notrunc status=none
	@echo "Created $(USB_IMAGE)"

run: image
	$(QEMU) \
		-machine q35 \
		-accel kvm \
		-cpu host \
		-m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-drive id=boot,file=$(MANGROVE_DIR)/Boot.img,format=raw,if=none \
		-drive id=root,file=$(MANGROVE_DIR)/Mangrove.img,format=raw,if=none \
		-device ide-hd,drive=boot,bus=ide.0 \
		-device ide-hd,drive=root,bus=ide.1 \
		-netdev user,id=net0 \
		-device e1000,netdev=net0,mac=52:54:00:18:01:01 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0,port=1

run-usb: usb-image
	$(QEMU) \
		-machine q35 \
		-accel kvm \
		-cpu host \
		-m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-drive id=usb,file=$(USB_IMAGE),format=raw,if=none \
		-netdev user,id=net0 \
		-device e1000,netdev=net0,mac=52:54:00:18:01:01 \
		-device qemu-xhci,id=xhci \
		-device usb-storage,bus=xhci.0,port=2,drive=usb,bootindex=1 \
		-device usb-kbd,bus=xhci.0,port=1

fresh-run: fresh-image
	$(QEMU) \
		-machine q35 \
		-accel kvm \
		-cpu host \
		-m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-drive id=boot,file=$(MANGROVE_DIR)/Boot.img,format=raw,if=none \
		-drive id=root,file=$(MANGROVE_DIR)/Mangrove.img,format=raw,if=none \
		-device ide-hd,drive=boot,bus=ide.0 \
		-device ide-hd,drive=root,bus=ide.1 \
		-netdev user,id=net0 \
		-device e1000,netdev=net0,mac=52:54:00:18:01:01 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0,port=1

clean:
	rm -rf $(BUILD_DIR)

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
                userspace/shoot/version.c \
                userspace/shoot/commands/clear.c \
                userspace/shoot/commands/exit.c \
                userspace/shoot/commands/help.c \
                userspace/shoot/commands/jump.c \
                userspace/shoot/commands/list.c \
                userspace/shoot/commands/locate.c \
                userspace/shoot/commands/move.c \
                userspace/shoot/commands/plant.c \
                userspace/shoot/commands/read.c \
                userspace/shoot/commands/remove.c \
                userspace/shoot/commands/version.c \
                userspace/shoot/commands/where.c

SHOOT_OBJS := $(patsubst userspace/shoot/%.c,$(SHOOT_DIR)/%.o,$(SHOOT_C_SRCS))

USER_C_OBJS := $(BUILD_DIR)/Sprout/sprout.o \
               $(COPY_DIR)/copy.o \
               $(SAY_DIR)/say.o \
               $(UPTIME_DIR)/uptime.o \
               $(BUILD_DIR)/Hello/hello.o \
               $(BUILD_DIR)/FsTest/fstest.o \
               $(NETTEST_DIR)/nettest.o \
               $(PING_DIR)/main.o \
               $(PING_DIR)/ping_args.o \
               $(RESOLVE_DIR)/main.o \
               $(FETCH_DIR)/main.o \
               $(FETCH_DIR)/fetch_url.o \
               $(NETWORK_DIR)/main.o \
               $(USER_LIBC_DIR)/syscall_c.o \
               $(USER_LIBC_DIR)/string.o \
               $(USER_LIBC_DIR)/allocator.o \
               $(USER_LIBC_DIR)/stdio.o \
               $(USER_LIBC_DIR)/native.o \
               $(USER_LIBC_DIR)/line_editor.o \
               $(USER_LIBC_DIR)/net.o \
               $(SHOOT_OBJS)
USER_DEPS := $(USER_C_OBJS:.o=.d)

$(BUILD_DIR)/Sprout/sprout.o: userspace/sprout/main.c \
                              include/mangrove_version.h \
                              userspace/sprout/version.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/sprout -c $< -o $@

$(SPROUT): $(BUILD_DIR)/Sprout/sprout.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(BUILD_DIR)/Sprout/sprout.o $(USER_LIBC)

$(COPY_DIR)/copy.o: userspace/copy/main.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(COPY): $(COPY_DIR)/copy.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(COPY_DIR)/copy.o $(USER_LIBC)

$(SAY_DIR)/say.o: userspace/say/main.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SAY): $(SAY_DIR)/say.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(SAY_DIR)/say.o $(USER_LIBC)

$(UPTIME_DIR)/uptime.o: userspace/uptime/main.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(UPTIME): $(UPTIME_DIR)/uptime.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(UPTIME_DIR)/uptime.o $(USER_LIBC)

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

$(PING_DIR)/main.o: userspace/ping/main.c userspace/ping/ping_args.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/ping -c $< -o $@

$(PING_DIR)/ping_args.o: userspace/ping/ping_args.c userspace/ping/ping_args.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/ping -c $< -o $@

$(PING): $(PING_DIR)/main.o $(PING_DIR)/ping_args.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(PING_DIR)/main.o $(PING_DIR)/ping_args.o $(USER_LIBC)

$(RESOLVE_DIR)/main.o: userspace/resolve/main.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(RESOLVE): $(RESOLVE_DIR)/main.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(RESOLVE_DIR)/main.o $(USER_LIBC)

$(FETCH_DIR)/main.o: userspace/fetch/main.c userspace/fetch/fetch_url.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/fetch -Wframe-larger-than=16384 -Werror -c $< -o $@

$(FETCH_DIR)/fetch_url.o: userspace/fetch/fetch_url.c userspace/fetch/fetch_url.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/fetch -c $< -o $@

$(FETCH): $(FETCH_DIR)/main.o $(FETCH_DIR)/fetch_url.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(FETCH_DIR)/main.o $(FETCH_DIR)/fetch_url.o $(USER_LIBC)

$(NETWORK_DIR)/main.o: userspace/network/main.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(NETWORK): $(NETWORK_DIR)/main.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(NETWORK_DIR)/main.o $(USER_LIBC)

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

$(SHOOT_DIR)/version.o: include/mangrove_version.h \
                        kernel/include/version.h \
                        userspace/shoot/version.h

$(SHOOT): $(SHOOT_OBJS) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(SHOOT_OBJS) $(USER_LIBC)

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
