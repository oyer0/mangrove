#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define B 4096ULL
#define REC 192ULL
#define RPB 21ULL
#define BB 32576ULL
#define BH 24ULL
#define MAGIC "MGFSv1\0\0"
#define CRC_POLY 0x42F0E1EBA9EA3693ULL
#define EXT_DATA 1ULL
#define EXT_DIR 2ULL
#define EL_MAGIC 0x315458455346474DULL
#define EL_HDR 64ULL
#define EL_PER 126ULL

typedef uint64_t u64; typedef uint8_t u8;
typedef struct { u64 ab,abn,rb,rbn,rt,rtn,rc,data,dn,total,root; } fs_t;
typedef struct { u64 block,record,kind; } owner_t;
static FILE *image; static fs_t fs; static u8 *record_bits; static u8 *seen;
static owner_t *owners; static size_t owner_count,owner_capacity;
static int errors,warnings,record_count,directory_count,reachable_count;

static u64 le64(const u8 *p){u64 v=0;for(int i=0;i<8;i++)v|=(u64)p[i]<<(i*8);return v;}
static u64 crc64(const u8 *p,size_t n){u64 c=0;for(size_t i=0;i<n;i++){c^=(u64)p[i]<<56;for(int j=0;j<8;j++)c=(c>>63)?(c<<1)^CRC_POLY:c<<1;}return c;}
static void errorf(const char *fmt,...){va_list ap;va_start(ap,fmt);fputs("error: ",stderr);vfprintf(stderr,fmt,ap);fputc('\n',stderr);va_end(ap);errors++;}
static void warningf(const char *fmt,...){va_list ap;va_start(ap,fmt);fputs("warning: ",stderr);vfprintf(stderr,fmt,ap);fputc('\n',stderr);va_end(ap);warnings++;}
static int read_block(u64 block,u8 *out){if(block>UINT64_MAX/B)return 0;if(fseek(image,(long)(block*B),SEEK_SET))return 0;return fread(out,1,B,image)==B;}
static int checked_crc(u8 *data,u64 offset,u64 length){u8 *copy=malloc(length);if(!copy)return 0;memcpy(copy,data,length);u64 stored=le64(copy+offset);memset(copy+offset,0,8);int ok=stored==crc64(copy,length);free(copy);return ok;}
static int bit(u64 n){return (record_bits[BH+n/8]>>(n%8))&1;}
static int add(u64 a,u64 b,u64 *r){if(a>UINT64_MAX-b)return 0;*r=a+b;return 1;}
static int own(u64 block,u64 record,u64 kind){for(size_t i=0;i<owner_count;i++)if(owners[i].block==block){if(owners[i].record!=record||owners[i].kind!=kind)errorf("duplicate block");return 1;}if(owner_count==owner_capacity){size_t n=owner_capacity?owner_capacity*2:256;owner_t *p=realloc(owners,n*sizeof(*p));if(!p)return 0;owners=p;owner_capacity=n;}owners[owner_count++]=(owner_t){block,record,kind};return 1;}
static int slot_for(u64 id,u64 *slot){u8 block[B];for(u64 s=0;s<fs.rc;s++)if(bit(s)){if(!read_block(fs.rt+s/RPB,block))return 0;if(le64(block+BH+(s%RPB)*REC+16)==id){*slot=s;return 1;}}return 0;}
static int read_record(u64 id,u8 out[REC]){u64 s;u8 block[B];if(!slot_for(id,&s)||!read_block(fs.rt+s/RPB,block))return 0;memcpy(out,block+BH+(s%RPB)*REC,REC);return checked_crc(out,184,REC)&&le64(out+16)==id;}
static int extents(u64 id,const u8 *r,u64 type,u64 *capacity){u64 inline_count=le64(r+48),total=le64(r+40),next=0,count=0,list=le64(r+56);if(le64(r+8)&1){*capacity=56;return 1;}if(inline_count>2||inline_count>total)return 0;for(u64 i=0;i<inline_count;i++){const u8 *e=r+64+i*32;u64 l=le64(e),p=le64(e+8),n=le64(e+16),f=le64(e+24),end;if(!n||f!=type||l!=next||!add(l,n,&end)||p<fs.data||p-fs.data>=fs.dn||n>fs.dn-(p-fs.data))return 0;for(u64 j=0;j<n;j++)own(p+j,id,1);next=end;count++;}while(list){u8 b[B];if(!read_block(list,b)||list<fs.data||list-fs.data>=fs.dn||le64(b)!=EL_MAGIC||le64(b+8)!=id||le64(b+24)==0||le64(b+24)>EL_PER||!checked_crc(b,32,B))return 0;own(list,id,2);u64 n=le64(b+24);for(u64 i=0;i<n;i++){const u8*e=b+EL_HDR+i*32;u64 l=le64(e),p=le64(e+8),nb=le64(e+16),f=le64(e+24),end;if(count>=total||!nb||f!=type||l!=next||!add(l,nb,&end)||p<fs.data||p-fs.data>=fs.dn||nb>fs.dn-(p-fs.data))return 0;for(u64 j=0;j<nb;j++)own(p+j,id,1);next=end;count++;}list=le64(b+16);}if(count!=total)return 0;*capacity=next;return 1;}
static int walk_dir(u64 id,const u8 *r){u64 size=le64(r+32),off=0;directory_count++;if(!size)return 1;if(le64(r+48)!=1||le64(r+40)<1)return 0;u64 physical=le64(r+72);if(physical<fs.data||physical-fs.data>=fs.dn)return 0;while(off<size){u8 b[B];if(!read_block(physical+off/B,b))return 0;u64 at=off%B,n=le64(b+at+8),len=(32+n+7)&~7ULL;if(!n||n>255||len>288||off+len>size||at+len>B||!checked_crc(b+at,24,len))return 0;if(le64(b+at+16)==1){u64 child=le64(b+at);u8 cr[REC];if(!read_record(child,cr)){errorf("directory Record %"PRIu64" entry at offset %"PRIu64" references free Record %"PRIu64,id,off,child);return 0;}if(!seen[child]){seen[child]=1;reachable_count++;u64 cap=0;if(!extents(child,cr,le64(cr)==2?EXT_DIR:EXT_DATA,&cap))errorf("invalid extents in Record %"PRIu64,child);if(le64(cr)==2&&!walk_dir(child,cr))errorf("invalid directory Record %"PRIu64,child);}}else if(le64(b+at+16)!=2)return 0;off+=len;}return 1;}
static int geometry(u64 n){u64 t=n/320;if(!t)t=1;fs.ab=1;fs.abn=(n+BB-1)/BB;fs.rb=fs.ab+fs.abn;fs.rc=t*21;fs.rbn=(fs.rc+BB-1)/BB;fs.rt=fs.rb+fs.rbn;fs.rtn=t;fs.data=fs.rt+t;fs.dn=n-fs.data;fs.total=n;return fs.data<n;}
int main(int argc,char **argv){u8 b[B];if(argc!=2){fprintf(stderr,"usage: %s <image>\n",argv[0]);return 2;}image=fopen(argv[1],"rb");if(!image){perror("mgfsck");return 2;}if(fseek(image,0,SEEK_END)||ftell(image)<0||(u64)ftell(image)%B){fclose(image);return 2;}u64 blocks=(u64)ftell(image)/B;if(!geometry(blocks)){fclose(image);return 1;}if(!read_block(0,b)||memcmp(b,MAGIC,8)||le64(b+8)!=1||le64(b+16)!=0||le64(b+24)!=200||le64(b+32)!=B||!checked_crc(b,192,200)){errorf("invalid superblock");}fs.root=le64(b+88);if(!fs.root){errorf("invalid root Record ID");}record_bits=calloc(fs.rbn,B);seen=calloc(fs.rc+1,1);if(!record_bits||!seen)return 2;for(u64 i=0;i<fs.rbn;i++){if(!read_block(fs.rb+i,b)||le64(b)!=2||le64(b+8)!=i||!checked_crc(b,16,B))errorf("invalid Record bitmap block");else memcpy(record_bits+i*B,b,B);}for(u64 s=0;s<fs.rc;s++)if(bit(s)){u8 r[REC];record_count++;if(!read_record(s+1,r)){errorf("Record %"PRIu64" checksum or ID invalid",s+1);continue;}u64 cap=0;if(le64(r)!=1&&le64(r)!=2)errorf("Record %"PRIu64" has invalid type",s+1);if(!extents(s+1,r,le64(r)==2?EXT_DIR:EXT_DATA,&cap))errorf("invalid extents in Record %"PRIu64,s+1);if(le64(r)==1&&!(le64(r+8)&1)&&le64(r+32)>cap*B)errorf("Record %"PRIu64" logical size exceeds extents",s+1);}u8 root[REC];if(!read_record(fs.root,root)||le64(root)!=2)errorf("root Record is invalid");else{seen[fs.root]=1;reachable_count=1;walk_dir(fs.root,root);}for(u64 s=0;s<fs.rc;s++)if(bit(s)&&!seen[s+1])warningf("Record %"PRIu64" is unreachable",s+1);printf("MGFS check: %s\nRecords checked: %d\nDirectories checked: %d\nReachable Records: %d\nReferenced blocks: %zu\nErrors: %d\nWarnings: %d\n",argv[1],record_count,directory_count,reachable_count,owner_count,errors,warnings);fclose(image);return errors?1:0;}
