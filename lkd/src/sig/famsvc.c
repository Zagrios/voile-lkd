/* famsvc.c: the family IPC service (contract in src/include/famsvc.h). Owner: sig.
 *
 * One listening AF_UNIX stream socket per host process, "<dir>/<host pid>.s", served by one host
 * thread that polls the listening socket and every accepted connection. Callers keep a small pool of
 * idle connections per target, checked for EOF before reuse. Wire format: a fixed 56-byte header, then
 * `len` payload bytes; descriptors ride as SCM_RIGHTS on the header bytes.
 */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/ucred.h>
#include <time.h>
#include <unistd.h>

#include "famsvc.h"
#include "task.h"

#define FS_MAGIC 0x53464b4cu /* "LKFS" */
#define FS_VERSION 1
#define FS_KIND_REQ 1
#define FS_KIND_RESP 2
#define FS_MAX_CONN 256
#define FS_POOL 32

struct fs_wire {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t op;
    uint32_t seq;
    int32_t status;
    int32_t target_pid, target_tid;
    int32_t caller_pid, caller_tid;
    uint32_t caller_uid;
    uint32_t nfds;
    uint32_t _pad;
    uint64_t len;
};
_Static_assert(sizeof(struct fs_wire) == 56, "famsvc wire header");

static lkd_famsvc_handler g_handlers[LKD_FAMSVC_OP_MAX];

static pthread_mutex_t g_dir_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_dir[PATH_MAX];
static int g_dir_ok;

static pthread_mutex_t g_svc_lock = PTHREAD_MUTEX_INITIALIZER; /* start/stop */
static int g_listen = -1;
static int g_stop[2] = { -1, -1 };
static pthread_t g_thread;
static _Atomic int g_running;
static pid_t g_owner;              /* host pid the running service belongs to */
static int g_atexit_done;
static _Thread_local int g_in_service;

static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { pid_t pid; int fd; int tmo; } g_pool[FS_POOL];
static int g_pool_n;
static _Atomic uint32_t g_seq;

static _Atomic uint64_t st_calls, st_served, st_connects, st_timeouts, st_adopted;

/* ---- small helpers ---------------------------------------------------------------------------- */

static void set_cloexec(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }
static void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

static void close_fds(int *fds, int n)
{
    for (int i = 0; i < n; i++)
        if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; }
}

static int sock_path(pid_t pid, char *out, size_t cap)
{
    const char *d = lkd_famsvc_dir();
    if (!d) return -1;
    int n = snprintf(out, cap, "%s/%d.s", d, (int)pid);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* Wait for fd to be ready; returns 0, -LX_ETIMEDOUT, or a negative errno. */
static int wait_fd(int fd, short ev, int64_t deadline)
{
    /* A blocking socket returns EAGAIN only when its SO_RCVTIMEO/SO_SNDTIMEO expired. */
    if (!(fcntl(fd, F_GETFL) & O_NONBLOCK)) return -LX_ETIMEDOUT;
    for (;;) {
        int64_t left = deadline - lkd_host_mono_ns();
        if (left <= 0) return -LX_ETIMEDOUT;
        int ms = (int)((left + 999999) / 1000000);
        struct pollfd p = { .fd = fd, .events = ev, .revents = 0 };
        int r = poll(&p, 1, ms);
        if (r < 0) { if (errno == EINTR) continue; return (int)LKD_HOST_ERR(); }
        if (r > 0) return 0;
    }
}

/* Send header (with descriptors) and payload on a nonblocking stream socket. */
static int send_msg(int fd, const struct fs_wire *h, const void *data, size_t len, const int *fds, int nfds, int64_t deadline)
{
    struct iovec iov[2] = { { (void *)h, sizeof *h }, { (void *)data, len } };
    union { struct cmsghdr c; char buf[CMSG_SPACE(sizeof(int) * LKD_FAMSVC_MAX_FDS)]; } cm;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = iov;
    mh.msg_iovlen = len ? 2 : 1;
    if (nfds > 0) {
        memset(&cm, 0, sizeof cm);
        mh.msg_control = cm.buf;
        mh.msg_controllen = (socklen_t)CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = (socklen_t)CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    }
    size_t total = sizeof *h + len, sent = 0;
    int first = 1;
    while (sent < total) {
        ssize_t r;
        if (first) {
            r = sendmsg(fd, &mh, 0);
        } else {
            const char *p;
            size_t n;
            if (sent < sizeof *h) { p = (const char *)h + sent; n = sizeof *h - sent; }
            else { p = (const char *)data + (sent - sizeof *h); n = total - sent; }
            r = send(fd, p, n, 0);
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                int w = wait_fd(fd, POLLOUT, deadline);
                if (w < 0) return w;
                continue;
            }
            return (int)LKD_HOST_ERR();
        }
        first = 0;
        sent += (size_t)r;
    }
    return 0;
}

