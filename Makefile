CC          := clang
LD_BOOT     := lld-link
LD_KERNEL   := ld.lld
OBJCOPY     := objcopy
QEMU        := qemu-system-x86_64

BUILD_DIR    := build
EFI_DIR      := $(BUILD_DIR)/EFI/BOOT
MANGROVE_DIR := $(BUILD_DIR)/Mangrove

EFI          := $(EFI_DIR)/BOOTX64.EFI
KERNEL       := $(MANGROVE_DIR)/kernel.elf
KERNEL_MAP   := $(MANGROVE_DIR)/kernel.map
OVMF_CODE    := /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS    := $(BUILD_DIR)/OVMF_VARS.fd

DEPFLAGS     := -MMD -MP

BOOT_CFLAGS  := --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -Iboot/include -Iinclude $(DEPFLAGS)
BOOT_ASFLAGS := --target=x86_64-pc-windows-msvc
BOOT_LDFLAGS := /subsystem:efi_application /entry:efi_main /nodefaultlib /fixed:no

KERNEL_CFLAGS  := --target=x86_64-elf -ffreestanding -fno-stack-protector -Ikernel/include -Iinclude -Ilibc/include -mno-red-zone $(DEPFLAGS)
KERNEL_ASFLAGS := --target=x86_64-elf
KERNEL_LDFLAGS := -T kernel/linker.ld -Map=$(KERNEL_MAP)

# Automatic Source Discovery
BOOT_C_SRCS    := $(shell find boot/src -name '*.c')
BOOT_S_SRCS    := $(shell find boot/src -name '*.s')
KERNEL_C_SRCS  := $(shell find kernel/src -name '*.c')
KERNEL_S_SRCS  := $(shell find kernel/src -name '*.s')
DRIVERS_C_SRCS := $(shell find drivers -name '*.c')
LIBC_C_SRCS    := $(shell find libc/src -name '*.c')

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

.PHONY: all binaries image run clean

# Targets
all: image

binaries: $(EFI) $(KERNEL)

image: binaries $(OVMF_VARS)
	./scripts/make_image.sh

run: image
	$(QEMU) \
		-machine q35 \
		-m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-drive format=raw,file=fat:rw:$(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)/boot \
	       $(BUILD_DIR)/kernel \
	       $(BUILD_DIR)/drivers \
	       $(BUILD_DIR)/libc \
	       $(BUILD_DIR)/EFI \
	       $(BUILD_DIR)/Mangrove \
	       $(OVMF_VARS)

# OVMF Variables
$(OVMF_VARS):
	@mkdir -p $(dir $@)
	cp /usr/share/edk2/x64/OVMF_VARS.4m.fd $@

# Bootloader Link
$(EFI): $(BOOT_OBJS)
	@mkdir -p $(dir $@)
	$(LD_BOOT) $(BOOT_LDFLAGS) /out:$@ $^

# Kernel Link
$(KERNEL): $(ALL_KERNEL_OBJS)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) $(KERNEL_LDFLAGS) -o $@ $^

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
$(BUILD_DIR)/kernel/font_blob.o: kernel/src/font.psf
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

-include $(DEPS)
