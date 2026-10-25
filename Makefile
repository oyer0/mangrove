UNAME       := $(shell uname -s)
HOST_ARCH   := $(shell uname -m)

BOOT_CC     := x86_64-w64-mingw32-gcc
BOOT_AS     := x86_64-w64-mingw32-gcc
BOOT_LD     := x86_64-w64-mingw32-ld

ifeq ($(UNAME),Darwin)
ELF_CC      := x86_64-elf-gcc
ELF_AS      := x86_64-elf-gcc
ELF_LD      := x86_64-elf-ld
ELF_AR      := x86_64-elf-ar
ELF_OBJCOPY := x86_64-elf-objcopy

# Homebrew keeps native GNU GCC versioned so it does not replace Apple's
# compiler.  Host tools run on macOS and therefore use this native compiler;
# kernel and userspace use the x86_64-elf cross compiler above.
HOST_GCC_PREFIX := $(shell brew --prefix gcc 2>/dev/null)
HOST_CC         := $(lastword $(sort $(wildcard $(HOST_GCC_PREFIX)/bin/gcc-[0-9]*)))
HOST_REED_TEST_FLAGS := -Wl,-dead_strip -include tests/shared/host_malloc_wrap.h
else
ELF_CC      := gcc
ELF_AS      := gcc
ELF_LD      := ld.bfd
ELF_AR      := ar
ELF_OBJCOPY := objcopy
HOST_CC     := gcc
HOST_REED_TEST_FLAGS := -Wl,--gc-sections -Wl,--wrap=malloc
endif

CC          := $(ELF_CC)
LD_KERNEL   := $(ELF_LD)
AR          := $(ELF_AR)
OBJCOPY     := $(ELF_OBJCOPY)
# Applications are linked directly with GNU ld. Include the compiler's own
# target helper archive explicitly when a runtime facility needs ABI helpers.
USER_LIBGCC := $(shell $(ELF_CC) -print-libgcc-file-name)

QEMU        := qemu-system-x86_64
QEMU_SMP    ?= 2

# Linux guest tests use KVM by default. TCG remains an explicit opt-in;
# non-Linux hosts retain the existing TCG default.
ifeq ($(UNAME),Linux)
NATIVE_GUEST_ACCEL ?= kvm
else
NATIVE_GUEST_ACCEL ?= tcg
endif
QEMU_ACCEL_PREFLIGHT = python3 tests/guest/qemu_accel.py --qemu "$(QEMU)" --accel

BOOT_TOOLS := $(BOOT_CC) $(BOOT_LD)
ELF_TOOLS  := $(ELF_CC) $(ELF_LD) $(ELF_AR) $(ELF_OBJCOPY)
HOST_TOOLS := $(HOST_CC)
MISSING_BOOT_TOOLS := $(foreach tool,$(BOOT_TOOLS),$(if $(shell command -v $(tool) 2>/dev/null),,$(tool)))
MISSING_ELF_TOOLS  := $(foreach tool,$(ELF_TOOLS),$(if $(shell command -v $(tool) 2>/dev/null),,$(tool)))
MISSING_HOST_TOOLS := $(foreach tool,$(HOST_TOOLS),$(if $(shell command -v $(tool) 2>/dev/null),,$(tool)))
ifneq ($(strip $(MISSING_BOOT_TOOLS)),)
$(error Missing required MinGW-w64 UEFI toolchain command(s): $(MISSING_BOOT_TOOLS). Install gcc-mingw-w64-x86-64 and binutils-mingw-w64-x86-64 on Linux, or mingw-w64 with Homebrew on macOS.)
endif
ifneq ($(strip $(MISSING_ELF_TOOLS)),)
ifeq ($(UNAME),Darwin)
$(error Missing required macOS x86_64-elf GNU toolchain command(s): $(MISSING_ELF_TOOLS). Install with: brew install x86_64-elf-gcc x86_64-elf-binutils)
else
$(error Missing required GNU ELF toolchain command(s): $(MISSING_ELF_TOOLS).)
endif
endif
ifneq ($(strip $(MISSING_HOST_TOOLS)),)
ifeq ($(UNAME),Darwin)
$(error Missing Homebrew GNU host GCC. Install with: brew install gcc)
else
$(error Missing host compiler: $(MISSING_HOST_TOOLS))
endif
endif

ifeq ($(UNAME),Darwin)
    QEMU_PREFIX       ?= $(shell brew --prefix qemu 2>/dev/null)
ifeq ($(HOST_ARCH),arm64)
    QEMU_ACCEL        := tcg
else
    QEMU_ACCEL        := hvf
endif
    QEMU_CPU          := max
    QEMU_DISPLAY_ARGS ?= -display cocoa,zoom-to-fit=on
    OVMF_CODE_SOURCE  := $(QEMU_PREFIX)/share/qemu/edk2-x86_64-code.fd
    OVMF_VARS_SOURCE  := $(QEMU_PREFIX)/share/qemu/edk2-i386-vars.fd
else
    QEMU_ACCEL        := kvm
    QEMU_CPU          := host
    OVMF_CODE_SOURCE := /usr/share/OVMF/OVMF_CODE_4M.fd
    OVMF_VARS_SOURCE := /usr/share/OVMF/OVMF_VARS_4M.fd
endif

QEMU_PLATFORM_ARGS := -accel $(QEMU_ACCEL) -cpu $(QEMU_CPU)

BUILD_DIR    := build
EFI_DIR      := $(BUILD_DIR)/EFI/BOOT
MANGROVE_DIR := $(BUILD_DIR)/Mangrove
STATE_DIR    ?= .mangrove
DEV_IMAGE    := $(STATE_DIR)/MangroveDev.img
DEV_ROOT_IMAGE := $(STATE_DIR)/MangroveDevRoot.img
LEGACY_DEV_IMAGE := $(MANGROVE_DIR)/Mangrove.img
FLASH_ROOT_IMAGE := $(MANGROVE_DIR)/MangroveFlash.img
USB_IMAGE    := $(MANGROVE_DIR)/MangroveUSB.img
SPROUT_DIR   := $(BUILD_DIR)/sprout
SPROUT_CMD_DIR := $(BUILD_DIR)/sproutcmd
SESSIOND_DIR := $(BUILD_DIR)/sessiond
LOGIND_DIR   := $(BUILD_DIR)/logind
LOGD_DIR     := $(BUILD_DIR)/logd
NETWORKD_DIR := $(BUILD_DIR)/networkd
DEVICED_DIR  := $(BUILD_DIR)/deviced
VOLUMED_DIR  := $(BUILD_DIR)/volumed
CLIPD_DIR    := $(BUILD_DIR)/clipd
MOUNT_DIR    := $(BUILD_DIR)/mount
UNMOUNT_DIR  := $(BUILD_DIR)/unmount
EJECT_DIR    := $(BUILD_DIR)/eject
LSPCI_DIR    := $(BUILD_DIR)/lspci
LSUSB_DIR    := $(BUILD_DIR)/lsusb
LSDISK_DIR   := $(BUILD_DIR)/lsdsk
DISKUTIL_DIR := $(BUILD_DIR)/diskutil
CREW_DIR     := $(BUILD_DIR)/crew
ABOUT_DIR    := $(BUILD_DIR)/about
VIEW_DIR     := $(BUILD_DIR)/view
INFO_DIR     := $(BUILD_DIR)/info
MEM_DIR      := $(BUILD_DIR)/mem
TIME_DIR     := $(BUILD_DIR)/time
TMON_DIR     := $(BUILD_DIR)/tmon
LOGV_DIR     := $(BUILD_DIR)/logv
HELLO_DIR    := $(BUILD_DIR)/hello
SHOOT_DIR    := $(BUILD_DIR)/shoot
CLEAR_DIR    := $(BUILD_DIR)/clear
COPY_DIR     := $(BUILD_DIR)/copy
LIST_DIR     := $(BUILD_DIR)/list
LOCATE_DIR   := $(BUILD_DIR)/locate
MOVE_DIR     := $(BUILD_DIR)/move
PLANT_DIR    := $(BUILD_DIR)/plant
TYPE_DIR     := $(BUILD_DIR)/type
WAIT_DIR     := $(BUILD_DIR)/wait
REED_DIR     := $(BUILD_DIR)/reed
DEL_DIR      := $(BUILD_DIR)/del
MKDIR_DIR    := $(BUILD_DIR)/mkdir

SAY_DIR      := $(BUILD_DIR)/say
UPTIME_DIR   := $(BUILD_DIR)/uptime
DATE_DIR     := $(BUILD_DIR)/date
VERSION_DIR  := $(BUILD_DIR)/version
WHERE_DIR    := $(BUILD_DIR)/where
FONT_SOURCE  := kernel/assets/font/unscii-16.hex
FONT_CONVERTER := tools/convert_unscii_hex.py
FONT_ASSET   := kernel/assets/font.psf
FSTEST_DIR   := $(BUILD_DIR)/fstest
NETTEST_DIR  := $(BUILD_DIR)/nettest
PING_DIR     := $(BUILD_DIR)/ping
RESOLVE_DIR  := $(BUILD_DIR)/resolve
FETCH_DIR    := $(BUILD_DIR)/fetch
NETINFO_DIR  := $(BUILD_DIR)/netinfo
NETCFG_DIR   := $(BUILD_DIR)/netcfg
POWER_DIR    := $(BUILD_DIR)/power
IDENTITY_DIR := $(BUILD_DIR)/identity
USER_CMD_DIR := $(BUILD_DIR)/user
SHUTDOWN_DIR := $(BUILD_DIR)/shutdown
REBOOT_DIR   := $(BUILD_DIR)/reboot
USER_LIBC_DIR := $(BUILD_DIR)/userspace/libc

EFI          := $(EFI_DIR)/BOOTX64.EFI
PITH         := $(MANGROVE_DIR)/pith.elf
KERNEL_MAP   := $(MANGROVE_DIR)/kernel.map
OVMF_CODE    := $(OVMF_CODE_SOURCE)
OVMF_VARS    ?= $(BUILD_DIR)/OVMF_VARS.fd
MKMGFS       := $(BUILD_DIR)/mkmgfs
SPROUT       := $(SPROUT_DIR)/sprout.elf
SPROUT_CMD   := $(SPROUT_CMD_DIR)/sprout.elf
SESSIOND     := $(SESSIOND_DIR)/sessiond.elf
LOGIND       := $(LOGIND_DIR)/logind.elf
LOGD         := $(LOGD_DIR)/logd.elf
NETWORKD     := $(NETWORKD_DIR)/networkd.elf
DEVICED      := $(DEVICED_DIR)/deviced.elf
VOLUMED      := $(VOLUMED_DIR)/volumed.elf
CLIPD        := $(CLIPD_DIR)/clipd.elf
MOUNT        := $(MOUNT_DIR)/mount.elf
UNMOUNT      := $(UNMOUNT_DIR)/unmount.elf
EJECT        := $(EJECT_DIR)/eject.elf
LSPCI        := $(LSPCI_DIR)/lspci.elf
LSUSB        := $(LSUSB_DIR)/lsusb.elf
LSDISK       := $(LSDISK_DIR)/lsdsk.elf
DISKUTIL     := $(DISKUTIL_DIR)/diskutil.elf
CREW         := $(CREW_DIR)/crew.elf
ABOUT        := $(ABOUT_DIR)/about.elf
VIEW         := $(VIEW_DIR)/view.elf
INFO         := $(INFO_DIR)/info.elf
MEM          := $(MEM_DIR)/mem.elf
TIME         := $(TIME_DIR)/time.elf
TMON         := $(TMON_DIR)/tmon.elf
LOGV         := $(LOGV_DIR)/logv.elf
HELLO        := $(HELLO_DIR)/hello.elf
SHOOT        := $(SHOOT_DIR)/shoot.elf
CLEAR        := $(CLEAR_DIR)/clear.elf
COPY         := $(COPY_DIR)/copy.elf
LIST         := $(LIST_DIR)/list.elf
LOCATE       := $(LOCATE_DIR)/locate.elf
MOVE         := $(MOVE_DIR)/move.elf
PLANT        := $(PLANT_DIR)/plant.elf
TYPE         := $(TYPE_DIR)/type.elf
WAIT         := $(WAIT_DIR)/wait.elf
REED         := $(REED_DIR)/reed.elf
DEL          := $(DEL_DIR)/del.elf
MKDIR        := $(MKDIR_DIR)/mkdir.elf

SAY          := $(SAY_DIR)/say.elf
UPTIME       := $(UPTIME_DIR)/uptime.elf
DATE         := $(DATE_DIR)/date.elf
VERSION      := $(VERSION_DIR)/version.elf
WHERE        := $(WHERE_DIR)/where.elf
FSTEST       := $(FSTEST_DIR)/fstest.elf
NETTEST      := $(NETTEST_DIR)/nettest.elf
PING         := $(PING_DIR)/ping.elf
RESOLVE      := $(RESOLVE_DIR)/resolve.elf
FETCH        := $(FETCH_DIR)/fetch.elf
NETINFO      := $(NETINFO_DIR)/netinfo.elf
NETCFG       := $(NETCFG_DIR)/netcfg.elf
POWER        := $(POWER_DIR)/power.elf
IDENTITY     := $(IDENTITY_DIR)/identity.elf
USER_CMD     := $(USER_CMD_DIR)/user.elf
SHUTDOWN     := $(SHUTDOWN_DIR)/shutdown.elf
REBOOT       := $(REBOOT_DIR)/reboot.elf
COMMAND_PATH_OBJ := $(BUILD_DIR)/userspace/command_path.o
HELP_OBJ     := $(BUILD_DIR)/userspace/help.o
TEXT_PAGER_OBJ := $(BUILD_DIR)/userspace/text_pager.o
STORAGE_SNAPSHOT_OBJ := $(BUILD_DIR)/userspace/storage_snapshot.o
USER_LIBC    := $(USER_LIBC_DIR)/libc.a
USER_LIBM    := $(BUILD_DIR)/userspace/libm.a
USER_CRT     := $(BUILD_DIR)/userspace/crt0.o

DEPFLAGS     := -MMD -MP

BOOT_CFLAGS  := -std=gnu11 -ffreestanding -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector -Iboot/include -Iinclude $(DEPFLAGS)
BOOT_ASFLAGS :=
# GNU ld uses numeric subsystem 10 for EFI applications.  Direct linking
# intentionally omits MinGW CRT/startup objects and all default libraries.
BOOT_LDFLAGS := --subsystem 10 --entry efi_main --enable-reloc-section --dynamicbase --disable-auto-import --no-insert-timestamp


# The kernel remains free of compiler-generated FPU/SIMD instructions.  User
# threads own an eager FXSAVE/FXRSTOR context, so userspace may use the normal
# x86-64 floating-point/SSE ABI without exposing that state to kernel C code.
KERNEL_CFLAGS  := -std=gnu11 -ffreestanding -fno-asynchronous-unwind-tables -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel -Ikernel/include -Ikernel/include/usb -Ikernel/include/pci -Ikernel/include/storage -Iinclude -Ilibc/include -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float $(DEPFLAGS)
KERNEL_ASFLAGS := -m64
INPUT_TRACE_DIAGNOSTICS ?= 0
ifeq ($(INPUT_TRACE_DIAGNOSTICS),1)
KERNEL_CFLAGS += -DINPUT_TRACE_DIAGNOSTICS=1
endif
KERNEL_LDFLAGS := -z max-page-size=0x1000 -T kernel/linker.ld -Map=$(KERNEL_MAP)

USER_CFLAGS := -std=gnu17 -ffreestanding -fno-asynchronous-unwind-tables -fno-stack-protector \
               -fno-builtin -fno-pic -fno-pie -mno-red-zone -nostdinc \
               -I. -Iuserspace/shoot -Ilibc/include -Iinclude \
               $(DEPFLAGS)
USER_LINKER_SCRIPT := userspace/linker/userspace.ld

# Boot-time subsystem smoke tests are intentionally excluded from the normal
# Mangrove boot path.  Build with DEBUG_BOOT_TESTS=1 to include them.
ifneq ($(DEBUG_BOOT_TESTS),)
KERNEL_CFLAGS += -DPITH_DEBUG_BOOT_TESTS
endif

# Low-level kernel bring-up details are excluded from normal production boot.
# Use KERNEL_BOOT_DEBUG=1 when auditing the initialization path.
ifeq ($(KERNEL_BOOT_DEBUG),1)
KERNEL_CFLAGS += -DKERNEL_BOOT_DEBUG=1
endif

