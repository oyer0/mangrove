CC := clang
LD := ld.lld

KERNEL := ../build/Mangrove/kernel.elf
BUILD_DIR := ../build/kernel
SRC_DIR := src
DRIVERS_DIR := ../drivers

OBJ := \
	$(BUILD_DIR)/main.o \
	$(BUILD_DIR)/font.o \
	$(BUILD_DIR)/font_blob.o \
	$(BUILD_DIR)/gdt.o \
	$(BUILD_DIR)/gdt_flush.o \
	$(BUILD_DIR)/idt.o \
	$(BUILD_DIR)/idt_asm.o \
	$(BUILD_DIR)/pmm.o \
	$(BUILD_DIR)/vmm.o \
	$(BUILD_DIR)/heap.o \
	$(BUILD_DIR)/timer.o \
	$(BUILD_DIR)/io.o \
	$(BUILD_DIR)/pic.o \
	$(BUILD_DIR)/pit.o \
	$(BUILD_DIR)/irq.o \
	$(BUILD_DIR)/keyboard.o \
	$(BUILD_DIR)/terminal.o \
	$(BUILD_DIR)/kprint.o \
	$(BUILD_DIR)/framebuffer.o \
	$(BUILD_DIR)/console.o

CFLAGS := \
	--target=x86_64-elf \
	-ffreestanding \
	-fno-stack-protector \
	-Iinclude \
	-I../include \
	-mno-red-zone

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

$(BUILD_DIR)/heap.o: $(SRC_DIR)/heap.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/timer.o: $(SRC_DIR)/timer.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/io.o: $(SRC_DIR)/io.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pic.o: $(SRC_DIR)/pic.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pit.o: $(SRC_DIR)/pit.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/irq.o: $(SRC_DIR)/irq.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/terminal.o: $(SRC_DIR)/terminal.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/keyboard.o: $(DRIVERS_DIR)/keyboard.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/framebuffer.o: $(SRC_DIR)/framebuffer.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/console.o: $(SRC_DIR)/console.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@


$(BUILD_DIR)/kprint.o: $(SRC_DIR)/kprint.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJ) | ../build/Mangrove
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

clean:
	rm -rf $(BUILD_DIR)