/* Receive one message. On success *data is malloc'ed (or NULL when len is 0) and fds[] are filled
 * (CLOEXEC set). Returns 0, -LX_ETIMEDOUT, -LX_EPROTO, 1 for a clean EOF before any byte, or -errno. */
static int recv_msg(int fd, struct fs_wire *h, void **data, int *fds, int *nfds, int64_t deadline)
{
    size_t got = 0;
    *nfds = 0;
    *data = NULL;
    while (got < sizeof *h) {
        union { struct cmsghdr c; char buf[CMSG_SPACE(sizeof(int) * LKD_FAMSVC_MAX_FDS)]; } cm;
        struct iovec iov = { (char *)h + got, sizeof *h - got };
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cm.buf;
        mh.msg_controllen = sizeof cm.buf;
        ssize_t r = recvmsg(fd, &mh, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                int w = wait_fd(fd, POLLIN, deadline);
                if (w < 0) { close_fds(fds, *nfds); *nfds = 0; return w; }
                continue;
            }
            int e = (int)LKD_HOST_ERR();
            close_fds(fds, *nfds); *nfds = 0;
            return e;
        }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            int *in = (int *)(void *)CMSG_DATA(c);
            for (int i = 0; i < n; i++) {
                set_cloexec(in[i]);
                if (*nfds < LKD_FAMSVC_MAX_FDS) fds[(*nfds)++] = in[i];
                else close(in[i]);
            }
        }
        if (r == 0) {
            close_fds(fds, *nfds); *nfds = 0;
            return got == 0 ? 1 : -LX_EPROTO;
        }
        got += (size_t)r;
    }
    if (h->magic != FS_MAGIC || h->version != FS_VERSION || h->len > LKD_FAMSVC_MAX_PAYLOAD ||
        h->nfds > LKD_FAMSVC_MAX_FDS) {
        close_fds(fds, *nfds); *nfds = 0;
        return -LX_EPROTO;
    }
    if (h->len) {
        char *buf = malloc(h->len);
        if (!buf) { close_fds(fds, *nfds); *nfds = 0; return -LX_ENOMEM; }
        size_t pg = 0;
        while (pg < h->len) {
            ssize_t r = recv(fd, buf + pg, h->len - pg, 0);
            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) {
                    int w = wait_fd(fd, POLLIN, deadline);
                    if (w < 0) { free(buf); close_fds(fds, *nfds); *nfds = 0; return w; }
                    continue;
                }
                int e = (int)LKD_HOST_ERR();
                free(buf); close_fds(fds, *nfds); *nfds = 0;
                return e;
            }
            if (r == 0) { free(buf); close_fds(fds, *nfds); *nfds = 0; return -LX_EPROTO; }
            pg += (size_t)r;
        }
        *data = buf;
    }
    return 0;
}

/* ---- directory -------------------------------------------------------------------------------- */

/* Remove family directories left by crashed runs: lkdfam.* under base, older than a minute, holding
 * only sockets of dead processes and the futex file. */
