CC := clang
LD := lld-link
EFI := ../build/EFI/BOOT/BOOTX64.EFI
BUILD_DIR := ../build/boot
OBJ := \
    $(BUILD_DIR)/main.obj \
    $(BUILD_DIR)/console.obj \
    $(BUILD_DIR)/memory.obj \
	$(BUILD_DIR)/filesystem.obj \
	$(BUILD_DIR)/kloader.obj

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

$(BUILD_DIR)/main.obj: $(SRC_DIR)/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/console.obj: $(SRC_DIR)/console.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/memory.obj: $(SRC_DIR)/memory.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/filesystem.obj: $(SRC_DIR)/filesystem.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kloader.obj: $(SRC_DIR)/kloader.c | $(BUILD_DIR)
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
