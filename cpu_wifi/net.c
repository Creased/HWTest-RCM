/*
 * hwtest - ARP, DHCP and ICMP echo, enough to prove a chip carries
 * traffic. See net.h for what this is and is not.
 *
 * Everything on the wire is big-endian; everything in net_t is host
 * order, converted at the edges, so nothing in the middle has to keep
 * track of which it is holding.
 */
#include "net.h"

#define DHCP_DISCOVER   1
#define DHCP_OFFER      2
#define DHCP_REQUEST    3
#define DHCP_ACK        5

#define RETRY_MS        1200u
#define TRIES_MAX       4u
#define PINGS           4u
#define PING_WAIT_MS    1000u

static const net_u8 BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- the wire's byte order ------------------------------------------ */

static void put16be(net_u8 *p, net_u16 v) { p[0] = (net_u8)(v >> 8); p[1] = (net_u8)v; }
static void put32be(net_u8 *p, net_u32 v)
{
    p[0] = (net_u8)(v >> 24); p[1] = (net_u8)(v >> 16);
    p[2] = (net_u8)(v >> 8);  p[3] = (net_u8)v;
}
static net_u16 get16be(const net_u8 *p) { return (net_u16)((p[0] << 8) | p[1]); }
static net_u32 get32be(const net_u8 *p)
{
    return ((net_u32)p[0] << 24) | ((net_u32)p[1] << 16) |
           ((net_u32)p[2] << 8) | p[3];
}