# Optional host-side TCP echo smoke test.  It is intentionally off in normal
# images and uses the DHCP-learned gateway at runtime.
ifeq ($(TCP_ECHO_TEST),1)
KERNEL_CFLAGS += -DPITH_TCP_ECHO_TEST=1
endif

# Optional one-shot kernel HTTP validation; disabled for normal images.
ifeq ($(HTTP_GET_TEST),1)
KERNEL_CFLAGS += -DPITH_HTTP_GET_TEST=1
endif

# Detailed USB/xHCI investigation traces are excluded from normal builds.
# Use `make -B XHCI_DEBUG=1` when the low-level controller traces are needed.
ifeq ($(XHCI_DEBUG),1)
KERNEL_CFLAGS += -DXHCI_DEBUG=1
endif

# Opt-in DHCP/boot-network milestone diagnostics.  Normal images remain
# silent; the stream is mirrored to QEMU serial output when enabled.
ifeq ($(NETWORK_BOOT_DIAG),1)
KERNEL_CFLAGS += -DNETWORK_BOOT_DIAG=1
endif

# Opt-in detailed ELF loader tracing.  Normal images do not print successful
# per-read or per-segment loader activity.
ifeq ($(ELF_LOADER_DEBUG),1)
KERNEL_CFLAGS += -DELF_LOADER_DEBUG=1
endif

# Opt-in ACPI battery/adapter discovery diagnostics for real-hardware tests.
# Normal images keep AML evaluation failures silent and report only the
# user-facing unavailable state.
ifeq ($(ACPI_POWER_DEBUG),1)
KERNEL_CFLAGS += -DACPI_POWER_DEBUG=1
endif

# Opt-in platform temperature diagnostics for real-hardware sensor tests.
ifeq ($(PLATFORM_THERMAL_DEBUG),1)
KERNEL_CFLAGS += -DPLATFORM_THERMAL_DEBUG=1
endif

# Explicit deployment assertion: the CMOS RTC is configured to contain UTC.
# Default hardware builds leave realtime unavailable. Rebuild when changing it.
RTC_UTC_TRUST ?= 0
ifneq ($(RTC_UTC_TRUST),0)
ifneq ($(RTC_UTC_TRUST),1)
$(error RTC_UTC_TRUST must be 0 or 1)
endif
endif

# Keep the time-source assertion attached to kernel objects so changing it in
# an incremental build cannot reuse an object compiled under the other policy.
RTC_UTC_TRUST_STAMP := $(BUILD_DIR)/kernel/.rtc-utc-trust
.PHONY: rtc-utc-trust-force
rtc-utc-trust-force:

$(RTC_UTC_TRUST_STAMP): rtc-utc-trust-force
	@mkdir -p $(dir $@)
	@if [ ! -f "$@" ] || [ "$$(cat "$@")" != "$(RTC_UTC_TRUST)" ]; then \
		printf '%s\n' "$(RTC_UTC_TRUST)" > "$@"; \
	fi

# Opt-in CMOS RTC decoding diagnostics.  Normal boots do not expose raw RTC
# registers or mode details on the framebuffer.
ifeq ($(RTC_DEBUG),1)
KERNEL_CFLAGS += -DRTC_DEBUG=1
endif

# Full kernel heap scans are available for corruption investigations.
# Rebuild with `gmake -B HEAP_VERIFY=1` to enable them.
ifeq ($(HEAP_VERIFY),1)
KERNEL_CFLAGS += -DHEAP_VERIFY=1
endif

# Diagnostic allocator profiling exists only in isolated test builds. It adds
# static counters and a private test syscall; normal kernels contain neither.
# The ordinary kernel uses the unified allocator. Set KERNEL_HYBRID_ALLOCATOR=0
# for an indexed-only comparison or recovery build.
KERNEL_HYBRID_ALLOCATOR ?= 1
KERNEL_HYBRID_ALLOCATOR_CUTOFF ?= 256
ifneq ($(filter $(KERNEL_HYBRID_ALLOCATOR_CUTOFF),256 288 320 352 384),$(KERNEL_HYBRID_ALLOCATOR_CUTOFF))
$(error KERNEL_HYBRID_ALLOCATOR_CUTOFF must be one of 256, 288, 320, 352, or 384)
endif
ifneq ($(KERNEL_HYBRID_ALLOCATOR),0)
ifneq ($(KERNEL_HYBRID_ALLOCATOR),1)
$(error KERNEL_HYBRID_ALLOCATOR must be zero or one)
endif
ifeq ($(KERNEL_HYBRID_ALLOCATOR),1)
KERNEL_CFLAGS += -DKERNEL_HYBRID_ALLOCATOR=1 \
	-DKERNEL_HYBRID_ALLOCATOR_CUTOFF=$(KERNEL_HYBRID_ALLOCATOR_CUTOFF)
endif
endif

HEAP_PROFILE_DIAGNOSTICS ?= 0
HYBRID_RUNTIME_DIAGNOSTICS ?= 0
FSTEST_REED_NO_CONFLICT ?= 0
FSTEST_REED_CONFLICT_SYNC ?= 0
REED_GUEST_CONFLICT_SYNC ?= 0
ifneq ($(HEAP_PROFILE_DIAGNOSTICS),0)
ifneq ($(HEAP_PROFILE_DIAGNOSTICS),1)
$(error HEAP_PROFILE_DIAGNOSTICS must be zero or one)
endif
endif
ifneq ($(HYBRID_RUNTIME_DIAGNOSTICS),0)
ifneq ($(HYBRID_RUNTIME_DIAGNOSTICS),1)
$(error HYBRID_RUNTIME_DIAGNOSTICS must be zero or one)
endif
endif
ifneq ($(FSTEST_REED_NO_CONFLICT),0)
ifneq ($(FSTEST_REED_NO_CONFLICT),1)
$(error FSTEST_REED_NO_CONFLICT must be zero or one)
endif
endif
ifneq ($(FSTEST_REED_CONFLICT_SYNC),0)
ifneq ($(FSTEST_REED_CONFLICT_SYNC),1)
$(error FSTEST_REED_CONFLICT_SYNC must be zero or one)
endif
endif
ifneq ($(REED_GUEST_CONFLICT_SYNC),0)
ifneq ($(REED_GUEST_CONFLICT_SYNC),1)
$(error REED_GUEST_CONFLICT_SYNC must be zero or one)
endif
endif
ifeq ($(FSTEST_REED_NO_CONFLICT),1)
ifeq ($(FSTEST_REED_CONFLICT_SYNC),1)
$(error Reed no-conflict and conflict-sync guest modes are mutually exclusive)
endif
endif
ifneq ($(FSTEST_REED_CONFLICT_SYNC),$(REED_GUEST_CONFLICT_SYNC))
$(error fstest and Reed conflict-sync guest modes must be enabled together)
endif
ifeq ($(HYBRID_RUNTIME_DIAGNOSTICS),1)
ifneq ($(KERNEL_HYBRID_ALLOCATOR),1)
$(error HYBRID_RUNTIME_DIAGNOSTICS requires KERNEL_HYBRID_ALLOCATOR=1)
endif
ifneq ($(HEAP_PROFILE_DIAGNOSTICS),0)
$(error use HYBRID_RUNTIME_DIAGNOSTICS instead of HEAP_PROFILE_DIAGNOSTICS with hybrid routing)
endif
KERNEL_CFLAGS += -DHEAP_PROFILE_DIAGNOSTICS=1 \
	-DHEAP_PROFILE_HYBRID_DIAGNOSTICS=1 -Itests/memory
FSTEST_PROFILE_CFLAGS := $(USER_CFLAGS) -DHEAP_PROFILE_GUEST=1 \
	-DHEAP_PROFILE_HYBRID_DIAGNOSTICS=1 -Itests/memory
else ifeq ($(HEAP_PROFILE_DIAGNOSTICS),1)
ifneq ($(KERNEL_HYBRID_ALLOCATOR),0)
$(error HEAP_PROFILE_DIAGNOSTICS requires indexed-only routing; use HYBRID_RUNTIME_DIAGNOSTICS for hybrid profiles)
endif
KERNEL_CFLAGS += -DHEAP_PROFILE_DIAGNOSTICS=1 -Itests/memory
FSTEST_PROFILE_CFLAGS := $(USER_CFLAGS) -DHEAP_PROFILE_GUEST=1 -Itests/memory
else
FSTEST_PROFILE_CFLAGS := $(USER_CFLAGS)
endif
FSTEST_VOLUME_CFLAGS := $(USER_CFLAGS)
ifeq ($(FSTEST_REED_NO_CONFLICT),1)
FSTEST_PROFILE_CFLAGS += -DREED_GUEST_NO_CONFLICT=1
FSTEST_VOLUME_CFLAGS += -DREED_GUEST_NO_CONFLICT=1
endif
ifeq ($(FSTEST_REED_CONFLICT_SYNC),1)
FSTEST_PROFILE_CFLAGS += -DFSTEST_REED_CONFLICT_SYNC=1
FSTEST_VOLUME_CFLAGS += -DFSTEST_REED_CONFLICT_SYNC=1
endif
ifeq ($(REED_GUEST_CONFLICT_SYNC),1)
REED_CFLAGS := $(USER_CFLAGS) -DREED_GUEST_CONFLICT_SYNC=1
else
REED_CFLAGS := $(USER_CFLAGS)
endif

# The indexed-only configuration omits the slab core. Diagnostic core tests
# and the production hybrid configuration link it.
SLAB_CORE_DIAGNOSTICS ?= 0
SLAB_DEEP_VALIDATION ?= 0
SLAB_CORE_GUEST_TEST ?= 0
SLAB_CORE_GUEST_BENCHMARK ?= 0
SLAB_CORE_GUEST_FREE_PROFILE ?= 0
VMM_KERNEL_RANGE_GUEST_TEST ?= 0
ifneq ($(VMM_KERNEL_RANGE_GUEST_TEST),0)
ifneq ($(VMM_KERNEL_RANGE_GUEST_TEST),1)
$(error VMM_KERNEL_RANGE_GUEST_TEST must be zero or one)
endif
endif
ifeq ($(VMM_KERNEL_RANGE_GUEST_TEST),1)
KERNEL_CFLAGS += -DVMM_KERNEL_RANGE_GUEST_TEST=1
endif
ifeq ($(SLAB_CORE_GUEST_BENCHMARK),1)
SLAB_CORE_GUEST_TEST := 1
KERNEL_CFLAGS += -DSLAB_CORE_GUEST_BENCHMARK=1
ifeq ($(SLAB_CORE_GUEST_FREE_PROFILE),1)
KERNEL_CFLAGS += -DKERNEL_BOOT_LOG_CAPACITY=524288U
else
KERNEL_CFLAGS += -DKERNEL_BOOT_LOG_CAPACITY=196608U
endif
endif
ifeq ($(SLAB_CORE_GUEST_FREE_PROFILE),1)
ifneq ($(SLAB_CORE_GUEST_BENCHMARK),1)
$(error SLAB_CORE_GUEST_FREE_PROFILE requires SLAB_CORE_GUEST_BENCHMARK=1)
endif
KERNEL_CFLAGS += -DSLAB_CORE_GUEST_FREE_PROFILE=1
endif
ifeq ($(SLAB_CORE_GUEST_TEST),1)
SLAB_CORE_DIAGNOSTICS := 1
KERNEL_CFLAGS += -DSLAB_CORE_GUEST_TEST=1
endif
ifeq ($(SLAB_CORE_DIAGNOSTICS),1)
ifneq ($(SLAB_DEEP_VALIDATION),0)
ifneq ($(SLAB_DEEP_VALIDATION),1)
$(error SLAB_DEEP_VALIDATION must be zero or one)
endif
endif
KERNEL_CFLAGS += -DSLAB_CORE_DIAGNOSTICS=1 \
	-DSLAB_DEEP_VALIDATION=$(SLAB_DEEP_VALIDATION)
else ifneq ($(SLAB_DEEP_VALIDATION),0)
$(error SLAB_DEEP_VALIDATION requires SLAB_CORE_DIAGNOSTICS=1)
endif
ifeq ($(KERNEL_HYBRID_ALLOCATOR),1)
KERNEL_CFLAGS += -DSLAB_DEEP_VALIDATION=$(SLAB_DEEP_VALIDATION)
endif

# Automatic Source Discovery
BOOT_C_SRCS    := $(shell find boot/src -name '*.c')
BOOT_S_SRCS    := $(shell find boot/src -name '*.s')
KERNEL_C_SRCS  := $(shell find kernel/src -name '*.c')
ifeq ($(HEAP_PROFILE_DIAGNOSTICS)$(HYBRID_RUNTIME_DIAGNOSTICS),00)
KERNEL_C_SRCS := $(filter-out kernel/src/heap_profile.c,$(KERNEL_C_SRCS))
endif
ifeq ($(KERNEL_HYBRID_ALLOCATOR)$(SLAB_CORE_DIAGNOSTICS),00)
KERNEL_C_SRCS := $(filter-out kernel/src/slab.c,$(KERNEL_C_SRCS))
endif
ifneq ($(SLAB_CORE_GUEST_TEST),1)
KERNEL_C_SRCS := $(filter-out kernel/src/slab_guest_test.c,$(KERNEL_C_SRCS))
endif
ifeq ($(VMM_KERNEL_RANGE_GUEST_TEST),0)
KERNEL_C_SRCS := $(filter-out kernel/src/vmm_guest_test.c,$(KERNEL_C_SRCS))
endif
ifneq ($(SLAB_CORE_GUEST_BENCHMARK),1)
KERNEL_C_SRCS := $(filter-out kernel/src/slab_guest_benchmark.c,$(KERNEL_C_SRCS))
endif
KERNEL_S_SRCS  := $(shell find kernel/src -name '*.s')
# Only these freestanding units are shared with the kernel. New userspace
# runtime files must never become kernel objects by default.
LIBC_C_SRCS    := libc/src/string.c libc/src/units.c libc/src/utf8.c

LIBM_C_SRCS := libc/src/math/ieee.c libc/src/math/primitives.c \
               libc/src/math/next.c libc/src/math/sqrt.c \
               libc/src/math/scale_remainder.c libc/src/math/root_hypot.c \
               libc/src/math/exp_log.c libc/src/math/power.c \
               libc/src/math/error_functions.c libc/src/math/gamma.c \
               libc/src/math/hyperbolic.c \
               libc/src/math/inverse_trig.c libc/src/math/integer_conversion.c \
               libc/src/math/sin_cos.c libc/src/math/fma.c \
               libc/src/math/fenv.c
LIBM_C_SRCS += libc/src/math/complex.c
LIBM_OBJS := $(patsubst libc/src/math/%.c,$(BUILD_DIR)/libm/%.o,$(LIBM_C_SRCS))
HOST_LIBM_TEST_DIR := $(BUILD_DIR)/tests/libm-host-sanitized
HOST_LIBM_OBJS := $(patsubst libc/src/math/%.c,$(HOST_LIBM_TEST_DIR)/%.o,$(LIBM_C_SRCS))
HOST_LIBM_TEST := $(HOST_LIBM_TEST_DIR)/libm.a
ifneq ($(filter x86_64 amd64,$(shell uname -m)),)
HOST_LIBM_TEST_PREREQ := $(HOST_LIBM_TEST)
endif
HOST_LIBM_CFLAGS := -std=gnu17 -O1 -g -fno-builtin -fno-strict-aliasing \
                    -frounding-math -ftrapping-math \
                    -fsanitize=address,undefined -fno-sanitize-recover=all \
                    -Ilibc/include -Iinclude
HOST_AR ?= ar

# Object Mappings
BOOT_OBJS := $(patsubst boot/src/%.c,$(BUILD_DIR)/boot/%.o,$(BOOT_C_SRCS))
BOOT_OBJS += $(patsubst boot/src/%.s,$(BUILD_DIR)/boot/%.o,$(BOOT_S_SRCS))

KERNEL_OBJS := $(patsubst kernel/src/%.c,$(BUILD_DIR)/kernel/%.o,$(KERNEL_C_SRCS))
KERNEL_OBJS += $(patsubst kernel/src/%.s,$(BUILD_DIR)/kernel/%.o,$(KERNEL_S_SRCS))
KERNEL_OBJS += $(BUILD_DIR)/kernel/font_blob.o
KERNEL_OBJS += $(BUILD_DIR)/kernel/time_convert.o

