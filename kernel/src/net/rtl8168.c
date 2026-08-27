#include <net/rtl8168.h>

#include <address_layout.h>
#include <irq.h>
#include <kprint.h>
#include <lapic.h>
#include <net/net.h>
#include <pci.h>
#include <pmm.h>
#include <stddef.h>
#include <string.h>
#include <timer.h>
#include <vmm.h>

/*
 * This is an independent implementation of the documented RTL8168
 * descriptor/MMIO contract.  It intentionally recognizes only the Lenovo
 * target's RTL8168h/RTL8111h revision instead of guessing at the many other
 * chips which share PCI device ID 10ec:8168.
 */

#define RTL8168_VENDOR_ID                 0x10ECU
#define RTL8168_DEVICE_ID                 0x8168U
#define RTL8168_PCI_REVISION_H            0x15U
#define RTL8168_XID_MASK                  0xFCFU
#define RTL8168_XID_H                     0x541U
#define RTL8168_MMIO_BAR                  2U
#define RTL8168_MMIO_SIZE                 0x1000U

#define RTL8168_RX_RING_COUNT             64U
#define RTL8168_TX_RING_COUNT             64U
#define RTL8168_BUFFER_SIZE               2048U
#define RTL8168_RX_MAX_SIZE               RTL8168_BUFFER_SIZE
#define RTL8168_TX_MAX_SIZE_UNITS         0x0CU

#define RTL8168_IRQ_VECTOR                IRQ_VECTOR_RTL8168
#define RTL8168_MSIX_ENTRY                0U

#define RTL8168_RESET_TIMEOUT_US          100000U
#define RTL8168_QUIESCE_TIMEOUT_US        20000U
#define RTL8168_RXDV_GATE_SETTLE_US         2000U
#define RTL8168_RX_DISABLE_SETTLE_US        1000U
#define RTL8168_LINK_TIMEOUT_US           5000000U
#define RTL8168_PHY_STATUS_UPDATE_US       300U
#define RTL8168_PCI_D0_RECOVERY_US        10000U

#define RTL8168_REG_MAC0                  0x00U
#define RTL8168_REG_MAC4                  0x04U
#define RTL8168_REG_MULTICAST0            0x08U
#define RTL8168_REG_MULTICAST4            0x0CU
#define RTL8168_REG_TX_DESC_LOW           0x20U
#define RTL8168_REG_TX_DESC_HIGH          0x24U
#define RTL8168_REG_COMMAND               0x37U
#define RTL8168_REG_TX_POLL               0x38U
#define RTL8168_REG_INTERRUPT_MASK        0x3CU
#define RTL8168_REG_INTERRUPT_STATUS      0x3EU
#define RTL8168_REG_TX_CONFIG             0x40U
#define RTL8168_REG_RX_CONFIG             0x44U
#define RTL8168_REG_RX_MISSED             0x4CU
#define RTL8168_REG_PHY_ACCESS            0x60U
#define RTL8168_REG_PHY_STATUS            0x6CU
#define RTL8168_REG_MCU                   0xD3U
#define RTL8168_REG_RX_MAX_SIZE           0xDAU
#define RTL8168_REG_CPLUS_COMMAND         0xE0U
#define RTL8168_REG_INTERRUPT_MITIGATION  0xE2U
#define RTL8168_REG_RX_DESC_LOW           0xE4U
#define RTL8168_REG_RX_DESC_HIGH          0xE8U
#define RTL8168_REG_TX_MAX_SIZE           0xECU
#define RTL8168_REG_MISC                  0xF0U
#define RTL8168_REG_MAC_OCP               0xB0U

#define RTL8168_COMMAND_RESET             (1U << 4)
#define RTL8168_COMMAND_RX_ENABLE         (1U << 3)
#define RTL8168_COMMAND_TX_ENABLE         (1U << 2)

#define RTL8168_TX_POLL_NORMAL            (1U << 6)

#define RTL8168_INTERRUPT_SYSTEM_ERROR    (1U << 15)
#define RTL8168_INTERRUPT_LINK_CHANGE     (1U << 5)
#define RTL8168_INTERRUPT_RX_UNAVAILABLE  (1U << 4)
#define RTL8168_INTERRUPT_TX_ERROR        (1U << 3)
#define RTL8168_INTERRUPT_TX_OK           (1U << 2)
#define RTL8168_INTERRUPT_RX_ERROR        (1U << 1)
#define RTL8168_INTERRUPT_RX_OK           (1U << 0)
#define RTL8168_INTERRUPT_RUNTIME_MASK    \
    (RTL8168_INTERRUPT_SYSTEM_ERROR | RTL8168_INTERRUPT_LINK_CHANGE | \
     RTL8168_INTERRUPT_RX_UNAVAILABLE | RTL8168_INTERRUPT_TX_ERROR | \
     RTL8168_INTERRUPT_TX_OK | RTL8168_INTERRUPT_RX_ERROR | \
     RTL8168_INTERRUPT_RX_OK)

#define RTL8168_TX_CONFIG_IFG             (3U << 24)
#define RTL8168_TX_CONFIG_EMPTY           (1U << 11)
#define RTL8168_TX_CONFIG_DMA_BURST       (7U << 8)
#define RTL8168_TX_CONFIG_AUTO_FIFO       (1U << 7)

/* RTL8168h receive-fetch controls plus the ordinary address filters. */
#define RTL8168_RX_CONFIG_128_BYTE_IRQ     (1U << 15)
#define RTL8168_RX_CONFIG_MULTIPLE_FETCH  (1U << 14)
#define RTL8168_RX_CONFIG_EARLY_OFF       (1U << 11)
#define RTL8168_RX_CONFIG_DMA_BURST       (7U << 8)
#define RTL8168_RX_CONFIG_BROADCAST       (1U << 3)
#define RTL8168_RX_CONFIG_MULTICAST       (1U << 2)
#define RTL8168_RX_CONFIG_PHYSICAL        (1U << 1)

#define RTL8168_CPLUS_RX_VLAN             (1U << 6)
#define RTL8168_CPLUS_RX_CHECKSUM         (1U << 5)
#define RTL8168_CPLUS_INTERRUPT_TIMER     0x0003U

#define RTL8168_PHY_1000_FULL             (1U << 4)
#define RTL8168_PHY_100                    (1U << 3)
#define RTL8168_PHY_10                     (1U << 2)
#define RTL8168_PHY_LINK                   (1U << 1)
#define RTL8168_PHY_FULL_DUPLEX            (1U << 0)

#define RTL8168_PHY_ACCESS_BUSY            (1U << 31)
#define RTL8168_PHY_ACCESS_REGISTER_SHIFT  16U
#define RTL8168_PHY_ACCESS_REGISTER_MASK   0x1FU
#define RTL8168_PHY_ACCESS_DATA_MASK       0xFFFFU
#define RTL8168_PHY_ACCESS_TIMEOUT_US      20000U

