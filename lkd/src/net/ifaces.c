/* ifaces.c: the network interfaces the guest sees (request wine-8), read-only, from the host's real state.
 *
 * Interfaces: the host loopback (lo0 -> "lo", index 1), then every host interface that is UP and RUNNING and
 * carries an IPv4 address or an IPv6 address that is not link-local, in host order. Linux names: Ethernet
 * (IFT_ETHER, en*) -> eth0, eth1...; point-to-point (utun VPN) -> tun0...; anything else -> net0... Indexes are
 * LKD's (1 for lo, then 2, 3...), stable for one snapshot; a snapshot is taken at each request.
 *
 * Three interfaces to them (Linux 6.12):
 *   - the SIOCGIF* ioctls on any socket (net/core/dev_ioctl.c): SIOCGIFCONF (IPv4 addresses, ifconf),
 *     SIOCGIFNAME, SIOCGIFINDEX, SIOCGIFFLAGS, SIOCGIFADDR, SIOCGIFNETMASK, SIOCGIFBRDADDR, SIOCGIFMTU,
 *     SIOCGIFHWADDR, SIOCGIFTXQLEN; every SIOCSIF* is -EPERM (no CAP_NET_ADMIN);
 *   - a NETLINK_ROUTE socket (net/core/rtnetlink.c): LXN_RTM_GETLINK, LXN_RTM_GETADDR, LXN_RTM_GETROUTE as dumps (and
 *     LXN_RTM_GETLINK of one interface), NLMSG_DONE, NLMSG_ERROR. Requests that change state (RTM_NEW*,
 *     RTM_DEL*, RTM_SET*) answer -EPERM; other types -EOPNOTSUPP. No multicast notifications are ever sent
 *     (memberships are accepted: LKD does not watch the host for changes, documented gap);
 *   - the route files /proc/net/route and /proc/net/ipv6_route (lkd_net_render_routes, published by procfs).
 *     Wine reads its routing table there, and without one every GetAdaptersAddresses fails (ERROR_NOT_SUPPORTED).
 *
 * Netlink transport: the guest socket is one end of a host AF_UNIX datagram socketpair. A request written by
 * the guest never reaches the host: LKD parses it and writes the answer, one netlink message per datagram,
 * into the other end, so recv, poll, epoll, MSG_PEEK, MSG_TRUNC and O_NONBLOCK work unchanged.
 *
 * Routes: one route per IPv4 and IPv6 prefix of each interface (proto kernel, scope link) and the host's
 * default routes (sysctl NET_RT_DUMP, RTF_GATEWAY with a zero destination). Other host routes are not shown.
 */
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "net_priv.h"

#define NIF_MAX 16
#define NADDR_MAX 64

struct nif_addr {
    int family;            /* LX_AF_INET or LX_AF_INET6 */
    uint8_t addr[16], mask_len, scope;
    uint8_t brd[4];
    int has_brd;
};

struct nif {
    char name[16];
    char hname[IFNAMSIZ];  /* host name (default routes) */
    int index;
    uint32_t flags;        /* Linux IFF_* */
    uint32_t mtu;
    uint16_t arphrd;       /* ARPHRD_* */
    uint8_t mac[6];
    int has_mac;
    int naddr;
    struct nif_addr a[8];
};

#define LX_IFF_UP 0x1
#define LX_IFF_BROADCAST 0x2
#define LX_IFF_LOOPBACK 0x8
#define LX_IFF_POINTOPOINT 0x10
#define LX_IFF_RUNNING 0x40
#define LX_IFF_NOARP 0x80
#define LX_IFF_PROMISC 0x100
#define LX_IFF_MULTICAST 0x1000
#define LX_IFF_LOWER_UP 0x10000
#define ARPHRD_ETHER 1
#define ARPHRD_LOOPBACK 772
#define ARPHRD_NONE 0xfffe

static uint32_t flags_from_host(unsigned f)
{
    uint32_t r = 0;
    if (f & IFF_UP) r |= LX_IFF_UP;
    if (f & IFF_BROADCAST) r |= LX_IFF_BROADCAST;
    if (f & IFF_LOOPBACK) r |= LX_IFF_LOOPBACK;
    if (f & IFF_POINTOPOINT) r |= LX_IFF_POINTOPOINT;
    if (f & IFF_RUNNING) r |= LX_IFF_RUNNING | LX_IFF_LOWER_UP;
    if (f & IFF_NOARP) r |= LX_IFF_NOARP;
    if (f & IFF_PROMISC) r |= LX_IFF_PROMISC;
    if (f & IFF_MULTICAST) r |= LX_IFF_MULTICAST;
    return r;
}

static int prefix_len(const uint8_t *m, int n)
{
    int l = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b = m[i];
        while (b & 0x80) { l++; b = (uint8_t)(b << 1); }
        if (m[i] != 0xff) break;
    }
    return l;
}

static int is_v6_linklocal(const uint8_t *a) { return a[0] == 0xfe && (a[1] & 0xc0) == 0x80; }