static void gc_stale_dirs(const char *base)
{
    DIR *b = opendir(base);
    if (!b) return;
    struct dirent *e;
    time_t now = time(NULL);
    while ((e = readdir(b))) {
        if (strncmp(e->d_name, "lkdfam.", 7) != 0) continue;
        char d[PATH_MAX];
        snprintf(d, sizeof d, "%s/%s", base, e->d_name);
        struct stat st;
        if (lstat(d, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || now - st.st_mtime < 60) continue;
        DIR *x = opendir(d);
        if (!x) continue;
        int keep = 0;
        struct dirent *f;
        while ((f = readdir(x)) && !keep) {
            if (!strcmp(f->d_name, ".") || !strcmp(f->d_name, "..") || !strcmp(f->d_name, "futex.shm") ||
                !strcmp(f->d_name, "sem.shm")) continue;
            size_t n = strlen(f->d_name);
            if (n < 3 || strcmp(f->d_name + n - 2, ".s") != 0) { keep = 1; break; }
            int pid = atoi(f->d_name);
            if (pid <= 0 || kill(pid, 0) == 0 || errno == EPERM) keep = 1;
        }
        closedir(x);
        if (keep) continue;
        x = opendir(d);
        if (!x) continue;
        while ((f = readdir(x))) {
            if (!strcmp(f->d_name, ".") || !strcmp(f->d_name, "..")) continue;
            char q[PATH_MAX];
            snprintf(q, sizeof q, "%s/%s", d, f->d_name);
            unlink(q);
        }
        closedir(x);
        rmdir(d);
    }
    closedir(b);
}

const char *lkd_famsvc_dir(void)
{
    pthread_mutex_lock(&g_dir_lock);
    if (!g_dir_ok) {
        const char *env = getenv("LKD_FAMSVC_DIR");
        if (env && env[0]) {
            if (strlen(env) < sizeof g_dir && (mkdir(env, 0700) == 0 || errno == EEXIST)) {
                snprintf(g_dir, sizeof g_dir, "%s", env);
                g_dir_ok = 1;
            }
        } else {
            /* sun_path holds 104 bytes: "<dir>/<10 digits>.s" must fit. */
            const char *tmp = getenv("TMPDIR");
            char base[PATH_MAX];
            snprintf(base, sizeof base, "%s", tmp && tmp[0] ? tmp : "/tmp");
            size_t bl = strlen(base);
            while (bl > 1 && base[bl - 1] == '/') base[--bl] = 0;
            if (bl + strlen("/lkdfam.XXXXXX") + 14 > sizeof(((struct sockaddr_un *)0)->sun_path))
                snprintf(base, sizeof base, "/tmp");
            char tmpl[PATH_MAX];
            snprintf(tmpl, sizeof tmpl, "%s/lkdfam.XXXXXX", base);
            gc_stale_dirs(base);
            if (mkdtemp(tmpl)) {
                snprintf(g_dir, sizeof g_dir, "%s", tmpl);
                setenv("LKD_FAMSVC_DIR", g_dir, 1);
                g_dir_ok = 1;
            }
        }
    }
    const char *r = g_dir_ok ? g_dir : NULL;
    pthread_mutex_unlock(&g_dir_lock);
    return r;
}

/* ---- registration ----------------------------------------------------------------------------- */

int lkd_famsvc_register(uint32_t op, lkd_famsvc_handler fn)
{
    if (op == 0 || op >= LKD_FAMSVC_OP_MAX || !fn) return -LX_EINVAL;
    if (g_handlers[op] && g_handlers[op] != fn) return -LX_EEXIST;
    g_handlers[op] = fn;
    return 0;
}

static void op_ping(lkd_famsvc_msg *req, lkd_famsvc_msg *resp)
{
    resp->status = (int32_t)getpid();
    if (req->len) {
        resp->data = malloc(req->len);
        if (!resp->data) { resp->status = -LX_ENOMEM; return; }
        memcpy(resp->data, req->data, req->len);
        resp->len = req->len;
    }
    /* echo descriptors: hand back the received ones */
    for (int i = 0; i < req->nfds; i++) { resp->fds[i] = req->fds[i]; req->fds[i] = -1; }
    resp->nfds = req->nfds;
}

/* ---- server ----------------------------------------------------------------------------------- */

struct conn { int fd; pid_t peer; };

static void serve_one(struct conn *c)
{
    struct fs_wire h;
    void *data = NULL;
    int fds[LKD_FAMSVC_MAX_FDS], nfds = 0;
    int64_t deadline = lkd_host_mono_ns() + 5000000000ll;
    int r = recv_msg(c->fd, &h, &data, fds, &nfds, deadline);
    if (r != 0) { close(c->fd); c->fd = -1; return; }
    if (h.kind != FS_KIND_REQ) { free(data); close_fds(fds, nfds); close(c->fd); c->fd = -1; return; }

    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.op = h.op;
    req.target_pid = h.target_pid; req.target_tid = h.target_tid;
    req.caller_pid = h.caller_pid; req.caller_tid = h.caller_tid;
    req.caller_uid = h.caller_uid;
    req.caller_host_pid = (int32_t)c->peer;
    req.data = data; req.len = (size_t)h.len;
    for (int i = 0; i < nfds; i++) req.fds[i] = fds[i];
    req.nfds = nfds;
    for (int i = 0; i < LKD_FAMSVC_MAX_FDS; i++) resp.fds[i] = -1;
    resp.op = h.op;

    lkd_famsvc_handler fn = h.op < LKD_FAMSVC_OP_MAX ? g_handlers[h.op] : NULL;
    if (fn) fn(&req, &resp);
    else resp.status = -LX_ENOSYS;
    atomic_fetch_add(&st_served, 1);
    close_fds(req.fds, req.nfds);
    free(data);

    if (resp.nfds < 0 || resp.nfds > LKD_FAMSVC_MAX_FDS || resp.len > LKD_FAMSVC_MAX_PAYLOAD) {
        close_fds(resp.fds, resp.nfds < 0 ? 0 : (resp.nfds > LKD_FAMSVC_MAX_FDS ? LKD_FAMSVC_MAX_FDS : resp.nfds));
        free(resp.data);
        resp.data = NULL; resp.len = 0; resp.nfds = 0; resp.status = -LX_E2BIG;
    }
    struct fs_wire o;
    memset(&o, 0, sizeof o);
    o.magic = FS_MAGIC; o.version = FS_VERSION; o.kind = FS_KIND_RESP; o.op = h.op; o.seq = h.seq;
    o.status = resp.status; o.nfds = (uint32_t)resp.nfds; o.len = resp.len;
    if (send_msg(c->fd, &o, resp.data, resp.len, resp.fds, resp.nfds, lkd_host_mono_ns() + 5000000000ll) < 0) {
        close(c->fd);
        c->fd = -1;
    }
    close_fds(resp.fds, resp.nfds);
    free(resp.data);
}

static void set_timeouts(int fd, int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* The service loop: one kqueue watches the stop pipe, the listening socket and every connection
 * (kqueue rather than poll: poll is rebuilt on every call on Darwin). Accepted sockets are blocking
 * with 5 s send and receive timeouts, so a stuck peer cannot hold the service forever. */
static void *service_main(void *arg)
{
    (void)arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    pthread_setname_np("lkd-famsvc");
    g_in_service = 1;
    int lfd = g_listen, sfd = g_stop[0];
    int kq = kqueue();
    if (kq < 0) { lkd_log("famsvc: kqueue failed (%d): service not running", errno); return NULL; }
    struct kevent ch[2];
    EV_SET(&ch[0], (uintptr_t)sfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    EV_SET(&ch[1], (uintptr_t)lfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(kq, ch, 2, NULL, 0, NULL);
    int nconn = 0;
    struct conn **all_conns = calloc(FS_MAX_CONN, sizeof *all_conns);
    for (int stop = 0; !stop;) {
        struct kevent ev[32];
        int n = kevent(kq, NULL, 0, ev, 32, NULL);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; i++) {
            int fd = (int)ev[i].ident;
            if (fd == sfd && !ev[i].udata) { stop = 1; break; }
            if (fd == lfd && !ev[i].udata) {
                for (;;) {
                    int c = accept(lfd, NULL, NULL);
                    if (c < 0) break;
                    set_cloexec(c);
                    fcntl(c, F_SETFL, fcntl(c, F_GETFL) & ~O_NONBLOCK);
                    int one = 1;
                    setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
                    set_timeouts(c, 5000);
                    uid_t uid; gid_t gid;
                    pid_t peer = 0;
                    socklen_t pl = sizeof peer;
                    struct conn *cn = NULL;
                    if (getpeereid(c, &uid, &gid) == 0 && uid == geteuid() &&
                        getsockopt(c, SOL_LOCAL, LOCAL_PEERPID, &peer, &pl) == 0 && nconn < FS_MAX_CONN)
                        cn = malloc(sizeof *cn);
                    if (!cn) { close(c); continue; }
                    cn->fd = c;
                    cn->peer = peer;
                    struct kevent a;
                    EV_SET(&a, (uintptr_t)c, EVFILT_READ, EV_ADD, 0, 0, cn);
                    if (kevent(kq, &a, 1, NULL, 0, NULL) != 0) { close(c); free(cn); continue; }
                    for (int k = 0; k < FS_MAX_CONN; k++) if (!all_conns[k]) { all_conns[k] = cn; break; }
                    nconn++;
                }
                continue;
            }
            struct conn *cn = ev[i].udata;
            if (!cn) continue;
            serve_one(cn);
            if (cn->fd < 0) { /* closed: kqueue dropped the registration with the descriptor */
                for (int k = 0; k < FS_MAX_CONN; k++) if (all_conns[k] == cn) { all_conns[k] = NULL; break; }
                /* later events of this batch may still name cn: mark them */
                for (int j = i + 1; j < n; j++) if (ev[j].udata == cn) ev[j].udata = NULL;
                free(cn);
                nconn--;
            }
        }
    }
    for (int k = 0; k < FS_MAX_CONN; k++) if (all_conns[k]) { close(all_conns[k]->fd); free(all_conns[k]); }
    free(all_conns);
    close(kq);
    return NULL;
}

/* Adopt the listening socket of the previous image after a host re-exec, if it is ours. */
static int adopt_inherited(const char *path)
{
    const char *env = getenv("LKD_FAMSVC_FD");
    if (!env) return -1;
    int fd = -1, pid = 0;
    if (sscanf(env, "%d:%d", &fd, &pid) != 2 || fd < 0 || pid != (int)getpid()) return -1;
    struct sockaddr_un a;
    socklen_t al = sizeof a;
    memset(&a, 0, sizeof a);
    int acc = 0; /* SO_ACCEPTCONN is not available on Darwin (ENOPROTOOPT, measured): check SO_TYPE */
    socklen_t accl = sizeof acc;
    if (getsockname(fd, (struct sockaddr *)&a, &al) != 0 || a.sun_family != AF_UNIX ||
        strcmp(a.sun_path, path) != 0 ||
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &acc, &accl) != 0 || acc != SOCK_STREAM) {
        if (getenv("LKD_FAMSVC_DEBUG")) lkd_log("famsvc: not adopting fd %d: errno %d fam %d path '%s' want '%s' acc %d", fd, errno, a.sun_family, a.sun_path, path, acc);
        return -1;
    }
    return fd;
}

static void atexit_shutdown(void) { lkd_famsvc_shutdown(); }

static int start_locked(void)
{
    if (atomic_load(&g_running) && g_owner == getpid()) return 0;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    if (sock_path(getpid(), path, sizeof path) < 0) return -LX_ENAMETOOLONG;
    (void)mkdir(lkd_famsvc_dir(), 0700); /* removed by the last shutdown: a restart recreates it */
    int fd = adopt_inherited(path);
    if (fd >= 0) atomic_fetch_add(&st_adopted, 1);
    if (fd < 0) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return (int)LKD_HOST_ERR();
        struct sockaddr_un a;
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
        unlink(path); /* a stale file of a dead process that had the same host pid */
        if (bind(fd, (struct sockaddr *)&a, (socklen_t)SUN_LEN(&a)) != 0 || listen(fd, 128) != 0) {
            int e = (int)LKD_HOST_ERR();
            close(fd);
            return e;
        }
        chmod(path, 0600);
    }
    /* Not close-on-exec on purpose: a host re-exec adopts it (LKD_FAMSVC_FD). */
    fcntl(fd, F_SETFD, 0);
    set_nonblock(fd);
    char env[64];
    snprintf(env, sizeof env, "%d:%d", fd, (int)getpid());
    setenv("LKD_FAMSVC_FD", env, 1);
    if (pipe(g_stop) != 0) { int e = (int)LKD_HOST_ERR(); close(fd); return e; }
    set_cloexec(g_stop[0]); set_cloexec(g_stop[1]);
    g_listen = fd;
    g_owner = getpid();
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512 * 1024);
    int e = pthread_create(&g_thread, &at, service_main, NULL);
    pthread_attr_destroy(&at);
    if (e != 0) {
        close(g_stop[0]); close(g_stop[1]); g_stop[0] = g_stop[1] = -1;
        close(fd); g_listen = -1;
        return -lkd_errno_from_darwin(e);
    }
    atomic_store(&g_running, 1);
    if (!g_atexit_done) { g_atexit_done = 1; atexit(atexit_shutdown); }
    return 0;
}

