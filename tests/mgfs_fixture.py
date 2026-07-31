#!/usr/bin/env python3
import shutil, struct, sys

B = 4096
CRC_POLY = 0x42F0E1EBA9EA3693

def p64(a, v): struct.pack_into('<Q', a, 0, v)
def w64(a, o, v): struct.pack_into('<Q', a, o, v)
def crc(data):
    c = 0
    for x in data:
        c ^= x << 56
        for _ in range(8): c = ((c << 1) ^ CRC_POLY) & ((1 << 64) - 1) if c & (1 << 63) else (c << 1) & ((1 << 64) - 1)
    return c
def checksum(a, off, n):
    a[off:off+8] = b'\0'*8
    w64(a, off, crc(a[:n]))
def extent(r, off, logical, physical, count, flags=1):
    for x in (logical, physical, count, flags): w64(r, off, x); off += 8

def record(slot, rid, typ, size=0, flags=0, exts=(), inline=b''):
    r = bytearray(192); w64(r,0,typ); w64(r,8,flags); w64(r,16,rid); w64(r,24,1); w64(r,32,size)
    w64(r,40,len(exts)); inline_n = min(2,len(exts)); w64(r,48,inline_n)
    for i,e in enumerate(exts[:2]): extent(r,64+i*32,*e)
    if len(exts)>2: w64(r,56,exts[2][1])
    r[128:128+len(inline)] = inline
    checksum(r,184,192); return r

def entry(rid, name, flags=1):
    n=name.encode(); z=(32+len(n)+7)&~7; e=bytearray(z); w64(e,0,rid); w64(e,8,len(n)); w64(e,16,flags); e[32:32+len(n)]=n; checksum(e,24,z); return e

def make(base, out):
    a=bytearray(open(base,'rb').read()); data=54
    blocks={}; nextb=data
    def put(data_bytes):
        nonlocal nextb
        b=nextb; nextb+=1; a[b*B:(b+1)*B]=data_bytes.ljust(B,b'\0'); blocks[b]=data_bytes; return b
    root = b''.join(entry(i,n) for i,n in [(3,'empty.txt'),(4,'one-byte.txt'),(5,'inline-56.txt'),(6,'extent-57.txt'),(7,'block-boundary.txt'),(8,'multi-extent.txt'),(9,'extent-list.txt'),(2,'docs')])
    rootb=put(root); docsb=put(entry(10,'nested.txt'))
    e57=put(b'X'*57); bb=put(b'A'*B); bb2=put(b'B'*128)
    m1=put(b'M'*B); put(b'gap'.ljust(B,b'\0')); m2=put(b'N'*B)
    l1=put(b'L'*B); l2=put(b'R'*B); l3=put(b'S'*B); listb=nextb; nextb+=1
    a[listb*B:(listb+1)*B]=b'\0'*B
    w64(a,listb*B+0,0x315458455346474D); w64(a,listb*B+8,9); w64(a,listb*B+24,1)
    extent(a[listb*B:(listb+1)*B],0,0,0,0) if False else None
    lb=bytearray(a[listb*B:(listb+1)*B]); extent(lb,64,2,l3,1); checksum(lb,32,B); a[listb*B:(listb+1)*B]=lb
    recs=[record(0,1,2,len(root),0,[(0,rootb,1,2)]),record(1,2,2, len(entry(10,'nested.txt')),0,[(0,docsb,1,2)]),
          record(2,3,1,0),record(3,4,1,1,1,inline=b'Z'),record(4,5,1,56,1,inline=b'Q'*56),
          record(5,6,1,57,0,[(0,e57,1,1)]),record(6,7,1,B+128,0,[(0,bb,1,1),(1,bb2,1,1)]),
          record(7,8,1,2*B,0,[(0,m1,1,1),(1,m2,1,1)]),record(8,9,1,3*B,0,[(0,l1,1,1),(1,l2,1,1),(2,listb,1,1)]),
          record(9,10,1,6,1,inline=b'nested')]
    table=a[3*B:4*B]
    for i,r in enumerate(recs): table[24+i*192:24+(i+1)*192]=r
    checksum(table,16,B); a[3*B:4*B]=table
    rb=a[2*B:3*B]
    for i in range(len(recs)): rb[24+i//8] |= 1<<(i%8)
    checksum(rb,16,B); a[2*B:3*B]=rb
    ab=a[1*B:2*B]
    for b in range(data,nextb):
        relative = b - data
        ab[24+relative//8] |= 1<<(relative%8)
    checksum(ab,16,B); a[1*B:2*B]=ab
    open(out,'wb').write(a)

if __name__ == '__main__': make(sys.argv[1],sys.argv[2])
