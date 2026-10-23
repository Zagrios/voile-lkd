# wine-4: setsockopt(IPPROTO_IP, IP_RECVERR) -> ENOPROTOOPT breaks all glibc DNS resolution

Requester: wine-linux agent. Blocking for HTTPS (EOS, EAC bootstrapper).

MEASURED (logs/wine-linux-dns-001.log, LKD_TRACE=1): `getent hosts www.epicgames.com` from the Debian rootfs, code 2 (not found). Sequence of the glibc 2.41 resolver (res_send, send_dg) for each server in resolv.conf:
```
socket(AF_INET, SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0) = 3
setsockopt(3, IPPROTO_IP(0), IP_RECVERR(11), &1, 4) = -92 (ENOPROTOOPT)
close(3)
```
then exit_group(2): no DNS packet is sent. glibc treats the IP_RECVERR failure as fatal for this server. On the Windows side (Wine, WinHTTP) the same failure gives ERROR_WINHTTP_NAME_NOT_RESOLVED (12007), logs/wine-linux-https-001.log.
Request: accept IP_RECVERR (and IPV6_RECVERR, level 41 option 25, same code in send_dg for an IPv6 server) on UDP sockets. The minimum is to answer 0 with no effect (ICMP errors will not be reported through MSG_ERRQUEUE), which is enough for the resolver.
Witness: `run/wine-lkd-own.sh 30 /usr/bin/getent hosts www.epicgames.com` must print an address.

## Handled on 2026-10-02 (integration maintainer)

`IP_RECVERR` (level IPPROTO_IP, 11) and `IPV6_RECVERR` (level IPPROTO_IPV6, 25) are accepted on AF_INET and AF_INET6 sockets (`src/net/sockopt.c`). The flag is stored and read back by getsockopt; clearing it empties the queue. An AF_UNIX socket keeps ENOPROTOOPT.
Error queue (`src/net/sockmsg.c`): an error that the host reports for a datagram (ECONNREFUSED, EHOSTUNREACH, ENETUNREACH, EHOSTDOWN) is also queued. `recvmsg(MSG_ERRQUEUE)` returns EAGAIN when the queue is empty, otherwise 0 bytes, the peer's address, `MSG_ERRQUEUE` and an IP_RECVERR control message (`sock_extended_err` with `SO_EE_ORIGIN_LOCAL`, ICMP type, code and sender unknown: the host gives only the errno). poll reports POLLERR as long as the queue is not empty.
Limits: a single queued entry (Linux keeps several), no payload of the failed datagram.
MEASURED: `test_net_inet` case `ip_recverr` (closed port on loopback: ECONNREFUSED then the queue entry, IPv6, AF_UNIX). Witness: `getent hosts www.epicgames.com` under `build-integration/host/fexdarwin-run` (Debian root) prints 104.18.21.94 and 104.18.20.94, code 0. Before: code 2 and `setsockopt level 0 option 11 not implemented`.
