CC := clang
LD := ld.lld

KERNEL := ../build/Mangrove/kernel.elf
BUILD_DIR := ../build/kernel
SRC_DIR := src

OBJ := \
	$(BUILD_DIR)/main.o

CFLAGS := \
	--target=x86_64-elf \
	-ffreestanding \
	-fno-stack-protector \
	-Iinclude \
	-I../include

LDFLAGS := \
	-T linker.ld

all: $(KERNEL)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

../build/Mangrove:
	mkdir -p ../build/Mangrove

$(BUILD_DIR)/main.o: $(SRC_DIR)/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJ) | ../build/Mangrove
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

clean:
	rm -rf $(BUILD_DIR)
