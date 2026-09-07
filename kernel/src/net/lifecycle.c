#include <net/lifecycle.h>

#include <net/e1000.h>
#include <net/rtl8168.h>
#include <pci.h>
#include <scheduler.h>

#define NULL ((void *)0)

#define NET_LIFECYCLE_SCAN_INTERVAL_MS 500U

static kernel_thread_t *lifecycle_worker;

static void net_lifecycle_worker(void *argument)
{
    (void)argument;
    for (;;) {
        /* PCI hotplug has no portable interrupt source in the current
         * machine profile.  This is deliberately a low-frequency,
         * scheduler-backed reconciliation, not a busy hotplug loop. */
        pci_process_hotplug();
        (void)pci_rescan();
        (void)e1000_rescan();
        (void)rtl8168_rescan();
        (void)scheduler_sleep(NET_LIFECYCLE_SCAN_INTERVAL_MS);
    }
}

bool net_lifecycle_init(void)
{
    if (lifecycle_worker) return true;
    lifecycle_worker = thread_create_with_priority(
        "net-lifecycle", net_lifecycle_worker, NULL,
        THREAD_PRIORITY_BACKGROUND);
    return lifecycle_worker != NULL;
}