#define RTL8168_MII_BASIC_CONTROL          0U
#define RTL8168_MII_ADVERTISEMENT          4U
#define RTL8168_MII_GIGABIT_CONTROL        9U
#define RTL8168_MII_AUTONEG_ENABLE         (1U << 12)
#define RTL8168_MII_POWER_DOWN             (1U << 11)
#define RTL8168_MII_ISOLATE                (1U << 10)
#define RTL8168_MII_RESTART_AUTONEG        (1U << 9)
#define RTL8168_MII_ADVERTISE_10_HALF      (1U << 5)
#define RTL8168_MII_ADVERTISE_10_FULL      (1U << 6)
#define RTL8168_MII_ADVERTISE_100_HALF     (1U << 7)
#define RTL8168_MII_ADVERTISE_100_FULL     (1U << 8)
#define RTL8168_MII_ADVERTISE_PAUSE        (1U << 10)
#define RTL8168_MII_ADVERTISE_ASYM_PAUSE   (1U << 11)
#define RTL8168_MII_SELECTOR_MASK          0x001FU
#define RTL8168_MII_ADVERTISE_1000_HALF    (1U << 8)
#define RTL8168_MII_ADVERTISE_1000_FULL    (1U << 9)

#define RTL8168_MCU_NOW_OOB                (1U << 7)
#define RTL8168_MCU_TX_EMPTY               (1U << 5)
#define RTL8168_MCU_RX_EMPTY               (1U << 4)
#define RTL8168_MCU_RX_TX_EMPTY            \
    (RTL8168_MCU_TX_EMPTY | RTL8168_MCU_RX_EMPTY)
#define RTL8168_MCU_LINK_LIST_READY       (1U << 1)
#define RTL8168_MISC_RXDV_GATE             (1U << 19)

#define RTL8168_MAC_OCP_WRITE             (1U << 31)
#define RTL8168_MAC_OCP_ADDRESS_SHIFT     15U
#define RTL8168_MAC_OCP_FIFO_CONTROL      0xE8DEU
#define RTL8168_MAC_OCP_FIFO_RELEASE      (1U << 14)
#define RTL8168_MAC_OCP_FIFO_RESTART      (1U << 15)

#define RTL8168_DESC_OWN                   (1U << 31)
#define RTL8168_DESC_END_RING              (1U << 30)
#define RTL8168_DESC_FIRST                 (1U << 29)
#define RTL8168_DESC_LAST                  (1U << 28)
#define RTL8168_RX_ERROR_SUMMARY           (1U << 21)
#define RTL8168_RX_LENGTH_MASK             0x3FFFU

#define RTL8168_PCI_STATUS_CAPABILITIES    (1U << 4)
#define RTL8168_PCI_CAPABILITY_POINTER     0x34U
#define RTL8168_PCI_CAPABILITY_PM          0x01U
#define RTL8168_PCI_PM_CONTROL             0x04U
#define RTL8168_PCI_PM_STATE_MASK          0x0003U
#define RTL8168_PCI_PM_PME_STATUS          (1U << 15)
#define RTL8168_PCI_COMMAND                0x04U
#define RTL8168_PCI_COMMAND_BUS_MASTER     (1U << 2)
#define RTL8168_PCI_COMMAND_INTX_DISABLE   (1U << 10)
#define RTL8168_PCI_CAPABILITY_LIMIT       48U

#define RTL8168_IRQ_SERVICE_PASSES         4U

typedef struct {
    volatile u32 options1;
    volatile u32 options2;
    volatile u64 buffer_address;
} __attribute__((packed, aligned(16))) rtl8168_descriptor_t;

_Static_assert(sizeof(rtl8168_descriptor_t) == 16,
               "RTL8168 descriptor must be 16 bytes");
_Static_assert(RTL8168_RX_RING_COUNT * sizeof(rtl8168_descriptor_t) <= PAGE_SIZE,
               "RTL8168 RX ring must fit one physical frame");
_Static_assert(RTL8168_TX_RING_COUNT * sizeof(rtl8168_descriptor_t) <= PAGE_SIZE,
               "RTL8168 TX ring must fit one physical frame");

typedef enum {
    RTL8168_STATE_UNINITIALIZED = 0,
    RTL8168_STATE_PROBING,
    RTL8168_STATE_RESETTING,
    RTL8168_STATE_RINGS_READY,
    RTL8168_STATE_RUNNING,
    RTL8168_STATE_READY,
    RTL8168_STATE_UNAVAILABLE,
    RTL8168_STATE_FAILED
} rtl8168_state_t;

typedef struct {
    rtl8168_state_t state;
    const char *result_reason;
    const pci_device_t *pci;
    volatile u8 *mmio;
    u32 xid;

    pci_msix_info_t msix;
    bool msix_prepared;
    bool irq_registered;
    bool dma_started;
    bool active;
    bool faulted;
    bool link_up;
    u8 phy_status;

    net_device_t device;

    phys_addr_t rx_ring_phys;
    rtl8168_descriptor_t *rx_ring;
    phys_addr_t rx_buffer_phys[RTL8168_RX_RING_COUNT];
    u8 *rx_buffer[RTL8168_RX_RING_COUNT];
    u32 rx_next;

    phys_addr_t tx_ring_phys;
    rtl8168_descriptor_t *tx_ring;
    phys_addr_t tx_buffer_phys[RTL8168_TX_RING_COUNT];
    u8 *tx_buffer[RTL8168_TX_RING_COUNT];
    u32 tx_producer;
    u32 tx_consumer;
    u32 tx_used;

    u64 received_frames;
    u64 transmitted_frames;
    u64 dropped_frames;
} rtl8168_controller_t;

static rtl8168_controller_t controller;

static inline void rtl8168_pause(void)
{
    __asm__ volatile("pause");
}

