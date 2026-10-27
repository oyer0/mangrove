/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <net/e1000.h>
#include <net/net.h>

#include <acpi.h>
#include <address_layout.h>
#include <cpu.h>
#include <ioapic.h>
#include <irq.h>
#include <kprint.h>
#include <lapic.h>
#include <pci.h>
#include <pmm.h>
#include <stddef.h>
#include <timer.h>
#include <vmm.h>
#include <string.h>
#include <spinlock.h>

/* Intel 8254x / QEMU e1000 register set.  This driver deliberately uses the
 * legacy descriptor format implemented by QEMU's e1000 device. */
#define E1000_VENDOR_ID             0x8086U
#define E1000_DEVICE_82540EM        0x100EU
#define E1000_DEVICE_82545EM        0x100FU

#define E1000_MMIO_SIZE             0x20000U
#define E1000_RX_RING_COUNT         64U
#define E1000_TX_RING_COUNT         64U
#define E1000_BUFFER_SIZE           2048U
#define E1000_RESET_TIMEOUT_US      100000U
#define E1000_EEPROM_TIMEOUT_US     100000U

#define E1000_REG_CTRL              0x0000U
#define E1000_REG_STATUS            0x0008U
#define E1000_REG_EERD              0x0014U
#define E1000_REG_ICR               0x00C0U
#define E1000_REG_IMS               0x00D0U
#define E1000_REG_IMC               0x00D8U
#define E1000_REG_RCTL              0x0100U
#define E1000_REG_TCTL              0x0400U
#define E1000_REG_TIPG              0x0410U
#define E1000_REG_RDBAL             0x2800U
#define E1000_REG_RDBAH             0x2804U
#define E1000_REG_RDLEN             0x2808U
#define E1000_REG_RDH               0x2810U
#define E1000_REG_RDT               0x2818U
#define E1000_REG_TDBAL             0x3800U
#define E1000_REG_TDBAH             0x3804U
#define E1000_REG_TDLEN             0x3808U
#define E1000_REG_TDH               0x3810U
#define E1000_REG_TDT               0x3818U
#define E1000_REG_RAL               0x5400U
#define E1000_REG_RAH               0x5404U

#define E1000_CTRL_SLU              (1U << 6)
#define E1000_CTRL_ASDE             (1U << 5)
#define E1000_CTRL_RST              (1U << 26)
#define E1000_STATUS_LU             (1U << 1)

#define E1000_EERD_START            (1U << 0)
#define E1000_EERD_DONE             (1U << 4)
#define E1000_EERD_ADDRESS_SHIFT    8U
#define E1000_EERD_DATA_SHIFT       16U

#define E1000_RCTL_EN               (1U << 1)
#define E1000_RCTL_UPE              (1U << 3)
#define E1000_RCTL_MPE              (1U << 4)
#define E1000_RCTL_BAM              (1U << 15)
#define E1000_RCTL_SECRC            (1U << 26)

#define E1000_TCTL_EN               (1U << 1)
#define E1000_TCTL_PSLU             (1U << 3)
#define E1000_TCTL_CT_SHIFT         4U
#define E1000_TCTL_COLD_SHIFT       12U
#define E1000_TCTL_CT_VALUE         0x10U
#define E1000_TCTL_COLD_VALUE       0x40U
#define E1000_TIPG_DEFAULT          0x0060200AU

#define E1000_INT_TXDW              (1U << 0)
#define E1000_INT_LSC               (1U << 2)
#define E1000_INT_RXDMT0            (1U << 4)
#define E1000_INT_RXT0              (1U << 7)

#define E1000_RX_STATUS_DD          (1U << 0)
#define E1000_RX_STATUS_EOP         (1U << 1)
#define E1000_TX_CMD_EOP            (1U << 0)
#define E1000_TX_CMD_IFCS           (1U << 1)
#define E1000_TX_CMD_RS             (1U << 3)
#define E1000_TX_STATUS_DD          (1U << 0)

typedef struct {
    u64 address;
    u16 length;
    u16 checksum;
    u8 status;
    u8 errors;
    u16 special;
} __attribute__((packed)) e1000_rx_descriptor_t;

