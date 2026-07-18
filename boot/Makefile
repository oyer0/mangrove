CC := clang
LD := lld-link
EFI := ../build/EFI/BOOT/BOOTX64.EFI
BUILD_DIR := ../build/boot
OBJ := $(BUILD_DIR)/main.obj
EFI_DIR := ../build/EFI/BOOT
CFLAGS := \
    --target=x86_64-pc-windows-msvc \
    -ffreestanding \
    -fno-stack-protector \
    -Iinclude \
    -I../include
SRC_DIR := src

all: $(EFI)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(EFI_DIR):
	mkdir -p $(EFI_DIR)

$(OBJ): $(SRC_DIR)/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(EFI): $(OBJ) | $(EFI_DIR)
	$(LD) \
		/subsystem:efi_application \
		/entry:efi_main \
		/nodefaultlib \
		/fixed:no \
		/out:$(EFI) \
		$(OBJ)

clean:
	rm -rf $(BUILD_DIR)
