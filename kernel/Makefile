CC := clang
LD := ld.lld

KERNEL := ../build/Mangrove/kernel.elf
BUILD_DIR := ../build/kernel
SRC_DIR := src

OBJ := \
	$(BUILD_DIR)/main.o \
	$(BUILD_DIR)/font.o \
	$(BUILD_DIR)/font_blob.o \
	$(BUILD_DIR)/gdt.o \
	$(BUILD_DIR)/gdt_flush.o \
	$(BUILD_DIR)/idt.o \
	$(BUILD_DIR)/idt_asm.o \
	$(BUILD_DIR)/pmm.o \
	$(BUILD_DIR)/vmm.o


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

$(BUILD_DIR)/gdt.o: $(SRC_DIR)/gdt.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/gdt_flush.o: $(SRC_DIR)/gdt_flush.s | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/idt.o: $(SRC_DIR)/idt.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/idt_asm.o: $(SRC_DIR)/idt_asm.s | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/pmm.o: $(SRC_DIR)/pmm.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vmm.o: $(SRC_DIR)/vmm.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJ) | ../build/Mangrove
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

clean:
	rm -rf $(BUILD_DIR)
