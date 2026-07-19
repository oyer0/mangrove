all:
	$(MAKE) -C kernel
	$(MAKE) -C boot

clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C boot clean

run: all
	# add this later
