#!/usr/bin/env python3
import sys, struct
from mgfs_fixture import crc, checksum, B

def w(a,o,v): struct.pack_into('<Q',a,o,v)
def fix_record(a, slot):
    ro=3*B+24+slot*192; r=bytearray(a[ro:ro+192]); checksum(r,184,192); a[ro:ro+192]=r
    table=bytearray(a[3*B:4*B]); table[24+slot*192:24+(slot+1)*192]=a[ro:ro+192]; checksum(table,16,B); a[3*B:4*B]=table
def main(src,case,out):
    a=bytearray(open(src,'rb').read()); ro=lambda s:3*B+24+s*192
    if case=='record-checksum': a[ro(5)+32]^=1
    elif case=='inline-state': w(a,ro(3)+40,1); fix_record(a,3)
    elif case=='out-of-bounds': w(a,ro(5)+72,999999); fix_record(a,5)
    elif case=='unsorted': w(a,ro(6)+96,0); fix_record(a,6)
    elif case=='overlap': w(a,ro(6)+104,57); fix_record(a,6)
    elif case=='wrong-type': w(a,ro(5)+88,2); fix_record(a,5)
    elif case=='list-checksum': a[65*B+100]^=1
    elif case=='list-owner': w(a,65*B+8,999); b=bytearray(a[65*B:66*B]); checksum(b,32,B); a[65*B:66*B]=b
    elif case=='list-loop': w(a,65*B+16,65); b=bytearray(a[65*B:66*B]); checksum(b,32,B); a[65*B:66*B]=b
    elif case=='incomplete': w(a,65*B+64+16,0); b=bytearray(a[65*B:66*B]); checksum(b,32,B); a[65*B:66*B]=b
    else: raise SystemExit('unknown case')
    open(out,'wb').write(a)
if __name__=='__main__': main(sys.argv[1],sys.argv[2],sys.argv[3])