int lkd_famsvc_start(void)
{
    lkd_famsvc_register(LKD_FAMSVC_OP_PING, op_ping);
    pthread_mutex_lock(&g_svc_lock);
    int r = start_locked();
    pthread_mutex_unlock(&g_svc_lock);
    return r;
}

int lkd_famsvc_running(void) { return atomic_load(&g_running) && g_owner == getpid(); }

static void pool_drop_all(void)
{
    for (int i = 0; i < g_pool_n; i++) close(g_pool[i].fd);
    g_pool_n = 0;
}

/* Remove sockets of dead processes; remove the directory when no live socket remains. */
static void sweep_dir(void)
{
    const char *d = lkd_famsvc_dir();
    if (!d) return;
    DIR *dir = opendir(d);
    if (!dir) return;
    int live = 0;
    struct dirent *e;
    while ((e = readdir(dir))) {
        size_t n = strlen(e->d_name);
        if (n < 3 || strcmp(e->d_name + n - 2, ".s") != 0) continue;
        int pid = atoi(e->d_name);
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", d, e->d_name);
        if (pid > 0 && (kill(pid, 0) == 0 || errno == EPERM)) live++;
        else unlink(p);
    }
    closedir(dir);
    if (!live) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/futex.shm", d);
        unlink(p);
        snprintf(p, sizeof p, "%s/sem.shm", d);
        unlink(p);
        rmdir(d);
    }
}