typedef struct {
    u64 address;
    u16 length;
    u8 cso;
    u8 command;
    u8 status;
    u8 css;
    u16 special;
} __attribute__((packed)) e1000_tx_descriptor_t;

_Static_assert(sizeof(e1000_rx_descriptor_t) == 16,
               "E1000 RX descriptor must be 16 bytes");
_Static_assert(sizeof(e1000_tx_descriptor_t) == 16,
               "E1000 TX descriptor must be 16 bytes");
_Static_assert(E1000_RX_RING_COUNT * sizeof(e1000_rx_descriptor_t) <= PAGE_SIZE,
               "E1000 RX ring must fit one physical frame");
_Static_assert(E1000_TX_RING_COUNT * sizeof(e1000_tx_descriptor_t) <= PAGE_SIZE,
               "E1000 TX ring must fit one physical frame");

typedef struct {
    irq_gate_t irq_gate;
    pci_device_t pci_snapshot;
    const pci_device_t *pci;
    bool pci_valid;
    volatile u8 *mmio;
    u8 irq;
    bool irq_enabled;
    bool active;

    net_device_t device;

    phys_addr_t rx_ring_phys;
    e1000_rx_descriptor_t *rx_ring;
    phys_addr_t rx_buffer_phys[E1000_RX_RING_COUNT];
    u8 *rx_buffer[E1000_RX_RING_COUNT];
    u32 rx_next;

    phys_addr_t tx_ring_phys;
    e1000_tx_descriptor_t *tx_ring;
    phys_addr_t tx_buffer_phys[E1000_TX_RING_COUNT];
    u8 *tx_buffer[E1000_TX_RING_COUNT];
    u32 tx_producer;
    u32 tx_consumer;
    spinlock_t tx_lock;

    u64 received_frames;
    u64 transmitted_frames;
} e1000_state_t;

#define E1000_MAX_CONTROLLERS NET_MAX_DEVICES

static e1000_state_t controllers[E1000_MAX_CONTROLLERS];
static bool e1000_irq_registered;
static volatile bool e1000_probe_in_progress;

/* Controller state is passed explicitly.  The device IRQ is routed to the
 * BSP, but transmit and lifecycle work may run on other CPUs; a shared
 * "current controller" pointer would let those paths change IRQ context. */

static inline void e1000_compiler_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline void e1000_pause(void)
{
    __asm__ volatile("pause");
}

static void e1000_state_reset(e1000_state_t *state)
{
    if (!state) return;
    /* The ISR may still probe the closed gate while walking the static
     * controller array. Keep that word atomic and clear the rest only after
     * any admitted per-controller service has drained. */
    irq_gate_reset(&state->irq_gate);
    memset((u8 *)state + sizeof(state->irq_gate), 0,
           sizeof(*state) - sizeof(state->irq_gate));
}

static void e1000_irq_quiesce(e1000_state_t *state)
{
    if (!state) return;
    if (irq_gate_begin_close(&state->irq_gate)) {
        irq_gate_wait_drained(&state->irq_gate);
    } else if (irq_gate_is_closing(&state->irq_gate)) {
        irq_gate_wait_drained(&state->irq_gate);
    }
}

static u64 e1000_irq_save(void)
{
#ifdef E1000_HOST_TEST
    return 0;
#else
    u64 flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
#endif
}

static void e1000_irq_restore(u64 flags)
{
#ifdef E1000_HOST_TEST
    (void)flags;
#else
    if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
#endif
}

static inline u32 e1000_read(const e1000_state_t *state, u32 reg)
{
    return *(volatile u32 *)(state->mmio + reg);
}

static inline void e1000_write(const e1000_state_t *state, u32 reg, u32 value)
{
    *(volatile u32 *)(state->mmio + reg) = value;
}

static bool e1000_supported(const pci_device_t *device)
{
    return device && device->vendor_id == E1000_VENDOR_ID &&
           (device->device_id == E1000_DEVICE_82540EM ||
            device->device_id == E1000_DEVICE_82545EM);
}