LIBC_OBJS    := $(patsubst libc/src/%.c,$(BUILD_DIR)/libc/%.o,$(LIBC_C_SRCS))

ALL_KERNEL_OBJS := $(KERNEL_OBJS) $(LIBC_OBJS)

DEPS := $(BOOT_OBJS:.o=.d) $(ALL_KERNEL_OBJS:.o=.d)

.PHONY: all help make fresh run fresh-run usb clean test exfat-upcase \
        binaries font sprout sessiond logind logd networkd deviced volumed clipd mount unmount eject lspci lsusb lsdsk diskutil crew about view info mem time tmon logv hello shoot clear copy list locate move mkdir plant type wait reed del say shutdown reboot uptime date version where fstest nettest ping resolve fetch netinfo netcfg power identity user \
        image fresh-image usb-image run-usb mkmgfs mgfsck test-time test-terminal test-mgfs-version test-mgfs-reconcile test-reed test-math \
        check-image-deps check-usb-deps check-qemu-deps qemu-warning dev-image fresh-dev-image flash-image test-launch test-password test-vmm test-heap test-history test-copy test-move test-del test-mfqs test-sync test-spinlock-context test-storage test-dev-tools test-unicode test-shoot test-shoot-help test-pager test-view test-info test-help test-list test-startup-presentation test-power-presentation test-rtc-build-policy

# Everyday targets
all: image

exfat-upcase:
	python3 tools/generate_exfat_upcase.py
	python3 tools/generate_exfat_upcase.py --check

help:
	@echo "Everyday commands:"
	@echo "  make             Build/update the persistent development image"
	@echo "  make run         Build and boot the persistent USB/xHCI development image"
	@echo "  make fresh       Reset the persistent development image"
	@echo "  make fresh-run   Reset the development image and boot it"
	@echo "  make usb         Build build/Mangrove/MangroveUSB.img for hardware"
	@echo "  make clean       Remove disposable build artifacts; preserve .mangrove/"
	@echo "  make test        Run the available host-side test suites"
	@echo "  make test-guest  MTS standard guest qualification (27 boots; not a quick test)"
	@echo "  make test-all    MTS comprehensive serial host and guest qualification"
	@echo "  Linux guest tests use KVM; select TCG explicitly with the target's *_ACCEL=tcg option"
	@echo
	@echo "Specialist targets: binaries image fresh-image usb-image mkmgfs mgfsck"
	@echo "                    test-time test-terminal test-help test-guest-shutdown test-guest-reed and individual programs"

make: image

fresh: fresh-dev-image

usb: flash-image

# MTS changes organization/reporting; individual test recipes remain usable.
# Empty output selects an automatically cleaned temporary workspace.
MTS_OUTPUT ?=
MTS_ACCEL ?= $(NATIVE_GUEST_ACCEL)
MTS_RETAIN_ARTIFACTS ?= 0
MTS_ARGS = --make "$(MAKE)" $(if $(MTS_OUTPUT),--output "$(MTS_OUTPUT)") $(if $(filter 1 yes true,$(MTS_RETAIN_ARTIFACTS)),--retain-artifacts) --accel "$(MTS_ACCEL)" --ovmf-code "$(OVMF_CODE_SOURCE)" --ovmf-vars "$(OVMF_VARS_SOURCE)"
.PHONY: test test-guest test-all test-mts
test:
	python3 tests/guest/mts.py host $(MTS_ARGS)

test-guest:
	$(QEMU_ACCEL_PREFLIGHT) "$(MTS_ACCEL)"
	python3 tests/guest/mts.py guest $(MTS_ARGS)

test-all:
	$(QEMU_ACCEL_PREFLIGHT) "$(MTS_ACCEL)"
	python3 tests/guest/mts.py all $(MTS_ARGS)

test-mts:
	python3 tests/guest/mts_test.py

binaries: $(EFI) $(PITH) $(USER_LIBM) $(SPROUT) $(SPROUT_CMD) $(SESSIOND) $(LOGIND) $(LOGD) $(NETWORKD) $(DEVICED) $(VOLUMED) $(CLIPD) $(MOUNT) $(UNMOUNT) $(EJECT) $(LSPCI) $(LSUSB) $(LSDISK) $(DISKUTIL) $(CREW) $(ABOUT) $(VIEW) $(INFO) $(MEM) $(TIME) $(TMON) $(LOGV) $(SHOOT) $(CLEAR) $(COPY) $(LIST) $(LOCATE) $(MOVE) $(MKDIR) $(PLANT) $(TYPE) $(WAIT) $(REED) $(DEL) $(SAY) $(SHUTDOWN) $(REBOOT) $(UPTIME) $(DATE) $(VERSION) $(WHERE) $(PING) $(RESOLVE) $(FETCH) $(NETINFO) $(NETCFG) $(POWER) $(IDENTITY) $(USER_CMD)

shoot: $(SHOOT)

clear: $(CLEAR)

copy: $(COPY)

list: $(LIST)
locate: $(LOCATE)
move: $(MOVE)
mkdir: $(MKDIR)
plant: $(PLANT)
type: $(TYPE)
wait: $(WAIT)
reed: $(REED)
del: $(DEL)

say: $(SAY)

shutdown: $(SHUTDOWN)

reboot: $(REBOOT)

uptime: $(UPTIME)

date: $(DATE)

version: $(VERSION)
where: $(WHERE)

sprout: $(SPROUT)

sessiond: $(SESSIOND)

logind: $(LOGIND)

logd: $(LOGD)

networkd: $(NETWORKD)

deviced: $(DEVICED)

mount: $(MOUNT)

unmount: $(UNMOUNT)

eject: $(EJECT)

volumed: $(VOLUMED)

lspci: $(LSPCI)

lsusb: $(LSUSB)

lsdsk: $(LSDISK)

diskutil: $(DISKUTIL)

crew: $(CREW)

about: $(ABOUT)

view: $(VIEW)

info: $(INFO)

mem: $(MEM)

time: $(TIME)

tmon: $(TMON)

logv: $(LOGV)

hello: $(HELLO)

fstest: $(FSTEST)

.PHONY: fsstress
fsstress: $(BUILD_DIR)/fsstress/fsstress.elf

FSSTRESS_OBJS := $(addprefix $(BUILD_DIR)/FsStress/,model.o engine.o main.o native.o)
$(BUILD_DIR)/FsStress/%.o: userspace/fsstress/%.c userspace/fsstress/fsstress.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -std=c17 -c $< -o $@

$(BUILD_DIR)/fsstress/fsstress.elf: $(FSSTRESS_OBJS) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ $(USER_CRT) $(FSSTRESS_OBJS) $(USER_LIBC)
nettest: $(NETTEST)
ping: $(PING)
resolve: $(RESOLVE)
fetch: $(FETCH)
netinfo: $(NETINFO)

netcfg: $(NETCFG)

power: $(POWER)

identity: $(IDENTITY)

user: $(USER_CMD)

mkmgfs: $(MKMGFS)

mgfsck: $(BUILD_DIR)/mgfsck

test-startup-presentation:
	python3 tests/kernel/startup_presentation_test.py "$(HOST_CC)"

test-rtc-build-policy:
	python3 tests/kernel/rtc_build_policy_test.py

test-power-presentation:
	python3 tests/userspace/power_presentation_test.py "$(HOST_CC)"

test-time:
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude tests/kernel/timekeeping_test.c \
		libc/src/time_convert.c -o /tmp/mangrove-timekeeping-test
	/tmp/mangrove-timekeeping-test
	python3 tests/userspace/time_libc_test.py "$(HOST_CC)"
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Ikernel/include -Iinclude tests/kernel/rtc_decode_test.c \
		kernel/src/rtc_decode.c -o /tmp/mangrove-rtc-decode-test
	/tmp/mangrove-rtc-decode-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pthread \
		-fsanitize=address,undefined -fno-builtin \
		-Ikernel/include -Iinclude -idirafter libc/include \
		tests/kernel/system_clock_test.c kernel/src/rtc_decode.c \
		libc/src/time_convert.c -o /tmp/mangrove-system-clock-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-system-clock-test
	@echo timekeeping tests passed

test-terminal:
	python3 tests/terminal/framebuffer_rect_test.py "$(HOST_CC)"
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Ikernel/include -Iinclude tests/terminal/utf8_decode_test.c \
		kernel/src/utf8.c libc/src/utf8.c -o /tmp/mangrove-utf8-test
	/tmp/mangrove-utf8-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Wno-unused-function \
		-fno-builtin -fsanitize=address,undefined -ffunction-sections \
		-fdata-sections -Ikernel/include -Iinclude -Ilibc/include \
		tests/terminal/terminal_render_test.c kernel/src/utf8.c libc/src/utf8.c \
		libc/src/line_editor.c libc/src/string.c -Wl,--gc-sections \
		-o /tmp/mangrove-terminal-render-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-terminal-render-test
	python3 tests/terminal/terminal_overlay_test.py "$(HOST_CC)"
	python3 tests/terminal/terminal_write_owner_test.py "$(HOST_CC)"
	python3 tests/terminal/console_input_smp_test.py "$(HOST_CC)"
	python3 tests/terminal/keyboard_repeat_smp_test.py "$(HOST_CC)"
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-Ilibc/include -Iinclude tests/terminal/line_editor_prompt_render_test.c \
		libc/src/utf8.c libc/src/string.c -Wl,--gc-sections \
		-o /tmp/mangrove-line-editor-prompt-render-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-line-editor-prompt-render-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-Ilibc/include -Iinclude tests/terminal/line_editor_key_event_test.c \
		libc/src/utf8.c libc/src/string.c -Wl,--gc-sections \
		-o /tmp/mangrove-line-editor-key-event-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-line-editor-key-event-test
	@echo terminal renderer, UTF-8, overlay, input synchronization and ownership tests passed

.PHONY: test-unicode
test-unicode:
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -Ilibc/include -Iinclude \
		tests/terminal/unicode_conversion_test.c libc/src/utf8.c libc/src/wchar.c \
		libc/src/string.c libc/src/errno.c -o /tmp/mangrove-unicode-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-unicode-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-Ilibc/include -Iinclude tests/terminal/line_editor_unicode_test.c \
		libc/src/line_editor.c libc/src/utf8.c libc/src/string.c \
		-Wl,--gc-sections -o /tmp/mangrove-line-editor-unicode-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-line-editor-unicode-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-Ilibc/include -Iinclude tests/terminal/line_editor_history_test.c \
		libc/src/line_editor.c libc/src/utf8.c libc/src/string.c \
		-Wl,--gc-sections -o /tmp/mangrove-line-editor-history-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-line-editor-history-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Wno-unused-function \
		-Wno-unused-variable \
		-fno-builtin -fsanitize=address,undefined -ffunction-sections \
		-fdata-sections -Ilibc/include -Iinclude \
		tests/terminal/text_metrics_test.c libc/src/utf8.c \
		-Wl,--gc-sections -o /tmp/mangrove-text-metrics-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-text-metrics-test

test-password:
	python3 tests/userspace/password_test.py "$(HOST_CC)"

test-reed:
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin -fsanitize=address,undefined \
		-Ilibc/include -Iinclude tests/userspace/command_path_test.c userspace/common/path.c -o /tmp/mangrove-command-path-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-command-path-test
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-Ilibc/include -Iinclude -Iuserspace/reed -Iuserspace/common \
		tests/userspace/reed_keymap_test.c -Wl,--gc-sections \
		-o /tmp/mangrove-reed-keymap-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-reed-keymap-test
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -Wno-unused-function \
		-Wno-unused-variable -fno-builtin -fsanitize=address,undefined \
		-ffunction-sections -fdata-sections \
		-Ilibc/include -Iinclude -Iuserspace/reed -Iuserspace/common \
		tests/userspace/reed_interaction_test.c libc/src/clipboard.c userspace/clipd/clipboard.c userspace/reed/buffer.c \
		userspace/reed/config.c libc/src/string.c libc/src/utf8.c \
		$(HOST_REED_TEST_FLAGS) \
		-o /tmp/mangrove-reed-interaction-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-reed-interaction-test
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -Wno-unused-function \
		-fno-builtin -fsanitize=address,undefined -ffunction-sections \
		-fdata-sections -DTEST_REED_RENDER -Ikernel/include -Iinclude -Ilibc/include \
		tests/terminal/fullscreen_render_test.c libc/src/clipboard.c userspace/clipd/clipboard.c userspace/reed/buffer.c \
		userspace/reed/config.c kernel/src/utf8.c libc/src/utf8.c \
		libc/src/line_editor.c libc/src/string.c -Wl,--gc-sections \
		-o /tmp/mangrove-reed-render-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-reed-render-test
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-Ilibc/include -Iinclude -Iuserspace/reed \
		tests/userspace/reed_config_test.c userspace/reed/config.c \
		userspace/common/app_directory.c libc/src/string.c -Wl,--gc-sections \
		-o /tmp/mangrove-reed-config-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-reed-config-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude -Iuserspace/reed \
		tests/userspace/reed_buffer_test.c userspace/reed/buffer.c libc/src/string.c \
		libc/src/utf8.c \
		-o /tmp/mangrove-reed-buffer-test
	/tmp/mangrove-reed-buffer-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-ffunction-sections -fdata-sections \
		-Ilibc/include -Iinclude -Iuserspace/reed \
		tests/userspace/reed_safe_save_test.c userspace/reed/buffer.c libc/src/string.c \
		libc/src/utf8.c \
		$(HOST_REED_TEST_FLAGS) \
		-o /tmp/mangrove-reed-safe-save-test
	/tmp/mangrove-reed-safe-save-test
	@echo Reed buffer and safe-save tests passed

.PHONY: test-shoot test-shoot-config
test-shoot: test-shoot-config

test-shoot-config:
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -I. -Iuserspace/shoot -Iinclude \
		-idirafter libc/include tests/userspace/shoot_config_migration_test.c \
		userspace/shoot/config.c userspace/common/app_directory.c \
		userspace/common/path.c libc/src/string.c \
		-o /tmp/mangrove-shoot-config-migration-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-shoot-config-migration-test

test-shoot:
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -I. -Iuserspace/shoot -Iinclude \
		-idirafter libc/include tests/userspace/shoot_exit_test.c \
		userspace/shoot/commands/exit.c -o /tmp/mangrove-shoot-exit-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-shoot-exit-test
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-I. -Iuserspace/shoot -Iinclude -idirafter libc/include \
		tests/userspace/shoot_language_test.c userspace/shoot/language.c \
		userspace/shoot/script.c userspace/shoot/shell.c \
		userspace/shoot/builtin.c userspace/common/path.c \
		-Wl,--gc-sections -o /tmp/mangrove-shoot-language-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-shoot-language-test

test-launch: test-vmm test-heap test-history

.PHONY: test-x86-cache test-framebuffer-mapping
test-x86-cache:
	python3 tests/memory/x86_cache_test.py "$(HOST_CC)"

test-framebuffer-mapping:
	python3 tests/memory/framebuffer_mapping_test.py "$(HOST_CC)"

test-vmm: test-x86-cache test-framebuffer-mapping
	python3 tests/memory/vmm_test.py "$(HOST_CC)"

.PHONY: test-pmm
test-pmm:
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin -pthread \
		-fsanitize=address,undefined -Ikernel/include -Iinclude \
		-idirafter libc/include tests/memory/pmm_test.c \
		-o /tmp/mangrove-pmm-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-pmm-test

.PHONY: test-idt
test-idt:
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-Ikernel/include -Iinclude -idirafter libc/include \
		tests/kernel/idt_ist_test.c -Wl,--gc-sections -o /tmp/mangrove-idt-ist-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-idt-ist-test

.PHONY: test-e1000
test-e1000: $(BUILD_DIR)/tests/e1000-context-test \
		$(BUILD_DIR)/tests/e1000-tx-concurrency-test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/tests/e1000-context-test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/tests/e1000-tx-concurrency-test

$(BUILD_DIR)/tests/e1000-context-test: tests/network/e1000_context_test.c \
						 kernel/src/net/e1000.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-DE1000_HOST_TEST \
		-Ikernel/include -Ikernel/include/pci -Iinclude \
		-idirafter libc/include $< \
		-Wl,--gc-sections -o $@

