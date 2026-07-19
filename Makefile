all:
	$(MAKE) -C kernel
	$(MAKE) -C boot

clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C boot clean

OVMF_CODE := /usr/share/edk2/x64/OVMF_CODE.4m.fd

run: all
	qemu-system-x86_64 \
		-machine q35 \
		-m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
		-drive format=raw,file=fat:rw:build
