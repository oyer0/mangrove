#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production xHCI class descriptor probes with controlled USB data."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    pattern = re.compile(r'^(?:static\s+)?[A-Za-z_][^;{}]*?\b' + name +
                         r'\s*\(', re.M | re.S)
    for match in pattern.finditer(source):
        brace = source.find('{', match.end())
        semicolon = source.find(';', match.end())
        if brace >= 0 and (semicolon < 0 or brace < semicolon):
            depth = 1
            end = brace + 1
            while depth:
                depth += (source[end] == '{') - (source[end] == '}')
                end += 1
            return source[match.start():end] + '\n'
    raise ValueError(name)


source = (ROOT / 'kernel/src/usb/xhci_descriptor.c').read_text()
production = ''.join(function(source, name) for name in (
    'usb_descriptor_u16', 'xhci_get_keyboard_endpoint_info',
    'xhci_get_mass_storage_endpoint_info', 'xhci_get_hub_endpoint_info'))

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef uint64_t u64; typedef size_t usize;
typedef enum { XHCI_SUCCESS=0, XHCI_ERR_NO_MEMORY=1, XHCI_ERR_TIMEOUT=2,
 XHCI_ERR_INVALID_PARAM=3, XHCI_ERR_NOT_SUPPORTED=4, XHCI_ERR_HW_HALTED=5,
 XHCI_ERR_CONTROLLER_BAD=6, XHCI_ERR_PORT_RESET_FAIL=7,
 XHCI_ERR_DEVICE_TIMEOUT=8, XHCI_ERR_TRANSACTION=9 } xhci_status_t;