/* Snapshot of the guest-visible interfaces. Returns the count. */
static int nif_snapshot(struct nif *out, int cap)
{
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) < 0) return 0;
    char hname[NIF_MAX][IFNAMSIZ];
    int n = 0, neth = 0, ntun = 0, noth = 0;
    /* pass 1: candidate interfaces in host order (loopback first) */
    for (int pass = 0; pass < 2; pass++) {
        for (struct ifaddrs *i = ifa; i && n < cap; i = i->ifa_next) {
            int loop = (i->ifa_flags & IFF_LOOPBACK) != 0;
            if (pass == 0 ? !loop : loop) continue;
            int seen = 0;
            for (int k = 0; k < n; k++) if (!strcmp(hname[k], i->ifa_name)) seen = 1;
            if (seen) continue;
            if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING)) continue;
            int usable = loop;
            for (struct ifaddrs *j = ifa; j && !usable; j = j->ifa_next) {
                if (strcmp(j->ifa_name, i->ifa_name) || !j->ifa_addr) continue;
                if (j->ifa_addr->sa_family == AF_INET) usable = 1;
                if (j->ifa_addr->sa_family == AF_INET6 &&
                    !is_v6_linklocal(((struct sockaddr_in6 *)j->ifa_addr)->sin6_addr.s6_addr)) usable = 1;
            }
            if (!usable) continue;
            struct nif *f = &out[n];
            memset(f, 0, sizeof *f);
            snprintf(hname[n], IFNAMSIZ, "%s", i->ifa_name);
            snprintf(f->hname, sizeof f->hname, "%s", i->ifa_name);
            f->index = n + 1;
            f->flags = flags_from_host(i->ifa_flags);
            f->mtu = loop ? 65536 : 1500;
            f->arphrd = loop ? ARPHRD_LOOPBACK : ARPHRD_ETHER;
            int ptp = (i->ifa_flags & IFF_POINTOPOINT) != 0;
            for (struct ifaddrs *j = ifa; j; j = j->ifa_next) {
                if (strcmp(j->ifa_name, i->ifa_name) || !j->ifa_addr || j->ifa_addr->sa_family != AF_LINK) continue;
                struct sockaddr_dl *dl = (struct sockaddr_dl *)j->ifa_addr;
                if (j->ifa_data) f->mtu = ((struct if_data *)j->ifa_data)->ifi_mtu;
                if (dl->sdl_type == IFT_ETHER && dl->sdl_alen == 6) { memcpy(f->mac, LLADDR(dl), 6); f->has_mac = 1; }
                if (dl->sdl_type != IFT_ETHER && !loop) ptp = 1;
            }
            if (loop) { snprintf(f->name, sizeof f->name, "lo"); f->mtu = 65536; }
            else if (ptp) { snprintf(f->name, sizeof f->name, "tun%d", ntun++); f->arphrd = ARPHRD_NONE; f->flags |= LX_IFF_NOARP; }
            else if (f->has_mac) snprintf(f->name, sizeof f->name, "eth%d", neth++);
            else snprintf(f->name, sizeof f->name, "net%d", noth++);
            if (loop) f->mtu = 65536; /* Linux lo; the host's lo0 says 16384 */
            for (struct ifaddrs *j = ifa; j && f->naddr < 8; j = j->ifa_next) {
                if (strcmp(j->ifa_name, i->ifa_name) || !j->ifa_addr) continue;
                struct nif_addr *x = &f->a[f->naddr];
                memset(x, 0, sizeof *x);
                if (j->ifa_addr->sa_family == AF_INET) {
                    x->family = LX_AF_INET;
                    memcpy(x->addr, &((struct sockaddr_in *)j->ifa_addr)->sin_addr, 4);
                    if (j->ifa_netmask) x->mask_len = (uint8_t)prefix_len((uint8_t *)&((struct sockaddr_in *)j->ifa_netmask)->sin_addr, 4);
                    if ((i->ifa_flags & IFF_BROADCAST) && j->ifa_broadaddr && j->ifa_broadaddr->sa_family == AF_INET) {
                        memcpy(x->brd, &((struct sockaddr_in *)j->ifa_broadaddr)->sin_addr, 4);
                        x->has_brd = 1;
                    }
                    x->scope = loop ? 254 : 0;                   /* RT_SCOPE_HOST / UNIVERSE */
                    f->naddr++;
                } else if (j->ifa_addr->sa_family == AF_INET6) {
                    x->family = LX_AF_INET6;
                    memcpy(x->addr, ((struct sockaddr_in6 *)j->ifa_addr)->sin6_addr.s6_addr, 16);
                    if (is_v6_linklocal(x->addr)) { x->addr[2] = x->addr[3] = 0; x->scope = 253; } /* KAME embedded scope */
                    else x->scope = loop ? 254 : 0;
                    if (j->ifa_netmask) x->mask_len = (uint8_t)prefix_len(((struct sockaddr_in6 *)j->ifa_netmask)->sin6_addr.s6_addr, 16);
                    f->naddr++;
                }
            }
            n++;
        }
    }
    freeifaddrs(ifa);
    return n;
}

