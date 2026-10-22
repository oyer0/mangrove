#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production AHCI command/transfer paths with deterministic MMIO and DMA.
W1C register assignments are emulated; controller timing is a fixture. This
verifies error admission and lifetime, not physical-controller recovery.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/storage/ahci.c').read_text()


def function(name):
    candidates = re.finditer(r'^(?:static )?[A-Za-z_][^\n;{}]*\b' + name + r'\s*\(', source, re.M)
    match = next(m for m in candidates if ';' not in source[m.end():source.index('{', m.end())])
    brace = source.index('{', match.end())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


headers = '\n'.join('\n'.join(line for line in (ROOT / path).read_text().splitlines()
                              if not line.startswith('#include') and line != '#pragma once')
                    for path in ('kernel/include/storage/ahci.h', 'kernel/include/storage/ahci_structs.h'))
pre = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef unsigned long long u64; typedef size_t usize;
#define PACKED __attribute__((packed))
typedef struct {void *virt; u64 phys;} dma_buffer_t;
typedef struct {void *driver_data; u32 sector_size; u64 sector_count;} block_device_t;
typedef struct {unsigned ticks;} timer_monotonic_deadline_t;
#define AHCI_OPERATION_TIMEOUT_US 1000000ULL
static void kprint(const char *format, ...) {(void)format;}
'''
model = r'''
static ahci_port_registers_t port;
static ahci_command_header_t headers[32];
static ahci_command_table_t table;
static unsigned mode, commands, recoveries;
static bool clock_fail;
static unsigned char bounce[4096];
static bool timer_monotonic_deadline_start(timer_monotonic_deadline_t *d,u64 us)
{assert(us==AHCI_OPERATION_TIMEOUT_US);d->ticks=0;return !clock_fail;}
static bool timer_monotonic_deadline_expired(timer_monotonic_deadline_t *d)
{
 d->ticks++;
 if (!(port.cmd & AHCI_PORT_CMD_ST)) {
  if (mode!=5) port.cmd &= ~AHCI_PORT_CMD_CR;
  if (!(port.cmd & AHCI_PORT_CMD_FRE) && mode!=6) port.cmd &= ~AHCI_PORT_CMD_FR;
  recoveries++;
 } else if (port.ci && mode!=4 && mode!=5 && mode!=6) {
  commands++;
  assert(table.prdt_entries[0].dba==0x8000 || !headers[0].prdtl);
  assert(table.prdt_entries[0].dbau==0);
  assert(headers[0].ctba==0x9000);
  headers[0].prdbc=headers[0].prdtl ? table.prdt_entries[0].dbc+1 : 0;
  if (mode==1) port.tfd=AHCI_PORT_TFD_ERR;
  if (mode==2) port.is=1U<<30;
  if (mode==3) headers[0].prdbc--;
  if (mode==7) port.serr=1;
  if (mode==8) port.tfd=AHCI_PORT_TFD_DF;
  if (mode==9) {port.is=1U<<30;return false;}
  if (mode==10) port.tfd=AHCI_PORT_TFD_DRQ;
  if (!headers[0].write) memset(bounce,'R',headers[0].prdbc);
  port.ci=0;
 }
 return d->ticks>=3;
}
'''
body = ''.join(function(n) for n in ('ahci_wait_clear','ahci_stop_port','ahci_start_port',
                                     'ahci_isolate','ahci_ready','ahci_execute','ahci_transfer',
                                     'ahci_read','ahci_write','ahci_flush'))
# Hardware W1C writes clear status; ordinary host struct assignment cannot.
assert body.count('port->is = (u32)-1;') == 1
body = body.replace('port->is = (u32)-1;', 'port->is = 0;')
body = body.replace('port->serr = (u32)-1;', 'port->serr = 0;')
# CPU pause is timing-only; the MMIO fixture advances on deadline checks.
body = body.replace('__asm__ volatile("pause");', '__asm__ volatile("" ::: "memory");')
post = r'''
static ahci_device_t seed(unsigned m) {
 memset(&port,0,sizeof(port));memset(headers,0,sizeof(headers));memset(&table,0,sizeof(table));
 memset(bounce,0,sizeof(bounce));mode=m;commands=recoveries=0;clock_fail=false;
 port.cmd=AHCI_PORT_CMD_ST|AHCI_PORT_CMD_FRE|AHCI_PORT_CMD_CR|AHCI_PORT_CMD_FR;
 return (ahci_device_t){.port=&port,.command_list={headers,0x7000},
 .command_table={&table,0x9000},.transfer={bounce,0x8000},.lba48=true,.flush_supported=true};
}
int main(void) {
 unsigned char input[9000],out[9000];memset(input,'W',sizeof(input));
 ahci_device_t a=seed(0);block_device_t b={&a,512,1024};
 assert(ahci_stop_port(&port));assert(ahci_start_port(&port));
 port.cmd |= AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR;
 assert(ahci_write(&b,5,17,input)&&commands==3&&!a.isolated);
 assert(!memcmp(bounce,input+8192,512));
 commands=0;memset(out,0,sizeof(out));assert(ahci_read(&b,5,17,out)&&commands==3);
 for(unsigned i=0;i<17*512;i++)assert(out[i]=='R');
 assert(ahci_flush(&b));assert(!headers[0].prdtl&&!headers[0].prdbc);
 a.flush_supported=false;assert(!ahci_flush(&b));
 a=seed(0);a.lba48=false;assert(ahci_write(&b,12,1,input));
 fis_reg_h2d_t *fis=(fis_reg_h2d_t *)table.command_fis;assert(fis->command_register==0xca);
 assert(!ahci_write(&b,1023,2,input));assert(!ahci_write(&b,0,0,input));
 b.sector_size=4096;assert(!ahci_read(&b,0,1,out));b.sector_size=512;
 for(unsigned failure=1;failure<=10;failure++) {
  a=seed(failure);memset(out,0x5a,sizeof(out));assert(!ahci_read(&b,5,1,out));
  assert(a.isolated);assert(out[0]==0x5a);assert(a.quiesced==(failure!=5&&failure!=6));
  unsigned char retained[4096];memcpy(retained,bounce,4096);
  unsigned before=commands;assert(!ahci_write(&b,5,1,input));assert(!ahci_flush(&b));
  assert(commands==before&&!memcmp(retained,bounce,4096));
  assert(a.transfer.virt==bounce&&a.command_table.virt==&table&&a.command_list.virt==headers);
 }
 a=seed(0);port.ci=1;assert(!ahci_write(&b,1,1,input)&&a.isolated&&commands==0);
 a=seed(0);port.sact=1;assert(!ahci_write(&b,1,1,input)&&a.isolated&&commands==0);
 a=seed(0);port.tfd=AHCI_PORT_TFD_BSY;assert(!ahci_write(&b,1,1,input)&&a.isolated&&commands==0);
 a=seed(0);clock_fail=true;assert(!ahci_write(&b,1,1,input)&&commands==0);
 puts("PASS: AHCI completion/status/short DMA, bounded timeout, CR/FR recovery failures, isolation, retained DMA, caller-buffer safety, batching and flush");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-ahci-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(pre + headers + model + body + post)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'cc', '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
