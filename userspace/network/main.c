#include <mangrove.h>
#include <stdio.h>
#include <string.h>

static void ip(const mg_ipv4_addr_t *a, char out[16]) { if (!mg_ipv4_format(a,out,16)) strcpy(out,"-"); }
static void mac(const u8 m[6]) { printf("%02x:%02x:%02x:%02x:%02x:%02x",m[0],m[1],m[2],m[3],m[4],m[5]); }
static void help(void)
{
    printf("Usage: network [interfaces|routes|neighbors|connections|dns|renew]\n");
    printf("  network              Show current networking.\n");
    printf("  network interfaces   Show network devices.\n");
    printf("  network routes       Show routing information.\n");
    printf("  network neighbors    Show known network neighbors.\n");
    printf("  network connections  Show active network endpoints.\n");
    printf("  network dns          Show resolver configuration.\n");
    printf("  network renew        Renew automatic network configuration.\n");
}
static void overview(void)
{
    mg_net_info_t n; mg_net_interface_info_t i[1]; char a[16],m[16],g[16],d[16];
    if(mg_net_info(&n)<0||mg_net_interfaces(i,sizeof(i))<1){printf("Network unavailable.\n");return;}
    ip(&n.address,a);ip(&n.netmask,m);ip(&n.gateway,g);ip(&n.dns,d);
    printf("Ethernet\n  State       %s\n  Address     %s\n  Netmask     %s\n  Gateway     %s\n  DNS         %s\n  MAC         ",i[0].link_up?"connected":"down",a,m,g,d);mac(i[0].mac);printf("\n  MTU         %u\n",i[0].mtu);
}
static void interfaces(void)
{
    mg_net_interface_info_t i[4]; mg_result_t n=mg_net_interfaces(i,sizeof(i)); char a[16];
    if(n<0){printf("Could not query interfaces.\n");return;} printf("Name  Type  State  IPv4  RX  TX\n");
    for(mg_result_t x=0;x<n&&x<4;x++){ip(&i[x].address,a);printf("%s  %s  %s  %s  %u  %u\n",i[x].name,i[x].type,i[x].link_up?"up":"down",a,(u32)i[x].rx_packets,(u32)i[x].tx_packets);}
}
static void routes(void)
{
    mg_net_route_info_t r[4]; mg_result_t n=mg_net_routes(r,sizeof(r)); char d[16],m[16],g[16];
    if(n<0){printf("Could not query routes.\n");return;} printf("Destination      Gateway        Interface\n");
    for(mg_result_t x=0;x<n&&x<4;x++){ip(&r[x].destination,d);ip(&r[x].netmask,m);ip(&r[x].gateway,g);printf("%s/%s %s %s\n",r[x].is_default?"default":d,r[x].is_default?"":m,r[x].is_default?g:"direct",r[x].interface_name);}
}
static void neighbors(void)
{
    mg_net_neighbor_info_t n[16]; mg_result_t count=mg_net_neighbors(n,sizeof(n)); char a[16];
    if(count<0){printf("Could not query neighbors.\n");return;} printf("Address  Hardware address  State\n");
    for(mg_result_t x=0;x<count&&x<16;x++){ip(&n[x].address,a);printf("%s  ",a);mac(n[x].mac);printf("  %s\n",n[x].state?"known":"unknown");}
}
static void connections(void)
{
    mg_net_connection_info_t c[8]; mg_result_t count=mg_net_connections(c,sizeof(c));
    if(count<0){printf("Could not query connections.\n");return;} printf("Protocol  Local          Remote         State       Process\n");
    for(mg_result_t x=0;x<count&&x<8;x++)printf("%s %u:%u -> %u:%u %u %s\n",c[x].protocol==6?"TCP":"UDP",c[x].local_address.octet[3],c[x].local_port,c[x].remote_address.octet[3],c[x].remote_port,c[x].state,c[x].process_name);
}
int main(int argc,char **argv)
{
    if(argc==1){overview();return 0;}
    if(argc!=2||!strcmp(argv[1],"--help")){if(argc==2)return (help(),0);help();return 1;}
    if(!strcmp(argv[1],"interfaces")){interfaces();return 0;} if(!strcmp(argv[1],"routes")){routes();return 0;} if(!strcmp(argv[1],"neighbors")){neighbors();return 0;} if(!strcmp(argv[1],"connections")){connections();return 0;}
    if(!strcmp(argv[1],"dns")){mg_net_info_t n;char d[16];if(mg_net_info(&n)<0)return 1;ip(&n.dns,d);printf("DNS\n  Server      %s\n  Source      DHCP\n",d);return 0;}
    if(!strcmp(argv[1],"renew")){mg_result_t r=mg_net_renew(MG_NET_TIMEOUT_DEFAULT);printf("%s\n",r<0?"Network renew failed.":"Network configuration renewed.");return r<0;}
    help();return 1;
}