static bool e1000_allocate_frame(phys_addr_t *phys, void **virt)
{
    if (!phys || !virt) return false;

    *phys = pmm_alloc_frame();
    if (!*phys) return false;
    *virt = phys_to_virt(*phys);
    if (!*virt) {
        pmm_free_frame(*phys);
        *phys = 0;
        return false;
    }
    return true;
}

static bool e1000_reset(e1000_state_t *state)
{
    timer_monotonic_deadline_t deadline;

    e1000_write(state, E1000_REG_IMC, 0xFFFFFFFFU);
    (void)e1000_read(state, E1000_REG_ICR);
    e1000_write(state, E1000_REG_CTRL,
                e1000_read(state, E1000_REG_CTRL) | E1000_CTRL_RST);

    if (!timer_monotonic_deadline_start(&deadline, E1000_RESET_TIMEOUT_US))
        return false;
    for (;;) {
        if (!(e1000_read(state, E1000_REG_CTRL) & E1000_CTRL_RST)) {
            return true;
        }
        if (timer_monotonic_deadline_expired(&deadline))
            break;
        e1000_pause();
    }
    return false;
}

static bool e1000_read_eeprom_word(e1000_state_t *state, u8 word, u16 *value)
{
    u32 request;
    timer_monotonic_deadline_t deadline;

    if (!value) return false;
    request = E1000_EERD_START | ((u32)word << E1000_EERD_ADDRESS_SHIFT);
    e1000_write(state, E1000_REG_EERD, request);
    if (!timer_monotonic_deadline_start(&deadline, E1000_EEPROM_TIMEOUT_US))
        return false;
    for (;;) {
        u32 response = e1000_read(state, E1000_REG_EERD);
        if (response & E1000_EERD_DONE) {
            *value = (u16)(response >> E1000_EERD_DATA_SHIFT);
            return true;
        }
        if (timer_monotonic_deadline_expired(&deadline))
            break;
        e1000_pause();
    }
    return false;
}

static bool e1000_read_mac(e1000_state_t *state, u8 mac[6])
{
    u32 ral = e1000_read(state, E1000_REG_RAL);
    u32 rah = e1000_read(state, E1000_REG_RAH);
    bool all_zero = true;
    bool all_ff = true;

    mac[0] = (u8)(ral >> 0);
    mac[1] = (u8)(ral >> 8);
    mac[2] = (u8)(ral >> 16);
    mac[3] = (u8)(ral >> 24);
    mac[4] = (u8)(rah >> 0);
    mac[5] = (u8)(rah >> 8);

    for (u32 i = 0; i < 6; i++) {
        all_zero &= mac[i] == 0;
        all_ff &= mac[i] == 0xFF;
    }
    if (!all_zero && !all_ff) return true;

    for (u8 word = 0; word < 3; word++) {
        u16 value;
        if (!e1000_read_eeprom_word(state, word, &value)) return false;
        mac[word * 2] = (u8)value;
        mac[word * 2 + 1] = (u8)(value >> 8);
    }
    return true;
}

static bool e1000_setup_rx(e1000_state_t *state)
{
    void *ring_virt;

    if (!e1000_allocate_frame(&state->rx_ring_phys, &ring_virt)) return false;
    state->rx_ring = (e1000_rx_descriptor_t *)ring_virt;

    for (u32 i = 0; i < E1000_RX_RING_COUNT; i++) {
        void *buffer;
        if (!e1000_allocate_frame(&state->rx_buffer_phys[i], &buffer)) {
            return false;
        }
        state->rx_buffer[i] = (u8 *)buffer;
        state->rx_ring[i].address = state->rx_buffer_phys[i];
    }

    state->rx_next = 0;
    e1000_compiler_barrier();
    e1000_write(state, E1000_REG_RDBAL, (u32)state->rx_ring_phys);
    e1000_write(state, E1000_REG_RDBAH, (u32)(state->rx_ring_phys >> 32));
    e1000_write(state, E1000_REG_RDLEN,
                E1000_RX_RING_COUNT * sizeof(e1000_rx_descriptor_t));
    e1000_write(state, E1000_REG_RDH, 0);
    e1000_write(state, E1000_REG_RDT, E1000_RX_RING_COUNT - 1U);
    e1000_write(state, E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM |
                E1000_RCTL_SECRC | E1000_RCTL_UPE | E1000_RCTL_MPE);
    return true;
}