static struct nif *nif_by_name(struct nif *v, int n, const char *name)
{
    for (int i = 0; i < n; i++) if (!strncmp(v[i].name, name, 16)) return &v[i];
    return NULL;
}

/* ---- SIOCGIF* ioctls ---------------------------------------------------------------------------------- */

#define LX_SIOCGIFNAME 0x8910
#define LX_SIOCGIFCONF 0x8912
#define LX_SIOCGIFFLAGS 0x8913
#define LX_SIOCSIFFLAGS 0x8914
#define LX_SIOCGIFADDR 0x8915
#define LX_SIOCGIFBRDADDR 0x8919
#define LX_SIOCGIFNETMASK 0x891b
#define LX_SIOCGIFMTU 0x8921
#define LX_SIOCGIFHWADDR 0x8927
#define LX_SIOCGIFINDEX 0x8933
#define LX_SIOCGIFTXQLEN 0x8942

struct lx_ifreq { char name[16]; union { struct lx_sockaddr sa; int32_t ival; int16_t flags; uint8_t pad[24]; } u; };
_Static_assert(sizeof(struct lx_ifreq) == 40, "ifreq");
struct lx_ifconf { int32_t len; int32_t pad; uint64_t buf; };

static void sa_in(struct lx_sockaddr *sa, const uint8_t *a4)
{
    memset(sa, 0, sizeof *sa);
    struct lx_sockaddr_in *s = (struct lx_sockaddr_in *)sa;
    s->sin_family = LX_AF_INET;
    memcpy(&s->sin_addr, a4, 4);
}

static const struct nif_addr *first_v4(const struct nif *f)
{
    for (int i = 0; i < f->naddr; i++) if (f->a[i].family == LX_AF_INET) return &f->a[i];
    return NULL;
}

/* Returns 1 and *ret when cmd is an interface ioctl, 0 otherwise. */
int net_if_ioctl(uint32_t cmd, uint64_t arg, int64_t *ret)
{
    if ((cmd & 0xff00) != 0x8900 || cmd < 0x8910 || cmd > 0x8960) return 0;
    struct nif v[NIF_MAX];
    if (cmd == LX_SIOCGIFCONF) {
        struct lx_ifconf ic;
        if (lkd_user_read(arg, &ic, sizeof ic) < 0) { *ret = -LX_EFAULT; return 1; }
        int n = nif_snapshot(v, NIF_MAX), used = 0;
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < v[i].naddr; k++) {
                if (v[i].a[k].family != LX_AF_INET) continue;
                if (ic.buf) {                                       /* inet_gifconf: stop when the next does not fit */
                    if (used + (int)sizeof(struct lx_ifreq) > ic.len) goto done;
                    struct lx_ifreq r;
                    memset(&r, 0, sizeof r);
                    memcpy(r.name, v[i].name, 16);
                    sa_in(&r.u.sa, v[i].a[k].addr);
                    if (lkd_user_write(ic.buf + (uint64_t)used, &r, sizeof r) < 0) { *ret = -LX_EFAULT; return 1; }
                }
                used += (int)sizeof(struct lx_ifreq);
            }
        }
    done:
        ic.len = used;
        *ret = lkd_user_write(arg, &ic.len, sizeof ic.len) < 0 ? -LX_EFAULT : 0;
        return 1;
    }
    struct lx_ifreq r;
    if (lkd_user_read(arg, &r, sizeof r) < 0) { *ret = -LX_EFAULT; return 1; }
    r.name[15] = 0;
    int n = nif_snapshot(v, NIF_MAX);
    if (cmd == LX_SIOCGIFNAME) {
        if (r.u.ival < 1 || r.u.ival > n) { *ret = -LX_ENODEV; return 1; }
        memcpy(r.name, v[r.u.ival - 1].name, 16);
        *ret = lkd_user_write(arg, &r, sizeof r) < 0 ? -LX_EFAULT : 0;
        return 1;
    }
    if (cmd != LX_SIOCGIFFLAGS && cmd != LX_SIOCGIFADDR && cmd != LX_SIOCGIFBRDADDR && cmd != LX_SIOCGIFNETMASK &&
        cmd != LX_SIOCGIFMTU && cmd != LX_SIOCGIFHWADDR && cmd != LX_SIOCGIFINDEX && cmd != LX_SIOCGIFTXQLEN) {
        /* SIOCSIF{FLAGS,ADDR,DSTADDR,BRDADDR,NETMASK,METRIC,MTU,NAME,HWADDR}, SIOC{ADD,DEL}MULTI, SIOCSIFTXQLEN:
         * CAP_NET_ADMIN (dev_ioctl); any other number of the range is not handled here */
        static const uint32_t sets[] = { 0x8914, 0x8916, 0x8918, 0x891a, 0x891c, 0x891e, 0x8922, 0x8923, 0x8924,
                                         0x8931, 0x8932, 0x8943 };
        for (unsigned i = 0; i < sizeof sets / sizeof sets[0]; i++) if (cmd == sets[i]) { *ret = -LX_EPERM; return 1; }
        return 0;
    }
    struct nif *f = nif_by_name(v, n, r.name);
    if (!f) { *ret = -LX_ENODEV; return 1; }
    const struct nif_addr *a4 = first_v4(f);
    memset(&r.u, 0, sizeof r.u);
    switch (cmd) {
    case LX_SIOCGIFFLAGS: r.u.flags = (int16_t)(f->flags & 0xffff); break;
    case LX_SIOCGIFINDEX: r.u.ival = f->index; break;
    case LX_SIOCGIFMTU: r.u.ival = (int32_t)f->mtu; break;
    case LX_SIOCGIFTXQLEN: r.u.ival = 1000; break;
    case LX_SIOCGIFHWADDR:
        r.u.sa.sa_family = f->arphrd;
        if (f->has_mac) memcpy(r.u.sa.sa_data, f->mac, 6);
        break;
    case LX_SIOCGIFADDR: case LX_SIOCGIFNETMASK: case LX_SIOCGIFBRDADDR: {
        if (!a4) { *ret = -LX_EADDRNOTAVAIL; return 1; }
        uint8_t m[4] = { 0 };
        if (cmd == LX_SIOCGIFADDR) sa_in(&r.u.sa, a4->addr);
        else if (cmd == LX_SIOCGIFNETMASK) {
            uint32_t mm = a4->mask_len ? htonl(~0u << (32 - a4->mask_len)) : 0;
            memcpy(m, &mm, 4);
            sa_in(&r.u.sa, m);
        } else {
            if (!a4->has_brd) { *ret = -LX_EADDRNOTAVAIL; return 1; }
            sa_in(&r.u.sa, a4->brd);
        }
        break;
    }
    default:
        *ret = -LX_EINVAL;
        return 1;
    }
    *ret = lkd_user_write(arg, &r, sizeof r) < 0 ? -LX_EFAULT : 0;
    return 1;
}

