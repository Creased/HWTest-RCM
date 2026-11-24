/*
 * hwtest - the smallest IP stack that can answer "does this chip carry
 * traffic?": an address from DHCP, the gateway's hardware address from
 * ARP, then ICMP echoes to a host out on the internet.
 *
 * It is deliberately a pure state machine over two calls - net_rx for a
 * frame that arrived, net_poll for whatever is due to be sent - so the
 * whole of it runs on the host under test_net.c, where a fake network
 * answers it, rather than only on a console with an AP in the room.
 * Nothing here knows about msgbuf, PCIe or the mailbox.
 *
 * What it is not: a stack. No fragmentation, no retransmission beyond a
 * fixed number of tries, no TCP, one outstanding request at a time, and
 * addresses are kept in host order. Enough to prove a path, not to use
 * one.
 */
#ifndef HWTEST_NET_H
#define HWTEST_NET_H

#ifdef NET_HOST
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef uint8_t  net_u8;
typedef uint16_t net_u16;
typedef uint32_t net_u32;
#else
#include <stdbool.h>
typedef unsigned char      net_u8;
typedef unsigned short     net_u16;
typedef unsigned int       net_u32;
#endif

/* Where the run got to. The mailbox's WIFI_NET_* mirror these. */
#define NET_IDLE     0
#define NET_ARP      1
#define NET_DHCP     2
#define NET_BOUND    3
#define NET_PING     4
#define NET_REACHED  5
#define NET_FAILED   6

#define NET_ETH_IP   0x0800u
#define NET_ETH_ARP  0x0806u

typedef struct {
    /* One frame out, ethernet payload only: the caller puts the header
     * on. Returns 0 when it went. */
    int      (*tx)(void *ctx, const net_u8 *dst, net_u16 ethertype,
                   const net_u8 *body, net_u32 len);
    net_u32  (*ms)(void *ctx);     /* a milliseconds-since-boot clock */
    net_u32  (*rnd)(void *ctx);    /* any 32 bits; the xid and the id */
    void      *ctx;
} net_io_t;

typedef struct {
    const net_io_t *io;
    net_u8   mac[6];
    int      state;

    /* DHCP */
    net_u32  xid, server_id;
    net_u32  ip, mask, gw, dns;
    net_u32  dhcp_t0, dhcp_ms;
    int      dhcp_phase;           /* 0 discover, 1 offered, 2 bound */

    /* ARP for the gateway */
    net_u8   gw_mac[6];
    bool     gw_known;

    /* The echo */
    net_u32  ping_target, ping_id;
    net_u32  ping_sent, ping_recv, ping_best_ms;
    net_u32  ping_t0;              /* when the outstanding one went */
    net_u16  ping_seq;
    bool     ping_outstanding;

    net_u32  t_next;               /* when the next send is due */
    net_u32  tries;
    const char *why;               /* set when state is NET_FAILED */
} net_t;

void net_init(net_t *n, const net_io_t *io, const net_u8 *mac,
              net_u32 ping_target);
/* A frame the chip handed up, from the ethernet header on. */
void net_rx(net_t *n, const net_u8 *fr, net_u32 len);
/* Sends whatever is due and moves the state on; call it often. */
int  net_poll(net_t *n);

#endif /* HWTEST_NET_H */