static bool e1000_setup_tx(e1000_state_t *state)
{
    void *ring_virt;

    if (!e1000_allocate_frame(&state->tx_ring_phys, &ring_virt)) return false;
    state->tx_ring = (e1000_tx_descriptor_t *)ring_virt;

    for (u32 i = 0; i < E1000_TX_RING_COUNT; i++) {
        void *buffer;
        if (!e1000_allocate_frame(&state->tx_buffer_phys[i], &buffer)) {
            return false;
        }
        state->tx_buffer[i] = (u8 *)buffer;
        state->tx_ring[i].address = state->tx_buffer_phys[i];
        state->tx_ring[i].status = E1000_TX_STATUS_DD;
    }

    state->tx_producer = 0;
    state->tx_consumer = 0;
    e1000_compiler_barrier();
    e1000_write(state, E1000_REG_TDBAL, (u32)state->tx_ring_phys);
    e1000_write(state, E1000_REG_TDBAH, (u32)(state->tx_ring_phys >> 32));
    e1000_write(state, E1000_REG_TDLEN,
                E1000_TX_RING_COUNT * sizeof(e1000_tx_descriptor_t));
    e1000_write(state, E1000_REG_TDH, 0);
    e1000_write(state, E1000_REG_TDT, 0);
    e1000_write(state, E1000_REG_TIPG, E1000_TIPG_DEFAULT);
    e1000_write(state, E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSLU |
                (E1000_TCTL_CT_VALUE << E1000_TCTL_CT_SHIFT) |
                (E1000_TCTL_COLD_VALUE << E1000_TCTL_COLD_SHIFT));
    return true;
}

/* Caller holds state->tx_lock. */
static void e1000_reclaim_tx(e1000_state_t *state)
{
    while (state->tx_consumer != state->tx_producer) {
        e1000_tx_descriptor_t *desc =
            &state->tx_ring[state->tx_consumer];
        e1000_compiler_barrier();
        if (!(desc->status & E1000_TX_STATUS_DD)) break;
        state->transmitted_frames++;
        state->tx_consumer =
            (state->tx_consumer + 1U) % E1000_TX_RING_COUNT;
    }
}

static void e1000_receive(e1000_state_t *state)
{
    u32 last_recycled = E1000_RX_RING_COUNT;

    while (state->rx_ring[state->rx_next].status & E1000_RX_STATUS_DD) {
        e1000_rx_descriptor_t *desc = &state->rx_ring[state->rx_next];
        u16 length = desc->length;
        u8 status = desc->status;
        u8 errors = desc->errors;

        e1000_compiler_barrier();
        if ((status & E1000_RX_STATUS_EOP) && !errors &&
            length >= NET_ETHERNET_HEADER_SIZE && length <= E1000_BUFFER_SIZE) {
            net_receive_frame(&state->device,
                              state->rx_buffer[state->rx_next], length);
            state->received_frames++;
        }

        desc->length = 0;
        desc->checksum = 0;
        desc->status = 0;
        desc->errors = 0;
        desc->special = 0;
        last_recycled = state->rx_next;
        state->rx_next = (state->rx_next + 1U) % E1000_RX_RING_COUNT;
    }

    if (last_recycled != E1000_RX_RING_COUNT) {
        e1000_compiler_barrier();
        e1000_write(state, E1000_REG_RDT, last_recycled);
    }
}

