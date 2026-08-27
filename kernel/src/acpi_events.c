#include <acpi.h>

#include <aml.h>
#include <heap.h>
#include <kprint.h>
#include <platform_power.h>
#include <scheduler.h>
#include <vmm.h>

#include <io.h>
#include <string.h>

#define ACPI_GPE_WORD_COUNT 8U

#ifdef ACPI_POWER_DEBUG
#define ACPI_EVENT_DEBUG(...) kprint(__VA_ARGS__)
#else
#define ACPI_EVENT_DEBUG(...) ((void)0)
#endif

typedef struct {
    acpi_generic_address_t base;
    volatile u8 *memory;
    u8 length;
    u8 status_bytes;
    u8 gpe_base;
    bool valid;
} acpi_gpe_block_t;

static acpi_gpe_block_t gpe_blocks[2];
static volatile u32 pending_gpes[ACPI_GPE_WORD_COUNT];
static kernel_thread_t *event_thread;
static bool prepared;
static bool started;

static bool acpi_gpe_block_prepare(acpi_gpe_block_t *block,
                                    const acpi_generic_address_t *gas,
                                    u8 length, u8 gpe_base)
{
    if (!block || !gas || !gas->address || !length || (length & 1U) ||
        (gas->address_space_id != ACPI_ADDRESS_SPACE_SYSTEM_IO &&
         gas->address_space_id != ACPI_ADDRESS_SPACE_SYSTEM_MEMORY))
        return false;

    *block = (acpi_gpe_block_t){
        .base = *gas,
        .length = length,
        .status_bytes = (u8)(length / 2U),
        .gpe_base = gpe_base,
    };
    if (gas->address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        if (gas->address > 0xffffU ||
            (u64)length - 1U > 0xffffU - gas->address)
            return false;
    } else {
        block->memory = (volatile u8 *)vmm_map_mmio(
            (phys_addr_t)gas->address, length);
        if (!block->memory ||
            !vmm_ioremap_contains((const void *)block->memory))
            return false;
    }
    block->valid = true;
    return true;
}

static u8 acpi_gpe_read(const acpi_gpe_block_t *block, u8 offset)
{
    if (block->base.address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_IO)
        return inb((u16)(block->base.address + offset));
    return block->memory[offset];
}

static void acpi_gpe_write(const acpi_gpe_block_t *block, u8 offset,
                           u8 value)
{
    if (block->base.address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        outb((u16)(block->base.address + offset), value);
        return;
    }
    block->memory[offset] = value;
    __asm__ volatile("" ::: "memory");
}

bool acpi_events_prepare(void)
{
    const acpi_fadt_info_t *fadt = acpi_fadt_get();

    if (prepared)
        return gpe_blocks[0].valid || gpe_blocks[1].valid;
    prepared = true;
    memset(gpe_blocks, 0, sizeof(gpe_blocks));
    memset((void *)pending_gpes, 0, sizeof(pending_gpes));
    if (!fadt || fadt->hardware_reduced)
        return false;

    (void)acpi_gpe_block_prepare(&gpe_blocks[0], &fadt->gpe0_block,
                                 fadt->gpe0_length, 0U);
    (void)acpi_gpe_block_prepare(&gpe_blocks[1], &fadt->gpe1_block,
                                 fadt->gpe1_length, fadt->gpe1_base);
    return gpe_blocks[0].valid || gpe_blocks[1].valid;
}

void acpi_sci_interrupt(void)
{
    bool work_pending = false;

    if (!prepared)
        return;

    for (u32 block_index = 0; block_index < 2U; block_index++) {
        acpi_gpe_block_t *block = &gpe_blocks[block_index];

        if (!block->valid)
            continue;
        for (u8 byte = 0; byte < block->status_bytes; byte++) {
            u8 status = acpi_gpe_read(block, byte);
            u8 enable = acpi_gpe_read(block,
                                      (u8)(block->status_bytes + byte));
            u8 active = status & enable;

            if (!active)
                continue;
            /* GPE status is write-one-to-clear.  Clear only the enabled
             * sources observed in this interrupt; unrelated status remains
             * available to firmware. */
            acpi_gpe_write(block, byte, active);
            for (u8 bit = 0; bit < 8U; bit++) {
                u16 gpe = (u16)block->gpe_base + (u16)byte * 8U + bit;
                if ((active & (1U << bit)) && gpe < 256U) {
                    __atomic_fetch_or(&pending_gpes[gpe / 32U],
                                      1U << (gpe % 32U), __ATOMIC_RELEASE);
                    ACPI_EVENT_DEBUG("[ACPI] SCI GPE %u pending\n", gpe);
                    work_pending = true;
                }
            }
        }
    }

    /* The owner normally blocks after draining the pending bitmap.  Wake it
     * here, after publishing the bitmap, so a GPE arriving while it is idle
     * cannot remain queued until an unrelated scheduler event. */
    if (work_pending && event_thread &&
        event_thread->state == THREAD_STATE_BLOCKED)
        (void)scheduler_unblock(event_thread);
}

static bool acpi_pending_work(void)
{
    for (u32 word = 0; word < ACPI_GPE_WORD_COUNT; word++) {
        if (__atomic_load_n(&pending_gpes[word], __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

static void acpi_event_thread_entry(void *argument)
{
    (void)argument;

    for (;;) {
        bool processed = false;

        for (u32 word = 0; word < ACPI_GPE_WORD_COUNT; word++) {
            u32 bits = __atomic_exchange_n(&pending_gpes[word], 0U,
                                           __ATOMIC_ACQ_REL);
            while (bits) {
                u32 bit = (u32)__builtin_ctz(bits);
                u8 gpe = (u8)(word * 32U + bit);
                bits &= ~(1U << bit);
                processed = true;
                (void)aml_dispatch_gpe(gpe);
            }
        }
        if (processed)
            platform_power_process_lid_events();

        /* Close the same wakeup window used by the other event owner: with
         * IF clear, an SCI cannot set work after this check but before the
         * scheduler changes this thread to BLOCKED. */
        u64 saved_flags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(saved_flags) ::
                         "memory");
        if (!acpi_pending_work())
            (void)scheduler_block();
        else
            (void)scheduler_yield();
        __asm__ volatile("pushq %0; popfq" :: "r"(saved_flags) : "memory");
    }
}

bool acpi_events_start(void)
{
    if (started)
        return event_thread != NULL;
    if (!prepared)
        (void)acpi_events_prepare();
    if (!gpe_blocks[0].valid && !gpe_blocks[1].valid)
        return false;
    event_thread = thread_create_suspended("acpi-events",
                                           acpi_event_thread_entry, NULL);
    if (!event_thread || !scheduler_enqueue(event_thread)) {
        if (event_thread)
            (void)thread_destroy(event_thread);
        event_thread = NULL;
        return false;
    }
    started = true;
    return true;
}
