/*
 * net.c against a fake network, on the host: a DHCP server that offers
 * and acks, a gateway that answers ARP, and a host out on the internet
 * that answers echoes. CI runs it, so the stack that goes on the console
 * has been all the way through its own state machine before it meets a
 * real AP.
 *
 *   cc -DNET_HOST -o test_net host_tools/test_net.c cpu_wifi/net.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../cpu_wifi/net.h"

static int fails;
#define CHECK(c, what) do {                                              \
        if (c) printf("ok   %s\n", what);                                \
        else { printf("FAIL %s\n", what); fails++; }                     \
    } while (0)

/* ---- the fake network ----------------------------------------------- */

#define OUR_IP    0xC0A80165u          /* 192.168.1.101 */
#define SRV_IP    0xC0A80101u          /* 192.168.1.1, also the gateway */
#define MASK      0xFFFFFF00u
#define DNS_IP    0x08080808u
#define PING_IP   0x08080808u

static const uint8_t STA_MAC[6] = { 0x00, 0x90, 0x4C, 0x1A, 0xA0, 0x01 };
static const uint8_t GW_MAC[6]  = { 0x3A, 0x07, 0x16, 0x08, 0x36, 0x70 };

typedef struct {
    net_t  *n;
    uint32_t now;
    int      arp_seen, discover_seen, request_seen, echo_seen;
    int      drop_offer;               /* make the first discover go missing */
    int      no_echo;                  /* a gateway that routes nowhere */
    int      no_gw_option;             /* a lease with no router in it */
} world_t;

static uint16_t g16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t g32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void p32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t ck(const uint8_t *p, uint32_t len)
{
    uint32_t s = 0;
    while (len > 1) { s += g16(p); p += 2; len -= 2; }
    if (len) s += (uint32_t)p[0] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}
static void iph(uint8_t *b, uint8_t proto, uint32_t src, uint32_t dst,
                uint16_t total)
{
    memset(b, 0, 20);
    b[0] = 0x45;
    p16(b + 2, total);
    b[8] = 64;
    b[9] = proto;
    p32(b + 12, src);
    p32(b + 16, dst);
    p16(b + 10, ck(b, 20));
}

/* Hand a frame back to the stack, ethernet header and all. */
static void deliver(world_t *w, const uint8_t *dst, uint16_t et,
                    const uint8_t *body, uint32_t len)
{
    uint8_t fr[1600];
    memcpy(fr, dst, 6);
    memcpy(fr + 6, GW_MAC, 6);
    p16(fr + 12, et);
    memcpy(fr + 14, body, len);
    net_rx(w->n, fr, 14 + len);
}

static void answer_dhcp(world_t *w, const uint8_t *bp, uint8_t kind)
{
    uint8_t pkt[20 + 8 + 300], *ip = pkt, *udp = pkt + 20, *b = pkt + 28;
    memset(pkt, 0, sizeof(pkt));
    b[0] = 2;                          /* BOOTREPLY */
    b[1] = 1;
    b[2] = 6;
    memcpy(b + 4, bp + 4, 4);          /* the xid back */
    p32(b + 16, OUR_IP);               /* yiaddr */
    p32(b + 20, SRV_IP);               /* siaddr */
    memcpy(b + 28, STA_MAC, 6);
    p32(b + 236, 0x63825363u);
    uint32_t o = 240;
    b[o++] = 53; b[o++] = 1; b[o++] = kind;
    b[o++] = 54; b[o++] = 4; p32(b + o, SRV_IP); o += 4;
    b[o++] = 1;  b[o++] = 4; p32(b + o, MASK);   o += 4;
    if (!w->no_gw_option) {
        b[o++] = 3; b[o++] = 4; p32(b + o, SRV_IP); o += 4;
    }
    b[o++] = 6;  b[o++] = 4; p32(b + o, DNS_IP);  o += 4;
    b[o++] = 255;
    uint32_t blen = o;
    p16(udp + 0, 67);
    p16(udp + 2, 68);
    p16(udp + 4, (uint16_t)(8 + blen));
    iph(ip, 17, SRV_IP, 0xFFFFFFFFu, (uint16_t)(20 + 8 + blen));
    deliver(w, STA_MAC, NET_ETH_IP, pkt, 20 + 8 + blen);
}

static void answer_arp(world_t *w, const uint8_t *a)
{
    uint8_t p[28];
    memset(p, 0, sizeof(p));
    p16(p + 0, 1);
    p16(p + 2, 0x0800);
    p[4] = 6; p[5] = 4;
    p16(p + 6, 2);                     /* reply */
    memcpy(p + 8, GW_MAC, 6);
    p32(p + 14, SRV_IP);
    memcpy(p + 18, STA_MAC, 6);
    p32(p + 24, g32(a + 14));
    deliver(w, STA_MAC, NET_ETH_ARP, p, sizeof(p));
}