void lkd_famsvc_shutdown(void)
{
    pthread_mutex_lock(&g_svc_lock);
    if (atomic_load(&g_running) && g_owner == getpid()) {
        char c = 1;
        (void)!write(g_stop[1], &c, 1);
        pthread_join(g_thread, NULL);
        close(g_stop[0]); close(g_stop[1]); g_stop[0] = g_stop[1] = -1;
        char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
        if (sock_path(getpid(), path, sizeof path) == 0) unlink(path);
        close(g_listen);
        g_listen = -1;
        atomic_store(&g_running, 0);
        unsetenv("LKD_FAMSVC_FD");
        pthread_mutex_lock(&g_pool_lock);
        pool_drop_all();
        pthread_mutex_unlock(&g_pool_lock);
        sweep_dir();
    }
    pthread_mutex_unlock(&g_svc_lock);
}

/* ---- client ----------------------------------------------------------------------------------- */

static int pool_take(pid_t pid, int *tmo)
{
    int fd = -1;
    pthread_mutex_lock(&g_pool_lock);
    for (int i = g_pool_n - 1; i >= 0; i--) {
        if (g_pool[i].pid != pid) continue;
        int c = g_pool[i].fd;
        int ct = g_pool[i].tmo;
        g_pool[i] = g_pool[--g_pool_n];
        /* An idle connection never has data: a readable one was closed by the server. */
        char b;
        if (recv(c, &b, 1, MSG_PEEK | MSG_DONTWAIT) >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) { close(c); continue; }
        fd = c;
        *tmo = ct;
        break;
    }
    pthread_mutex_unlock(&g_pool_lock);
    return fd;
}

