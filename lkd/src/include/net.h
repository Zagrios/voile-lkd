/* net.h: sockets, polling and event objects. OWNER: net.
 * Core provides weak placeholders (src/core/stubs_net.c): lkd_net_object_released is a no-op.
 *
 * Everything here is reached through syscalls registered by lkd_net_init(); the header only lists what
 * other modules may call. epoll is built over kqueue: kqueue descriptors are not inherited by fork and
 * cannot cross SCM_RIGHTS (measured), so lkd_net_fork_child rebuilds every epoll set from its table.
 */
#ifndef LKD_NET_H
#define LKD_NET_H

#include "fd.h"

/* Wait until an object is ready: the common building block of poll/ppoll/select and of blocking
 * operations that other modules implement on fd objects. Waits for any of `want` (LX_POLL*) on o, until
 * deadline_ns (lkd_host_mono_ns scale, < 0 forever) or interruption. Returns the ready events (> 0), 0 on
 * timeout, -LX_ERESTARTSYS if interrupted. Uses o->host_fd/o->notify_fd and ops->poll. */
int lkd_net_wait_object(lkd_task *t, lkd_fdobj *o, uint32_t want, int64_t deadline_ns);

/* Create an eventfd/timerfd/signalfd-style readiness pipe pair for objects with no kernel-visible
 * readiness: returns 0 and the two host fds (rd is nonblocking CLOEXEC; write one byte to wake
 * pollers). Used by sig for signalfd. */
int lkd_net_make_notify_pipe(int *rd, int *wr);

/* The sig module calls this after queueing a signal for a task so that signalfd readers and epoll sets
 * that watch signalfd objects of that process re-evaluate. No-op until a signalfd exists. */
void lkd_net_signal_queued(lkd_task *t);

/* The inotify limits net applies (request procfs-4 point 3), published by procfs as
 * /proc/sys/fs/inotify/{max_user_instances, max_user_watches, max_queued_events}. ANY. */
struct lkd_inotify_limits { uint32_t max_user_instances, max_user_watches, max_queued_events; };
void lkd_net_inotify_limits(struct lkd_inotify_limits *out);

/* /proc/net/route (ipv6 0) or /proc/net/ipv6_route (ipv6 1): the routes of the guest-visible interfaces
 * (ifaces.c), as the kernel prints them. Malloc'ed text, its length in *len_out; NULL on allocation failure. ANY. */
char *lkd_net_render_routes(int ipv6, size_t *len_out);

#endif /* LKD_NET_H */
