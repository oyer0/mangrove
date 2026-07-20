CC := clang
LD := ld.lld

KERNEL := ../build/Mangrove/kernel.elf
BUILD_DIR := ../build/kernel
SRC_DIR := src

OBJ := \
	$(BUILD_DIR)/main.o \
	$(BUILD_DIR)/font.o \
	$(BUILD_DIR)/font_blob.o

CFLAGS := \
	--target=x86_64-elf \
	-ffreestanding \
	-fno-stack-protector \
	-Iinclude \
	-I../include

LDFLAGS := \
	-T linker.ld \
	-Map=../build/Mangrove/kernel.map

all: $(KERNEL)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

../build/Mangrove:
	mkdir -p ../build/Mangrove

$(BUILD_DIR)/main.o: $(SRC_DIR)/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@


$(BUILD_DIR)/font.o: $(SRC_DIR)/font.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/font_blob.o: $(SRC_DIR)/font.psf | $(BUILD_DIR)
	objcopy -I binary -O elf64-x86-64 -B i386 src/font.psf $(BUILD_DIR)/font_blob.o

$(KERNEL): $(OBJ) | ../build/Mangrove
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

clean:
	rm -rf $(BUILD_DIR)
