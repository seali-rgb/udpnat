#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <errno.h>

#include <linux/if.h>
#include <linux/if_tun.h>

#include <fcntl.h>

#include "tunudp.h"

int tunudp_debug = 0;

// http://www.microhowto.info/howto/calculate_an_internet_protocol_checksum_in_c.html#idp22656
static uint16_t ip_checksum(void* vdata,size_t length) {
    // Cast the data pointer to one that can be indexed.
    char* data=(char*)vdata;

    // Initialise the accumulator.
    uint32_t acc=0xffff;

    // Handle complete 16-bit blocks.
    size_t i;
    for (i=0;i+1<length;i+=2) {
        uint16_t word;
        memcpy(&word,data+i,2);
        acc+=ntohs(word);
        if (acc>0xffff) {
            acc-=0xffff;
        }
    }

    // Handle any partial block at the end of the data.
    if (length&1) {
        uint16_t word=0;
        memcpy(&word,data+length-1,1);
        acc+=ntohs(word);
        if (acc>0xffff) {
            acc-=0xffff;
        }
    }

    // Return the checksum in network byte order.
    return htons(~acc);
}


int open_tun(const char* devnettun, const char* devname, int extraflags) {
    int tundev = open(devnettun, O_RDWR);
    if (tundev == -1) { perror("open(tun)"); return -1; }
    
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI | IFF_NOARP | extraflags;
    strncpy(ifr.ifr_name, devname, IFNAMSIZ);
    if(-1 == ioctl(tundev, TUNSETIFF, (void *) &ifr)) {
        perror("ioctl(TUNSETIFF)");
        close(tundev);
        return -1;
    }
    return tundev;
}

int receive_udp_packet_from_tun(
            int tundev, char *buf, size_t bufsize,
            struct sockaddr_in *src, struct sockaddr_in *dst, char **data)
{
    memset(buf, 0, bufsize);
    int ret = read(tundev, buf, bufsize);
    if (tunudp_debug) {
        fprintf(stderr, "ret=%4d ", ret);
        if (ret != -1) {
            int i;
            for(i=0; i<ret; ++i) {
                fprintf(stderr, "%02X", (int)buf[i]);
            }
        }
        fprintf(stderr, "\n");
    }
    
    struct ip* hi = (struct ip*)buf;
    struct udphdr *hu = (struct udphdr*)(buf + hi->ip_hl*4);
    
    size_t dataoffset = hi->ip_hl * 4 + sizeof *hu;
    
    if (tunudp_debug) {
        fprintf(stderr, "ip_hl=%d ip_v=%d ip_p=%d ip_len=%d ip_ttl=%d ",
                hi->ip_hl,
                hi->ip_v,
                hi->ip_p,
                ntohs(hi->ip_len),
                hi->ip_ttl);
        fprintf(stderr, "%s:%d -> ",
                inet_ntoa(hi->ip_src),
                ntohs(hu->uh_sport));
        fprintf(stderr, "%s:%d len=%d\n",
                inet_ntoa(hi->ip_dst),
                ntohs(hu->uh_dport),
                ntohs(hu->uh_ulen));
        fprintf(stderr, "Content: ");
        int i;
        for (i=hi->ip_hl*4 + sizeof *hu; i<ret; ++i) {
            fprintf(stderr, "%02X", (int)buf[i]);
        }
        fprintf(stderr, "\n");
    }
    
    if (hi->ip_v != 4) { errno=EAGAIN; return -1;}
    if (hi->ip_p != 17) { errno=EAGAIN; return -1;}
    if (dataoffset > ret) { errno=EAGAIN; return -1;}
    
    if (src) {
        memset(src, 0, sizeof(*src));
        src->sin_family = AF_INET;
        src->sin_addr = hi->ip_src;
        src->sin_port = hu->uh_sport;
    }
    if (dst) {
        memset(dst, 0, sizeof(*dst));
        dst->sin_family = AF_INET;
        dst->sin_addr = hi->ip_dst;
        dst->sin_port = hu->uh_dport;
    }
    
    *data = buf + dataoffset;
    return ret - dataoffset;
}

int send_udp_packet_to_tun(
            int tundev, char *buf_reply, size_t bufsize,
            const struct sockaddr_in *src, const struct sockaddr_in *dst, 
            const char *data, size_t datalen, uint16_t *ip_id)
{
    size_t reqsize = 20 + sizeof(struct udphdr) + datalen;
    if (bufsize < reqsize) { 
        errno = EMSGSIZE;
        return -1; 
    }
    if (!src || !dst || src->sin_family != AF_INET || dst->sin_family != AF_INET) {
        errno = ENOTSUP;
        return -1;
    }
    
    struct ip* hi_r = (struct ip*)buf_reply;
    hi_r->ip_v = 4;
    hi_r->ip_p = 17;
    hi_r->ip_hl = 5;
    hi_r->ip_tos = 0;
    hi_r->ip_len = htons(reqsize);
    hi_r->ip_id = htons((*ip_id)++);
    hi_r->ip_off = htons(0);
    hi_r->ip_ttl=63;
    hi_r->ip_sum = htons(0);
    hi_r->ip_src = src->sin_addr;
    hi_r->ip_dst = dst->sin_addr;
    struct udphdr *hu_r = (struct udphdr*)(buf_reply + hi_r->ip_hl*4);
    hu_r->uh_sport = src->sin_port;
    hu_r->uh_dport = dst->sin_port;
    hu_r->uh_ulen = htons(8+datalen);
    hu_r->uh_sum = 0;
    char *data_ = buf_reply + hi_r->ip_hl*4 + sizeof(*hu_r);
    
    hi_r->ip_sum = ip_checksum(hi_r, 20);
    
    memcpy(data_, data, datalen);
    
    return write(tundev, buf_reply, reqsize);
}

/*
 // 450000218EB940003F|11|F8680|A000000|55555555|CFB8|4444|000DA8A6|414243440A
    
    // $1 = {ip_hl = 5, ip_v = 4, ip_tos = 0 '\000', ip_len = 8192, ip_id = 51635, ip_off = 64, ip_ttl = 63 '?', ip_p = 17 '\021', ip_sum = 22995,  ip_src = {s_addr = 10}, ip_dst = {s_addr = 1431655765}}
// $2 = {{{uh_sport = 47311, uh_dport = 17476, uh_ulen = 3072, uh_sum = 58034}, {source = 47311, dest = 17476, len = 3072, check = 58034}}} 
 
struct ip {
    unsigned int ip_hl:4;
    unsigned int ip_v:4;
    uint8_t ip_tos;
    uint16_t ip_len;
    uint16_t ip_id;
    uint16_t ip_off;
    uint8_t ip_ttl;
    uint8_t ip_p;
    uint16_t ip_sum;
    struct in_addr ip_src, ip_dst;
};

struct udphdr {
    uint16_t uh_sport;
    uint16_t uh_dport;
    uint16_t uh_ulen;
    uint16_t uh_sum;
};
*/