/* The internet checksum of 16-bit words, ones' complement (RFC 1071). */
static net_u16 cksum(const net_u8 *p, net_u32 len, net_u32 seed)
{
    net_u32 sum = seed;
    while (len > 1) {
        sum += get16be(p);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (net_u32)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (net_u16)~sum;
}

/* ---- building ------------------------------------------------------- */

/* An IPv4 header into buf, 20 bytes, with the checksum filled in.
 * total is the whole datagram, header included. */
static void ip_hdr(net_u8 *b, net_u8 proto, net_u32 src, net_u32 dst,
                   net_u16 total, net_u16 ident)
{
    b[0] = 0x45;                       /* v4, 5 words of header */
    b[1] = 0;
    put16be(b + 2, total);
    put16be(b + 4, ident);
    put16be(b + 6, 0);                 /* no fragmentation */
    b[8] = 64;                         /* TTL */
    b[9] = proto;
    put16be(b + 10, 0);
    put32be(b + 12, src);
    put32be(b + 16, dst);
    put16be(b + 10, cksum(b, 20, 0));
}

static int send_arp_request(net_t *n, net_u32 target)
{
    net_u8 p[28];
    put16be(p + 0, 1);                 /* ethernet */
    put16be(p + 2, 0x0800);            /* looking for an IPv4 address */
    p[4] = 6;
    p[5] = 4;
    put16be(p + 6, 1);                 /* request */
    for (int i = 0; i < 6; i++)
        p[8 + i] = n->mac[i];
    put32be(p + 14, n->ip);
    for (int i = 0; i < 6; i++)
        p[18 + i] = 0;
    put32be(p + 24, target);
    return n->io->tx(n->io->ctx, BCAST, NET_ETH_ARP, p, sizeof(p));
}

/* DISCOVER or REQUEST. Both are a BOOTP request with a few options; the
 * difference is option 53 and, for REQUEST, which address is asked for. */
static int send_dhcp(net_t *n, net_u8 kind)
{
    net_u8 p[20 + 8 + 300];
    net_u8 *ip = p, *udp = p + 20, *bp = p + 28;
    net_u32 blen;
    for (net_u32 i = 0; i < sizeof(p); i++)
        p[i] = 0;

    bp[0] = 1;                         /* BOOTREQUEST */
    bp[1] = 1;                         /* ethernet */
    bp[2] = 6;                         /* address length */
    put32be(bp + 4, n->xid);
    /* The broadcast flag stays clear, and that is the whole reason this
     * works on a real AP. A broadcast answer is encrypted with the group
     * key, and this station never installs one - the group key arrives
     * wrapped in message 3 and is deliberately left alone - so a
     * broadcast offer is a frame the chip throws away before we see it.
     * Clear, the server unicasts to our hardware address, under the
     * pairwise key we do have. */
    put16be(bp + 10, 0);
    for (int i = 0; i < 6; i++)
        bp[28 + i] = n->mac[i];
    put32be(bp + 236, 0x63825363u);    /* the magic cookie */

    net_u32 o = 240;
    bp[o++] = 53; bp[o++] = 1; bp[o++] = kind;
    if (kind == DHCP_REQUEST) {
        bp[o++] = 50; bp[o++] = 4; put32be(bp + o, n->ip); o += 4;
        bp[o++] = 54; bp[o++] = 4; put32be(bp + o, n->server_id); o += 4;
    }
    bp[o++] = 55; bp[o++] = 3; bp[o++] = 1; bp[o++] = 3; bp[o++] = 6;
    bp[o++] = 255;
    while (o & 3)
        bp[o++] = 0;
    blen = o;

    put16be(udp + 0, 68);              /* bootpc */
    put16be(udp + 2, 67);              /* bootps */
    put16be(udp + 4, (net_u16)(8 + blen));
    put16be(udp + 6, 0);               /* no UDP checksum: allowed on IPv4 */
    ip_hdr(ip, 17, 0, 0xFFFFFFFFu, (net_u16)(20 + 8 + blen), 0);
    return n->io->tx(n->io->ctx, BCAST, NET_ETH_IP, p,
                     (net_u32)(20 + 8 + blen));
}

static int send_ping(net_t *n)
{
    net_u8 p[20 + 8 + 32];
    net_u8 *ip = p, *ic = p + 20;
    for (net_u32 i = 0; i < sizeof(p); i++)
        p[i] = 0;
    ic[0] = 8;                         /* echo request */
    ic[1] = 0;
    put16be(ic + 4, (net_u16)n->ping_id);
    put16be(ic + 6, n->ping_seq);
    for (net_u32 i = 0; i < 32; i++)
        ic[8 + i] = (net_u8)('a' + (i % 26));
    put16be(ic + 2, cksum(ic, 8 + 32, 0));
    ip_hdr(ip, 1, n->ip, n->ping_target, (net_u16)(20 + 8 + 32),
           (net_u16)n->ping_seq);
    return n->io->tx(n->io->ctx, n->gw_mac, NET_ETH_IP, p, sizeof(p));
}

/* ---- receiving ------------------------------------------------------ */

static void rx_arp(net_t *n, const net_u8 *a, net_u32 len)
{
    if (len < 28 || get16be(a + 6) != 2)        /* a reply */
        return;
    if (get32be(a + 14) != n->gw)
        return;
    for (int i = 0; i < 6; i++)
        n->gw_mac[i] = a[8 + i];
    n->gw_known = true;
}

static void rx_dhcp(net_t *n, const net_u8 *bp, net_u32 len)
{
    if (len < 240 || bp[0] != 2 || get32be(bp + 4) != n->xid)
        return;
    if (get32be(bp + 236) != 0x63825363u)
        return;

    net_u8 kind = 0;
    net_u32 mask = 0, gw = 0, dns = 0, sid = 0;
    for (net_u32 o = 240; o < len && bp[o] != 255; ) {
        if (bp[o] == 0) { o++; continue; }
        if (o + 2 > len)
            break;
        net_u8 t = bp[o], l = bp[o + 1];
        if (o + 2 + l > len)
            break;
        const net_u8 *v = bp + o + 2;
        if (t == 53 && l >= 1) kind = v[0];
        else if (t == 1 && l >= 4) mask = get32be(v);
        else if (t == 3 && l >= 4) gw = get32be(v);
        else if (t == 6 && l >= 4) dns = get32be(v);
        else if (t == 54 && l >= 4) sid = get32be(v);
        o += 2u + l;
    }

    if (kind == DHCP_OFFER && n->dhcp_phase == 0) {
        n->ip = get32be(bp + 16);      /* yiaddr */
        n->server_id = sid;
        if (mask) n->mask = mask;
        if (gw) n->gw = gw;
        if (dns) n->dns = dns;
        n->dhcp_phase = 1;
        n->tries = 0;
        n->t_next = 0;                 /* the request goes out at once */
    } else if (kind == DHCP_ACK && n->dhcp_phase == 1) {
        n->ip = get32be(bp + 16);
        if (mask) n->mask = mask;
        if (gw) n->gw = gw;
        if (dns) n->dns = dns;
        n->dhcp_phase = 2;
        n->dhcp_ms = n->io->ms(n->io->ctx) - n->dhcp_t0;
        n->state = NET_BOUND;
        n->tries = 0;
        n->t_next = 0;
    }
}

static void rx_icmp(net_t *n, const net_u8 *ic, net_u32 len, net_u32 from)
{
    if (len < 8 || ic[0] != 0 || from != n->ping_target)
        return;                        /* only an echo reply from our host */
    if (get16be(ic + 4) != (net_u16)n->ping_id)
        return;
    net_u32 rtt = n->io->ms(n->io->ctx) - n->ping_t0;
    n->ping_recv++;
    if (!n->ping_best_ms || rtt < n->ping_best_ms)
        n->ping_best_ms = rtt;
    n->ping_outstanding = false;
    n->state = NET_REACHED;
}

void net_rx(net_t *n, const net_u8 *fr, net_u32 len)
{
    if (len < 14)
        return;
    /* Ours or a broadcast, and nothing else: the chip hands up what it
     * hands up, and a frame for somebody else only wastes the parse. */
    bool mine = true, bcast = true;
    for (int i = 0; i < 6; i++) {
        if (fr[i] != n->mac[i]) mine = false;
        if (fr[i] != 0xFF) bcast = false;
    }
    if (!mine && !bcast)
        return;

    net_u16 et = get16be(fr + 12);
    const net_u8 *p = fr + 14;
    net_u32 plen = len - 14;
    /* The LLC/SNAP form some firmware hands up instead of ethernet II. */
    if (et <= 1500 && plen > 8 && p[0] == 0xAA && p[1] == 0xAA &&
        p[2] == 0x03 && p[3] == 0 && p[4] == 0 && p[5] == 0) {
        et = get16be(p + 6);
        p += 8;
        plen -= 8;
    }

    if (et == NET_ETH_ARP) {
        rx_arp(n, p, plen);
        return;
    }
    if (et != NET_ETH_IP || plen < 20 || (p[0] >> 4) != 4)
        return;

    net_u32 ihl = (net_u32)(p[0] & 0xF) * 4u;
    net_u32 total = get16be(p + 2);
    if (ihl < 20 || total < ihl || total > plen)
        return;
    net_u8 proto = p[9];
    net_u32 src = get32be(p + 12);
    const net_u8 *pay = p + ihl;
    net_u32 paylen = total - ihl;

    if (proto == 17 && paylen >= 8) {           /* UDP */
        if (get16be(pay + 2) != 68)             /* to the DHCP client port */
            return;
        net_u32 ulen = get16be(pay + 4);
        if (ulen < 8 || ulen > paylen)
            ulen = paylen;
        rx_dhcp(n, pay + 8, ulen - 8);
    } else if (proto == 1) {                    /* ICMP */
        rx_icmp(n, pay, paylen, src);
    }
}

/* ---- the state machine ---------------------------------------------- */

void net_init(net_t *n, const net_io_t *io, const net_u8 *mac,
              net_u32 ping_target)
{
    net_u8 *z = (net_u8 *)n;
    for (net_u32 i = 0; i < sizeof(*n); i++)
        z[i] = 0;
    n->io = io;
    for (int i = 0; i < 6; i++)
        n->mac[i] = mac[i];
    n->ping_target = ping_target;
    n->xid = io->rnd(io->ctx);
    n->ping_id = io->rnd(io->ctx) & 0xFFFFu;
    n->state = NET_DHCP;
    n->dhcp_t0 = io->ms(io->ctx);
    n->t_next = 0;
}

static void fail(net_t *n, const char *why)
{
    n->state = NET_FAILED;
    n->why = why;
}

int net_poll(net_t *n)
{
    net_u32 now = n->io->ms(n->io->ctx);

    switch (n->state) {
    case NET_DHCP:
        if (now < n->t_next)
            break;
        if (n->tries >= TRIES_MAX) {
            fail(n, n->dhcp_phase ? "no DHCP ack" : "no DHCP offer");
            break;
        }
        n->tries++;
        n->t_next = now + RETRY_MS;
        (void)send_dhcp(n, n->dhcp_phase ? DHCP_REQUEST : DHCP_DISCOVER);
        break;

    case NET_BOUND:
        /* An address, and now somewhere to send through. A gateway the
         * lease did not name is a network this cannot route off, so the
         * echo goes no further than saying so. */
        if (!n->gw) {
            fail(n, "the lease named no gateway");
            break;
        }
        if (n->gw_known) {
            n->state = NET_PING;
            n->tries = 0;
            n->t_next = 0;
            break;
        }
        if (now < n->t_next)
            break;
        if (n->tries >= TRIES_MAX) {
            fail(n, "the gateway did not answer ARP");
            break;
        }
        n->tries++;
        n->t_next = now + RETRY_MS;
        (void)send_arp_request(n, n->gw);
        break;

    case NET_PING:
        if (n->ping_outstanding && now - n->ping_t0 < PING_WAIT_MS)
            break;
        n->ping_outstanding = false;
        if (n->ping_sent >= PINGS) {
            if (!n->ping_recv)
                fail(n, "no echo reply came back");
            break;
        }
        n->ping_seq++;
        n->ping_sent++;
        n->ping_t0 = now;
        n->ping_outstanding = true;
        (void)send_ping(n);
        break;

    default:
        break;
    }
    return n->state;
}