static bool e1000_transmit(net_device_t *device, const void *frame,
                           usize length)
{
    e1000_state_t *state;
    u64 flags;
    const u8 *source = (const u8 *)frame;
    e1000_tx_descriptor_t *desc;
    u32 index;
    usize wire_length;

    state = device ? (e1000_state_t *)device->driver_data : NULL;
    if (!state || device != &state->device ||
        !frame || length < NET_ETHERNET_HEADER_SIZE ||
        length > NET_ETHERNET_MAX_FRAME) {
        return false;
    }

    flags = spin_lock_irqsave(&state->tx_lock);
    if (!state->active || !state->mmio || !state->tx_ring) {
        spin_unlock_irqrestore(&state->tx_lock, flags);
        return false;
    }

    e1000_reclaim_tx(state);
    index = state->tx_producer;
    desc = &state->tx_ring[index];
    if (!(desc->status & E1000_TX_STATUS_DD)) {
        spin_unlock_irqrestore(&state->tx_lock, flags);
        return false;
    }

    for (usize i = 0; i < length; i++) {
        state->tx_buffer[index][i] = source[i];
    }
    wire_length = length < NET_ETHERNET_MIN_FRAME ? NET_ETHERNET_MIN_FRAME : length;
    for (usize i = length; i < wire_length; i++) {
        state->tx_buffer[index][i] = 0;
    }

    desc->length = (u16)wire_length;
    desc->cso = 0;
    desc->command = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    desc->css = 0;
    desc->special = 0;
    e1000_compiler_barrier();
    desc->status = 0;
    e1000_compiler_barrier();
    state->tx_producer = (index + 1U) % E1000_TX_RING_COUNT;
    e1000_write(state, E1000_REG_TDT, state->tx_producer);
    spin_unlock_irqrestore(&state->tx_lock, flags);
    return true;
}

static void e1000_service_irq(e1000_state_t *state)
{
    u32 causes;

    if (!state || !state->mmio) return;

    /* Reading ICR acknowledges device causes.  LAPIC EOI remains centralized
     * in the common APIC interrupt epilogue after this callback. */
    causes = e1000_read(state, E1000_REG_ICR);
    if (!causes) return;
    if (causes & E1000_INT_LSC)
        (void)net_device_set_link(&state->device, true,
                                  (e1000_read(state, E1000_REG_STATUS) &
                                   E1000_STATUS_LU) != 0);
    if (causes & (E1000_INT_RXT0 | E1000_INT_RXDMT0)) e1000_receive(state);
    if (causes & E1000_INT_TXDW) {
        u64 flags = spin_lock_irqsave(&state->tx_lock);
        e1000_reclaim_tx(state);
        spin_unlock_irqrestore(&state->tx_lock, flags);
    }
}

static void e1000_irq_handler(struct cpu_registers *regs)
{
    (void)regs;
    /* Probing suppresses service while controller slots are prepared; the
     * per-controller gates independently protect teardown lifetimes. */
    if (__atomic_load_n(&e1000_probe_in_progress, __ATOMIC_ACQUIRE)) return;
    for (u32 index = 0; index < E1000_MAX_CONTROLLERS; index++) {
        e1000_state_t *state = &controllers[index];
        if (!irq_gate_enter(&state->irq_gate)) continue;
        e1000_service_irq(state);
        irq_gate_leave(&state->irq_gate);
    }
}

static void e1000_send_boot_frame(e1000_state_t *state)
{
    u8 frame[NET_ETHERNET_MIN_FRAME] = {0};
    static const char marker[] = "Mangrove raw Ethernet";

    for (u32 i = 0; i < 6; i++) frame[i] = 0xFF;
    for (u32 i = 0; i < 6; i++) frame[6 + i] = state->device.mac[i];
    frame[12] = 0x88;
    frame[13] = 0xB5;
    for (u32 i = 0; i < sizeof(marker) - 1U && 14U + i < sizeof(frame); i++) {
        frame[14U + i] = marker[i];
    }
    (void)netdev_transmit(&state->device, frame, sizeof(frame));
}