/* ---- NETLINK_ROUTE --------------------------------------------------------------------------------------- */

struct nlmsghdr_ { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct rtattr_ { uint16_t len, type; };
struct ifinfomsg_ { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; };
struct ifaddrmsg_ { uint8_t family, prefixlen, flags, scope; uint32_t index; };
struct rtmsg_ { uint8_t family, dst_len, src_len, tos, table, protocol, scope, type; uint32_t flags; };

#define NLMSG_NOOP 1
#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define NLM_F_REQUEST 1
#define NLM_F_MULTI 2
#define NLM_F_ACK 4
#define NLM_F_DUMP 0x300
#define LXN_RTM_NEWLINK 16
#define LXN_RTM_GETLINK 18
#define LXN_RTM_NEWADDR 20
#define LXN_RTM_GETADDR 22
#define LXN_RTM_NEWROUTE 24
#define LXN_RTM_GETROUTE 26
#define LXN_IFLA_ADDRESS 1
#define LXN_IFLA_BROADCAST 2
#define LXN_IFLA_IFNAME 3
#define LXN_IFLA_MTU 4
#define LXN_IFLA_TXQLEN 13
#define LXN_IFLA_OPERSTATE 16
#define LXN_IFA_ADDRESS 1
#define LXN_IFA_LOCAL 2
#define LXN_IFA_LABEL 3
#define LXN_IFA_BROADCAST 4
#define LXN_IFA_FLAGS 8
#define LXN_RTA_DST 1
#define LXN_RTA_OIF 4
#define LXN_RTA_GATEWAY 5
#define LXN_RTA_PRIORITY 6
#define LXN_RTA_PREFSRC 7
#define LXN_RTA_TABLE 15
#define NLA_ALIGN(x) (((x) + 3u) & ~3u)

struct nlbuf { uint8_t b[4096]; uint32_t n; };

static void *nl_put(struct nlbuf *m, const void *p, uint32_t len)
{
    if (m->n + NLA_ALIGN(len) > sizeof m->b) return NULL;
    void *d = m->b + m->n;
    memset(d, 0, NLA_ALIGN(len));
    if (p) memcpy(d, p, len);
    m->n += NLA_ALIGN(len);
    return d;
}

static void nl_attr(struct nlbuf *m, uint16_t type, const void *p, uint16_t len)
{
    struct rtattr_ a = { (uint16_t)(sizeof a + len), type };
    nl_put(m, &a, sizeof a);
    nl_put(m, p, len);
}

static void nl_begin(struct nlbuf *m, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid)
{
    m->n = 0;
    struct nlmsghdr_ h = { 0, type, flags, seq, pid };
    nl_put(m, &h, sizeof h);
}

/* Queue one finished message for the guest. */
static int nl_emit(int peer, struct nlbuf *m)
{
    ((struct nlmsghdr_ *)m->b)->len = m->n;
    ssize_t w;
    do { w = send(peer, m->b, m->n, 0); } while (w < 0 && errno == EINTR);
    return w == (ssize_t)m->n ? 0 : -LX_ENOBUFS;
}

static int nl_error(int peer, const struct nlmsghdr_ *req, int err, uint32_t pid)
{
    struct nlbuf m;
    nl_begin(&m, NLMSG_ERROR, 0, req->seq, pid);
    int32_t e = err;
    nl_put(&m, &e, sizeof e);
    nl_put(&m, req, sizeof *req);   /* NETLINK_CAP_ACK not set: only the request header is echoed (with the
                                     * real kernel, the whole request); the header is what readers check */
    return nl_emit(peer, &m);
}

static int nl_done(int peer, const struct nlmsghdr_ *req, uint32_t pid)
{
    struct nlbuf m;
    nl_begin(&m, NLMSG_DONE, NLM_F_MULTI, req->seq, pid);
    int32_t z = 0;
    nl_put(&m, &z, sizeof z);
    return nl_emit(peer, &m);
}

static int put_link(int peer, const struct nif *f, uint16_t flags, uint32_t seq, uint32_t pid)
{
    struct nlbuf m;
    nl_begin(&m, LXN_RTM_NEWLINK, flags, seq, pid);
    struct ifinfomsg_ ii = { 0, 0, f->arphrd, f->index, f->flags, 0 };
    nl_put(&m, &ii, sizeof ii);
    nl_attr(&m, LXN_IFLA_IFNAME, f->name, (uint16_t)(strlen(f->name) + 1));
    uint32_t mtu = f->mtu, txq = 1000;
    nl_attr(&m, LXN_IFLA_MTU, &mtu, 4);
    nl_attr(&m, LXN_IFLA_TXQLEN, &txq, 4);
    uint8_t oper = (f->flags & LX_IFF_RUNNING) ? 6 : 2;   /* IF_OPER_UP / IF_OPER_DOWN */
    if (f->arphrd == ARPHRD_LOOPBACK) oper = 0;           /* IF_OPER_UNKNOWN, as Linux reports lo */
    nl_attr(&m, LXN_IFLA_OPERSTATE, &oper, 1);
    if (f->arphrd != ARPHRD_NONE) {
        uint8_t z[6] = { 0 }, ff[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        nl_attr(&m, LXN_IFLA_ADDRESS, f->has_mac ? f->mac : z, 6);
        nl_attr(&m, LXN_IFLA_BROADCAST, f->arphrd == ARPHRD_LOOPBACK ? z : ff, 6);
    }
    return nl_emit(peer, &m);
}

static int put_addr(int peer, const struct nif *f, const struct nif_addr *a, uint32_t seq, uint32_t pid)
{
    struct nlbuf m;
    nl_begin(&m, LXN_RTM_NEWADDR, NLM_F_MULTI, seq, pid);
    struct ifaddrmsg_ am = { (uint8_t)a->family, a->mask_len, 0x80 /* IFA_F_PERMANENT */, a->scope, (uint32_t)f->index };
    nl_put(&m, &am, sizeof am);
    uint16_t al = a->family == LX_AF_INET ? 4 : 16;
    nl_attr(&m, LXN_IFA_ADDRESS, a->addr, al);
    if (a->family == LX_AF_INET) {
        nl_attr(&m, LXN_IFA_LOCAL, a->addr, al);
        if (a->has_brd) nl_attr(&m, LXN_IFA_BROADCAST, a->brd, 4);
        nl_attr(&m, LXN_IFA_LABEL, f->name, (uint16_t)(strlen(f->name) + 1));
    }
    uint32_t fl = 0x80;
    nl_attr(&m, LXN_IFA_FLAGS, &fl, 4);
    return nl_emit(peer, &m);
}

/* A route of the main table: the prefix route of an address (gw NULL) or a default (dst_len 0, gw set). */
struct nroute {
    int family;               /* LX_AF_INET or LX_AF_INET6 */
    uint8_t dst[16];          /* the masked prefix */
    int dst_len;
    const uint8_t *gw;
    const uint8_t *prefsrc;   /* the address a prefix route comes from */
    const struct nif *f;      /* outgoing interface */
};

/* IPv6 metric (ip6_route_add): 256 for a prefix route, 1024 for a gateway. */
static uint32_t route_metric6(const struct nroute *rt) { return rt->gw ? 1024 : 256; }

static int put_route(int peer, const struct nroute *rt, uint32_t seq, uint32_t pid)
{
    struct nlbuf m;
    nl_begin(&m, LXN_RTM_NEWROUTE, NLM_F_MULTI, seq, pid);
    uint8_t proto = rt->gw ? 3 /* RTPROT_BOOT */ : 2 /* RTPROT_KERNEL */;
    uint8_t scope = rt->gw ? 0 /* RT_SCOPE_UNIVERSE */ : 253 /* RT_SCOPE_LINK */;
    struct rtmsg_ rm = { (uint8_t)rt->family, (uint8_t)rt->dst_len, 0, 0, 254 /* RT_TABLE_MAIN */, proto, scope, 1 /* RTN_UNICAST */, 0 };
    nl_put(&m, &rm, sizeof rm);
    uint16_t al = rt->family == LX_AF_INET ? 4 : 16;
    uint32_t table = 254, oifv = (uint32_t)rt->f->index;
    nl_attr(&m, LXN_RTA_TABLE, &table, 4);
    if (rt->dst_len) nl_attr(&m, LXN_RTA_DST, rt->dst, al);
    if (rt->gw) nl_attr(&m, LXN_RTA_GATEWAY, rt->gw, al);
    if (rt->prefsrc && rt->family == LX_AF_INET) nl_attr(&m, LXN_RTA_PREFSRC, rt->prefsrc, al);
    if (rt->family == LX_AF_INET6) { uint32_t prio = route_metric6(rt); nl_attr(&m, LXN_RTA_PRIORITY, &prio, 4); }
    nl_attr(&m, LXN_RTA_OIF, &oifv, 4);
    return nl_emit(peer, &m);
}

/* Default gateways of the host for one family: (gateway, host interface name). Returns the count. */
static int host_defaults(int af, uint8_t gw[][16], char ifn[][IFNAMSIZ], int cap)
{
    int mib[6] = { CTL_NET, PF_ROUTE, 0, af, NET_RT_FLAGS, RTF_GATEWAY };
    size_t len = 0;
    if (sysctl(mib, 6, NULL, &len, NULL, 0) < 0 || !len) return 0;
    char *buf = malloc(len);
    if (!buf) return 0;
    int n = 0;
    if (sysctl(mib, 6, buf, &len, NULL, 0) == 0) {
        for (char *p = buf; p < buf + len && n < cap;) {
            struct rt_msghdr *rt = (struct rt_msghdr *)p;
            if (!rt->rtm_msglen) break;
            struct sockaddr *sa = (struct sockaddr *)(rt + 1), *dst = NULL, *gwy = NULL;
            for (int i = 0; i < RTAX_MAX; i++) {
                if (!(rt->rtm_addrs & (1 << i))) continue;
                if (i == RTAX_DST) dst = sa;
                if (i == RTAX_GATEWAY) gwy = sa;
                int l = sa->sa_len ? (int)((sa->sa_len + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1)) : (int)sizeof(uint32_t);
                sa = (struct sockaddr *)((char *)sa + l);
            }
            int isdef = 0;
            if (dst && gwy && gwy->sa_family == af && !(rt->rtm_flags & RTF_IFSCOPE)) {
                if (af == AF_INET) isdef = ((struct sockaddr_in *)dst)->sin_addr.s_addr == 0;
                else isdef = IN6_IS_ADDR_UNSPECIFIED(&((struct sockaddr_in6 *)dst)->sin6_addr);
            }
            if (isdef && if_indextoname(rt->rtm_index, ifn[n])) {
                if (af == AF_INET) memcpy(gw[n], &((struct sockaddr_in *)gwy)->sin_addr, 4);
                else {
                    memcpy(gw[n], ((struct sockaddr_in6 *)gwy)->sin6_addr.s6_addr, 16);
                    if (is_v6_linklocal(gw[n])) gw[n][2] = gw[n][3] = 0;
                }
                n++;
            }
            p += rt->rtm_msglen;
        }
    }
    free(buf);
    return n;
}

/* The guest interface of a host interface (default routes), NULL when the guest does not see it. */
static const struct nif *nif_of_host(const struct nif *v, int n, const char *hostname)
{
    for (int i = 0; i < n; i++) if (!strcmp(v[i].hname, hostname)) return &v[i];
    return NULL;
}

static void mask_prefix(uint8_t *d, const uint8_t *a, int bytes, int plen)
{
    for (int i = 0; i < bytes; i++) {
        int bits = plen - i * 8;
        uint8_t m = bits >= 8 ? 0xff : bits <= 0 ? 0 : (uint8_t)(0xff << (8 - bits));
        d[i] = a[i] & m;
    }
}

#define NROUTE_MAX (NIF_MAX * 8 + 4)

/* The main table of one family, as the kernel derives it: the prefix route of each address (none for IPv4 on lo,
 * those live in the local table), then the host's defaults through the interfaces the guest sees. The defaults
 * point into gw. Returns the count. */
static int route_list(const struct nif *v, int n, int family, uint8_t gw[4][16], struct nroute *out)
{
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (family == LX_AF_INET && (v[i].flags & LX_IFF_LOOPBACK)) continue;
        for (int k = 0; k < v[i].naddr; k++) {
            const struct nif_addr *a = &v[i].a[k];
            if (a->family != family) continue;
            struct nroute *rt = &out[c++];
            *rt = (struct nroute){ .family = family, .dst_len = a->mask_len, .prefsrc = a->addr, .f = &v[i] };
            mask_prefix(rt->dst, a->addr, family == LX_AF_INET ? 4 : 16, a->mask_len);
        }
    }
    char ifn[4][IFNAMSIZ];
    int d = host_defaults(family == LX_AF_INET6 ? AF_INET6 : AF_INET, gw, ifn, 4);
    for (int i = 0; i < d; i++) {
        const struct nif *f = nif_of_host(v, n, ifn[i]);
        if (f) out[c++] = (struct nroute){ .family = family, .gw = gw[i], .f = f };   /* else not visible to the guest */
    }
    return c;
}

static int nl_dump(int peer, const struct nlmsghdr_ *h, const uint8_t *body, uint32_t blen, uint32_t pid)
{
    struct nif v[NIF_MAX];
    int n = nif_snapshot(v, NIF_MAX), r = 0;
    int fam = blen >= 1 ? body[0] : 0;   /* rtgenmsg / ifinfomsg / ifaddrmsg / rtmsg: family first */
    switch (h->type) {
    case LXN_RTM_GETLINK:
        for (int i = 0; i < n && !r; i++) r = put_link(peer, &v[i], NLM_F_MULTI, h->seq, pid);
        break;
    case LXN_RTM_GETADDR:
        for (int i = 0; i < n && !r; i++)
            for (int k = 0; k < v[i].naddr && !r; k++)
                if (!fam || fam == v[i].a[k].family) r = put_addr(peer, &v[i], &v[i].a[k], h->seq, pid);
        break;
    case LXN_RTM_GETROUTE:
        for (int af = 0; af < 2 && !r; af++) {
            int family = af ? LX_AF_INET6 : LX_AF_INET;
            if (fam && fam != family) continue;
            uint8_t gw[4][16];
            struct nroute rt[NROUTE_MAX];
            int c = route_list(v, n, family, gw, rt);
            for (int i = 0; i < c && !r; i++) r = put_route(peer, &rt[i], h->seq, pid);
        }
        break;
    }
    if (!r) r = nl_done(peer, h, pid);
    return r;
}

/* ---- /proc/net/route and /proc/net/ipv6_route ------------------------------------------------------------ */

static void put_hex6(char *o, const uint8_t *a)
{
    for (int i = 0; i < 16; i++) snprintf(o + 2 * i, 3, "%02x", a[i]);
}

/* route_list as fib_route_seq_show (net/ipv4/fib_trie.c) and rt6_info_route (net/ipv6/route.c) print it. IPv4:
 * addresses as the hex of the network-order word, every line padded to 127 columns, the default first (trie order).
 * IPv6: 32 hex digits, flags RTF_UP (1) | RTF_GATEWAY (2). */
char *lkd_net_render_routes(int ipv6, size_t *len_out)
{
    struct nif v[NIF_MAX];
    int n = nif_snapshot(v, NIF_MAX);
    uint8_t gw[4][16];
    struct nroute rt[NROUTE_MAX];
    int c = route_list(v, n, ipv6 ? LX_AF_INET6 : LX_AF_INET, gw, rt);
    size_t cap = (size_t)(c + 1) * 192, o = 0;
    char *t = malloc(cap);
    if (!t) return NULL;
    if (!ipv6) {
        o += (size_t)snprintf(t + o, cap - o, "%-127s\n",
                              "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT");
        for (int defaults = 1; defaults >= 0; defaults--) {
            for (int i = 0; i < c; i++) {
                if (!rt[i].gw != !defaults) continue;
                uint32_t dst, gwv = 0, mask = rt[i].dst_len ? htonl(~0u << (32 - rt[i].dst_len)) : 0;
                memcpy(&dst, rt[i].dst, 4);
                if (rt[i].gw) memcpy(&gwv, rt[i].gw, 4);
                char line[128];
                snprintf(line, sizeof line, "%s\t%08X\t%08X\t%04X\t0\t0\t0\t%08X\t0\t0\t0", rt[i].f->name, dst, gwv,
                         rt[i].gw ? 0x3u : 0x1u, mask);
                o += (size_t)snprintf(t + o, cap - o, "%-127s\n", line);
            }
        }
    } else {
        static const char zero[] = "00000000000000000000000000000000";
        for (int i = 0; i < c; i++) {
            char dh[33], gh[33];
            put_hex6(dh, rt[i].dst);
            if (rt[i].gw) put_hex6(gh, rt[i].gw);
            else memcpy(gh, zero, sizeof gh);
            o += (size_t)snprintf(t + o, cap - o, "%s %02x %s 00 %s %08x %08x %08x %08x %8s\n", dh, rt[i].dst_len, zero, gh,
                                  route_metric6(&rt[i]), 1u, 0u, rt[i].gw ? 0x3u : 0x1u, rt[i].f->name);
        }
    }
    *len_out = o;
    return t;
}

/* One request message from the guest. */
static void nl_request(int peer, const struct nlmsghdr_ *h, const uint8_t *body, uint32_t blen, uint32_t pid)
{
    uint16_t t = h->type;
    if (t == NLMSG_NOOP || t == NLMSG_DONE || t == NLMSG_ERROR) return;
    if (t < 16) { nl_error(peer, h, -LX_EINVAL, pid); return; }
    if (!(h->flags & NLM_F_REQUEST)) return;                  /* netlink_rcv_skb: not a request, ignored */
    int kind = (t - 16) & 3;                                   /* NEW, DEL, GET, SET */
    if (kind != 2) { nl_error(peer, h, -LX_EPERM, pid); return; }      /* changes need CAP_NET_ADMIN */
    if (t != LXN_RTM_GETLINK && t != LXN_RTM_GETADDR && t != LXN_RTM_GETROUTE) { nl_error(peer, h, -LX_EOPNOTSUPP, pid); return; }
    if ((h->flags & NLM_F_DUMP) == NLM_F_DUMP) { (void)nl_dump(peer, h, body, blen, pid); return; }
    if (t == LXN_RTM_GETLINK) {
        struct nif v[NIF_MAX];
        int n = nif_snapshot(v, NIF_MAX);
        const struct nif *f = NULL;
        if (blen >= sizeof(struct ifinfomsg_)) {
            const struct ifinfomsg_ *ii = (const struct ifinfomsg_ *)body;
            if (ii->index > 0 && ii->index <= n) f = &v[ii->index - 1];
            for (uint32_t off = sizeof *ii; !f && off + sizeof(struct rtattr_) <= blen;) {
                const struct rtattr_ *a = (const struct rtattr_ *)(body + off);
                if (a->len < sizeof *a || off + a->len > blen) break;
                if (a->type == LXN_IFLA_IFNAME) {
                    char nm[16] = { 0 };
                    memcpy(nm, a + 1, a->len - sizeof *a < 15 ? a->len - sizeof *a : 15);
                    f = nif_by_name(v, n, nm);
                }
                off += NLA_ALIGN(a->len);
            }
        }
        if (!f) { nl_error(peer, h, -LX_ENODEV, pid); return; }
        put_link(peer, f, 0, h->seq, pid);
        if (h->flags & NLM_F_ACK) nl_error(peer, h, 0, pid);
        return;
    }
    nl_error(peer, h, -LX_EOPNOTSUPP, pid);   /* single GETADDR/GETROUTE (route lookup) not modeled */
}

/* sendmsg/sendto/write on a netlink socket: the whole buffer is a batch of requests. Returns len. */
int64_t net_nl_send(lkd_fdobj *o, const uint8_t *buf, uint64_t len)
{
    struct net_sock *sk = o->priv;
    for (uint64_t off = 0; off + sizeof(struct nlmsghdr_) <= len;) {
        struct nlmsghdr_ h;
        memcpy(&h, buf + off, sizeof h);
        if (h.len < sizeof h || off + h.len > len) break;
        nl_request(sk->nl_peer, &h, buf + off + sizeof h, h.len - (uint32_t)sizeof h, sk->nl_pid);
        off += NLA_ALIGN(h.len);
    }
    return (int64_t)len;
}

/* socket(AF_NETLINK, SOCK_RAW or SOCK_DGRAM, NETLINK_ROUTE). */
int net_nl_create(int type, int protocol, uint32_t lx_flags, lkd_fdobj **out)
{
    if (type != LX_SOCK_RAW && type != LX_SOCK_DGRAM) return -LX_ESOCKTNOSUPPORT;
    if (protocol != 0 /* NETLINK_ROUTE */) {
        lkd_log_once("net: AF_NETLINK protocol %d is not implemented (-EPROTONOSUPPORT), only NETLINK_ROUTE", protocol);
        return -LX_EPROTONOSUPPORT;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) return (int)LKD_HOST_ERR();
    int big = 1 << 20;
    for (int i = 0; i < 2; i++) {
        setsockopt(sv[i], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
        setsockopt(sv[i], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    }
    fcntl(sv[1], F_SETFD, FD_CLOEXEC);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    int e = net_sock_wrap(sv[0], LX_AF_NETLINK, type, protocol, lx_flags, 1, out);
    if (e) { close(sv[0]); close(sv[1]); return e; }
    net_sk(*out)->nl_peer = sv[1];
    return 0;
}

/* The guest-visible address of the socket: {AF_NETLINK, pad, port id, groups}. */
uint32_t net_nl_name(struct net_sock *sk, uint8_t *out, int peer)
{
    uint8_t a[12] = { 0 };
    uint16_t f = LX_AF_NETLINK;
    memcpy(a, &f, 2);
    uint32_t pid = peer ? 0 : sk->nl_pid, groups = peer ? 0 : sk->nl_groups;
    memcpy(a + 4, &pid, 4);
    memcpy(a + 8, &groups, 4);
    memcpy(out, a, 12);
    return 12;
}

/* bind: the port id (0 = autobind to the thread group id, netlink_autobind) and the groups. */
int net_nl_bind(lkd_task *t, struct net_sock *sk, uint64_t addr, uint32_t len)
{
    uint8_t a[12] = { 0 };
    if (len < 12) return -LX_EINVAL;
    if (lkd_user_read(addr, a, 12) < 0) return -LX_EFAULT;
    uint16_t f;
    memcpy(&f, a, 2);
    if (f != LX_AF_NETLINK) return -LX_EINVAL;
    uint32_t pid, groups;
    memcpy(&pid, a + 4, 4);
    memcpy(&groups, a + 8, 4);
    if (sk->nl_pid && pid && pid != sk->nl_pid) return -LX_EINVAL;
    if (!sk->nl_pid) sk->nl_pid = pid ? pid : (uint32_t)t->proc->pid;
    sk->nl_groups = groups;   /* accepted; no notification is ever sent (header comment) */
    return 0;
}