$(BUILD_DIR)/tests/e1000-tx-concurrency-test: \
			tests/network/e1000_tx_concurrency_test.c kernel/src/net/e1000.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin -pthread \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-DE1000_HOST_TEST \
		-Ikernel/include -Ikernel/include/pci -Iinclude \
		-idirafter libc/include $< -Wl,--gc-sections -o $@

.PHONY: test-net
test-net: test-e1000
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -pthread -Ikernel/include -Iinclude \
		-idirafter libc/include tests/network/udp_concurrency_test.c \
		-o /tmp/mangrove-udp-concurrency-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-udp-concurrency-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -pthread -Ikernel/include -Iinclude \
		-idirafter libc/include tests/network/icmp_state_test.c \
		-o /tmp/mangrove-icmp-state-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-icmp-state-test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -pthread -ffunction-sections -fdata-sections \
		-Ikernel/include -Iinclude -idirafter libc/include \
		tests/network/net_user_table_test.c -Wl,--gc-sections \
		-o /tmp/mangrove-net-user-table-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-net-user-table-test

.PHONY: test-libc
test-libc: test-unicode test-libc-link test-math
	python3 tests/userspace/libc_string_test.py "$(HOST_CC)"
	python3 tests/userspace/runtime_foundations_test.py "$(HOST_CC)"
	python3 tests/userspace/runtime_stdio_test.py "$(HOST_CC)"

RUNTIME_API_LINK_OBJ := $(BUILD_DIR)/tests/runtime-api-link-probe.o
RUNTIME_API_LINK_ELF := $(BUILD_DIR)/tests/runtime-api-link-probe.elf

.PHONY: test-libc-link
test-libc-link: $(RUNTIME_API_LINK_ELF)
	@echo "Native standard-library declaration/link probe: PASS ($(RUNTIME_API_LINK_ELF))"