static void e1000_release_buffers(e1000_state_t *state)
{
    if (!state) return;
    for (u32 index = 0; index < E1000_RX_RING_COUNT; index++) {
        if (state->rx_buffer_phys[index])
            pmm_free_frame(state->rx_buffer_phys[index]);
        state->rx_buffer_phys[index] = 0;
        state->rx_buffer[index] = NULL;
    }
    for (u32 index = 0; index < E1000_TX_RING_COUNT; index++) {
        if (state->tx_buffer_phys[index])
            pmm_free_frame(state->tx_buffer_phys[index]);
        state->tx_buffer_phys[index] = 0;
        state->tx_buffer[index] = NULL;
    }
    if (state->rx_ring_phys) pmm_free_frame(state->rx_ring_phys);
    if (state->tx_ring_phys) pmm_free_frame(state->tx_ring_phys);
    state->rx_ring_phys = 0;
    state->tx_ring_phys = 0;
    state->rx_ring = NULL;
    state->tx_ring = NULL;
}

static bool e1000_same_pci_instance(const e1000_state_t *state,
                                    const pci_device_t *device)
{
    return state && device && state->pci_valid && device->present &&
           state->pci_snapshot.generation == device->generation &&
           state->pci_snapshot.bus == device->bus &&
           state->pci_snapshot.device == device->device &&
           state->pci_snapshot.function == device->function;
}

static e1000_state_t *e1000_find_pci(const pci_device_t *device)
{
    for (u32 index = 0; index < E1000_MAX_CONTROLLERS; index++)
        if (controllers[index].active &&
            e1000_same_pci_instance(&controllers[index], device))
            return &controllers[index];
    return NULL;
}

static bool e1000_probe_device(const pci_device_t *device)
{
    e1000_state_t *state = NULL;
    pci_bar_t bar;
    bool registered = false;

    if (!device || !device->present || !e1000_supported(device)) return false;
    for (u32 index = 0; index < E1000_MAX_CONTROLLERS; index++) {
        if (!controllers[index].pci_valid) {
            state = &controllers[index];
            break;
        }
    }
    if (!state || !pci_enable_memory_busmaster(device)) return false;

    e1000_state_reset(state);
    spinlock_init(&state->tx_lock);
    state->pci_snapshot = *device;
    state->pci = &state->pci_snapshot;
    state->pci_valid = true;
    bar = pci_get_bar(state->pci, 0);
    if (bar.io || !bar.address) goto fail;
    state->mmio = (volatile u8 *)vmm_map_mmio((phys_addr_t)bar.address,
                                               E1000_MMIO_SIZE);
    if (!state->mmio ||
        !vmm_ioremap_contains((const void *)state->mmio)) {
        state->mmio = NULL;
        goto fail;
    }
    if (!e1000_reset(state) || !e1000_read_mac(state, state->device.mac) ||
        !e1000_setup_rx(state) || !e1000_setup_tx(state)) goto fail;

    state->device.name = "ethernet";
    state->device.mtu = 1500;
    state->device.transmit = e1000_transmit;
    state->device.driver_data = state;
    state->device.link_known = true;
    state->device.link_up =
        (e1000_read(state, E1000_REG_STATUS) & E1000_STATUS_LU) != 0;
    if (!net_register_device(&state->device)) goto fail;
    registered = true;

    state->irq = pci_read_config8(state->pci, 0x3C);
    if (state->irq < 16U) {
        if (!e1000_irq_registered) {
            if (irq_register_vector(IRQ_VECTOR_E1000, e1000_irq_handler))
                e1000_irq_registered = true;
        }
        cpu_local_t *bsp = cpu_by_index(0);
        if (bsp && bsp->bsp && bsp->online && e1000_irq_registered &&
            ioapic_route_gsi(acpi_irq_to_gsi(state->irq),
                             IRQ_VECTOR_E1000,
                             bsp->apic_id,
                             acpi_irq_flags(state->irq)))
            state->irq_enabled = true;
    }

    (void)e1000_read(state, E1000_REG_ICR);
    state->active = true;
    if (!irq_gate_open(&state->irq_gate)) goto fail;
    e1000_write(state, E1000_REG_IMS, E1000_INT_RXT0 | E1000_INT_RXDMT0 |
                E1000_INT_TXDW | E1000_INT_LSC);
    e1000_send_boot_frame(state);
    KERNEL_BOOT_DEBUG_LOG(
        "[OK] Ethernet controller active: e1000 %02x:%02x:%02x:%02x:%02x:%02x\n",
        state->device.mac[0], state->device.mac[1], state->device.mac[2],
        state->device.mac[3], state->device.mac[4], state->device.mac[5]);
    return true;

fail:
    if (state->mmio) e1000_write(state, E1000_REG_IMC, 0xFFFFFFFFU);
    e1000_irq_quiesce(state);
    if (registered) (void)net_unregister_device(&state->device);
    e1000_release_buffers(state);
    e1000_state_reset(state);
    return false;
}

