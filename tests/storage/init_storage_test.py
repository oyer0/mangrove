#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Storage admission uses completed discovery, not instantaneous runtime idleness."""
from pathlib import Path
import subprocess
import sys
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'kernel/src/init.c').read_text()
a = s.index('static init_result_t init_storage(')
b = s.index('\n}', a) + 2
code = r'''
#include <assert.h>
#include <stddef.h>
#include <init.h>
static void *g_xhc;
static init_state_t state;
static unsigned devices;
init_state_t kernel_init_state(init_id_t id) { assert(id == INIT_XHCI); return state; }
static unsigned block_device_count(void) { return devices; }
'''+s[a:b]+r'''
int main(void) {
    const char *reason = NULL;
    g_xhc = (void *)1; devices = 3; state = INIT_READY;
    /* Keyboard/event work may be active now: it cannot undo boot completion. */
    assert(init_storage(&reason) == INIT_RESULT_OK);
    state = INIT_INITIALIZING; assert(init_storage(&reason) == INIT_RESULT_UNAVAILABLE);
    state = INIT_FAILED; assert(init_storage(&reason) == INIT_RESULT_UNAVAILABLE);
    state = INIT_READY; devices = 0; assert(init_storage(&reason) == INIT_RESULT_UNAVAILABLE);
    g_xhc = NULL; devices = 1; assert(init_storage(&reason) == INIT_RESULT_OK);
    devices = 0; assert(init_storage(&reason) == INIT_RESULT_UNAVAILABLE);
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-init-storage-') as temp:
    source=Path(temp)/'test.c'; source.write_text(code); binary=Path(temp)/'test'
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-Ikernel/include','-Iinclude',str(source),'-o',str(binary)],cwd=root,check=True)
    subprocess.run([str(binary)],check=True)
print('Storage init: completed discovery, live runtime work, failed/absent discovery and empty registry passed')
