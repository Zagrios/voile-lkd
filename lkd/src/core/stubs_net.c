/* stubs_net.c: weak placeholders for the net module. Owner: core until net overrides them. */
#include <fcntl.h>
#include <unistd.h>

#include "net.h"

LKD_WEAK void lkd_net_object_released(lkd_fdobj *o) { (void)o; }
LKD_WEAK void lkd_net_signal_queued(lkd_task *t) { (void)t; }

LKD_WEAK int lkd_net_make_notify_pipe(int *rd, int *wr)
{
    int p[2];
    if (pipe(p) < 0) return LKD_HOST_ERR();
    for (int i = 0; i < 2; i++) {
        fcntl(p[i], F_SETFL, fcntl(p[i], F_GETFL) | O_NONBLOCK);
        fcntl(p[i], F_SETFD, FD_CLOEXEC);
    }
    *rd = p[0];
    *wr = p[1];
    return 0;
}

LKD_WEAK int lkd_net_wait_object(lkd_task *t, lkd_fdobj *o, uint32_t want, int64_t deadline_ns)
{
    (void)t; (void)o; (void)want; (void)deadline_ns;
    return -LX_ENOSYS;
}

LKD_WEAK char *lkd_net_render_routes(int ipv6, size_t *len_out) { (void)ipv6; if (len_out) *len_out = 0; return NULL; }

/* No net module: no inotify, procfs does not publish the files; the Linux defaults for completeness. */
LKD_WEAK void lkd_net_inotify_limits(struct lkd_inotify_limits *out)
{
    out->max_user_instances = 128;
    out->max_user_watches = 65536;
    out->max_queued_events = 16384;
}