static void pool_put(pid_t pid, int fd, int tmo)
{
    pthread_mutex_lock(&g_pool_lock);
    if (g_pool_n < FS_POOL) { g_pool[g_pool_n].pid = pid; g_pool[g_pool_n].fd = fd; g_pool[g_pool_n].tmo = tmo; g_pool_n++; fd = -1; }
    pthread_mutex_unlock(&g_pool_lock);
    if (fd >= 0) close(fd);
}

static int connect_to(pid_t pid, int64_t deadline)
{
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (sock_path(pid, a.sun_path, sizeof a.sun_path) < 0) return -LX_ESRCH;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return (int)LKD_HOST_ERR();
    set_cloexec(fd);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    for (;;) {
        if (connect(fd, (struct sockaddr *)&a, (socklen_t)SUN_LEN(&a)) == 0) break;
        if (errno == EINTR) continue;
        int e = errno;
        close(fd);
        if (e == ENOENT || e == ECONNREFUSED || e == ENOTDIR) return -LX_ESRCH;
        if (e == EAGAIN && lkd_host_mono_ns() < deadline) { /* backlog full */
            usleep(200);
            fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return (int)LKD_HOST_ERR();
            set_cloexec(fd);
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
            continue;
        }
        return -lkd_errno_from_darwin(e);
    }
    atomic_fetch_add(&st_connects, 1);
    return fd;
}