static void e1000_detach(e1000_state_t *state, bool hardware_present)
{
    bool any_active = false;

    if (!state || !state->pci_valid) return;
    if (hardware_present && state->mmio)
        e1000_write(state, E1000_REG_IMC, 0xFFFFFFFFU);
    e1000_irq_quiesce(state);
    state->active = false;
    state->device.administrative_enabled = false;
    (void)net_unregister_device(&state->device);
    e1000_release_buffers(state);
    e1000_state_reset(state);
    for (u32 index = 0; index < E1000_MAX_CONTROLLERS; index++)
        any_active |= controllers[index].active;
    if (!any_active && e1000_irq_registered) {
        irq_unregister_vector(IRQ_VECTOR_E1000);
        e1000_irq_registered = false;
    }
}

static void e1000_poll_links(void)
{
    for (u32 index = 0; index < E1000_MAX_CONTROLLERS; index++) {
        e1000_state_t *state = &controllers[index];
        u64 flags;
        bool up;

        if (!state->active || !state->mmio) continue;
        flags = e1000_irq_save();
        up = (e1000_read(state, E1000_REG_STATUS) & E1000_STATUS_LU) != 0;
        (void)net_device_set_link(&state->device, true, up);
        e1000_irq_restore(flags);
    }
}

bool e1000_rescan(void)
{
    bool changed = false;

    for (u32 index = 0; index < E1000_MAX_CONTROLLERS; index++) {
        e1000_state_t *state = &controllers[index];
        bool present = false;

        if (!state->pci_valid) continue;
        for (u32 pci_index = 0; pci_index < pci_get_device_count();
             pci_index++) {
            const pci_device_t *candidate = pci_get_device(pci_index);
            if (candidate && e1000_same_pci_instance(state, candidate)) {
                present = true;
                break;
            }
        }
        if (!present) {
            e1000_detach(state, false);
            changed = true;
        }
    }

    for (u32 index = 0; index < pci_get_device_count(); index++) {
        const pci_device_t *device = pci_get_device(index);
        if (!device || !e1000_supported(device) ||
            e1000_find_pci(device)) continue;
        __atomic_store_n(&e1000_probe_in_progress, true, __ATOMIC_RELEASE);
        if (e1000_probe_device(device)) changed = true;
        __atomic_store_n(&e1000_probe_in_progress, false, __ATOMIC_RELEASE);
    }
    e1000_poll_links();
    return changed;
}

bool e1000_init(void)
{
    bool initialized = false;

    memset(controllers, 0, sizeof(controllers));
    e1000_irq_registered = false;
    e1000_probe_in_progress = false;
    for (u32 index = 0; index < pci_get_device_count(); index++) {
        const pci_device_t *device = pci_get_device(index);
        if (!device || !e1000_supported(device)) continue;
        __atomic_store_n(&e1000_probe_in_progress, true, __ATOMIC_RELEASE);
        initialized |= e1000_probe_device(device);
        __atomic_store_n(&e1000_probe_in_progress, false, __ATOMIC_RELEASE);
    }
    return initialized;
}

u64 e1000_received_frames(void)
{
    u64 total = 0;
    for (u32 index = 0; index < E1000_MAX_CONTROLLERS; index++)
        total += controllers[index].received_frames;
    return total;
}

u64 e1000_transmitted_frames(void)
{
    u64 total = 0;
    for (u32 index = 0; index < E1000_MAX_CONTROLLERS; index++)
        total += controllers[index].transmitted_frames;
    return total;
}