static void answer_echo(world_t *w, const uint8_t *ic, uint32_t len)
{
    uint8_t pkt[20 + 128];
    if (len > 108)
        len = 108;
    iph(pkt, 1, PING_IP, OUR_IP, (uint16_t)(20 + len));
    memcpy(pkt + 20, ic, len);
    pkt[20] = 0;                       /* echo reply */
    p16(pkt + 22, 0);
    p16(pkt + 22, ck(pkt + 20, len));
    deliver(w, STA_MAC, NET_ETH_IP, pkt, 20 + len);
}

/* What the stack sends reaches the fake network here. */
static int w_tx(void *ctx, const net_u8 *dst, net_u16 et,
                const net_u8 *body, net_u32 len)
{
    world_t *w = (world_t *)ctx;
    (void)dst;
    if (et == NET_ETH_ARP) {
        w->arp_seen++;
        if (g16(body + 6) == 1)
            answer_arp(w, body);
        return 0;
    }
    if (et != NET_ETH_IP || len < 28)
        return 0;
    uint32_t ihl = (uint32_t)(body[0] & 0xF) * 4u;
    /* The header the stack built must check out, or nothing on a real
     * network would look at it twice. */
    if (ck(body, ihl) != 0) {
        printf("FAIL ip header checksum\n");
        fails++;
    }
    const uint8_t *pay = body + ihl;
    if (body[9] == 17 && g16(pay + 2) == 67) {
        const uint8_t *bp = pay + 8;
        uint8_t kind = 0;
        for (uint32_t o = 240; o < len && bp[o] != 255; ) {
            if (bp[o] == 0) { o++; continue; }
            if (bp[o] == 53) kind = bp[o + 2];
            o += 2u + bp[o + 1];
        }
        if (kind == 1) {
            w->discover_seen++;
            if (w->drop_offer && w->discover_seen == 1)
                return 0;
            answer_dhcp(w, bp, 2);
        } else if (kind == 3) {
            w->request_seen++;
            answer_dhcp(w, bp, 5);
        }
    } else if (body[9] == 1) {
        if (ck(pay, g16(body + 2) - ihl) != 0) {
            printf("FAIL icmp checksum\n");
            fails++;
        }
        w->echo_seen++;
        if (!w->no_echo)
            answer_echo(w, pay, g16(body + 2) - ihl);
    }
    return 0;
}

static net_u32 w_ms(void *ctx) { return ((world_t *)ctx)->now; }
static net_u32 w_rnd(void *ctx) { (void)ctx; return 0x12345678u; }

static int run(world_t *w, net_t *n, uint32_t ms_limit)
{
    net_io_t io = { w_tx, w_ms, w_rnd, w };
    w->n = n;
    w->now = 0;
    net_init(n, &io, STA_MAC, PING_IP);
    int st = NET_IDLE;
    for (uint32_t t = 0; t < ms_limit; t += 10) {
        w->now = t;
        st = net_poll(n);
        if (st == NET_REACHED || st == NET_FAILED)
            break;
    }
    return st;
}

int main(void)
{
    net_t n;
    world_t w;

    memset(&w, 0, sizeof(w));
    int st = run(&w, &n, 20000);
    CHECK(st == NET_REACHED, "a clean network gets all the way through");
    CHECK(n.ip == OUR_IP, "the offered address is taken");
    CHECK(n.mask == MASK, "the netmask comes off option 1");
    CHECK(n.gw == SRV_IP, "the gateway comes off option 3");
    CHECK(n.dns == DNS_IP, "the resolver comes off option 6");
    CHECK(w.discover_seen == 1 && w.request_seen == 1,
          "one discover and one request");
    CHECK(n.gw_known && memcmp(n.gw_mac, GW_MAC, 6) == 0,
          "the gateway's hardware address comes off ARP");
    CHECK(n.ping_recv == 1 && n.ping_sent == 1, "one echo, one reply");

    memset(&w, 0, sizeof(w));
    w.drop_offer = 1;
    st = run(&w, &n, 20000);
    CHECK(st == NET_REACHED && w.discover_seen == 2,
          "a lost discover is sent again");

    memset(&w, 0, sizeof(w));
    w.no_echo = 1;
    st = run(&w, &n, 30000);
    CHECK(st == NET_FAILED && n.ping_sent == 4,
          "a gateway that routes nowhere fails after four tries");
    CHECK(n.why && strstr(n.why, "echo"), "and says so");

    memset(&w, 0, sizeof(w));
    w.no_gw_option = 1;
    st = run(&w, &n, 20000);
    CHECK(st == NET_FAILED && n.ip == OUR_IP,
          "a lease with no router binds but goes no further");

    printf(fails ? "%d FAILED\n" : "all OK\n", fails);
    return fails ? 1 : 0;
}
