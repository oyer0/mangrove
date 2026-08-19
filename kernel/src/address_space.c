#include <address_space.h>

static bool direct_map_ready;

bool phys_map_contains(phys_addr_t phys)
{
    return phys < PHYS_MAP_LIMIT;
}

bool phys_map_is_ready(void)
{
    return direct_map_ready;
}

void phys_map_activate(void)
{
    direct_map_ready = true;
}

void *phys_to_virt(phys_addr_t phys)
{
    if (!phys_map_contains(phys)) return 0;

    /* This is used only while building the direct-map hierarchy itself. */
    if (!direct_map_ready) return (void *)(uintptr_t)phys;
    return (void *)(uintptr_t)(PHYS_MAP_BASE + phys);
}

phys_addr_t virt_to_phys(const void *virt)
{
    virt_addr_t address = (virt_addr_t)(uintptr_t)virt;

    if (address >= PHYS_MAP_BASE &&
        address - PHYS_MAP_BASE < PHYS_MAP_LIMIT) {
        return address - PHYS_MAP_BASE;
    }

    /* Bootstrap-only identity interpretation; never accepted afterwards. */
    if (!direct_map_ready && address < PHYS_MAP_LIMIT) return address;
    return 0;
}