static inline void rtl8168_store_fence(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

static inline void rtl8168_load_fence(void)
{
    __asm__ volatile("lfence" ::: "memory");
}

static inline u8 rtl8168_read8(u32 reg)
{
    u8 value = *(volatile u8 *)(controller.mmio + reg);
    __asm__ volatile("" ::: "memory");
    return value;
}

static inline u16 rtl8168_read16(u32 reg)
{
    u16 value = *(volatile u16 *)(controller.mmio + reg);
    __asm__ volatile("" ::: "memory");
    return value;
}

static inline u32 rtl8168_read32(u32 reg)
{
    u32 value = *(volatile u32 *)(controller.mmio + reg);
    __asm__ volatile("" ::: "memory");
    return value;
}

static inline void rtl8168_write8(u32 reg, u8 value)
{
    __asm__ volatile("" ::: "memory");
    *(volatile u8 *)(controller.mmio + reg) = value;
}

static inline void rtl8168_write16(u32 reg, u16 value)
{
    __asm__ volatile("" ::: "memory");
    *(volatile u16 *)(controller.mmio + reg) = value;
}

static inline void rtl8168_write32(u32 reg, u32 value)
{
    __asm__ volatile("" ::: "memory");
    *(volatile u32 *)(controller.mmio + reg) = value;
}

/* RTL8168h exposes the MAC-internal 16-bit OCP register space through one
 * synchronous MMIO command register.  The following read commits a preceding
 * write and also gives each ownership transition an explicit verification
 * point. */
static bool rtl8168_mac_ocp_read(u16 reg, u16 *value)
{
    u32 result;

    if (!value || (reg & 1U))
        return false;
    rtl8168_write32(RTL8168_REG_MAC_OCP,
                    (u32)reg << RTL8168_MAC_OCP_ADDRESS_SHIFT);
    result = rtl8168_read32(RTL8168_REG_MAC_OCP);
    *value = (u16)result;
    return true;
}

static bool rtl8168_mac_ocp_write(u16 reg, u16 value)
{
    u16 observed;

    if (reg & 1U)
        return false;
    rtl8168_write32(RTL8168_REG_MAC_OCP,
                    RTL8168_MAC_OCP_WRITE |
                    ((u32)reg << RTL8168_MAC_OCP_ADDRESS_SHIFT) |
                    value);
    return rtl8168_mac_ocp_read(reg, &observed);
}

static bool rtl8168_mac_ocp_update(u16 reg, u16 clear, u16 set,
                                   u16 *observed)
{
    u16 value;

    if (!rtl8168_mac_ocp_read(reg, &value))
        return false;
    value = (u16)((value & ~clear) | set);
    if (!rtl8168_mac_ocp_write(reg, value))
        return false;
    if (!rtl8168_mac_ocp_read(reg, &value) ||
        (value & (clear | set)) != set) {
        return false;
    }
    if (observed)
        *observed = value;
    return true;
}

static bool rtl8168_mac_ocp_trigger(u16 reg, u16 trigger)
{
    u16 value;

    if (!rtl8168_mac_ocp_read(reg, &value))
        return false;

    /* Trigger bits initiate an internal transition and need not remain set.
     * Completion is proven by the transition's status condition, not by
     * reading the trigger back as persistent register state. */
    rtl8168_write32(RTL8168_REG_MAC_OCP,
                    RTL8168_MAC_OCP_WRITE |
                    ((u32)reg << RTL8168_MAC_OCP_ADDRESS_SHIFT) |
                    value | trigger);
    return true;
}

static bool rtl8168_wait8(u32 reg, u8 mask, u8 expected, u64 timeout_us,
                          u8 *last)
{
    timer_monotonic_deadline_t deadline;
    u8 value;

    if (!timer_monotonic_deadline_start(&deadline, timeout_us))
        return false;

    do {
        value = rtl8168_read8(reg);
        if ((value & mask) == expected) {
            if (last) *last = value;
            return true;
        }
        rtl8168_pause();
    } while (!timer_monotonic_deadline_expired(&deadline));

    if (last) *last = value;
    return false;
}

static bool rtl8168_wait32(u32 reg, u32 mask, u32 expected, u64 timeout_us,
                           u32 *last)
{
    timer_monotonic_deadline_t deadline;
    u32 value;

    if (!timer_monotonic_deadline_start(&deadline, timeout_us))
        return false;

    do {
        value = rtl8168_read32(reg);
        if ((value & mask) == expected) {
            if (last) *last = value;
            return true;
        }
        rtl8168_pause();
    } while (!timer_monotonic_deadline_expired(&deadline));

    if (last) *last = value;
    return false;
}

static bool rtl8168_phy_read(u8 phy_register, u16 *value)
{
    timer_monotonic_deadline_t deadline;

    if (!value || phy_register > RTL8168_PHY_ACCESS_REGISTER_MASK)
        return false;

    rtl8168_write32(RTL8168_REG_PHY_ACCESS,
                    (u32)phy_register << RTL8168_PHY_ACCESS_REGISTER_SHIFT);
    if (!timer_monotonic_deadline_start(
            &deadline, RTL8168_PHY_ACCESS_TIMEOUT_US)) {
        return false;
    }
    do {
        u32 result = rtl8168_read32(RTL8168_REG_PHY_ACCESS);
        if (result & RTL8168_PHY_ACCESS_BUSY) {
            *value = (u16)(result & RTL8168_PHY_ACCESS_DATA_MASK);
            return true;
        }
        rtl8168_pause();
    } while (!timer_monotonic_deadline_expired(&deadline));
    return false;
}

static bool rtl8168_phy_write(u8 phy_register, u16 value)
{
    timer_monotonic_deadline_t deadline;

    if (phy_register > RTL8168_PHY_ACCESS_REGISTER_MASK)
        return false;

    rtl8168_write32(RTL8168_REG_PHY_ACCESS,
                    RTL8168_PHY_ACCESS_BUSY |
                    ((u32)phy_register << RTL8168_PHY_ACCESS_REGISTER_SHIFT) |
                    value);
    if (!timer_monotonic_deadline_start(
            &deadline, RTL8168_PHY_ACCESS_TIMEOUT_US)) {
        return false;
    }
    do {
        if (!(rtl8168_read32(RTL8168_REG_PHY_ACCESS) &
              RTL8168_PHY_ACCESS_BUSY)) {
            return true;
        }
        rtl8168_pause();
    } while (!timer_monotonic_deadline_expired(&deadline));
    return false;
}

static bool rtl8168_start_autonegotiation(void)
{
    u16 advertisement;
    u16 gigabit;
    u16 control;

    if (!rtl8168_phy_read(RTL8168_MII_ADVERTISEMENT, &advertisement) ||
        !rtl8168_phy_read(RTL8168_MII_GIGABIT_CONTROL, &gigabit) ||
        !rtl8168_phy_read(RTL8168_MII_BASIC_CONTROL, &control)) {
        return false;
    }

    advertisement &= RTL8168_MII_SELECTOR_MASK;
    advertisement |= RTL8168_MII_ADVERTISE_10_HALF |
                     RTL8168_MII_ADVERTISE_10_FULL |
                     RTL8168_MII_ADVERTISE_100_HALF |
                     RTL8168_MII_ADVERTISE_100_FULL |
                     RTL8168_MII_ADVERTISE_PAUSE |
                     RTL8168_MII_ADVERTISE_ASYM_PAUSE;
    gigabit &= ~RTL8168_MII_ADVERTISE_1000_HALF;
    gigabit |= RTL8168_MII_ADVERTISE_1000_FULL;
    control &= ~(RTL8168_MII_POWER_DOWN | RTL8168_MII_ISOLATE);
    control |= RTL8168_MII_AUTONEG_ENABLE | RTL8168_MII_RESTART_AUTONEG;

    return rtl8168_phy_write(RTL8168_MII_ADVERTISEMENT, advertisement) &&
           rtl8168_phy_write(RTL8168_MII_GIGABIT_CONTROL, gigabit) &&
           rtl8168_phy_write(RTL8168_MII_BASIC_CONTROL, control);
}

static bool rtl8168_supported_pci_device(const pci_device_t *device)
{
    return device && device->vendor_id == RTL8168_VENDOR_ID &&
           device->device_id == RTL8168_DEVICE_ID &&
           device->class_code == 0x02U && device->subclass == 0x00U &&
           device->prog_if == 0x00U;
}

static bool rtl8168_enter_d0(const pci_device_t *device)
{
    u8 capability;

    if (!(pci_read_config16(device, 0x06U) &
          RTL8168_PCI_STATUS_CAPABILITIES)) {
        return true;
    }

    capability = pci_read_config8(device, RTL8168_PCI_CAPABILITY_POINTER) &
                 0xFCU;
    for (u32 visited = 0;
         capability >= 0x40U && visited < RTL8168_PCI_CAPABILITY_LIMIT;
         visited++) {
        u8 id = pci_read_config8(device, capability);
        u8 next = pci_read_config8(device, (u8)(capability + 1U)) & 0xFCU;

        if (id == RTL8168_PCI_CAPABILITY_PM) {
            u8 control_offset = (u8)(capability + RTL8168_PCI_PM_CONTROL);
            u16 control = pci_read_config16(device, control_offset);
            if ((control & RTL8168_PCI_PM_STATE_MASK) == 0)
                return true;

            control &= ~(RTL8168_PCI_PM_STATE_MASK |
                         RTL8168_PCI_PM_PME_STATUS);
            pci_write_config16(device, control_offset, control);

            /* PCI PM defines a D3hot-to-D0 recovery interval during which
             * register state is not yet safe to consume. */
            if (!timer_monotonic_delay_us(RTL8168_PCI_D0_RECOVERY_US))
                return false;
            return (pci_read_config16(device, control_offset) &
                    RTL8168_PCI_PM_STATE_MASK) == 0;
        }

        if (!next || next == capability)
            break;
        capability = next;
    }
    return true;
}

static bool rtl8168_enable_pci(const pci_device_t *device)
{
    u16 command;

    if (!pci_enable_memory_busmaster(device))
        return false;

    /* This target is operated exclusively through MSI-X.  Keep its unusable
     * legacy pin path disabled while the MSI-X entry remains masked. */
    command = pci_read_config16(device, RTL8168_PCI_COMMAND);
    command |= RTL8168_PCI_COMMAND_INTX_DISABLE;
    pci_write_config16(device, RTL8168_PCI_COMMAND, command);
    command = pci_read_config16(device, RTL8168_PCI_COMMAND);
    return (command & (RTL8168_PCI_COMMAND_BUS_MASTER |
                       RTL8168_PCI_COMMAND_INTX_DISABLE)) ==
           (RTL8168_PCI_COMMAND_BUS_MASTER |
            RTL8168_PCI_COMMAND_INTX_DISABLE);
}

static void rtl8168_disable_bus_master(void)
{
    u16 command;

    if (!controller.pci)
        return;
    command = pci_read_config16(controller.pci, RTL8168_PCI_COMMAND);
    command &= ~RTL8168_PCI_COMMAND_BUS_MASTER;
    command |= RTL8168_PCI_COMMAND_INTX_DISABLE;
    pci_write_config16(controller.pci, RTL8168_PCI_COMMAND, command);
}

static bool rtl8168_read_mac(u8 mac[6])
{
    u32 low = rtl8168_read32(RTL8168_REG_MAC0);
    u32 high = rtl8168_read32(RTL8168_REG_MAC4);
    bool all_zero = true;
    bool all_ff = true;

    mac[0] = (u8)low;
    mac[1] = (u8)(low >> 8);
    mac[2] = (u8)(low >> 16);
    mac[3] = (u8)(low >> 24);
    mac[4] = (u8)high;
    mac[5] = (u8)(high >> 8);

    for (u32 i = 0; i < 6U; i++) {
        all_zero &= mac[i] == 0;
        all_ff &= mac[i] == 0xFFU;
    }
    return !all_zero && !all_ff && !(mac[0] & 1U);
}

static bool rtl8168_allocate_page(phys_addr_t *physical, void **virtual)
{
    phys_addr_t frame;
    void *mapping;

    if (!physical || !virtual)
        return false;

    frame = pmm_alloc_frame();
    if (!frame)
        return false;
    mapping = phys_to_virt(frame);
    if (!mapping) {
        pmm_free_frame(frame);
        return false;
    }
    memset(mapping, 0, PAGE_SIZE);
    *physical = frame;
    *virtual = mapping;
    return true;
}

static inline u32 rtl8168_ring_end(u32 index, u32 count)
{
    return index + 1U == count ? RTL8168_DESC_END_RING : 0;
}

static void rtl8168_publish_rx(u32 index)
{
    rtl8168_descriptor_t *descriptor = &controller.rx_ring[index];
    u32 end = rtl8168_ring_end(index, RTL8168_RX_RING_COUNT);

    descriptor->options1 = end;
    descriptor->options2 = 0;
    descriptor->buffer_address = controller.rx_buffer_phys[index];
    rtl8168_store_fence();
    __atomic_store_n(&descriptor->options1,
                     RTL8168_DESC_OWN | end | RTL8168_BUFFER_SIZE,
                     __ATOMIC_RELEASE);
}

static bool rtl8168_allocate_rings(void)
{
    void *mapping;

    if (!rtl8168_allocate_page(&controller.rx_ring_phys, &mapping))
        return false;
    controller.rx_ring = (rtl8168_descriptor_t *)mapping;

    if (!rtl8168_allocate_page(&controller.tx_ring_phys, &mapping))
        return false;
    controller.tx_ring = (rtl8168_descriptor_t *)mapping;

    for (u32 i = 0; i < RTL8168_RX_RING_COUNT; i++) {
        if (!rtl8168_allocate_page(&controller.rx_buffer_phys[i], &mapping))
            return false;
        controller.rx_buffer[i] = (u8 *)mapping;
        rtl8168_publish_rx(i);
    }

    for (u32 i = 0; i < RTL8168_TX_RING_COUNT; i++) {
        rtl8168_descriptor_t *descriptor = &controller.tx_ring[i];
        if (!rtl8168_allocate_page(&controller.tx_buffer_phys[i], &mapping))
            return false;
        controller.tx_buffer[i] = (u8 *)mapping;
        descriptor->options1 = rtl8168_ring_end(i, RTL8168_TX_RING_COUNT);
        descriptor->options2 = 0;
        descriptor->buffer_address = controller.tx_buffer_phys[i];
    }

    controller.rx_next = 0;
    controller.tx_producer = 0;
    controller.tx_consumer = 0;
    controller.tx_used = 0;
    rtl8168_store_fence();
    return true;
}

static void rtl8168_release_rings(void)
{
    for (u32 i = 0; i < RTL8168_RX_RING_COUNT; i++) {
        if (controller.rx_buffer_phys[i])
            pmm_free_frame(controller.rx_buffer_phys[i]);
        controller.rx_buffer_phys[i] = 0;
        controller.rx_buffer[i] = NULL;
    }
    for (u32 i = 0; i < RTL8168_TX_RING_COUNT; i++) {
        if (controller.tx_buffer_phys[i])
            pmm_free_frame(controller.tx_buffer_phys[i]);
        controller.tx_buffer_phys[i] = 0;
        controller.tx_buffer[i] = NULL;
    }
    if (controller.rx_ring_phys)
        pmm_free_frame(controller.rx_ring_phys);
    if (controller.tx_ring_phys)
        pmm_free_frame(controller.tx_ring_phys);
    controller.rx_ring_phys = 0;
    controller.tx_ring_phys = 0;
    controller.rx_ring = NULL;
    controller.tx_ring = NULL;
}

static bool rtl8168_leave_firmware_receive_mode(void)
{
    u8 command;
    u8 mcu;
    u8 last;
    u32 last_tx_config;

    /* RTL8168h has a receive-valid gate and a shared firmware/driver receive
     * link list.  This handoff is required even when firmware happened to
     * leave ChipCmd disabled: those visible bits do not prove that the
     * internal receive list belongs to the host driver. */
    rtl8168_write32(RTL8168_REG_MISC,
                    rtl8168_read32(RTL8168_REG_MISC) |
                    RTL8168_MISC_RXDV_GATE);
    if (!(rtl8168_read32(RTL8168_REG_MISC) & RTL8168_MISC_RXDV_GATE) ||
        !timer_monotonic_delay_us(RTL8168_RXDV_GATE_SETTLE_US)) {
        return false;
    }

    /* With new receive data gated, observe both MAC and shared FIFOs empty
     * before taking the receive list away from firmware. */
    if (!rtl8168_wait32(RTL8168_REG_TX_CONFIG,
                        RTL8168_TX_CONFIG_EMPTY,
                        RTL8168_TX_CONFIG_EMPTY,
                        RTL8168_QUIESCE_TIMEOUT_US, &last_tx_config)) {
        return false;
    }
    if (!rtl8168_wait8(RTL8168_REG_MCU, RTL8168_MCU_RX_TX_EMPTY,
                       RTL8168_MCU_RX_TX_EMPTY,
                       RTL8168_QUIESCE_TIMEOUT_US, &last)) {
        return false;
    }

    command = rtl8168_read8(RTL8168_REG_COMMAND);
    rtl8168_write8(RTL8168_REG_COMMAND,
                   command & ~(RTL8168_COMMAND_RX_ENABLE |
                               RTL8168_COMMAND_TX_ENABLE));
    if ((rtl8168_read8(RTL8168_REG_COMMAND) &
         (RTL8168_COMMAND_RX_ENABLE | RTL8168_COMMAND_TX_ENABLE)) ||
        !timer_monotonic_delay_us(RTL8168_RX_DISABLE_SETTLE_US)) {
        return false;
    }

    mcu = rtl8168_read8(RTL8168_REG_MCU);
    if (mcu & RTL8168_MCU_NOW_OOB) {
        rtl8168_write8(RTL8168_REG_MCU, mcu & ~RTL8168_MCU_NOW_OOB);
    }
    if (!rtl8168_wait8(RTL8168_REG_MCU, RTL8168_MCU_NOW_OOB, 0,
                       RTL8168_QUIESCE_TIMEOUT_US, &last)) {
        return false;
    }

    if (!rtl8168_mac_ocp_update(RTL8168_MAC_OCP_FIFO_CONTROL,
                                RTL8168_MAC_OCP_FIFO_RELEASE, 0, NULL) ||
        !rtl8168_wait8(RTL8168_REG_MCU, RTL8168_MCU_LINK_LIST_READY,
                       RTL8168_MCU_LINK_LIST_READY,
                       RTL8168_QUIESCE_TIMEOUT_US, &last)) {
        return false;
    }
    if (!rtl8168_mac_ocp_trigger(RTL8168_MAC_OCP_FIFO_CONTROL,
                                 RTL8168_MAC_OCP_FIFO_RESTART) ||
        !rtl8168_wait8(RTL8168_REG_MCU, RTL8168_MCU_LINK_LIST_READY,
                       RTL8168_MCU_LINK_LIST_READY,
                       RTL8168_QUIESCE_TIMEOUT_US, &last)) {
        return false;
    }
    return true;
}

static bool rtl8168_reset(void)
{
    bool handoff_complete;
    bool reset_complete;
    u8 last;

    rtl8168_write16(RTL8168_REG_INTERRUPT_MASK, 0);
    rtl8168_write16(RTL8168_REG_INTERRUPT_STATUS, 0xFFFFU);
    handoff_complete = rtl8168_leave_firmware_receive_mode();
    if (!handoff_complete) {
        return false;
    }

    rtl8168_write8(RTL8168_REG_COMMAND, RTL8168_COMMAND_RESET);
    reset_complete = rtl8168_wait8(RTL8168_REG_COMMAND,
                                   RTL8168_COMMAND_RESET, 0,
                                   RTL8168_RESET_TIMEOUT_US, &last);
    if (!reset_complete) {
        return false;
    }

    /* Keep receive data gated until the host RX descriptors and filters have
     * been fully published.  rtl8168_program_hardware() releases this gate. */
    rtl8168_write32(RTL8168_REG_MISC,
                    rtl8168_read32(RTL8168_REG_MISC) |
                    RTL8168_MISC_RXDV_GATE);
    reset_complete =
        (rtl8168_read32(RTL8168_REG_MISC) & RTL8168_MISC_RXDV_GATE) != 0;
    return reset_complete;
}

static bool rtl8168_program_hardware(void)
{
    u16 cplus;
    u8 command;

    cplus = rtl8168_read16(RTL8168_REG_CPLUS_COMMAND);
    cplus &= ~(RTL8168_CPLUS_RX_VLAN | RTL8168_CPLUS_RX_CHECKSUM |
               RTL8168_CPLUS_INTERRUPT_TIMER);
    rtl8168_write16(RTL8168_REG_CPLUS_COMMAND, cplus);
    rtl8168_write16(RTL8168_REG_INTERRUPT_MITIGATION, 0);

    /* The low half commits each descriptor base; publish the high half first. */
    rtl8168_write32(RTL8168_REG_TX_DESC_HIGH,
                    (u32)(controller.tx_ring_phys >> 32));
    rtl8168_write32(RTL8168_REG_TX_DESC_LOW,
                    (u32)controller.tx_ring_phys);
    rtl8168_write32(RTL8168_REG_RX_DESC_HIGH,
                    (u32)(controller.rx_ring_phys >> 32));
    rtl8168_write32(RTL8168_REG_RX_DESC_LOW,
                    (u32)controller.rx_ring_phys);

    rtl8168_write16(RTL8168_REG_RX_MAX_SIZE, RTL8168_RX_MAX_SIZE);
    rtl8168_write8(RTL8168_REG_TX_MAX_SIZE, RTL8168_TX_MAX_SIZE_UNITS);
    rtl8168_write32(RTL8168_REG_MULTICAST0, 0xFFFFFFFFU);
    rtl8168_write32(RTL8168_REG_MULTICAST4, 0xFFFFFFFFU);

    rtl8168_write32(RTL8168_REG_TX_CONFIG,
                    RTL8168_TX_CONFIG_IFG | RTL8168_TX_CONFIG_DMA_BURST |
                    RTL8168_TX_CONFIG_AUTO_FIFO);
    rtl8168_write32(RTL8168_REG_RX_CONFIG,
                    RTL8168_RX_CONFIG_128_BYTE_IRQ |
                    RTL8168_RX_CONFIG_MULTIPLE_FETCH |
                    RTL8168_RX_CONFIG_EARLY_OFF |
                    RTL8168_RX_CONFIG_DMA_BURST |
                    RTL8168_RX_CONFIG_BROADCAST |
                    RTL8168_RX_CONFIG_MULTICAST |
                    RTL8168_RX_CONFIG_PHYSICAL);

    /* OWN, buffer addresses, ring bases, and receive filters must all be
     * visible before the H-generation receive-valid gate is opened. */
    rtl8168_store_fence();
    (void)rtl8168_read32(RTL8168_REG_RX_DESC_LOW);
    rtl8168_write32(RTL8168_REG_MISC,
                    rtl8168_read32(RTL8168_REG_MISC) &
                    ~RTL8168_MISC_RXDV_GATE);
    if (rtl8168_read32(RTL8168_REG_MISC) & RTL8168_MISC_RXDV_GATE)
        return false;

    controller.dma_started = true;
    rtl8168_write8(RTL8168_REG_COMMAND,
                   RTL8168_COMMAND_RX_ENABLE | RTL8168_COMMAND_TX_ENABLE);

    command = rtl8168_read8(RTL8168_REG_COMMAND);
    if ((command & (RTL8168_COMMAND_RX_ENABLE |
                    RTL8168_COMMAND_TX_ENABLE)) !=
        (RTL8168_COMMAND_RX_ENABLE | RTL8168_COMMAND_TX_ENABLE)) {
        return false;
    }
    if (rtl8168_read32(RTL8168_REG_TX_DESC_LOW) !=
            (u32)controller.tx_ring_phys ||
        rtl8168_read32(RTL8168_REG_TX_DESC_HIGH) !=
            (u32)(controller.tx_ring_phys >> 32) ||
        rtl8168_read32(RTL8168_REG_RX_DESC_LOW) !=
            (u32)controller.rx_ring_phys ||
        rtl8168_read32(RTL8168_REG_RX_DESC_HIGH) !=
            (u32)(controller.rx_ring_phys >> 32)) {
        return false;
    }
    if ((rtl8168_read32(RTL8168_REG_RX_CONFIG) &
         (RTL8168_RX_CONFIG_BROADCAST |
          RTL8168_RX_CONFIG_MULTICAST |
          RTL8168_RX_CONFIG_PHYSICAL)) !=
        (RTL8168_RX_CONFIG_BROADCAST |
         RTL8168_RX_CONFIG_MULTICAST |
         RTL8168_RX_CONFIG_PHYSICAL)) {
        return false;
    }

    return true;
}

static bool rtl8168_wait_for_link(void)
{
    timer_monotonic_deadline_t deadline;

    if (!timer_monotonic_deadline_start(&deadline,
                                        RTL8168_LINK_TIMEOUT_US)) {
        return false;
    }

    do {
        controller.phy_status = rtl8168_read8(RTL8168_REG_PHY_STATUS);
        if (controller.phy_status & RTL8168_PHY_LINK) {
            controller.link_up = true;
            return true;
        }
        /* PHYStatus is documented to update at most once per 300 us.  The
         * link bit, rather than this sampling interval, is the success test. */
        if (!timer_monotonic_delay_us(RTL8168_PHY_STATUS_UPDATE_US))
            return false;
    } while (!timer_monotonic_deadline_expired(&deadline));

    controller.link_up = false;
    controller.phy_status = rtl8168_read8(RTL8168_REG_PHY_STATUS);
    return false;
}

static u32 rtl8168_link_speed(u8 status)
{
    if (status & RTL8168_PHY_1000_FULL) return 1000U;
    if (status & RTL8168_PHY_100) return 100U;
    if (status & RTL8168_PHY_10) return 10U;
    return 0;
}

static u64 rtl8168_irq_save(void)
{
    u64 flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void rtl8168_irq_restore(u64 flags)
{
    if (flags & (1ULL << 9))
        __asm__ volatile("sti" ::: "memory");
}

static inline u32 rtl8168_descriptor_status(rtl8168_descriptor_t *descriptor)
{
    u32 status = __atomic_load_n(&descriptor->options1, __ATOMIC_ACQUIRE);
    rtl8168_load_fence();
    return status;
}

static void rtl8168_reclaim_tx(u16 interrupt_status)
{
    (void)interrupt_status;
    while (controller.tx_used) {
        u32 index = controller.tx_consumer;
        rtl8168_descriptor_t *descriptor =
            &controller.tx_ring[index];
        u32 status = rtl8168_descriptor_status(descriptor);
        if (status & RTL8168_DESC_OWN)
            break;

        controller.tx_consumer =
            (controller.tx_consumer + 1U) % RTL8168_TX_RING_COUNT;
        controller.tx_used--;
        controller.transmitted_frames++;
    }
}

static bool rtl8168_transmit(net_device_t *device, const void *frame,
                             usize length)
{
    rtl8168_descriptor_t *descriptor;
    const u8 *source = (const u8 *)frame;
    u64 flags;
    u32 index;
    usize wire_length;
    u32 options;

    if (!device || device != &controller.device || !controller.active ||
        controller.faulted || !controller.link_up || !frame ||
        length < NET_ETHERNET_HEADER_SIZE ||
        length > NET_ETHERNET_MAX_FRAME) {
        return false;
    }

    flags = rtl8168_irq_save();
    rtl8168_reclaim_tx(0);
    if (controller.tx_used == RTL8168_TX_RING_COUNT) {
        rtl8168_irq_restore(flags);
        return false;
    }

    index = controller.tx_producer;
    descriptor = &controller.tx_ring[index];
    if (rtl8168_descriptor_status(descriptor) & RTL8168_DESC_OWN) {
        rtl8168_irq_restore(flags);
        return false;
    }

    for (usize i = 0; i < length; i++)
        controller.tx_buffer[index][i] = source[i];
    wire_length = length < NET_ETHERNET_MIN_FRAME ?
        NET_ETHERNET_MIN_FRAME : length;
    for (usize i = length; i < wire_length; i++)
        controller.tx_buffer[index][i] = 0;

    options = rtl8168_ring_end(index, RTL8168_TX_RING_COUNT) |
              RTL8168_DESC_FIRST | RTL8168_DESC_LAST | (u32)wire_length;
    descriptor->options1 = options;
    descriptor->options2 = 0;
    descriptor->buffer_address = controller.tx_buffer_phys[index];
    rtl8168_store_fence();
    __atomic_store_n(&descriptor->options1, options | RTL8168_DESC_OWN,
                     __ATOMIC_RELEASE);
    rtl8168_store_fence();

    controller.tx_producer = (index + 1U) % RTL8168_TX_RING_COUNT;
    controller.tx_used++;
    rtl8168_write8(RTL8168_REG_TX_POLL, RTL8168_TX_POLL_NORMAL);
    rtl8168_irq_restore(flags);
    return true;
}

static bool rtl8168_rx_ready(void)
{
    if (!controller.rx_ring)
        return false;
    return !(rtl8168_descriptor_status(
        &controller.rx_ring[controller.rx_next]) & RTL8168_DESC_OWN);
}

static u32 rtl8168_receive(u32 budget)
{
    u32 processed = 0;

    while (processed < budget) {
        u32 index = controller.rx_next;
        rtl8168_descriptor_t *descriptor = &controller.rx_ring[index];
        u32 status = rtl8168_descriptor_status(descriptor);
        u32 dma_length;
        u32 frame_length;

        if (status & RTL8168_DESC_OWN)
            break;

        dma_length = status & RTL8168_RX_LENGTH_MASK;
        frame_length = dma_length >= 4U ? dma_length - 4U : 0;
        if ((status & (RTL8168_DESC_FIRST | RTL8168_DESC_LAST)) ==
                (RTL8168_DESC_FIRST | RTL8168_DESC_LAST) &&
            !(status & RTL8168_RX_ERROR_SUMMARY) &&
            frame_length >= NET_ETHERNET_HEADER_SIZE &&
            frame_length <= NET_ETHERNET_MAX_FRAME) {
            net_receive_frame(&controller.device,
                              controller.rx_buffer[index], frame_length);
            controller.received_frames++;
        } else {
            controller.dropped_frames++;
        }

        rtl8168_publish_rx(index);
        controller.rx_next = (index + 1U) % RTL8168_RX_RING_COUNT;
        processed++;
    }
    return processed;
}

static void rtl8168_irq_handler(struct cpu_registers *registers)
{
    (void)registers;
    if (!controller.active || controller.faulted)
        return;

    /* Device masking bounds one service pass.  Causes which latch while
     * masked remain pending and generate MSI-X when the mask is restored. */
    rtl8168_write16(RTL8168_REG_INTERRUPT_MASK, 0);
    for (u32 pass = 0; pass < RTL8168_IRQ_SERVICE_PASSES; pass++) {
        u16 status = rtl8168_read16(RTL8168_REG_INTERRUPT_STATUS);
        u16 acknowledged;

        if (status == 0xFFFFU) {
            controller.faulted = true;
            break;
        }

        acknowledged = status & RTL8168_INTERRUPT_RUNTIME_MASK;
        if (acknowledged)
            rtl8168_write16(RTL8168_REG_INTERRUPT_STATUS, acknowledged);

        if ((status & (RTL8168_INTERRUPT_RX_OK |
                       RTL8168_INTERRUPT_RX_ERROR |
                       RTL8168_INTERRUPT_RX_UNAVAILABLE)) ||
            rtl8168_rx_ready()) {
            (void)rtl8168_receive(RTL8168_RX_RING_COUNT);
        }
        if (status & (RTL8168_INTERRUPT_TX_OK |
                      RTL8168_INTERRUPT_TX_ERROR)) {
            rtl8168_reclaim_tx(status);
        }
        if (status & RTL8168_INTERRUPT_LINK_CHANGE) {
            controller.phy_status =
                rtl8168_read8(RTL8168_REG_PHY_STATUS);
            controller.link_up =
                (controller.phy_status & RTL8168_PHY_LINK) != 0;
        }
        if (status & RTL8168_INTERRUPT_SYSTEM_ERROR) {
            controller.faulted = true;
            kprint("[RTL8168h] fatal PCI/DMA interrupt isr=%04x\n",
                   (u32)status);
            break;
        }

        if (!acknowledged && !rtl8168_rx_ready())
            break;
    }

    if (!controller.faulted)
        rtl8168_write16(RTL8168_REG_INTERRUPT_MASK,
                        RTL8168_INTERRUPT_RUNTIME_MASK);
    /* Commit the device mask before LAPIC EOI in the common IRQ epilogue. */
    (void)rtl8168_read16(RTL8168_REG_INTERRUPT_MASK);
}

static bool rtl8168_prepare_msix(void)
{
    u8 apic_id;

    if (!lapic_enabled() ||
        !pci_get_msix_info(controller.pci, &controller.msix) ||
        controller.msix.table_size <= RTL8168_MSIX_ENTRY ||
        !pci_map_msix_table(&controller.msix)) {
        return false;
    }

    apic_id = (u8)(lapic_read(LAPIC_ID) >> 24);
    if (!pci_prepare_msix_vector(controller.pci, &controller.msix,
                                 RTL8168_MSIX_ENTRY, apic_id,
                                 RTL8168_IRQ_VECTOR)) {
        return false;
    }
    controller.msix_prepared = true;
    return true;
}

static bool rtl8168_enable_interrupts(void)
{
    if (!irq_register_vector(RTL8168_IRQ_VECTOR, rtl8168_irq_handler))
        return false;
    controller.irq_registered = true;

    rtl8168_write16(RTL8168_REG_INTERRUPT_MASK, 0);
    rtl8168_write16(RTL8168_REG_INTERRUPT_STATUS, 0xFFFFU);
    controller.active = true;
    rtl8168_write16(RTL8168_REG_INTERRUPT_MASK,
                    RTL8168_INTERRUPT_RUNTIME_MASK);
    if (rtl8168_read16(RTL8168_REG_INTERRUPT_MASK) !=
        RTL8168_INTERRUPT_RUNTIME_MASK) {
        controller.active = false;
        irq_unregister_vector(RTL8168_IRQ_VECTOR);
        controller.irq_registered = false;
        return false;
    }
    if (!pci_unmask_msix_vector(controller.pci, &controller.msix,
                                RTL8168_MSIX_ENTRY)) {
        controller.active = false;
        rtl8168_write16(RTL8168_REG_INTERRUPT_MASK, 0);
        irq_unregister_vector(RTL8168_IRQ_VECTOR);
        controller.irq_registered = false;
        return false;
    }
    return true;
}

static bool rtl8168_stop_dma(void)
{
    u8 last;

    if (!controller.mmio || !controller.dma_started)
        return true;

    rtl8168_write16(RTL8168_REG_INTERRUPT_MASK, 0);
    rtl8168_write8(RTL8168_REG_COMMAND, RTL8168_COMMAND_RESET);
    if (!rtl8168_wait8(RTL8168_REG_COMMAND, RTL8168_COMMAND_RESET, 0,
                       RTL8168_RESET_TIMEOUT_US, &last)) {
        rtl8168_disable_bus_master();
        return false;
    }
    controller.dma_started = false;
    return true;
}

static void rtl8168_cleanup_failed_start(void)
{
    bool safe_to_release;

    controller.active = false;
    if (controller.mmio)
        rtl8168_write16(RTL8168_REG_INTERRUPT_MASK, 0);
    if (controller.msix_prepared) {
        pci_disable_msix(controller.pci, &controller.msix,
                         RTL8168_MSIX_ENTRY);
        controller.msix_prepared = false;
    }
    if (controller.irq_registered) {
        irq_unregister_vector(RTL8168_IRQ_VECTOR);
        controller.irq_registered = false;
    }

    safe_to_release = rtl8168_stop_dma();
    rtl8168_disable_bus_master();
    if (safe_to_release)
        rtl8168_release_rings();
}

static void rtl8168_log_failure(const char *phase)
{
    u16 fifo_control = 0;
    bool ocp_valid;

    if (!controller.mmio) {
        kprint("[RTL8168h] init failed phase=%s\n", phase);
        return;
    }

    ocp_valid = rtl8168_mac_ocp_read(RTL8168_MAC_OCP_FIFO_CONTROL,
                                     &fifo_control);
    kprint("[RTL8168h] init failed phase=%s cmd=%02x isr=%04x "
           "phy=%02x txc=%08x mcu=%02x misc=%08x ocp=%s%04x\n",
           phase, (u32)rtl8168_read8(RTL8168_REG_COMMAND),
           (u32)rtl8168_read16(RTL8168_REG_INTERRUPT_STATUS),
           (u32)rtl8168_read8(RTL8168_REG_PHY_STATUS),
           rtl8168_read32(RTL8168_REG_TX_CONFIG),
           (u32)rtl8168_read8(RTL8168_REG_MCU),
           rtl8168_read32(RTL8168_REG_MISC), ocp_valid ? "" : "?",
           (u32)fifo_control);
}

static rtl8168_init_result_t rtl8168_finish_failure(const char *reason,
                                                     const char *phase,
                                                     bool unavailable)
{
    rtl8168_log_failure(phase);
    rtl8168_cleanup_failed_start();
    controller.result_reason = reason;
    controller.state = unavailable ? RTL8168_STATE_UNAVAILABLE :
                                     RTL8168_STATE_FAILED;
    return unavailable ? RTL8168_INIT_UNAVAILABLE : RTL8168_INIT_FAILED;
}

rtl8168_init_result_t rtl8168_init(const char **reason)
{
    const pci_device_t *device = NULL;
    pci_bar_t bar;
    u32 tx_config;

    if (reason) *reason = NULL;
    if (controller.state == RTL8168_STATE_READY)
        return RTL8168_INIT_READY;
    if (controller.state == RTL8168_STATE_UNAVAILABLE) {
        if (reason) *reason = controller.result_reason;
        return RTL8168_INIT_UNAVAILABLE;
    }
    if (controller.state != RTL8168_STATE_UNINITIALIZED) {
        if (reason) *reason = controller.result_reason;
        return RTL8168_INIT_FAILED;
    }

    controller.state = RTL8168_STATE_PROBING;
    for (u32 i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t *candidate = pci_get_device(i);
        if (rtl8168_supported_pci_device(candidate)) {
            device = candidate;
            break;
        }
    }
    if (!device) {
        controller.result_reason = "no RTL8168 Ethernet controller";
        controller.state = RTL8168_STATE_UNAVAILABLE;
        if (reason) *reason = controller.result_reason;
        return RTL8168_INIT_ABSENT;
    }
    controller.pci = device;

    if (!timer_monotonic_ready()) {
        controller.result_reason = "RTL8168h monotonic deadline unavailable";
        controller.state = RTL8168_STATE_FAILED;
        if (reason) *reason = controller.result_reason;
        return RTL8168_INIT_FAILED;
    }
    if (!rtl8168_enter_d0(device) || !rtl8168_enable_pci(device)) {
        rtl8168_disable_bus_master();
        controller.result_reason = "RTL8168h PCI enable failed";
        controller.state = RTL8168_STATE_FAILED;
        if (reason) *reason = controller.result_reason;
        return RTL8168_INIT_FAILED;
    }

    bar = pci_get_bar(device, RTL8168_MMIO_BAR);
    if (!bar.address || bar.io) {
        rtl8168_disable_bus_master();
        controller.result_reason = "RTL8168h BAR2 MMIO unavailable";
        controller.state = RTL8168_STATE_FAILED;
        if (reason) *reason = controller.result_reason;
        return RTL8168_INIT_FAILED;
    }
    controller.mmio = (volatile u8 *)vmm_map_mmio(
        (phys_addr_t)bar.address, RTL8168_MMIO_SIZE);
    if (!controller.mmio ||
        !vmm_ioremap_contains((const void *)controller.mmio)) {
        controller.mmio = NULL;
        rtl8168_disable_bus_master();
        controller.result_reason = "RTL8168h MMIO mapping failed";
        controller.state = RTL8168_STATE_FAILED;
        if (reason) *reason = controller.result_reason;
        return RTL8168_INIT_FAILED;
    }

    rtl8168_write16(RTL8168_REG_INTERRUPT_MASK, 0);
    tx_config = rtl8168_read32(RTL8168_REG_TX_CONFIG);
    controller.xid = (tx_config >> 20) & RTL8168_XID_MASK;
    if (device->revision != RTL8168_PCI_REVISION_H ||
        controller.xid != RTL8168_XID_H) {
        return rtl8168_finish_failure("unsupported RTL8168 revision",
                                      "identify", true);
    }
    if (!rtl8168_read_mac(controller.device.mac)) {
        return rtl8168_finish_failure("RTL8168h MAC address unavailable",
                                      "read-mac", false);
    }
    if (!rtl8168_prepare_msix()) {
        return rtl8168_finish_failure("RTL8168h MSI-X unavailable",
                                      "prepare-msix", false);
    }

    KERNEL_BOOT_DEBUG_LOG(
        "[RTL8168h] %02x:%02x.%u rev=%02x xid=%03x bar2=%llx "
        "msix=%u\n",
        (u32)device->bus, (u32)device->device, (u32)device->function,
        (u32)device->revision, controller.xid, (u64)bar.address,
        (u32)controller.msix.table_size);

    controller.state = RTL8168_STATE_RESETTING;
    if (!rtl8168_reset()) {
        return rtl8168_finish_failure("RTL8168h reset/ownership timeout",
                                      "reset", false);
    }
    if (!rtl8168_start_autonegotiation()) {
        return rtl8168_finish_failure("RTL8168h PHY setup timeout",
                                      "phy-autoneg", false);
    }
    if (!rtl8168_allocate_rings()) {
        return rtl8168_finish_failure("RTL8168h DMA allocation failed",
                                      "allocate-rings", false);
    }
    controller.state = RTL8168_STATE_RINGS_READY;
    if (!rtl8168_program_hardware()) {
        return rtl8168_finish_failure("RTL8168h ring activation failed",
                                      "start-rings", false);
    }
    controller.state = RTL8168_STATE_RUNNING;

    if (!rtl8168_wait_for_link()) {
        return rtl8168_finish_failure("RTL8168h link unavailable",
                                      "wait-link", true);
    }

    controller.device.name = "rtl8168h";
    controller.device.mtu = 1500;
    controller.device.transmit = rtl8168_transmit;
    controller.device.driver_data = &controller;

    if (!rtl8168_enable_interrupts()) {
        return rtl8168_finish_failure("RTL8168h interrupt activation failed",
                                      "enable-msix", false);
    }
    if (!net_register_device(&controller.device)) {
        return rtl8168_finish_failure("RTL8168h network registration failed",
                                      "register-device", false);
    }

    controller.state = RTL8168_STATE_READY;
    controller.result_reason = NULL;
    KERNEL_BOOT_DEBUG_LOG(
        "[OK] Ethernet controller active: rtl8168h "
        "%02x:%02x:%02x:%02x:%02x:%02x link=%u/%s\n",
        (u32)controller.device.mac[0], (u32)controller.device.mac[1],
        (u32)controller.device.mac[2], (u32)controller.device.mac[3],
        (u32)controller.device.mac[4], (u32)controller.device.mac[5],
        rtl8168_link_speed(controller.phy_status),
        (controller.phy_status & RTL8168_PHY_FULL_DUPLEX) ?
            "full" : "half");
    return RTL8168_INIT_READY;
}