typedef u8 xhci_speed_t;
#define XHCI_SPEED_SUPER 4U
#define XHCI_USB_DESC_TYPE_DEVICE 1U
#define XHCI_USB_DESC_TYPE_CONFIGURATION 2U
#define XHCI_USB_DESC_TYPE_INTERFACE 4U
#define XHCI_USB_DESC_TYPE_ENDPOINT 5U
#define XHCI_USB_DESC_TYPE_HID 0x21U
#define XHCI_USB_DESC_TYPE_HID_REPORT 0x22U
#define XHCI_USB_DESC_TYPE_SS_EP_COMPANION 0x30U
#define XHCI_USB_CLASS_HID 3U
#define XHCI_USB_SUBCLASS_BOOT 1U
#define XHCI_USB_PROTOCOL_KEYBOARD 1U
#define XHCI_USB_CLASS_HUB 9U
#define XHCI_USB_EP_ATTR_TYPE_MASK 3U
#define XHCI_USB_EP_ATTR_INTR 3U
#define XHCI_USB_EP_DIR_IN 0x80U
#define XHCI_USB_EP_ATTR_TYPE_MASK 3U
#define XHCI_USB_REQ_TYPE_DIR_IN 0x80U
#define XHCI_USB_REQ_TYPE_STANDARD 0U
#define XHCI_USB_REQ_RECIP_INTERFACE 1U
#define XHCI_USB_REQ_GET_DESCRIPTOR 6U
#define XHCI_USB_EP_DIR_IN 0x80U
#define XHCI_DEBUG_LOG(...) kprint(__VA_ARGS__)
typedef struct xhci_controller { unsigned unused; } xhci_controller_t;
typedef struct __attribute__((packed)) {
 u8 bLength,bDescriptorType; u16 bcdUSB; u8 bDeviceClass,bDeviceSubClass;
 u8 bDeviceProtocol,bMaxPacketSize0; u16 idVendor,idProduct,bcdDevice;
 u8 iManufacturer,iProduct,iSerialNumber,bNumConfigurations;
} usb_device_descriptor_t;
typedef struct __attribute__((packed)) {
 u8 bLength,bDescriptorType; u16 wTotalLength; u8 bNumInterfaces;
 u8 bConfigurationValue,iConfiguration,bmAttributes,bMaxPower;
} usb_config_descriptor_t;
typedef struct __attribute__((packed)) {
 u8 bLength,bDescriptorType,bInterfaceNumber,bAlternateSetting,bNumEndpoints;
 u8 bInterfaceClass,bInterfaceSubClass,bInterfaceProtocol,iInterface;
} usb_interface_descriptor_t;
typedef struct __attribute__((packed)) {
 u8 bLength,bDescriptorType,bEndpointAddress,bmAttributes;
 u16 wMaxPacketSize; u8 bInterval;
} usb_endpoint_descriptor_t;
typedef struct {
 u8 endpoint_address,interval,config_value,interface_number,interface_protocol;
 u16 max_packet_size; u8 max_burst,mult; u16 bytes_per_interval;
 bool has_ss_companion;
} xhci_hub_endpoint_info_t;
static u8 dma[4096], descriptors[4096];
static xhci_controller_t controller;
static u16 descriptor_length;
static unsigned control_calls, fail_call;
static xhci_status_t fail_result;
static xhci_status_t xhci_control_get_descriptor(xhci_controller_t *x,u8 s,
 u8 type,u8 index,u16 length,uintptr_t phys)
{
 (void)x;(void)s;(void)type;(void)index;(void)phys;
 control_calls++;
 if (control_calls==fail_call) return fail_result;
 if (length>descriptor_length) return XHCI_ERR_TRANSACTION;
 memcpy(dma,descriptors,length);
 return XHCI_SUCCESS;
}
static void *xhci_dma_alloc(usize size,uintptr_t *physical)
{ if(size>sizeof(dma))return NULL;*physical=1;return dma; }
static void xhci_dma_free(void *memory,usize size) { (void)memory;(void)size; }
static void xhci_diag_set_phase(const char *phase) { (void)phase; }
static void xhci_dump_configuration(u8 slot,const u8 *data,u16 length)
{ (void)slot;(void)data;(void)length; }
static void kprint(const char *format,...) { (void)format; }
'''

checks = r'''
static void begin_config(void)
{
 memset(descriptors,0,sizeof(descriptors));
 descriptors[0]=9;descriptors[1]=2;descriptors[4]=0;
 descriptors[5]=1;descriptors[7]=0x80;descriptors[8]=50;
 descriptor_length=9;control_calls=0;fail_call=0;fail_result=XHCI_SUCCESS;
}
static void finish_config(void)
{ descriptors[2]=(u8)descriptor_length;descriptors[3]=(u8)(descriptor_length>>8); }
static void interface_desc(u8 cls,u8 sub,u8 proto,u8 eps)
{
 u8 d[9]={9,4,0,0,eps,cls,sub,proto,0};
 memcpy(descriptors+descriptor_length,d,sizeof(d));descriptor_length+=sizeof(d);
 descriptors[4]++;
}
static void endpoint_desc(u8 address,u8 attributes)
{
 u8 d[7]={7,5,address,attributes,8,0,10};
 memcpy(descriptors+descriptor_length,d,sizeof(d));descriptor_length+=sizeof(d);
}
static xhci_status_t keyboard(void)
{
 u8 ep=0,interval=0,cfg=0,iface=0;u16 pkt=0,report=0;
 return xhci_get_keyboard_endpoint_info(&controller,1,&ep,&pkt,&interval,&cfg,
                                         &iface,&report);
}
static xhci_status_t storage(void)
{
 u8 in=0,out=0,cfg=0;u16 in_pkt=0,out_pkt=0;
 return xhci_get_mass_storage_endpoint_info(&controller,1,&in,&in_pkt,&out,
                                              &out_pkt,&cfg,false);
}
static xhci_status_t hub(u8 speed)
{ xhci_hub_endpoint_info_t info;return xhci_get_hub_endpoint_info(&controller,1,speed,&info); }
int main(void)
{
 /* A complete valid configuration without a supported interface is a quiet
    no-match for all three production probes. */
 begin_config();finish_config();
 assert(keyboard()==XHCI_ERR_NOT_SUPPORTED);
 assert(storage()==XHCI_ERR_NOT_SUPPORTED);
 assert(hub(3)==XHCI_ERR_NOT_SUPPORTED);

 /* Descriptor transport failures survive each probe at both header and full
    configuration reads; they cannot become unsupported-device outcomes. */
 for(unsigned which=0;which<3;which++) for(unsigned call=1;call<=2;call++) {
  begin_config();finish_config();fail_call=call;
  fail_result=call==1?XHCI_ERR_CONTROLLER_BAD:XHCI_ERR_INVALID_PARAM;
  xhci_status_t result=which==0?keyboard():which==1?storage():hub(3);
  assert(result==fail_result);
 }

 /* Malformed descriptor chains and recognized class interfaces missing the
    endpoints required by the bound driver remain visible transaction errors. */
 for(unsigned which=0;which<3;which++) {
  begin_config();
  u8 bad[8]={8,4,0,0,0,3,1,1};
  memcpy(descriptors+descriptor_length,bad,sizeof(bad));
  descriptor_length+=sizeof(bad);finish_config();
  xhci_status_t result=which==0?keyboard():which==1?storage():hub(3);
  assert(result==XHCI_ERR_TRANSACTION);
 }
 begin_config();interface_desc(3,1,1,1);finish_config();
 assert(keyboard()==XHCI_ERR_TRANSACTION);
 begin_config();interface_desc(8,6,0x50,2);finish_config();
 assert(storage()==XHCI_ERR_TRANSACTION);
 begin_config();interface_desc(9,0,0,1);finish_config();
 assert(hub(3)==XHCI_ERR_TRANSACTION);

 /* Each existing supported binding still parses successfully. */
 begin_config();interface_desc(3,1,1,1);endpoint_desc(0x81,3);finish_config();
 assert(keyboard()==XHCI_SUCCESS);
 begin_config();interface_desc(3,1,1,1);endpoint_desc(0x81,3);
 descriptors[descriptor_length++]=1;finish_config();
 assert(keyboard()==XHCI_ERR_TRANSACTION);
 begin_config();interface_desc(8,6,0x50,2);endpoint_desc(0x81,2);
 endpoint_desc(0x01,2);finish_config();assert(storage()==XHCI_SUCCESS);
 begin_config();interface_desc(9,0,0,1);endpoint_desc(0x81,3);
 finish_config();assert(hub(3)==XHCI_SUCCESS);

 /* A SuperSpeed hub interface without its required companion is malformed
    for that supported binding, not an absent driver. */
 assert(hub(XHCI_SPEED_SUPER)==XHCI_ERR_TRANSACTION);
 begin_config();interface_desc(9,0,0,1);endpoint_desc(0x81,3);
 u8 companion[6]={6,0x30,0,0,8,0};
 memcpy(descriptors+descriptor_length,companion,sizeof(companion));
 descriptor_length+=sizeof(companion);finish_config();
 assert(hub(XHCI_SPEED_SUPER)==XHCI_SUCCESS);
 puts("xHCI descriptor probes: no-match is unsupported; transfer, malformed descriptor, and endpoint failures propagate");
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='mangrove-xhci-descriptor-probe-') as temp:
    c = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    c.write_text(fixture + production + checks)
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'gcc',
                    '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(c), '-o', str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
