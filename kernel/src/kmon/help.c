#include <kmon/help.h>
#include <kprint.h>

void kmon_help(int argc, char **argv) {
    (void)argc; (void)argv;
    kprint("Available commands:\n");
    kprint("  help     - Display this help message\n");
    kprint("  version  - Display OS version\n");
    kprint("  clear    - Clear the terminal screen\n");
    kprint("  mem      - Display physical memory usage\n");
    kprint("  uptime   - Display system uptime\n");
    kprint("  heap     - Display kernel heap usage\n");
    kprint("  pci      - List PCI devices\n");
    kprint("  ahci     - Display AHCI storage controller status\n");
    kprint("  block    - List registered block devices\n");
    kprint("  pwd      - Display current working directory\n");
    kprint("  cd       - Change working directory\n");
    kprint("  ls       - List directory contents\n");
    kprint("  cat      - Display file contents\n");
    kprint("  touch    - Create an empty file\n");
    kprint("  mkdir    - Create a directory\n");
    kprint("  rm       - Remove a file\n");
    kprint("  rmdir    - Remove an empty directory\n");
    kprint("  mv       - Rename or move a file or directory\n");
}