void lkd_famsvc_msg_free(lkd_famsvc_msg *m)
{
    if (!m) return;
    free(m->data);
    m->data = NULL;
    m->len = 0;
    if (m->nfds > LKD_FAMSVC_MAX_FDS) m->nfds = LKD_FAMSVC_MAX_FDS;
    for (int i = 0; i < m->nfds; i++) if (m->fds[i] >= 0) close(m->fds[i]);
    m->nfds = 0;
}

int lkd_famsvc_call(int32_t host_pid, lkd_famsvc_msg *req, lkd_famsvc_msg *resp, int timeout_ms)
{
    if (!req || !resp || host_pid <= 0 || req->op == 0 || req->op >= LKD_FAMSVC_OP_MAX) return -LX_EINVAL;
    if (g_in_service) return -LX_EINVAL;
    if (req->len > LKD_FAMSVC_MAX_PAYLOAD || req->nfds < 0 || req->nfds > LKD_FAMSVC_MAX_FDS) return -LX_E2BIG;
    memset(resp, 0, sizeof *resp);
    for (int i = 0; i < LKD_FAMSVC_MAX_FDS; i++) resp->fds[i] = -1;
    if (timeout_ms <= 0) timeout_ms = LKD_FAMSVC_TIMEOUT_MS;
    int64_t deadline = lkd_host_mono_ns() + (int64_t)timeout_ms * 1000000ll;
    atomic_fetch_add(&st_calls, 1);

    struct fs_wire h;
    memset(&h, 0, sizeof h);
    h.magic = FS_MAGIC; h.version = FS_VERSION; h.kind = FS_KIND_REQ; h.op = req->op;
    h.seq = atomic_fetch_add(&g_seq, 1);
    h.target_pid = req->target_pid; h.target_tid = req->target_tid;
    h.caller_pid = req->caller_pid; h.caller_tid = req->caller_tid; h.caller_uid = req->caller_uid;
    lkd_task *cur = lkd_current();
    if (cur && cur->magic == LKD_TASK_MAGIC) {
        if (!h.caller_pid) h.caller_pid = cur->proc->pid;
        if (!h.caller_tid) h.caller_tid = cur->tid;
        if (!h.caller_uid) h.caller_uid = cur->proc->cred.euid;
    }
    h.nfds = (uint32_t)req->nfds;
    h.len = req->len;

    for (int attempt = 0; attempt < 2; attempt++) {
        int pooled = 1, tmo = -1;
        int fd = pool_take((pid_t)host_pid, &tmo);
        if (fd < 0) {
            pooled = 0;
            fd = connect_to((pid_t)host_pid, deadline);
            if (fd < 0) return fd;
        }
        if (tmo != timeout_ms) { set_timeouts(fd, timeout_ms); tmo = timeout_ms; }
        int e = send_msg(fd, &h, req->data, req->len, req->fds, req->nfds, deadline);
        if (e < 0) {
            close(fd);
            /* A pooled connection the server closed meanwhile: retry once on a fresh one. Nothing
             * was processed because the request never reached the server. */
            if (pooled && (e == -LX_EPIPE || e == -LX_ECONNRESET)) continue;
            if (e == -LX_ETIMEDOUT) {
                atomic_fetch_add(&st_timeouts, 1);
                lkd_log("famsvc: op %u to host pid %d timed out after %d ms", req->op, (int)host_pid, timeout_ms);
            }
            return e == -LX_EPIPE || e == -LX_ECONNRESET ? -LX_ESRCH : e;
        }
        struct fs_wire o;
        void *data = NULL;
        int nf = 0;
        e = recv_msg(fd, &o, &data, resp->fds, &nf, deadline);
        if (e != 0) {
            close(fd);
            if (e == -LX_ETIMEDOUT) {
                atomic_fetch_add(&st_timeouts, 1);
                lkd_log("famsvc: op %u to host pid %d timed out after %d ms", req->op, (int)host_pid, timeout_ms);
            }
            if (e == 1 || e == -LX_ECONNRESET) return -LX_ESRCH; /* the target died while serving */
            return e;
        }
        if (o.kind != FS_KIND_RESP || o.seq != h.seq || o.nfds != (uint32_t)nf) {
            free(data);
            close_fds(resp->fds, nf);
            close(fd);
            return -LX_EPROTO;
        }
        pool_put((pid_t)host_pid, fd, tmo);
        resp->op = o.op;
        resp->status = o.status;
        resp->data = data;
        resp->len = (size_t)o.len;
        resp->nfds = nf;
        return o.status;
    }
    return -LX_ESRCH;
}

