# wine-8: no visible network interface (AF_NETLINK absent, SIOCGIFCONF -> ENOTTY); this was the game stopping after 96 frames

Requester: wine-linux agent.

MEASURED:
- The main thread of RocketLeague.exe (0130) always stops after `gethostname() -> "obelisk"`, `gethostbyname("obelisk")`, then the nsiproxy ioctl 0x121000 (IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, ifinfo) that never returns (WINEDEBUG +winsock, evidence/eac-runs/20261002T232124; Launch.log stops at the same moment, no more Present).
- Witness src/wine-linux/tests/hostent.c (gethostbyname of the machine name then GetAdaptersAddresses), logs/wine-linux-hostent-001.log: same hang, `nsi_ioctl ioctl 121000` then `ifinfo_enumerate_all` without return.
- Cause on the Wine side (fixed): nsiproxy calls if_nameindex(); glibc builds it through netlink; LKD answers `AF_NETLINK is not implemented (-EAFNOSUPPORT)`, if_nameindex returns NULL, Wine dereferenced NULL while holding if_list_lock, the thread died and all following calls stayed blocked on the lock. Fix in dlls/nsiproxy.sys/ndis.c (NULL checked, fallback SIOCGIFCONF + SIOCGIFINDEX). MEASURED after the fix: gethostbyname returns in 22 ms (NULL: no address).
- What remains is that no interface exists: the SIOCGIFCONF fallback returns `Inappropriate ioctl for device` (ENOTTY) under LKD (logs/wine-linux-hostent-003.log). Under Windows there is always at least the loopback and the active interface; gethostbyname(machine name) always succeeds, and GetAdaptersAddresses with zero interfaces crashes Wine's iphlpapi (case never seen under Linux).
Request, either:
1. Minimal NETLINK_ROUTE (socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE); RTM_GETLINK and RTM_GETADDR as dumps; RTM_GETROUTE would be useful to nsiproxy for the routes): makes glibc's getifaddrs/if_nameindex and nsiproxy (iphlpapi) work. This is what all Linux programs do.
2. Failing that, the SIOCGIFCONF, SIOCGIFINDEX, SIOCGIFFLAGS, SIOCGIFADDR, SIOCGIFNETMASK, SIOCGIFHWADDR, SIOCGIFMTU ioctls on an AF_INET socket, from the host's interfaces (macOS getifaddrs: lo0 -> "lo", en0 -> "eth0" for example).

## Handled on 2026-10-03 (integration maintainer)

New file `src/net/ifaces.c`. Everything is read-only and read from the host's real state (getifaddrs, route sysctl) at each request.

Interfaces seen by the guest:
- `lo` (the host's lo0, index 1, MTU 65536 like Linux);
- then each host interface that is UP and RUNNING and carries an IPv4 address or a non-link-local IPv6 address, in the host's order.
- Linux names: Ethernet (en*) gives `ethN`, point-to-point (VPN utun) gives `tunN`, the rest gives `netN`.
- On this Mac: `lo` and `eth0` (en0, 192.168.1.167, two global IPv6). The utun without a global address, awdl, llw, bridge, anpi and inactive en* are not shown.

ioctl on any socket: SIOCGIFCONF (IPv4, whole ifreq only, size probe with a NULL buffer), SIOCGIFNAME, SIOCGIFINDEX, SIOCGIFFLAGS, SIOCGIFADDR, SIOCGIFNETMASK, SIOCGIFBRDADDR, SIOCGIFMTU, SIOCGIFHWADDR, SIOCGIFTXQLEN. The SIOCSIF* return EPERM (no CAP_NET_ADMIN). An unknown name returns ENODEV.

NETLINK_ROUTE (`socket(AF_NETLINK, SOCK_RAW or SOCK_DGRAM, 0)`):
- RTM_GETLINK, RTM_GETADDR and RTM_GETROUTE dumps, plus RTM_GETLINK of a single interface (by index or IFLA_IFNAME);
- NLMSG_DONE and NLMSG_ERROR, with the seq and port id of the request;
- bind and autobind (port id = tgid), getsockname, source address {AF_NETLINK, 0};
- SOL_NETLINK options accepted with no effect;
- RTM_NEW*, RTM_DEL* and RTM_SET* return EPERM, the other types EOPNOTSUPP.
- Transport: a host AF_UNIX datagram pair, the answer being written by LKD, one netlink message per datagram. recv, poll, epoll, MSG_PEEK, MSG_TRUNC and O_NONBLOCK work without special code.
- Routes: one connected prefix per address (proto kernel, scope link) and the host's default routes (gateway, on an interface that the guest sees).

The other netlink protocols return EPROTONOSUPPORT.

MEASURED:
- `tests/test_net_ifaces.c` (new, 46 assertions): ioctl on lo (index 1, flags, 127.0.0.1/8, MTU 65536, ARPHRD_LOOPBACK), SIOCGIFNAME, ENODEV, EPERM, SIOCGIFCONF compared with the host's getifaddrs; netlink dumps (lo first with IFLA_IFNAME, addresses, routes), seq and port id checked, NLMSG_ERROR -EPERM, EAGAIN.
- `test_net_unix` updated (NETLINK_ROUTE accepted, other protocols EPROTONOSUPPORT).
- glibc 2.41 witness `witnesses/glibc/ifaces.c`, dynamically linked by zig (x86_64-linux-gnu), launched in the Debian root under `build-integration/host/fexdarwin-run`:
  - before: getifaddrs and if_nameindex return EAFNOSUPPORT, SIOCGIFCONF returns ENOTTY, 0 interfaces;
  - after: getifaddrs gives lo and eth0 (AF_PACKET, 127.0.0.1, ::1, fe80::1, 192.168.1.167, IPv6 of eth0); if_nameindex gives 1 lo and 2 eth0; SIOCGIFCONF then SIOCGIFINDEX are consistent; PASS.
- `ip` (iproute2) is not in the Debian root: not tested.

Limits:
1. The MAC address is the one that macOS gives the process: 02:00:00:00:00:00, a privacy value. MEASURED: the host's getifaddrs and sysctl NET_RT_IFLIST2 return the same value for en0, while `ifconfig` shows the real one.
2. No multicast notifications (address or link change).
3. No RTM_GETROUTE for a single destination.
4. No interface statistics (IFLA_STATS).
5. A netlink socket sent through SCM_RIGHTS arrives as an AF_UNIX socket.