$(RUNTIME_API_LINK_OBJ): tests/userspace/runtime_api_link_probe.c $(wildcard libc/include/*.h libc/include/mg/*.h) $(USER_LIBC) $(USER_LIBM)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(RUNTIME_API_LINK_ELF): $(RUNTIME_API_LINK_OBJ) $(USER_CRT) $(USER_LIBM) $(USER_LIBC) $(USER_LIBGCC) $(USER_LINKER_SCRIPT)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(RUNTIME_API_LINK_OBJ) $(USER_LIBM) $(USER_LIBC) $(USER_LIBGCC)

.PHONY: test-math
test-math: $(USER_LIBM) $(HOST_LIBM_TEST_PREREQ)
	python3 tests/userspace/libm_accuracy_test.py "$(HOST_CC)" "$(USER_LIBM)"
	python3 tests/userspace/libm_accuracy_test.py "$(HOST_CC)" "$(HOST_LIBM_TEST)" --sanitized
	python3 tests/userspace/libm_complex_accuracy_test.py "$(HOST_CC)" "$(USER_LIBM)"
	python3 tests/userspace/libm_complex_accuracy_test.py "$(HOST_CC)" "$(HOST_LIBM_TEST)" --sanitized

$(HOST_LIBM_TEST_DIR)/%.o: libc/src/math/%.c libc/src/math/math_private.h \
                           libc/include/math.h libc/include/fenv.h \
                           libc/include/complex.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_LIBM_CFLAGS) -c $< -o $@

$(HOST_LIBM_TEST): $(HOST_LIBM_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(HOST_AR) rcs $@ $^

test-heap:
	python3 tests/memory/heap_test.py "$(HOST_CC)"

.PHONY: test-slab test-slab-benchmark
test-slab:
	python3 tests/memory/slab_test.py "$(HOST_CC)"

SLAB_GUEST_BUILD ?= $(abspath $(BUILD_DIR))/slab-guest-build
SLAB_GUEST_OUTPUT ?=
SLAB_GUEST_ACCEL ?= $(NATIVE_GUEST_ACCEL)
.PHONY: test-slab-guest
test-slab-guest: test-slab
	$(QEMU_ACCEL_PREFLIGHT) "$(SLAB_GUEST_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(SLAB_GUEST_BUILD)" \
		KERNEL_HYBRID_ALLOCATOR=0 SLAB_CORE_GUEST_TEST=1 \
		VMM_KERNEL_RANGE_GUEST_TEST=1 \
		binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_slab_test.py --build-dir "$(SLAB_GUEST_BUILD)" \
		$(if $(SLAB_GUEST_OUTPUT),--output "$(SLAB_GUEST_OUTPUT)-1vcpu") --cpus 1 \
		--accel "$(SLAB_GUEST_ACCEL)" \
		--deep-validation "$(SLAB_DEEP_VALIDATION)"
	python3 tests/guest/guest_slab_test.py --build-dir "$(SLAB_GUEST_BUILD)" \
		$(if $(SLAB_GUEST_OUTPUT),--output "$(SLAB_GUEST_OUTPUT)-2vcpu") --cpus 2 \
		--accel "$(SLAB_GUEST_ACCEL)" \
		--deep-validation "$(SLAB_DEEP_VALIDATION)"

HYBRID_GUEST_BUILD ?= $(abspath $(BUILD_DIR))/hybrid-guest-build
HYBRID_GUEST_OUTPUT ?=
.PHONY: test-hybrid-guest
test-hybrid-guest: test-slab
	$(QEMU_ACCEL_PREFLIGHT) "$(SLAB_GUEST_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(HYBRID_GUEST_BUILD)" \
		KERNEL_HYBRID_ALLOCATOR=1 SLAB_CORE_DIAGNOSTICS=1 \
		SLAB_CORE_GUEST_TEST=1 SLAB_DEEP_VALIDATION="$(SLAB_DEEP_VALIDATION)" \
		VMM_KERNEL_RANGE_GUEST_TEST=1 \
		binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_slab_test.py --build-dir "$(HYBRID_GUEST_BUILD)" \
		$(if $(HYBRID_GUEST_OUTPUT),--output "$(HYBRID_GUEST_OUTPUT)-1vcpu") --cpus 1 \
		--accel "$(SLAB_GUEST_ACCEL)" \
		--deep-validation "$(SLAB_DEEP_VALIDATION)" --hybrid-routing \
		--hybrid-cutoff "$(KERNEL_HYBRID_ALLOCATOR_CUTOFF)"
	python3 tests/guest/guest_slab_test.py --build-dir "$(HYBRID_GUEST_BUILD)" \
		$(if $(HYBRID_GUEST_OUTPUT),--output "$(HYBRID_GUEST_OUTPUT)-2vcpu") --cpus 2 \
		--accel "$(SLAB_GUEST_ACCEL)" \
		--deep-validation "$(SLAB_DEEP_VALIDATION)" --hybrid-routing \
		--hybrid-cutoff "$(KERNEL_HYBRID_ALLOCATOR_CUTOFF)"

VMM_RANGE_GUEST_BUILD ?= $(abspath $(BUILD_DIR))/vmm-range-guest-build
VMM_RANGE_GUEST_OUTPUT ?=
VMM_RANGE_GUEST_ACCEL ?= $(NATIVE_GUEST_ACCEL)
.PHONY: test-vmm-range-guest
test-vmm-range-guest:
	$(QEMU_ACCEL_PREFLIGHT) "$(VMM_RANGE_GUEST_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(VMM_RANGE_GUEST_BUILD)" \
		KERNEL_HYBRID_ALLOCATOR="$(KERNEL_HYBRID_ALLOCATOR)" \
		VMM_KERNEL_RANGE_GUEST_TEST=1 binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_vmm_range_test.py --build-dir "$(VMM_RANGE_GUEST_BUILD)" \
		$(if $(VMM_RANGE_GUEST_OUTPUT),--output "$(VMM_RANGE_GUEST_OUTPUT)-1vcpu") --cpus 1 \
		--accel "$(VMM_RANGE_GUEST_ACCEL)" \
		--hybrid-routing "$(KERNEL_HYBRID_ALLOCATOR)"
	python3 tests/guest/guest_vmm_range_test.py --build-dir "$(VMM_RANGE_GUEST_BUILD)" \
		$(if $(VMM_RANGE_GUEST_OUTPUT),--output "$(VMM_RANGE_GUEST_OUTPUT)-2vcpu") --cpus 2 \
		--accel "$(VMM_RANGE_GUEST_ACCEL)" \
		--hybrid-routing "$(KERNEL_HYBRID_ALLOCATOR)"

.PHONY: test-hybrid-public-benchmark
test-hybrid-public-benchmark:
	python3 tests/memory/hybrid_public_benchmark.py --self-test

ALLOCATOR_ROUTING_OUTPUT ?= $(abspath $(BUILD_DIR))/allocator-routing-api.json
.PHONY: test-allocator-routing
test-allocator-routing:
	python3 tests/memory/hybrid_public_benchmark.py --api-check-only \
		--output "$(ALLOCATOR_ROUTING_OUTPUT)"

SLAB_NATIVE_BENCH_BUILD ?= $(abspath $(BUILD_DIR))/slab-native-bench-build
SLAB_NATIVE_BENCH_OUTPUT ?=
SLAB_NATIVE_BENCH_ACCEL ?= $(NATIVE_GUEST_ACCEL)
SLAB_NATIVE_PROFILE_BUILD ?= $(abspath $(BUILD_DIR))/slab-native-profile-build
SLAB_NATIVE_PROFILE_OUTPUT ?=
.PHONY: test-slab-native-benchmark
test-slab-native-benchmark:
	$(QEMU_ACCEL_PREFLIGHT) "$(SLAB_NATIVE_BENCH_ACCEL)"
	python3 tests/guest/guest_slab_benchmark_evidence_test.py
	$(MAKE) -B -j4 BUILD_DIR="$(SLAB_NATIVE_BENCH_BUILD)" \
		KERNEL_HYBRID_ALLOCATOR=0 SLAB_CORE_GUEST_BENCHMARK=1 \
		SLAB_CORE_GUEST_FREE_PROFILE=0 \
		binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_slab_test.py --build-dir "$(SLAB_NATIVE_BENCH_BUILD)" \
		$(if $(SLAB_NATIVE_BENCH_OUTPUT),--output "$(SLAB_NATIVE_BENCH_OUTPUT)-1vcpu") --cpus 1 \
		--accel "$(SLAB_NATIVE_BENCH_ACCEL)" --benchmark \
		--deep-validation "$(SLAB_DEEP_VALIDATION)"
	python3 tests/guest/guest_slab_test.py --build-dir "$(SLAB_NATIVE_BENCH_BUILD)" \
		$(if $(SLAB_NATIVE_BENCH_OUTPUT),--output "$(SLAB_NATIVE_BENCH_OUTPUT)-2vcpu") --cpus 2 \
		--accel "$(SLAB_NATIVE_BENCH_ACCEL)" --benchmark \
		--deep-validation "$(SLAB_DEEP_VALIDATION)"
	$(MAKE) -B -j4 BUILD_DIR="$(SLAB_NATIVE_PROFILE_BUILD)" \
		KERNEL_HYBRID_ALLOCATOR=0 SLAB_CORE_GUEST_BENCHMARK=1 \
		SLAB_CORE_GUEST_FREE_PROFILE=1 \
		binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_slab_test.py --build-dir "$(SLAB_NATIVE_PROFILE_BUILD)" \
		$(if $(SLAB_NATIVE_PROFILE_OUTPUT),--output "$(SLAB_NATIVE_PROFILE_OUTPUT)-1vcpu") --cpus 1 \
		--accel "$(SLAB_NATIVE_BENCH_ACCEL)" --benchmark --free-profile \
		--deep-validation "$(SLAB_DEEP_VALIDATION)"

test-slab-benchmark:
	python3 tests/memory/slab_benchmark.py "$(HOST_CC)"

test-mfqs:
	python3 tests/filesystems/mfqs/mfqs_test.py "$(HOST_CC)"

.PHONY: test-heap-profile
test-heap-profile:
	python3 tests/memory/heap_profile_test.py "$(HOST_CC)"
	python3 tests/memory/heap_profile_analyze_test.py

HYBRID_RUNTIME_PROFILE_BUILD ?= $(abspath $(BUILD_DIR))/hybrid-runtime-profile-build
HYBRID_RUNTIME_PROFILE_OUTPUT ?=
HYBRID_RUNTIME_PROFILE_CUTOFF ?= 384
.PHONY: test-hybrid-runtime-profile-guest
test-hybrid-runtime-profile-guest: test-heap-profile
	$(QEMU_ACCEL_PREFLIGHT) "$(HEAP_PROFILE_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(HYBRID_RUNTIME_PROFILE_BUILD)" \
		KERNEL_HYBRID_ALLOCATOR=1 \
		KERNEL_HYBRID_ALLOCATOR_CUTOFF="$(HYBRID_RUNTIME_PROFILE_CUTOFF)" \
		HYBRID_RUNTIME_DIAGNOSTICS=1 RTC_UTC_TRUST=1 \
		binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_safe_test.py --build-dir "$(HYBRID_RUNTIME_PROFILE_BUILD)" \
		$(if $(HYBRID_RUNTIME_PROFILE_OUTPUT),--output "$(HYBRID_RUNTIME_PROFILE_OUTPUT)-1vcpu") --accel "$(HEAP_PROFILE_ACCEL)" \
		--backend mgfs --repetitions 1 --readonly --negative-control \
		--heap-profile --cpus 1
	python3 tests/guest/guest_safe_test.py --build-dir "$(HYBRID_RUNTIME_PROFILE_BUILD)" \
		$(if $(HYBRID_RUNTIME_PROFILE_OUTPUT),--output "$(HYBRID_RUNTIME_PROFILE_OUTPUT)-2vcpu") --accel "$(HEAP_PROFILE_ACCEL)" \
		--backend mgfs --repetitions 1 --readonly --heap-profile --cpus 2

HEAP_PROFILE_BUILD ?= $(abspath $(BUILD_DIR))/heap-profile-build
HEAP_PROFILE_OUTPUT ?=
HEAP_PROFILE_ACCEL ?= $(NATIVE_GUEST_ACCEL)
.PHONY: test-heap-profile-guest
test-heap-profile-guest: test-heap-profile
	$(QEMU_ACCEL_PREFLIGHT) "$(HEAP_PROFILE_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(HEAP_PROFILE_BUILD)" \
		KERNEL_HYBRID_ALLOCATOR=0 HEAP_PROFILE_DIAGNOSTICS=1 \
		RTC_UTC_TRUST=1 binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_safe_test.py --build-dir "$(HEAP_PROFILE_BUILD)" $(if $(HEAP_PROFILE_OUTPUT),--output "$(HEAP_PROFILE_OUTPUT)-1vcpu") --accel "$(HEAP_PROFILE_ACCEL)" --backend mgfs --repetitions 1 --readonly --negative-control --heap-profile --cpus 1
	python3 tests/guest/guest_safe_test.py --build-dir "$(HEAP_PROFILE_BUILD)" $(if $(HEAP_PROFILE_OUTPUT),--output "$(HEAP_PROFILE_OUTPUT)-2vcpu") --accel "$(HEAP_PROFILE_ACCEL)" --backend mgfs --repetitions 1 --readonly --heap-profile --cpus 2

.PHONY: test-fsstress
test-fsstress:
	python3 tests/filesystems/stress/fsstress_test.py "$(HOST_CC)"
	python3 tests/guest/guest_stress_evidence_test.py

.PHONY: fsstress-host
fsstress-host: $(BUILD_DIR)/fsstress/fsstress-host

$(BUILD_DIR)/fsstress/fsstress-host: $(addprefix userspace/fsstress/,model.c engine.c main.c posix.c fsstress.h)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -Wno-misleading-indentation $(filter %.c,$^) -o $@

test-history:
	python3 tests/filesystems/mgfs/mgfs_write_test.py "$(HOST_CC)"
	python3 tests/filesystems/mgfs/mgfs_positional_write_test.py "$(HOST_CC)"
	python3 tests/filesystems/mgfs/mgfs_directory_test.py "$(HOST_CC)"
	python3 tests/filesystems/mgfs/mgfs_directory_allocation_test.py "$(HOST_CC)"
	python3 tests/filesystems/mgfs/mgfs_record_lifecycle_test.py "$(HOST_CC)"
	python3 tests/filesystems/mgfs/mgfs_mutation_failure_test.py "$(HOST_CC)"
	python3 tests/filesystems/mgfs/mgfs_ownership_preflight_test.py "$(HOST_CC)"
	python3 tests/filesystems/mgfs/mgfs_mount_ownership_test.py "$(HOST_CC)"
	python3 tests/filesystems/mgfs/mgfs_stage_cancel_test.py "$(HOST_CC)"
	python3 tests/filesystems/mgfs/history_fs_test.py "$(HOST_CC)"
	python3 tests/userspace/history_test.py "$(HOST_CC)"

.PHONY: test-app-directory
test-app-directory:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude tests/userspace/app_directory_test.c \
		userspace/common/app_directory.c -o $(BUILD_DIR)/tests/app-directory-test
	$(BUILD_DIR)/tests/app-directory-test

.PHONY: test-plant
test-plant:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude tests/userspace/plant_test.c \
		-o $(BUILD_DIR)/tests/plant-test
	$(BUILD_DIR)/tests/plant-test

.PHONY: test-mkdir
test-mkdir:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude tests/userspace/mkdir_test.c \
		-o $(BUILD_DIR)/tests/mkdir-test
	$(BUILD_DIR)/tests/mkdir-test

.PHONY: test-del
test-del:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude tests/userspace/del_test.c \
		-o $(BUILD_DIR)/tests/del-test
	$(BUILD_DIR)/tests/del-test

.PHONY: test-copy
test-copy:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude tests/userspace/copy_test.c \
		-o $(BUILD_DIR)/tests/copy-test
	$(BUILD_DIR)/tests/copy-test

.PHONY: test-move
test-move:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude tests/userspace/move_test.c \
		-o $(BUILD_DIR)/tests/move-test
	$(BUILD_DIR)/tests/move-test

.PHONY: test-list
test-list:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -Wno-unused-function \
		-fno-builtin -fsanitize=address,undefined \
		-Ilibc/include -Iinclude -Iuserspace/common \
		tests/userspace/list_render_test.c libc/src/utf8.c \
		-o $(BUILD_DIR)/tests/list-render-test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/tests/list-render-test

.PHONY: test-shoot-help
test-shoot-help:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -fno-builtin \
		-Ilibc/include -Iinclude -Iuserspace/shoot \
		tests/userspace/shoot_help_test.c -o $(BUILD_DIR)/tests/shoot-help-test
	$(BUILD_DIR)/tests/shoot-help-test

.PHONY: test-pager
test-pager:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -Wno-unused-function \
		-Wno-unused-variable -fno-builtin -fsanitize=address,undefined \
		-ffunction-sections -fdata-sections -Ilibc/include -Iinclude \
		tests/userspace/text_pager_test.c libc/src/utf8.c \
		-Wl,--gc-sections -o $(BUILD_DIR)/tests/text-pager-test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/tests/text-pager-test
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -Wno-unused-function \
		-fno-builtin -fsanitize=address,undefined -ffunction-sections \
		-fdata-sections -Ikernel/include -Iinclude -Ilibc/include \
		tests/terminal/fullscreen_render_test.c kernel/src/utf8.c libc/src/utf8.c \
		libc/src/line_editor.c libc/src/string.c -Wl,--gc-sections \
		-o $(BUILD_DIR)/tests/pager-render-test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/tests/pager-render-test

.PHONY: test-about
test-about:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-Ilibc/include -Iinclude -Iuserspace/common \
		tests/userspace/about_test.c -Wl,--gc-sections \
		-o $(BUILD_DIR)/tests/about-test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/tests/about-test

.PHONY: test-view
test-view:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-Ilibc/include -Iinclude tests/userspace/view_command_test.c \
		-Wl,--gc-sections -o $(BUILD_DIR)/tests/view-command-test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/tests/view-command-test

.PHONY: test-info
test-info:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-Ilibc/include -Iinclude tests/userspace/info_command_test.c \
		-Wl,--gc-sections -o $(BUILD_DIR)/tests/info-command-test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/tests/info-command-test

.PHONY: test-help
test-help:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-Iinclude -idirafter libc/include tests/userspace/help_record_test.c \
		libc/src/utf8.c -Wl,--gc-sections \
		-o $(BUILD_DIR)/tests/help-record-test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/tests/help-record-test share/help
	python3 tests/userspace/help_records_test.py

test-dev-tools:
	python3 tests/guest/artifact_lifecycle_test.py
	python3 tests/guest/qemu_accel_test.py
	python3 tests/guest/reed_input_diagnostic_test.py
	python3 tests/guest/guest_mgfs_evidence_test.py
	python3 tests/guest/guest_secondary_evidence_test.py
	python3 tests/guest/guest_lifetime_evidence_test.py
	python3 tests/guest/guest_safe_evidence_test.py
	python3 tests/guest/dev_image_guard_test.py

test-spinlock-context:
	python3 tests/kernel/spinlock_context_test.py "$(HOST_CC)"

test-sync: test-spinlock-context
	python3 tests/filesystems/mgfs/mgfs_handoff_test.py "$(HOST_CC)"
	python3 tests/filesystems/fat32/fat_handoff_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/vfs_location_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/vfs_checked_read_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/vfs_lookup_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/vfs_lookup_callers_test.py "$(HOST_CC)"
	python3 tests/kernel/elf_checked_read_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/recovery_inspection_test.py "$(HOST_CC)"
	python3 tests/storage/init_storage_test.py "$(HOST_CC)"
	python3 tests/storage/device_snapshot_test.py "$(HOST_CC)"
	python3 tests/userspace/directory_enumeration_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/file_create_contents_syscall_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/file_replace_syscall_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/vfs_create_contents_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/vfs_unlink_pin_test.py "$(HOST_CC)"
	python3 tests/kernel/path_move_syscall_test.py "$(HOST_CC)"
	python3 tests/kernel/path_remove_syscall_test.py "$(HOST_CC)"
	python3 tests/kernel/pathname_copy_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/vfs_write_pin_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/vfs_read_only_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/vfs_create_pin_test.py "$(HOST_CC)"
	python3 tests/kernel/termination_lifecycle_test.py "$(HOST_CC)"
	python3 tests/kernel/process_reap_test.py "$(HOST_CC)"
	python3 tests/kernel/shutdown_lifecycle_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/vfs_admission_lifetime_test.py
	python3 tests/filesystems/vfs/vfs_file_facts_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/vfs_recovery_pin_test.py "$(HOST_CC)"
	python3 tests/kernel/scheduler_handoff_test.py
	python3 tests/kernel/scheduler_current_test.py
	python3 tests/kernel/mutex_test.py
	python3 tests/storage/block_gate_test.py

test-storage:
	python3 tests/storage/ahci_signature_test.py "$(HOST_CC)"
	python3 tests/storage/storage_coherency_test.py "$(HOST_CC)"
	python3 tests/filesystems/exfat/storage_format_test.py "$(HOST_CC)"
	python3 tests/filesystems/mgfs/mgfs_label_test.py "$(HOST_CC)"
	python3 tests/filesystems/fat32/fat32_mutation_boundary_test.py "$(HOST_CC)"
	python3 tests/filesystems/mgfs/mgfs_device_identity_test.py "$(HOST_CC)"
	python3 tests/filesystems/exfat/fat_exfat_mount_identity_test.py "$(HOST_CC)"
	python3 tests/filesystems/fat32/fat32_replace_flush_test.py "$(HOST_CC)"
	python3 tests/filesystems/fat32/fat32_snapshot_stream_test.py "$(HOST_CC)"
	python3 tests/filesystems/exfat/exfat_names_test.py "$(HOST_CC)"
	python3 tests/filesystems/exfat/exfat_snapshot_stream_test.py "$(HOST_CC)"
	python3 tests/filesystems/exfat/exfat_system_test.py "$(HOST_CC)"
	python3 tests/filesystems/exfat/exfat_create_test.py "$(HOST_CC)"
	python3 tests/filesystems/exfat/exfat_extension_test.py "$(HOST_CC)"
	python3 tests/filesystems/exfat/exfat_delete_identity_test.py "$(HOST_CC)"
	python3 tests/filesystems/exfat/exfat_zero_directory_test.py "$(HOST_CC)"
	python3 tests/filesystems/exfat/exfat_label_test.py "$(HOST_CC)"
	python3 tests/filesystems/fat32/fat32_label_test.py "$(HOST_CC)"
	python3 tests/filesystems/exfat/exfat_metadata_test.py "$(HOST_CC)"
	python3 tests/filesystems/vfs/vfs_node_registration_test.py "$(HOST_CC)"
	python3 tests/storage/usb_teardown_test.py
	python3 tests/storage/storage_lifecycle_race_test.py
	python3 tests/storage/storage_lifecycle_test.py
	python3 tests/storage/xhci_lifecycle_test.py "$(HOST_CC)"
	python3 tests/storage/xhci_admission_test.py "$(HOST_CC)"
	python3 tests/storage/usb_bot_test.py "$(HOST_CC)"
	python3 tests/storage/ahci_test.py "$(HOST_CC)"
	python3 tests/storage/storage_read_test.py "$(HOST_CC)"
	python3 tests/storage/usb_storage_test.py "$(HOST_CC)"

test-mgfs-version: $(MKMGFS) $(BUILD_DIR)/mgfsck
	python3 tests/filesystems/mgfs/mgfs_version_test.py $(MKMGFS) $(BUILD_DIR)/mgfsck "$(HOST_CC)"

test-mgfs-reconcile: $(MKMGFS) $(BUILD_DIR)/mgfsck
	python3 tests/filesystems/mgfs/mgfs_update_record_reconcile_test.py $(MKMGFS) $(BUILD_DIR)/mgfsck

check-image-deps:
	@missing=""; \
	for tool in mkfs.fat mmd mcopy python3; do \
		command -v "$$tool" >/dev/null 2>&1 || missing="$$missing $$tool"; \
	done; \
	if [ -n "$$missing" ]; then \
		echo "Missing image-build tools:$$missing" >&2; \
		if [ "$(UNAME)" = Darwin ]; then \
			echo "Install them with: brew install dosfstools mtools" >&2; \
		else \
			echo "On Debian/Ubuntu, install them with: sudo apt-get install dosfstools mtools python3" >&2; \
		fi; \
		exit 1; \
	fi; \
	if [ ! -f "$(OVMF_VARS_SOURCE)" ]; then \
		echo "Missing UEFI firmware: $(OVMF_VARS_SOURCE)" >&2; \
		if [ "$(UNAME)" = Darwin ]; then \
			echo "Install it with: brew install qemu" >&2; \
		else \
			echo "On Debian/Ubuntu, install it with: sudo apt-get install ovmf" >&2; \
		fi; \
		exit 1; \
	fi

check-usb-deps: check-image-deps
	@if [ "$(UNAME)" = Darwin ]; then \
		if ! command -v sgdisk >/dev/null 2>&1; then \
			echo "Missing macOS USB-image tool: sgdisk" >&2; \
			echo "Install it with: brew install gptfdisk" >&2; \
			exit 1; \
		fi; \
	elif [ "$(UNAME)" = Linux ]; then \
		if ! command -v parted >/dev/null 2>&1; then \
			echo "Missing Linux USB-image tool: parted" >&2; \
			echo "On Debian/Ubuntu, install it with: sudo apt-get install parted" >&2; \
			exit 1; \
		fi; \
	else \
		echo "Unsupported host OS for usb-image: $(UNAME)" >&2; \
		exit 1; \
	fi

check-qemu-deps:
	@if ! command -v "$(QEMU)" >/dev/null 2>&1; then \
		echo "Missing QEMU executable: $(QEMU)" >&2; \
		if [ "$(UNAME)" = Darwin ]; then \
			echo "Install it with: brew install qemu" >&2; \
		else \
			echo "On Debian/Ubuntu, install it with: sudo apt-get install qemu-system-x86 ovmf" >&2; \
		fi; \
		exit 1; \
	fi; \
	if ! "$(QEMU)" -accel help 2>/dev/null | grep -q "^[[:space:]]*$(QEMU_ACCEL)[[:space:]]*$$"; then \
		echo "$(QEMU) does not provide the required $(QEMU_ACCEL) accelerator on $(UNAME)." >&2; \
		echo "Available accelerators:" >&2; \
		"$(QEMU)" -accel help >&2; \
		exit 1; \
	fi; \
	if [ "$(UNAME)" = Linux ] && [ ! -r /dev/kvm -o ! -w /dev/kvm ]; then \
		echo "KVM is unavailable: /dev/kvm must exist and be readable and writable." >&2; \
		echo "Load the KVM module and add your user to the kvm group, then log in again." >&2; \
		exit 1; \
	fi; \
	if [ "$(UNAME)" = Darwin ] && [ "$(QEMU_ACCEL)" = hvf ] && [ "$$(sysctl -n kern.hv_support 2>/dev/null)" != 1 ]; then \
		echo "HVF is unavailable: this Mac does not report Hypervisor Framework support." >&2; \
		exit 1; \
	fi; \
	if [ ! -f "$(OVMF_CODE_SOURCE)" ]; then \
		echo "Missing UEFI firmware: $(OVMF_CODE_SOURCE)" >&2; \
		exit 1; \
	fi

ifeq ($(QEMU_ACCEL),tcg)
qemu-warning:
	@echo "Warning: Running x86_64 Mangrove under TCG software emulation on Apple Silicon; performance will be slower." >&2
else
qemu-warning:
endif

dev-image: check-usb-deps binaries $(MKMGFS) $(OVMF_VARS)
	@mkdir -p $(STATE_DIR)
	./scripts/update_dev_image.sh --disk "$(DEV_IMAGE)" --root "$(DEV_ROOT_IMAGE)"

fresh-dev-image: check-usb-deps binaries $(MKMGFS) $(OVMF_VARS)
	@mkdir -p $(STATE_DIR)
	@echo "[FRESH] Factory-resetting persistent development image $(DEV_IMAGE)"
	./scripts/update_dev_image.sh --fresh --disk "$(DEV_IMAGE)" --root "$(DEV_ROOT_IMAGE)"

# The standard QEMU development environment explicitly sets its RTC to UTC.
# This assertion flows to binaries and the kernel objects needed by the image.
dev-image fresh-dev-image: override RTC_UTC_TRUST = 1

flash-image: check-usb-deps binaries $(MKMGFS) $(OVMF_VARS)
	./scripts/make_image.sh --fresh --root "$(FLASH_ROOT_IMAGE)" --autologin developer
	@mkdir -p $(MANGROVE_DIR)
	@rm -f $(USB_IMAGE)
	@dd if=/dev/zero of=$(USB_IMAGE) bs=1 count=0 seek=135283200 2>/dev/null
ifeq ($(UNAME),Darwin)
	@sgdisk --zap-all \
		--new=1:2048:133119 --typecode=1:EF00 --change-name=1:MANGROVE_ESP \
		--new=2:133120:264191 --typecode=2:8300 --change-name=2:MANGROVE_ROOT \
		$(USB_IMAGE) >/dev/null 2>&1 || { \
			echo "sgdisk failed while creating the GPT in $(USB_IMAGE)" >&2; \
			exit 1; \
		}
else
	@parted -s -a minimal $(USB_IMAGE) mklabel gpt
	@parted -s -a minimal $(USB_IMAGE) mkpart MANGROVE_ESP fat32 2048s 133119s
	@parted -s -a minimal $(USB_IMAGE) set 1 esp on
	@parted -s -a minimal $(USB_IMAGE) mkpart MANGROVE_ROOT 133120s 264191s
endif
	@dd if=$(MANGROVE_DIR)/Boot.img of=$(USB_IMAGE) bs=512 seek=2048 conv=notrunc 2>/dev/null
	@dd if=$(FLASH_ROOT_IMAGE) of=$(USB_IMAGE) bs=512 seek=133120 conv=notrunc 2>/dev/null
	@echo "Created $(USB_IMAGE)"

# Specialist compatibility names.
image: dev-image
fresh-image: fresh-dev-image
usb-image: flash-image

QEMU_EXTRA_ARGS ?=
# Keep the old escape hatch usable without emitting two -smp options.  CPU
# topology is otherwise selected through QEMU_SMP.
QEMU_SMP_ARGS = $(if $(filter -smp -smp=%,$(QEMU_EXTRA_ARGS)),,-smp $(QEMU_SMP))

QEMU_RUN_ARGS = \
	-machine q35 \
	$(QEMU_PLATFORM_ARGS) \
	$(QEMU_DISPLAY_ARGS) \
	$(QEMU_SMP_ARGS) \
	-m 512M \
	-rtc base=utc,clock=vm \
	-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	-drive if=pflash,format=raw,file=$(OVMF_VARS) \
	-drive id=usb,file=$(DEV_IMAGE),format=raw,if=none \
	-netdev user,id=net0 \
	-device e1000,netdev=net0,mac=52:54:00:18:01:01 \
	-device qemu-xhci,id=xhci \
	-device usb-storage,id=boot-storage,bus=xhci.0,port=2,drive=usb,bootindex=1 \
	-device usb-kbd,id=boot-kbd,bus=xhci.0,port=1 \
	$(QEMU_EXTRA_ARGS)

run: check-qemu-deps qemu-warning dev-image
	$(QEMU) $(QEMU_RUN_ARGS)

# Uses run's exact platform/device arguments with private generated media.
.PHONY: diagnose-reed-input
diagnose-reed-input:
	python3 scripts/diagnose_reed_input.py --qemu "$(QEMU)" $(if $(REED_INPUT_REPLAY),--replay "$(REED_INPUT_REPLAY)") -- $(QEMU_RUN_ARGS)

run-usb: run

fresh-run: check-qemu-deps qemu-warning fresh-dev-image
	$(QEMU) $(QEMU_RUN_ARGS)

clean:
	@mkdir -p $(STATE_DIR)
	@if [ ! -f "$(DEV_IMAGE)" ] && [ ! -f "$(DEV_ROOT_IMAGE)" ] && [ -f "$(LEGACY_DEV_IMAGE)" ]; then \
		echo "[CLEAN] Preserving legacy MGFS state as $(DEV_ROOT_IMAGE)"; \
		cp "$(LEGACY_DEV_IMAGE)" "$(DEV_ROOT_IMAGE)"; \
	fi
	rm -rf $(BUILD_DIR)
	@echo "[CLEAN] Removed $(BUILD_DIR); preserved $(STATE_DIR)/"

# OVMF Variable
$(OVMF_VARS):
	@mkdir -p $(dir $@)
	cp $(OVMF_VARS_SOURCE) $@

# Bootloader Link
$(EFI): $(BOOT_OBJS)
	@mkdir -p $(dir $@)
	$(BOOT_LD) $(BOOT_LDFLAGS) -o $@ $^

# Kernel Link
$(PITH): $(ALL_KERNEL_OBJS) kernel/linker.ld
	@mkdir -p $(dir $@)
	$(LD_KERNEL) $(KERNEL_LDFLAGS) -o $@ $(ALL_KERNEL_OBJS)

$(MKMGFS): tools/mkmgfs.c libc/src/utf8.c include/mg/utf8.h include/types.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -Iinclude \
		tools/mkmgfs.c libc/src/utf8.c -o $@

SHOOT_C_SRCS := userspace/shoot/main.c \
                userspace/shoot/shell.c \
                userspace/shoot/builtin.c \
                userspace/shoot/config.c \
                userspace/shoot/history.c \
                userspace/shoot/completion.c \
                userspace/shoot/help.c \
                userspace/shoot/commands/exit.c \
                userspace/shoot/commands/help.c \
                userspace/shoot/commands/jump.c \
                userspace/shoot/commands/reload.c \
                userspace/shoot/commands/history.c \
                userspace/shoot/language.c \
                userspace/shoot/script.c

SHOOT_OBJS := $(patsubst userspace/shoot/%.c,$(SHOOT_DIR)/%.o,$(SHOOT_C_SRCS))

USER_C_OBJS := $(SPROUT_DIR)/sprout.o \
               $(SPROUT_CMD_DIR)/sprout.o \
               $(SESSIOND_DIR)/sessiond.o \
               $(LOGIND_DIR)/main.o \
               $(LOGD_DIR)/main.o \
               $(NETWORKD_DIR)/networkd.o \
               $(DEVICED_DIR)/main.o \
               $(VOLUMED_DIR)/main.o \
               $(CLIPD_DIR)/main.o $(CLIPD_DIR)/clipboard.o \
               $(BUILD_DIR)/cliptest/main.o \
               $(MOUNT_DIR)/main.o \
               $(UNMOUNT_DIR)/main.o \
               $(EJECT_DIR)/main.o \
               $(BUILD_DIR)/userspace/volume_client.o \
               $(LSPCI_DIR)/main.o \
               $(LSUSB_DIR)/main.o \
               $(BUILD_DIR)/userspace/device_query.o \
               $(BUILD_DIR)/userspace/hardware_ids.o \
               $(LSDISK_DIR)/main.o \
               $(BUILD_DIR)/userspace/network_client.o \
               $(DISKUTIL_DIR)/main.o \
               $(STORAGE_SNAPSHOT_OBJ) \
               $(CREW_DIR)/main.o \
               $(ABOUT_DIR)/main.o \
               $(VIEW_DIR)/main.o \
               $(INFO_DIR)/main.o \
               $(MEM_DIR)/main.o \
               $(TIME_DIR)/main.o \
               $(TMON_DIR)/main.o \
               $(LOGV_DIR)/main.o \
               $(CLEAR_DIR)/clear.o \
               $(COPY_DIR)/main.o \
               $(LIST_DIR)/main.o \
               $(LOCATE_DIR)/locate.o \
               $(MOVE_DIR)/main.o \
               $(MKDIR_DIR)/main.o \
               $(PLANT_DIR)/plant.o \
               $(TYPE_DIR)/type.o \
               $(WAIT_DIR)/wait.o \
               $(DEL_DIR)/main.o \
               $(SAY_DIR)/say.o \
               $(SHUTDOWN_DIR)/shutdown.o \
               $(REBOOT_DIR)/reboot.o \
               $(UPTIME_DIR)/uptime.o \
               $(DATE_DIR)/date.o \
               $(VERSION_DIR)/version.o \
               $(WHERE_DIR)/where.o \
               $(PING_DIR)/main.o \
               $(PING_DIR)/ping_args.o \
               $(RESOLVE_DIR)/main.o \
               $(FETCH_DIR)/main.o \
               $(FETCH_DIR)/fetch_url.o \
               $(NETINFO_DIR)/main.o \
               $(NETCFG_DIR)/main.o \
               $(POWER_DIR)/power.o \
               $(IDENTITY_DIR)/identity.o \
               $(USER_CMD_DIR)/user.o \
               $(USER_LIBC_DIR)/syscall_c.o \
               $(USER_LIBC_DIR)/string.o \
               $(USER_LIBC_DIR)/allocator.o \
               $(USER_LIBC_DIR)/clipboard.o \
               $(USER_LIBC_DIR)/stdio.o \
               $(USER_LIBC_DIR)/native.o \
               $(USER_LIBC_DIR)/line_editor.o \
               $(USER_LIBC_DIR)/net.o \
               $(USER_LIBC_DIR)/time.o \
               $(USER_LIBC_DIR)/time_convert.o \
               $(COMMAND_PATH_OBJ) \
               $(HELP_OBJ) \
               $(TEXT_PAGER_OBJ) \
               $(BUILD_DIR)/userspace/secret_input.o \
               $(SHOOT_OBJS)
USER_DEPS := $(USER_C_OBJS:.o=.d)

$(SPROUT_DIR)/sprout.o: userspace/sprout/main.c \
                              libc/include/mg/service.h \
                              $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/sprout -c $< -o $@

$(SPROUT_CMD_DIR)/sprout.o: userspace/sproutctl/main.c \
                             libc/include/mg/service.h \
                             userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/sproutctl -Iuserspace/common -c $< -o $@

$(BUILD_DIR)/userspace/secret_input.o: userspace/common/secret_input.c \
                                      userspace/common/secret_input.h \
                                      $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(HELP_OBJ): userspace/common/help.c userspace/common/help.h \
             libc/include/mg/filesystem.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(TEXT_PAGER_OBJ): userspace/common/text_pager.c \
                   userspace/common/text_pager.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(SPROUT): $(SPROUT_DIR)/sprout.o $(USER_CRT) $(USER_LIBC) \
           $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(SPROUT_DIR)/sprout.o \
		$(USER_LIBC)

$(SPROUT_CMD): $(SPROUT_CMD_DIR)/sprout.o $(HELP_OBJ) $(USER_CRT) \
               $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(SPROUT_CMD_DIR)/sprout.o $(HELP_OBJ) $(USER_LIBC)

$(SESSIOND_DIR)/sessiond.o: userspace/sessiond/main.c \
                                  libc/include/mg/session.h \
                                  libc/include/mg/session_service.h \
                                  include/mangrove_version.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/sessiond -Iuserspace/common -c $< -o $@

$(SESSIOND): $(SESSIOND_DIR)/sessiond.o $(USER_CRT) $(USER_LIBC) \
            $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(SESSIOND_DIR)/sessiond.o \
		$(USER_LIBC)

$(LOGIND_DIR)/main.o: userspace/logind/main.c \
                      userspace/common/secret_input.h \
                      libc/include/mg/pass.h \
                      libc/include/mg/session.h \
                      libc/include/mg/session_service.h \
                      include/mangrove_version.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/logind -Iuserspace/common -c $< -o $@

$(LOGIND): $(LOGIND_DIR)/main.o $(BUILD_DIR)/userspace/secret_input.o \
           $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(LOGIND_DIR)/main.o \
		$(BUILD_DIR)/userspace/secret_input.o $(USER_LIBC)

$(NETWORKD_DIR)/networkd.o: userspace/networkd/main.c \
                           libc/include/mg/network_service.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/networkd -c $< -o $@

$(NETWORKD): $(NETWORKD_DIR)/networkd.o $(USER_CRT) $(USER_LIBC) \
            $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(NETWORKD_DIR)/networkd.o $(USER_LIBC)

$(DEVICED_DIR)/main.o: userspace/deviced/main.c \
                       libc/include/mg/device_service.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/deviced -c $< -o $@

$(DEVICED): $(DEVICED_DIR)/main.o $(USER_CRT) $(USER_LIBC) \
            $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(DEVICED_DIR)/main.o $(USER_LIBC)

$(VOLUMED_DIR)/main.o: userspace/volumed/main.c include/mg/utf8.h \
                       libc/include/mg/device_service.h \
                       libc/include/mg/volume_service.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/volumed -c $< -o $@

$(VOLUMED): $(VOLUMED_DIR)/main.o $(USER_CRT) $(USER_LIBC) \
            $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(VOLUMED_DIR)/main.o $(USER_LIBC)

$(BUILD_DIR)/userspace/volume_client.o: userspace/common/volume_client.c \
                                       userspace/common/volume_client.h \
                                       libc/include/mg/volume_service.h \
                                       $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(MOUNT_DIR)/main.o: userspace/mount/main.c userspace/common/help.h \
                     userspace/common/volume_client.h \
                     libc/include/mg/volume_service.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/mount -Iuserspace/common -c $< -o $@

$(MOUNT): $(MOUNT_DIR)/main.o $(BUILD_DIR)/userspace/volume_client.o \
          $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(MOUNT_DIR)/main.o \
		$(BUILD_DIR)/userspace/volume_client.o $(HELP_OBJ) $(USER_LIBC)

$(UNMOUNT_DIR)/main.o: userspace/unmount/main.c userspace/common/help.h \
                       userspace/common/volume_client.h \
                       libc/include/mg/volume_service.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/unmount -Iuserspace/common -c $< -o $@

$(UNMOUNT): $(UNMOUNT_DIR)/main.o $(BUILD_DIR)/userspace/volume_client.o \
            $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(UNMOUNT_DIR)/main.o \
		$(BUILD_DIR)/userspace/volume_client.o $(HELP_OBJ) $(USER_LIBC)

$(EJECT_DIR)/main.o: userspace/eject/main.c userspace/common/help.h \
                     userspace/common/volume_client.h \
                     libc/include/mg/volume_service.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/eject -Iuserspace/common -c $< -o $@

$(EJECT): $(EJECT_DIR)/main.o $(BUILD_DIR)/userspace/volume_client.o \
          $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(EJECT_DIR)/main.o \
		$(BUILD_DIR)/userspace/volume_client.o $(HELP_OBJ) $(USER_LIBC)

$(BUILD_DIR)/userspace/device_query.o: userspace/common/device_query.c \
                                       userspace/common/device_query.h \
                                       libc/include/mg/device_service.h \
                                       $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(STORAGE_SNAPSHOT_OBJ): userspace/common/storage_snapshot.c \
                         userspace/common/storage_snapshot.h \
                         libc/include/mg/device_service.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(BUILD_DIR)/userspace/hardware_ids.o: userspace/common/hardware_ids.c \
                                      userspace/common/hardware_ids.h \
                                      libc/include/mg/device_service.h \
                                      libc/include/mg/object.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(LSPCI_DIR)/main.o: userspace/lspci/main.c \
                     userspace/common/device_query.h \
                     userspace/common/hardware_ids.h \
                     userspace/common/help.h userspace/common/table.h \
                     libc/include/mg/device_service.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/lspci -Iuserspace/common -c $< -o $@

$(LSPCI): $(LSPCI_DIR)/main.o $(BUILD_DIR)/userspace/device_query.o \
          $(BUILD_DIR)/userspace/hardware_ids.o $(HELP_OBJ) $(USER_CRT) \
          $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(LSPCI_DIR)/main.o \
		$(BUILD_DIR)/userspace/device_query.o \
		$(BUILD_DIR)/userspace/hardware_ids.o $(HELP_OBJ) $(USER_LIBC)

$(LSUSB_DIR)/main.o: userspace/lsusb/main.c \
                     userspace/common/device_query.h \
                     userspace/common/hardware_ids.h \
                     userspace/common/help.h userspace/common/table.h \
                     libc/include/mg/device_service.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/lsusb -Iuserspace/common -c $< -o $@

$(LSUSB): $(LSUSB_DIR)/main.o $(BUILD_DIR)/userspace/device_query.o \
          $(BUILD_DIR)/userspace/hardware_ids.o $(HELP_OBJ) $(USER_CRT) \
          $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(LSUSB_DIR)/main.o \
		$(BUILD_DIR)/userspace/device_query.o \
		$(BUILD_DIR)/userspace/hardware_ids.o $(HELP_OBJ) $(USER_LIBC)

$(LSDISK_DIR)/main.o: userspace/lsdsk/main.c \
                     userspace/common/help.h \
                     userspace/common/storage_snapshot.h \
                     userspace/common/table.h \
                     libc/include/mg/device_service.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/lsdsk -Iuserspace/common -c $< -o $@

$(LSDISK): $(LSDISK_DIR)/main.o $(STORAGE_SNAPSHOT_OBJ) $(HELP_OBJ) \
          $(USER_CRT) $(USER_LIBC) \
          $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(LSDISK_DIR)/main.o $(STORAGE_SNAPSHOT_OBJ) \
		$(HELP_OBJ) $(USER_LIBC)

$(BUILD_DIR)/userspace/network_client.o: userspace/common/network_client.c \
                                         userspace/common/network_client.h \
                                         userspace/common/help.h \
                                         userspace/common/table.h \
                                         libc/include/mg/network_service.h \
                                         $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(DISKUTIL_DIR)/main.o: userspace/diskutil/main.c \
                        userspace/common/help.h \
                        userspace/common/storage_snapshot.h \
                        userspace/common/table.h \
                        libc/include/mg/device_service.h \
                        libc/include/mg/line_editor.h \
                        libc/include/mg/storage.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/diskutil -Iuserspace/common -c $< -o $@

$(DISKUTIL): $(DISKUTIL_DIR)/main.o $(STORAGE_SNAPSHOT_OBJ) $(HELP_OBJ) \
            $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(DISKUTIL_DIR)/main.o $(STORAGE_SNAPSHOT_OBJ) \
		$(HELP_OBJ) $(USER_LIBC)

$(CREW_DIR)/main.o: userspace/crew/main.c userspace/common/help.h \
                    userspace/common/process_format.h \
                    userspace/common/table.h libc/include/mg/inspection.h \
                    libc/include/mg/process.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/crew -Iuserspace/common -c $< -o $@

$(CREW): $(CREW_DIR)/main.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) \
          $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(CREW_DIR)/main.o $(HELP_OBJ) $(USER_LIBC)

$(ABOUT_DIR)/main.o: userspace/about/main.c userspace/common/help.h \
                    include/mangrove_version.h libc/include/mg/inspection.h \
                    libc/include/mg/identity.h libc/include/mg/terminal.h \
                    $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/about -Iuserspace/common -c $< -o $@

$(ABOUT): $(ABOUT_DIR)/main.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) \
         $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(ABOUT_DIR)/main.o $(HELP_OBJ) $(USER_LIBC)

$(VIEW_DIR)/main.o: userspace/view/main.c userspace/common/help.h \
                    userspace/common/text_pager.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/view -Iuserspace/common -c $< -o $@

$(VIEW): $(VIEW_DIR)/main.o $(TEXT_PAGER_OBJ) $(HELP_OBJ) $(USER_CRT) \
         $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(VIEW_DIR)/main.o $(TEXT_PAGER_OBJ) $(HELP_OBJ) \
		$(USER_LIBC)

$(INFO_DIR)/main.o: userspace/info/main.c userspace/common/help.h \
                    userspace/common/text_pager.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/info -Iuserspace/common -c $< -o $@

$(INFO): $(INFO_DIR)/main.o $(TEXT_PAGER_OBJ) $(HELP_OBJ) $(USER_CRT) \
         $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(INFO_DIR)/main.o $(TEXT_PAGER_OBJ) $(HELP_OBJ) \
		$(USER_LIBC)

$(MEM_DIR)/main.o: userspace/mem/main.c userspace/common/help.h \
                      userspace/common/table.h \
                      libc/include/mg/inspection.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/mem -Iuserspace/common -c $< -o $@

$(MEM): $(MEM_DIR)/main.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) \
           $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(MEM_DIR)/main.o $(HELP_OBJ) $(USER_LIBC)

$(TIME_DIR)/main.o: userspace/time/main.c userspace/common/help.h \
                    userspace/common/path.h libc/include/mg/process.h \
                    libc/include/mg/time.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/time -Iuserspace/common -c $< -o $@

$(TIME): $(TIME_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) \
         $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(TIME_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) \
		$(USER_LIBC)

$(TMON_DIR)/main.o: userspace/tmon/main.c userspace/common/help.h \
                    userspace/common/process_format.h \
                    userspace/common/table.h libc/include/mg/inspection.h \
                    libc/include/mg/memory.h libc/include/mg/terminal.h \
                    libc/include/mg/time.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/tmon -Iuserspace/common -c $< -o $@

$(TMON): $(TMON_DIR)/main.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) \
         $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(TMON_DIR)/main.o $(HELP_OBJ) $(USER_LIBC)

$(LOGD_DIR)/main.o: userspace/logd/main.c libc/include/mg/log_service.h \
                    libc/include/mg/object.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/logd -c $< -o $@

$(LOGD): $(LOGD_DIR)/main.o $(USER_CRT) $(USER_LIBC) \
         $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(LOGD_DIR)/main.o $(USER_LIBC)

$(LOGV_DIR)/main.o: userspace/logv/main.c userspace/common/help.h \
                    libc/include/mg/log_service.h libc/include/mg/terminal.h \
                    $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/logv -Iuserspace/common -c $< -o $@

$(LOGV): $(LOGV_DIR)/main.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) \
         $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(LOGV_DIR)/main.o $(HELP_OBJ) $(USER_LIBC)

$(COMMAND_PATH_OBJ): userspace/common/path.c userspace/common/path.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(CLEAR_DIR)/clear.o: userspace/clear/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(CLEAR): $(CLEAR_DIR)/clear.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(CLEAR_DIR)/clear.o $(HELP_OBJ) $(USER_LIBC)

$(COPY_DIR)/main.o: userspace/copy/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(COPY): $(COPY_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(COPY_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(LIST_DIR)/main.o: userspace/list/main.c userspace/common/path.h \
                  userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(LIST): $(LIST_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(LIST_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(LOCATE_DIR)/locate.o: userspace/locate/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(LOCATE): $(LOCATE_DIR)/locate.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(LOCATE_DIR)/locate.o $(HELP_OBJ) $(USER_LIBC)

$(MOVE_DIR)/main.o: userspace/move/main.c userspace/common/path.h \
                  userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(MOVE): $(MOVE_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(MOVE_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(MKDIR_DIR)/main.o: userspace/mkdir/main.c userspace/common/path.h \
                     userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(MKDIR): $(MKDIR_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(MKDIR_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(PLANT_DIR)/plant.o: userspace/plant/main.c userspace/common/path.h \
                      userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(PLANT): $(PLANT_DIR)/plant.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(PLANT_DIR)/plant.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(TYPE_DIR)/type.o: userspace/type/main.c userspace/common/path.h \
                    userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(TYPE): $(TYPE_DIR)/type.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(TYPE_DIR)/type.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(WAIT_DIR)/wait.o: userspace/wait/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/wait -Iuserspace/common -c $< -o $@

$(WAIT): $(WAIT_DIR)/wait.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(WAIT_DIR)/wait.o $(HELP_OBJ) $(USER_LIBC)

$(REED_DIR)/%.o: userspace/reed/%.c userspace/reed/buffer.h \
                 userspace/reed/editor.h userspace/reed/config.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(REED_CFLAGS) -Iuserspace/reed -Iuserspace/common -c $< -o $@

$(BUILD_DIR)/userspace/app_directory.o: userspace/common/app_directory.c \
                                        userspace/common/app_directory.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(REED): $(REED_DIR)/main.o $(REED_DIR)/editor.o $(REED_DIR)/buffer.o \
         $(REED_DIR)/config.o \
         $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(BUILD_DIR)/userspace/app_directory.o \
         $(USER_CRT) $(USER_LIBC) \
         $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(REED_DIR)/main.o $(REED_DIR)/editor.o \
		$(REED_DIR)/buffer.o $(REED_DIR)/config.o $(HELP_OBJ) \
		$(COMMAND_PATH_OBJ) $(BUILD_DIR)/userspace/app_directory.o $(USER_LIBC)

$(DEL_DIR)/main.o: userspace/del/main.c userspace/common/path.h \
                   userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/common -c $< -o $@

$(DEL): $(DEL_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(DEL_DIR)/main.o $(HELP_OBJ) $(COMMAND_PATH_OBJ) $(USER_LIBC)

$(SAY_DIR)/say.o: userspace/say/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SAY): $(SAY_DIR)/say.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(SAY_DIR)/say.o $(HELP_OBJ) $(USER_LIBC)

$(SHUTDOWN_DIR)/shutdown.o: userspace/shutdown/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SHUTDOWN): $(SHUTDOWN_DIR)/shutdown.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(SHUTDOWN_DIR)/shutdown.o $(HELP_OBJ) $(USER_LIBC)

$(REBOOT_DIR)/reboot.o: userspace/reboot/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(REBOOT): $(REBOOT_DIR)/reboot.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(REBOOT_DIR)/reboot.o $(HELP_OBJ) $(USER_LIBC)

$(POWER_DIR)/power.o: userspace/power/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(POWER): $(POWER_DIR)/power.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(POWER_DIR)/power.o $(HELP_OBJ) $(USER_LIBC)

$(IDENTITY_DIR)/identity.o: userspace/identity/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(IDENTITY): $(IDENTITY_DIR)/identity.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(IDENTITY_DIR)/identity.o $(HELP_OBJ) $(USER_LIBC)

$(USER_CMD_DIR)/user.o: userspace/user/main.c userspace/common/help.h \
                        userspace/common/table.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_CMD): $(USER_CMD_DIR)/user.o $(BUILD_DIR)/userspace/secret_input.o \
             $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(USER_CMD_DIR)/user.o \
		$(BUILD_DIR)/userspace/secret_input.o $(HELP_OBJ) $(USER_LIBC)

$(UPTIME_DIR)/uptime.o: userspace/uptime/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(UPTIME): $(UPTIME_DIR)/uptime.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(UPTIME_DIR)/uptime.o $(HELP_OBJ) $(USER_LIBC)

$(DATE_DIR)/date.o: userspace/date/main.c userspace/common/help.h \
                    libc/include/mg/time.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(DATE): $(DATE_DIR)/date.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(DATE_DIR)/date.o $(HELP_OBJ) $(USER_LIBC)

$(VERSION_DIR)/version.o: userspace/version/main.c include/mangrove_version.h userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -I. -c $< -o $@

$(VERSION): $(VERSION_DIR)/version.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(VERSION_DIR)/version.o $(HELP_OBJ) $(USER_LIBC)

$(WHERE_DIR)/where.o: userspace/where/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(WHERE): $(WHERE_DIR)/where.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(WHERE_DIR)/where.o $(HELP_OBJ) $(USER_LIBC)

$(BUILD_DIR)/Hello/hello.o: userspace/hello/main.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(HELLO): $(BUILD_DIR)/Hello/hello.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(BUILD_DIR)/Hello/hello.o $(USER_LIBC)

$(BUILD_DIR)/FsTest/fstest.o: userspace/fstest/main.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(FSTEST_PROFILE_CFLAGS) -c $< -o $@

$(BUILD_DIR)/FsTest/volume.o: userspace/fstest/volume.c userspace/common/volume_client.h userspace/common/storage_snapshot.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(FSTEST_VOLUME_CFLAGS) -c $< -o $@

$(BUILD_DIR)/FsTest/safe.o: userspace/fstest/safe.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(FSTEST_PROFILE_CFLAGS) -c $< -o $@

$(BUILD_DIR)/FsTest/stress.o: userspace/fstest/stress.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/FsTest/stdio.o: userspace/fstest/stdio.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/FsTest/time.o: userspace/fstest/time.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/FsTest/math.o: userspace/fstest/math.c $(USER_LIBM) $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -frounding-math -c $< -o $@

FSTEST_VOLUME_OBJS := $(BUILD_DIR)/FsTest/stress.o $(BUILD_DIR)/FsTest/safe.o $(BUILD_DIR)/FsTest/volume.o $(BUILD_DIR)/FsTest/stdio.o $(BUILD_DIR)/FsTest/time.o $(BUILD_DIR)/FsTest/math.o $(BUILD_DIR)/userspace/volume_client.o $(BUILD_DIR)/userspace/storage_snapshot.o

$(FSTEST): $(BUILD_DIR)/FsTest/fstest.o $(FSTEST_VOLUME_OBJS) $(USER_CRT) $(USER_LIBM) $(USER_LIBC) $(USER_LIBGCC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(BUILD_DIR)/FsTest/fstest.o $(FSTEST_VOLUME_OBJS) $(USER_LIBM) $(USER_LIBC) $(USER_LIBGCC)

$(NETTEST_DIR)/nettest.o: userspace/nettest/main.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(NETTEST): $(NETTEST_DIR)/nettest.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(NETTEST_DIR)/nettest.o $(USER_LIBC)

$(PING_DIR)/main.o: userspace/ping/main.c userspace/ping/ping_args.h \
                    userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/ping -c $< -o $@

$(PING_DIR)/ping_args.o: userspace/ping/ping_args.c userspace/ping/ping_args.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/ping -c $< -o $@

$(PING): $(PING_DIR)/main.o $(PING_DIR)/ping_args.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(PING_DIR)/main.o $(PING_DIR)/ping_args.o $(HELP_OBJ) $(USER_LIBC)

$(RESOLVE_DIR)/main.o: userspace/resolve/main.c userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(RESOLVE): $(RESOLVE_DIR)/main.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(RESOLVE_DIR)/main.o $(HELP_OBJ) $(USER_LIBC)

$(FETCH_DIR)/main.o: userspace/fetch/main.c userspace/fetch/fetch_url.h \
                    userspace/common/help.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/fetch -Wframe-larger-than=16384 -Werror -c $< -o $@

$(FETCH_DIR)/fetch_url.o: userspace/fetch/fetch_url.c userspace/fetch/fetch_url.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/fetch -c $< -o $@

$(FETCH): $(FETCH_DIR)/main.o $(FETCH_DIR)/fetch_url.o $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(FETCH_DIR)/main.o $(FETCH_DIR)/fetch_url.o $(HELP_OBJ) $(USER_LIBC)


$(NETINFO_DIR)/main.o: userspace/netinfo/main.c \
                       userspace/common/network_client.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/netinfo -Iuserspace/common -c $< -o $@


$(NETCFG_DIR)/main.o: userspace/netcfg/main.c \
                      userspace/common/network_client.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/netcfg -Iuserspace/common -c $< -o $@

$(NETINFO): $(NETINFO_DIR)/main.o $(BUILD_DIR)/userspace/network_client.o \
            $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(NETINFO_DIR)/main.o \
		$(BUILD_DIR)/userspace/network_client.o $(HELP_OBJ) $(USER_LIBC)

$(NETCFG): $(NETCFG_DIR)/main.o $(BUILD_DIR)/userspace/network_client.o \
           $(HELP_OBJ) $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(NETCFG_DIR)/main.o \
		$(BUILD_DIR)/userspace/network_client.o $(HELP_OBJ) $(USER_LIBC)

$(USER_LIBC_DIR)/syscall.o: libc/src/mangrove_syscall.s
	@mkdir -p $(dir $@)
	$(ELF_AS) -m64 -mno-red-zone -c $< -o $@

$(BUILD_DIR)/libm/%.o: libc/src/math/%.c libc/src/math/math_private.h \
                       libc/include/math.h libc/include/fenv.h \
                       libc/include/complex.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -O2 -frounding-math -c $< -o $@

$(USER_LIBM): $(LIBM_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(USER_LIBC_DIR)/syscall_c.o: libc/src/mangrove_syscall.c libc/include/mangrove.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/string.o: libc/src/string.c libc/include/string.h libc/include/stdint.h libc/include/stddef.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/strtok.o: libc/src/strtok.c libc/include/string.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/utf8.o: libc/src/utf8.c include/mg/utf8.h include/types.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/wchar.o: libc/src/wchar.c libc/include/wchar.h libc/include/stdlib.h \
		libc/include/errno.h libc/include/limits.h libc/include/stdio.h \
		libc/include/stdint.h libc/include/string.h include/mg/utf8.h include/types.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC): $(USER_LIBC_DIR)/syscall.o $(USER_LIBC_DIR)/syscall_c.o $(USER_LIBC_DIR)/string.o $(USER_LIBC_DIR)/strtok.o $(USER_LIBC_DIR)/utf8.o $(USER_LIBC_DIR)/wchar.o $(USER_LIBC_DIR)/allocator.o $(USER_LIBC_DIR)/clipboard.o $(USER_LIBC_DIR)/stdio.o $(USER_LIBC_DIR)/stdio_stream.o $(USER_LIBC_DIR)/floatconv.o $(USER_LIBC_DIR)/scanf.o $(USER_LIBC_DIR)/native.o $(USER_LIBC_DIR)/line_editor.o $(USER_LIBC_DIR)/net.o $(USER_LIBC_DIR)/time.o $(USER_LIBC_DIR)/time_convert.o $(USER_LIBC_DIR)/time_standard.o $(USER_LIBC_DIR)/log.o $(USER_LIBC_DIR)/ctype.o $(USER_LIBC_DIR)/errno.o $(USER_LIBC_DIR)/error.o $(USER_LIBC_DIR)/exit.o $(USER_LIBC_DIR)/stdlib.o $(USER_LIBC_DIR)/assert.o
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^


$(USER_LIBC_DIR)/ctype.o: libc/src/ctype.c libc/include/ctype.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/errno.o: libc/src/errno.c libc/include/errno.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/error.o: libc/src/error.c libc/include/errno.h libc/include/mg/error.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/exit.o: libc/src/exit.c libc/include/stdlib.h libc/include/mg/process.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/stdlib.o: libc/src/stdlib.c libc/include/stdlib.h libc/include/ctype.h libc/include/errno.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/assert.o: libc/src/assert.c libc/include/assert.h libc/include/stdio.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/allocator.o: libc/src/allocator.c libc/include/stdlib.h libc/include/mangrove.h libc/include/string.h libc/include/errno.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/stdio.o: libc/src/stdio.c libc/src/floatconv.h libc/include/stdio.h libc/include/mangrove.h libc/include/string.h libc/include/errno.h libc/include/limits.h libc/include/stdint.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/stdio_stream.o: libc/src/stdio_stream.c libc/include/stdio.h libc/include/mangrove.h libc/include/mg/filesystem.h libc/include/mg/object.h libc/include/errno.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/floatconv.o: libc/src/floatconv.c libc/src/floatconv.h libc/include/float.h libc/include/limits.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/scanf.o: libc/src/scanf.c libc/include/stdio.h libc/include/stdlib.h libc/include/ctype.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/native.o: libc/src/native.c libc/include/mangrove.h libc/include/string.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/line_editor.o: libc/src/line_editor.c libc/include/mg/line_editor.h libc/include/mg/object.h libc/include/mg/terminal.h libc/include/string.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/net.o: libc/src/net.c libc/include/mg/net.h libc/include/mangrove_errors.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/log.o: libc/src/log.c libc/include/mg/log_service.h libc/include/mangrove.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/time.o: libc/src/time.c libc/include/mg/time.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/time_convert.o: libc/src/time_convert.c libc/include/mg/time.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIBC_DIR)/time_standard.o: libc/src/time_standard.c libc/include/time.h libc/include/mg/time.h libc/include/mg/error.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_CRT): libc/crt/crt0.s
	@mkdir -p $(dir $@)
	$(ELF_AS) -m64 -mno-red-zone -c $< -o $@

$(SHOOT_DIR)/%.o: userspace/shoot/%.c $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SHOOT): $(SHOOT_OBJS) $(HELP_OBJ) $(COMMAND_PATH_OBJ) \
          $(BUILD_DIR)/userspace/app_directory.o $(USER_CRT) $(USER_LIBC) \
          $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ \
		$(USER_CRT) $(SHOOT_OBJS) $(HELP_OBJ) $(COMMAND_PATH_OBJ) \
		$(BUILD_DIR)/userspace/app_directory.o $(USER_LIBC)

$(BUILD_DIR)/mgfsck: tools/mgfsck.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Wno-parentheses -Wno-unused-parameter -O2 $< -o $@

# Bootloader Compilation
$(BUILD_DIR)/boot/%.o: boot/src/%.c
	@mkdir -p $(dir $@)
	$(BOOT_CC) $(BOOT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/boot/%.o: boot/src/%.s
	@mkdir -p $(dir $@)
	$(BOOT_AS) $(BOOT_ASFLAGS) -c $< -o $@

# Kernel Compilation
$(BUILD_DIR)/kernel/time_convert.o: libc/src/time_convert.c libc/include/mg/time.h
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# PBKDF2 is CPU intensive; optimize this translation unit while keeping the
# kernel's existing freestanding and register restrictions.
$(BUILD_DIR)/kernel/password.o: private KERNEL_CFLAGS += -O2

$(BUILD_DIR)/kernel/%.o: kernel/src/%.c $(RTC_UTC_TRUST_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) $(if $(filter 1,$(RTC_UTC_TRUST)),-DRTC_UTC_TRUST=1,) -c $< -o $@

$(BUILD_DIR)/kernel/%.o: kernel/src/%.s
	@mkdir -p $(dir $@)
	$(ELF_AS) $(KERNEL_ASFLAGS) -c $< -o $@

# Upstream UNSCII conversion and font blob compilation
font: $(FONT_ASSET)

$(FONT_ASSET): $(FONT_SOURCE) $(FONT_CONVERTER)
	python3 $(FONT_CONVERTER) --source $(FONT_SOURCE) --output $@

$(BUILD_DIR)/kernel/font_blob.o: $(FONT_ASSET)
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386 $< $@

# Libc Compilation
$(BUILD_DIR)/libc/%.o: libc/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

-include $(DEPS) $(USER_DEPS)

# Explicitly opt in: isolated UTC-trusted QEMU build, firmware variables and
# media. No canonical image target is invoked and fstest is absent from normal
# payloads.
GUEST_MGFS_BUILD ?= /tmp/mangrove-guest-build
GUEST_MGFS_OUTPUT ?=
GUEST_MGFS_ACCEL ?= $(NATIVE_GUEST_ACCEL)
.PHONY: test-guest-mgfs
test-guest-mgfs:
	$(QEMU_ACCEL_PREFLIGHT) "$(GUEST_MGFS_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(GUEST_MGFS_BUILD)" RTC_UTC_TRUST=1 binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_mgfs_test.py --build-dir "$(GUEST_MGFS_BUILD)" $(if $(GUEST_MGFS_OUTPUT),--output "$(GUEST_MGFS_OUTPUT)") --accel "$(GUEST_MGFS_ACCEL)" --repetitions 2 --negative-control

# Bounded shutdown/reboot/readback guest lifecycle for active Shoot history.
GUEST_SHUTDOWN_BUILD ?= /tmp/mangrove-shutdown-build
GUEST_SHUTDOWN_OUTPUT ?=
GUEST_SHUTDOWN_ACCEL ?= $(NATIVE_GUEST_ACCEL)
.PHONY: test-guest-shutdown
test-guest-shutdown:
	$(QEMU_ACCEL_PREFLIGHT) "$(GUEST_SHUTDOWN_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(GUEST_SHUTDOWN_BUILD)" RTC_UTC_TRUST=1 binaries mkmgfs mgfsck
	python3 tests/guest/guest_mgfs_test.py --build-dir "$(GUEST_SHUTDOWN_BUILD)" $(if $(GUEST_SHUTDOWN_OUTPUT),--output "$(GUEST_SHUTDOWN_OUTPUT)") --accel "$(GUEST_SHUTDOWN_ACCEL)" --cpus 2 --power-history-smoke

# Disposable secondary USB volumes; explicit UTC trust applies only to these guests.
GUEST_SECONDARY_BUILD ?= /tmp/mangrove-secondary-build
GUEST_SECONDARY_OUTPUT ?=
GUEST_SECONDARY_ACCEL ?= $(NATIVE_GUEST_ACCEL)
GUEST_SECONDARY_REPETITIONS ?= 2
.PHONY: test-guest-secondary
test-guest-secondary:
	$(QEMU_ACCEL_PREFLIGHT) "$(GUEST_SECONDARY_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(GUEST_SECONDARY_BUILD)" RTC_UTC_TRUST=1 binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_secondary_test.py --build-dir "$(GUEST_SECONDARY_BUILD)" $(if $(GUEST_SECONDARY_OUTPUT),--output "$(GUEST_SECONDARY_OUTPUT)") --accel "$(GUEST_SECONDARY_ACCEL)" --repetitions "$(GUEST_SECONDARY_REPETITIONS)"

# Idle physical unplug/reattach, with explicit deferred-lifetime exclusions.
GUEST_HOTPLUG_BUILD ?= $(abspath $(BUILD_DIR))/guest-hotplug-build
GUEST_HOTPLUG_OUTPUT ?=
.PHONY: test-guest-hotplug
test-guest-hotplug:
	$(QEMU_ACCEL_PREFLIGHT) "$(GUEST_SECONDARY_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(GUEST_HOTPLUG_BUILD)" RTC_UTC_TRUST=1 binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_secondary_test.py --hotplug --hotplug-negative-control --backend mgfs --repetitions 2 --build-dir "$(GUEST_HOTPLUG_BUILD)" $(if $(GUEST_HOTPLUG_OUTPUT),--output "$(GUEST_HOTPLUG_OUTPUT)/idle") --accel "$(GUEST_SECONDARY_ACCEL)"
	python3 tests/guest/guest_secondary_test.py --hotplug --hotplug-handles --backend mgfs --repetitions 2 --build-dir "$(GUEST_HOTPLUG_BUILD)" $(if $(GUEST_HOTPLUG_OUTPUT),--output "$(GUEST_HOTPLUG_OUTPUT)/references") --accel "$(GUEST_SECONDARY_ACCEL)"

# Actual native safe-file/Reed/Shoot paths on isolated disposable USB volumes.
GUEST_SAFE_BUILD ?= $(abspath $(BUILD_DIR))/guest-safe-build
GUEST_SAFE_OUTPUT ?=
.PHONY: test-guest-safe
test-guest-safe:
	$(QEMU_ACCEL_PREFLIGHT) "$(GUEST_SECONDARY_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(GUEST_SAFE_BUILD)" RTC_UTC_TRUST=1 binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_safe_test.py --build-dir "$(GUEST_SAFE_BUILD)" $(if $(GUEST_SAFE_OUTPUT),--output "$(GUEST_SAFE_OUTPUT)/applications") --accel "$(GUEST_SECONDARY_ACCEL)" --repetitions 2 --readonly --negative-control --faults
	python3 tests/guest/guest_lifetime_test.py --build-dir "$(GUEST_SAFE_BUILD)" $(if $(GUEST_SAFE_OUTPUT),--output "$(GUEST_SAFE_OUTPUT)/stage-write") --accel "$(GUEST_SECONDARY_ACCEL)" --mutation replace --kind write
	python3 tests/guest/guest_lifetime_test.py --build-dir "$(GUEST_SAFE_BUILD)" $(if $(GUEST_SAFE_OUTPUT),--output "$(GUEST_SAFE_OUTPUT)/stage-flush") --accel "$(GUEST_SECONDARY_ACCEL)" --mutation replace --kind flush
	python3 tests/guest/guest_lifetime_test.py --build-dir "$(GUEST_SAFE_BUILD)" $(if $(GUEST_SAFE_OUTPUT),--output "$(GUEST_SAFE_OUTPUT)/publication") --accel "$(GUEST_SECONDARY_ACCEL)" --mutation replace --kind write --replacement-phase publication
	python3 tests/guest/guest_lifetime_test.py --build-dir "$(GUEST_SAFE_BUILD)" $(if $(GUEST_SAFE_OUTPUT),--output "$(GUEST_SAFE_OUTPUT)/cleanup") --accel "$(GUEST_SECONDARY_ACCEL)" --mutation replace --kind write --replacement-phase cleanup
	python3 tests/guest/guest_lifetime_test.py --build-dir "$(GUEST_SAFE_BUILD)" $(if $(GUEST_SAFE_OUTPUT),--output "$(GUEST_SAFE_OUTPUT)/missing-fault") --accel "$(GUEST_SECONDARY_ACCEL)" --mutation replace --kind write --negative-control

# Native Reed edit/save/restore without the host UDP rendezvous used only by
# the separate stale-save conflict workflow.
GUEST_REED_BUILD ?= $(abspath $(BUILD_DIR))/guest-reed-build
GUEST_REED_ACCEL ?= $(NATIVE_GUEST_ACCEL)
GUEST_REED_TIMEOUT ?= 300
.PHONY: test-guest-reed
test-guest-reed:
	$(QEMU_ACCEL_PREFLIGHT) "$(GUEST_REED_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(GUEST_REED_BUILD)" \
		KERNEL_HYBRID_ALLOCATOR=1 FSTEST_REED_NO_CONFLICT=1 RTC_UTC_TRUST=1 \
		binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_safe_test.py --build-dir "$(GUEST_REED_BUILD)" \
		--backend mgfs --repetitions 1 --readonly --reed-no-conflict --keyboard-repeat \
		--cpus 2 --timeout "$(GUEST_REED_TIMEOUT)" --accel "$(GUEST_REED_ACCEL)"

# Native stale-save qualification uses a bounded guest-side file rendezvous:
# fstest replaces the opened file through VFS before Reed enters its screen.
GUEST_REED_CONFLICT_BUILD ?= $(abspath $(BUILD_DIR))/guest-reed-conflict-build
.PHONY: test-guest-reed-conflict
test-guest-reed-conflict:
	$(QEMU_ACCEL_PREFLIGHT) "$(GUEST_REED_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(GUEST_REED_CONFLICT_BUILD)" \
		KERNEL_HYBRID_ALLOCATOR=1 FSTEST_REED_CONFLICT_SYNC=1 \
		REED_GUEST_CONFLICT_SYNC=1 RTC_UTC_TRUST=1 \
		binaries fstest mkmgfs mgfsck
	python3 tests/guest/guest_safe_test.py --build-dir "$(GUEST_REED_CONFLICT_BUILD)" \
		--backend mgfs --repetitions 1 --cpus 2 --timeout "$(GUEST_REED_TIMEOUT)" \
		--accel "$(GUEST_REED_ACCEL)" --reed-guest-conflict-sync

# Standalone Ring 3 model-based workloads; test binaries remain opt-in.
GUEST_STRESS_BUILD ?= $(abspath $(BUILD_DIR))/guest-stress-build
GUEST_STRESS_OUTPUT ?=
.PHONY: test-guest-stress
test-guest-stress:
	$(QEMU_ACCEL_PREFLIGHT) "$(GUEST_SECONDARY_ACCEL)"
	$(MAKE) -B -j4 BUILD_DIR="$(GUEST_STRESS_BUILD)" RTC_UTC_TRUST=1 binaries fstest fsstress mkmgfs mgfsck
	$(MAKE) BUILD_DIR="$(GUEST_STRESS_BUILD)" fsstress-host
	python3 tests/guest/guest_stress_test.py --build-dir "$(GUEST_STRESS_BUILD)" --host "$(GUEST_STRESS_BUILD)/fsstress/fsstress-host" $(if $(GUEST_STRESS_OUTPUT),--output "$(GUEST_STRESS_OUTPUT)") --accel "$(GUEST_SECONDARY_ACCEL)" --controls --extra-controls

.PHONY: clipd
clipd: $(CLIPD)

$(CLIPD_DIR)/%.o: userspace/clipd/%.c userspace/clipd/clipboard.h libc/include/mg/clipboard.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(CLIPD): $(CLIPD_DIR)/main.o $(CLIPD_DIR)/clipboard.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ $(USER_CRT) $(CLIPD_DIR)/main.o $(CLIPD_DIR)/clipboard.o $(USER_LIBC)

$(USER_LIBC_DIR)/clipboard.o: libc/src/clipboard.c libc/include/mg/clipboard.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

.PHONY: test-clipboard
test-clipboard: $(MKMGFS) $(BUILD_DIR)/mgfsck
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin -fsanitize=address,undefined \
		-ffunction-sections -fdata-sections -Iinclude -idirafter libc/include tests/userspace/clipboard_test.c \
		userspace/clipd/clipboard.c libc/src/clipboard.c libc/src/utf8.c \
		$(HOST_REED_TEST_FLAGS) -o /tmp/mangrove-clipboard-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-clipboard-test
	python3 tests/userspace/clipboard_image_test.py $(MKMGFS) $(BUILD_DIR)/mgfsck

.PHONY: cliptest
cliptest: $(BUILD_DIR)/cliptest/cliptest.elf
$(BUILD_DIR)/cliptest/main.o: userspace/cliptest/main.c libc/include/mg/clipboard.h $(USER_LIBC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD_DIR)/cliptest/cliptest.elf: $(BUILD_DIR)/cliptest/main.o $(USER_CRT) $(USER_LIBC) $(USER_LINKER_SCRIPT)
	$(LD_KERNEL) -z max-page-size=0x1000 -T $(USER_LINKER_SCRIPT) -o $@ $(USER_CRT) $(BUILD_DIR)/cliptest/main.o $(USER_LIBC)

.PHONY: test-reed-range-edit
test-reed: test-reed-range-edit
test-reed-range-edit:
	$(HOST_CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin -fsanitize=address,undefined \
		-Iinclude -idirafter libc/include tests/userspace/reed_range_edit_test.c \
		libc/src/utf8.c -o /tmp/mangrove-reed-range-edit-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/mangrove-reed-range-edit-test

GUEST_CLIPBOARD_BUILD ?= $(abspath $(BUILD_DIR))/guest-clipboard-build
GUEST_CLIPBOARD_OUTPUT ?=
GUEST_CLIPBOARD_ACCEL ?= $(NATIVE_GUEST_ACCEL)
.PHONY: test-guest-clipboard
test-guest-clipboard:
	$(QEMU_ACCEL_PREFLIGHT) "$(GUEST_CLIPBOARD_ACCEL)"
	$(MAKE) -j4 BUILD_DIR="$(GUEST_CLIPBOARD_BUILD)" RTC_UTC_TRUST=1 binaries cliptest mkmgfs mgfsck
	python3 tests/guest/guest_clipboard_test.py --build-dir "$(GUEST_CLIPBOARD_BUILD)" \
		$(if $(GUEST_CLIPBOARD_OUTPUT),--output "$(GUEST_CLIPBOARD_OUTPUT)") --accel "$(GUEST_CLIPBOARD_ACCEL)" \
		--ovmf-code "$(OVMF_CODE_SOURCE)" --ovmf-vars "$(OVMF_VARS_SOURCE)"

.PHONY: test-large-safe-files test-reed-large
test-large-safe-files:
	python3 tests/filesystems/large_safe_file_test.py "$(HOST_CC)"
test-reed-large:
	ASAN_OPTIONS=detect_leaks=0 python3 tests/userspace/reed_large_document_test.py "$(HOST_CC)"

test-sync: test-large-safe-files
test-reed: test-reed-large