int lkd_famsvc_call_pid(int32_t guest_pid, lkd_famsvc_msg *req, lkd_famsvc_msg *resp, int timeout_ms)
{
    lkd_proc_info *pi = malloc(sizeof *pi);
    if (!pi) return -LX_ENOMEM;
    int r = lkd_proc_lookup(guest_pid, pi);
    int32_t host = pi->host_pid;
    char state = pi->state;
    free(pi);
    if (r < 0 || host <= 0 || state == LKD_PS_ZOMBIE || state == LKD_PS_DEAD) {
        if (resp) { memset(resp, 0, sizeof *resp); }
        return -LX_ESRCH;
    }
    if (!req->target_pid) req->target_pid = guest_pid;
    return lkd_famsvc_call(host, req, resp, timeout_ms);
}

/* ---- fork ------------------------------------------------------------------------------------- */

void lkd_famsvc_fork_prepare(void)
{
    pthread_mutex_lock(&g_svc_lock);
    pthread_mutex_lock(&g_pool_lock);
}

void lkd_famsvc_fork_parent(void)
{
    pthread_mutex_unlock(&g_pool_lock);
    pthread_mutex_unlock(&g_svc_lock);
}

void lkd_famsvc_fork_child(void)
{
    lkd_fork_reinit_mutex(&g_pool_lock, 0);
    lkd_fork_reinit_mutex(&g_svc_lock, 0);
    lkd_fork_reinit_mutex(&g_dir_lock, 0);
    /* Connections and the listening socket belong to the parent's service: close our copies. */
    pool_drop_all();
    int was = atomic_load(&g_running);
    if (g_listen >= 0) close(g_listen);
    if (g_stop[0] >= 0) close(g_stop[0]);
    if (g_stop[1] >= 0) close(g_stop[1]);
    g_listen = -1;
    g_stop[0] = g_stop[1] = -1;
    atomic_store(&g_running, 0);
    g_in_service = 0;
    unsetenv("LKD_FAMSVC_FD");
    if (was) {
        int e = start_locked();
        if (e < 0) lkd_log("famsvc: fork child could not start its service: %d", e);
    }
}

void lkd_famsvc_get_stats(struct lkd_famsvc_stats *out)
{
    out->calls = atomic_load(&st_calls);
    out->served = atomic_load(&st_served);
    out->connects = atomic_load(&st_connects);
    out->timeouts = atomic_load(&st_timeouts);
    out->adopted = atomic_load(&st_adopted);
}
