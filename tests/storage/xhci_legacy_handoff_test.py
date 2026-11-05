#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production handoff with both firmware semaphore states."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/usb/xhci.c').read_text()
start = source.index('static bool xhci_legacy_handoff(')
end = source.index('{', start) + 1
depth = 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
production = source[start:end]
constants = '\n'.join(line for line in source.splitlines()
                      if line.startswith('#define XHCI_LEGACY_') or
                      line.startswith('#define XHCI_MAX_EXT_CAPS') or
                      line.startswith('#define XHCI_EXT_CAP_ID_LEGACY'))
fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <xhci_regs.h>
static void discard_log(const char *format, ...) {(void)format;}
#define XHCI_DEBUG_LOG(...) discard_log(__VA_ARGS__)
#define kprint(...) discard_log(__VA_ARGS__)
typedef struct {xhci_cap_regs_t *cap_regs;} xhci_controller_t;
static volatile u32 legacy[2];
static unsigned slept, release_after;
static volatile u32 *xhci_extended_capability(xhci_controller_t *x,u32 offset,u32 size) {
 (void)x;assert(offset==0x8000);assert(size==4||size==8);return legacy;
}
static void timer_sleep(u64 ms) {
 assert(ms==1);slept++;
 if(release_after&&slept>=release_after)legacy[0]&=~(1U<<16);
}
'''
checks = r'''
int main(void) {
 xhci_cap_regs_t cap={.hccparams1=0x20000000};
 xhci_controller_t controller={.cap_regs=&cap};
 /* BIOS released the controller at ExitBootServices: OS must still claim it. */
 legacy[0]=1;legacy[1]=0xffffffffU;
 assert(xhci_legacy_handoff(&controller));
 assert(legacy[0]&XHCI_LEGACY_OS_OWNED);assert(slept==0);
 assert(legacy[1]==(XHCI_LEGACY_PRESERVE_BITS|XHCI_LEGACY_SMI_EVENTS));
 /* Firmware responds to the OS's ownership request. */
 legacy[0]=1|XHCI_LEGACY_BIOS_OWNED;release_after=3;
 assert(xhci_legacy_handoff(&controller));
 assert(slept==3&&!(legacy[0]&XHCI_LEGACY_BIOS_OWNED));
 assert(legacy[0]&XHCI_LEGACY_OS_OWNED);
 /* Preserve the existing bounded forced-release fallback. */
 legacy[0]=1|XHCI_LEGACY_BIOS_OWNED;release_after=0;slept=0;
 assert(xhci_legacy_handoff(&controller));
 assert(slept==1000&&!(legacy[0]&XHCI_LEGACY_BIOS_OWNED));
 assert(legacy[0]&XHCI_LEGACY_OS_OWNED);
 /* No legacy capability: no fabricated semaphore writes or sleeps. */
 legacy[0]=2;slept=0;
 assert(xhci_legacy_handoff(&controller));assert(legacy[0]==2&&slept==0);
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-xhci-handoff-') as temp:
    c = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    c.write_text(fixture + constants + '\n' + production + checks)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc', '-std=c11',
                    '-Wall', '-Wextra', '-Werror',
                    '-Iinclude', '-Ikernel/include/usb',
                    str(c), '-o', str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('PASS: xHCI ownership claim, firmware release, bounded fallback, absent legacy capability')
