/* remote.c: cross-process guest memory service and process_vm_readv/writev. Owner: mm.
 *
 * Transport: every host process that owns a production address space runs one service thread that
 * accepts requests on an AF_UNIX socket, $TMPDIR/lkdmm.<host pid>.sock. The target process performs the
 * access itself, in its own window, so it can (1) open a read-only or PROT_NONE page for a forced
 * access (ptrace PEEK/POKE and /proc/<pid>/mem use FOLL_FORCE) and (2) invalidate its own translated
 * code after a write, before answering. No task port, no task_for_pid, no authorization prompt.
 * A guest pid is turned into a host pid through the family registry (lkd_proc_lookup).
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "mm_priv.h"

enum { OP_READ = 1, OP_WRITE = 2, OP_MAPS = 3 };
#define RF_FORCE 1u
#define CHUNK (1u << 20)

struct req { uint32_t op, flags; uint64_t gva, len; };
struct rsp { int64_t result; };

static int srv_fd = -1;
static pthread_t srv_thr;
static int srv_running;
static _Atomic int srv_stop;
static char srv_path[104];

/* ---- Local access (the target side) ------------------------------------------------------------ */

/* Copy up to n bytes between the guest memory of this process and buf, stopping at the first page that
 * is not accessible. Returns the bytes copied, or -LX_EFAULT when none. */
int64_t mm_local_rw(lkd_mm *mm, uint64_t gva, void *buf, uint64_t n, int write, int force)
{
    if (!mm) return -LX_ESRCH;
    struct mm_inval iv = { 0 };
    uint64_t done = 0;
    mm_lock(mm);
    while (done < n) {
        uint64_t a = gva + done;
        if (a < gva || a < mm_win.g_lo || a >= mm_win.g_hi) break;
        uint64_t c = GP - (a & (GP - 1));
        if (c > n - done) c = n - done;
        if (mm_check_range_mm(mm, a, c, write, force)) break;
        mm_vma *v = vt_find(&mm->vt, a);
        uint64_t hpg = HP_DOWN(a);
        /* A page holding translated code (watch.c): drop the translations first, the same path as a guest
         * store, then copy under the watch lock so the page cannot be watched again during the copy. */
        int dropped = 0;
        for (int tries = 0;; tries++) {
            if (write && mm_watched(hpg)) dropped |= mm_unwatch_for_write(mm, a, !force, &iv) > 0;
            mm_watch_lock();
            if (!(write && mm_watched(hpg)) || tries >= 2) break;
            mm_watch_unlock();
        }
        int hp = 0;
        for (mm_vma *o = vt_first_end_after(&mm->vt, hpg); o && o->start < hpg + mm_hps; o = vt_next(&mm->vt, o))
            hp |= mm_host_prot_of(o->prot);
        if (mm_page_guarded(mm, hpg)) hp = PROT_NONE; /* opened for the copy, closed again by mm_host_refresh */
        if (write && mm_watched(hpg)) {
            /* translated again in the meantime: unwatch now, invalidate once the locks are dropped */
            mm_unwatch_bit(hpg);
            mm_inval_add(&iv, hpg, hpg + mm_hps);
            hp &= ~PROT_WRITE;
        }
        /* A FOLL_FORCE unwatch leaves the host page read-only (watch.c): the VMA union may say RW (code and
         * data in one 16 KiB page), so open it explicitly; mm_host_refresh restores the union afterwards. */
        if (dropped && force) hp &= ~PROT_WRITE;
        int need_open = write ? !(hp & PROT_WRITE) : !(hp & PROT_READ);
        if (need_open && mm_temp_access(mm, a, a + c, write)) { mm_watch_unlock(); break; }
        /* The VMA check was done above (with FOLL_FORCE rules): copy with the fault recovery of lkd_user_*
         * but without its per-task check, by detaching the task binding for the copy. */
        lkd_task *cur = lkd_current();
        lkd_set_current(NULL);
        int r = write ? lkd_user_write(a, (const char *)buf + done, c) : lkd_user_read(a, (char *)buf + done, c);
        lkd_set_current(cur);
        if (need_open) mm_host_refresh(mm, a, a + c);
        mm_watch_unlock();
        if (r) break;
        if (write && (v->prot & LX_PROT_EXEC)) mm_inval_add(&iv, a, a + c);
        done += c;
    }
    mm_unlock(mm);
    mm_inval_flush(&iv);
    return done ? (int64_t)done : -LX_EFAULT;
}

/* ---- Service thread ---------------------------------------------------------------------------- */

static int full_io(int fd, void *p, size_t n, int wr)
{
    size_t d = 0;
    while (d < n) {
        ssize_t r = wr ? write(fd, (char *)p + d, n - d) : read(fd, (char *)p + d, n - d);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        d += (size_t)r;
    }
    return 0;
}

static void serve_one(int c)
{
    struct req q;
    struct rsp a = { -LX_EINVAL };
    if (full_io(c, &q, sizeof q, 0)) return;
    lkd_mm *mm = mm_win.owner;
    if (q.op == OP_MAPS) {
        size_t len = 0;
        char *txt = mm ? lkd_mm_render_maps(mm, &len) : NULL;
        a.result = txt ? (int64_t)len : (mm ? -LX_ENOMEM : -LX_ESRCH);
        if (!full_io(c, &a, sizeof a, 1) && txt) full_io(c, txt, len, 1);
        free(txt);
        return;
    }
    if (q.len > CHUNK) q.len = CHUNK;
    char *buf = malloc(q.len ? q.len : 1);
    if (!buf) { a.result = -LX_ENOMEM; full_io(c, &a, sizeof a, 1); return; }
    if (q.op == OP_WRITE) {
        if (full_io(c, buf, q.len, 0)) { free(buf); return; }
        a.result = mm_local_rw(mm, q.gva, buf, q.len, 1, q.flags & RF_FORCE);
        full_io(c, &a, sizeof a, 1);
    } else if (q.op == OP_READ) {
        a.result = mm_local_rw(mm, q.gva, buf, q.len, 0, q.flags & RF_FORCE);
        if (!full_io(c, &a, sizeof a, 1) && a.result > 0) full_io(c, buf, (size_t)a.result, 1);
    } else {
        full_io(c, &a, sizeof a, 1);
    }
    free(buf);
}

static void *serve(void *arg)
{
    (void)arg;
    for (;;) {
        int c = accept(srv_fd, NULL, NULL);
        if (atomic_load(&srv_stop)) { if (c >= 0) close(c); break; }
        if (c < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            break;
        }
        int one = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        serve_one(c);
        close(c);
    }
    return NULL;
}

static void sock_path(pid_t hp, char *out, size_t cap)
{
    const char *d = getenv("TMPDIR");
    if (!d || !*d || strlen(d) > 70) d = "/tmp";
    size_t l = strlen(d);
    snprintf(out, cap, "%s%slkdmm.%d.sock", d, l && d[l - 1] == '/' ? "" : "/", (int)hp);
}

int mm_remote_start(void)
{
    if (srv_running) return 0;
    sock_path(getpid(), srv_path, sizeof srv_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return (int)LKD_HOST_ERR();
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", srv_path);
    unlink(srv_path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 16) < 0) {
        int64_t e = LKD_HOST_ERR();
        close(fd);
        lkd_log("mm: remote memory service unavailable (%s): %lld", srv_path, (long long)e);
        return (int)e;
    }
    srv_fd = fd;
    atomic_store(&srv_stop, 0);
    if (pthread_create(&srv_thr, NULL, serve, NULL)) {
        close(fd);
        unlink(srv_path);
        srv_fd = -1;
        return -LX_EAGAIN;
    }
    srv_running = 1;
    return 0;
}

void mm_remote_stop(void)
{
    if (!srv_running) return;
    atomic_store(&srv_stop, 1);
    /* wake accept() with a connection of our own (closing a listening socket does not wake it) */
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c >= 0) {
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", srv_path);
        connect(c, (struct sockaddr *)&sa, sizeof sa);
        close(c);
    }
    pthread_join(srv_thr, NULL);
    close(srv_fd);
    unlink(srv_path);
    srv_fd = -1;
    srv_running = 0;
}

/* The service thread does not survive fork(); the child listens on its own path. */
void mm_remote_fork_child(void)
{
    if (!srv_running) return;
    close(srv_fd);
    srv_fd = -1;
    srv_running = 0;
    mm_remote_start();
}

/* ---- Client side ------------------------------------------------------------------------------- */

static int connect_host(pid_t hp)
{
    char path[104];
    sock_path(hp, path, sizeof path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return (int)LKD_HOST_ERR();
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(fd);
        return -LX_ESRCH;
    }
    return fd;
}

/* One request to the service of host process hp. Returns bytes or -errno. */
int64_t mm_remote_rw_host(pid_t hp, uint64_t gva, void *buf, uint64_t n, int write, int force)
{
    uint64_t done = 0;
    while (done < n) {
        uint64_t c = n - done < CHUNK ? n - done : CHUNK;
        int fd = connect_host(hp);
        if (fd < 0) return done ? (int64_t)done : fd;
        struct req q = { write ? OP_WRITE : OP_READ, force ? RF_FORCE : 0, gva + done, c };
        struct rsp a;
        int bad = full_io(fd, &q, sizeof q, 1) || (write && full_io(fd, (char *)buf + done, c, 1)) ||
                  full_io(fd, &a, sizeof a, 0);
        if (!bad && !write && a.result > 0) bad = full_io(fd, (char *)buf + done, (size_t)a.result, 0);
        close(fd);
        if (bad) return done ? (int64_t)done : -LX_ESRCH;
        if (a.result < 0) return done ? (int64_t)done : a.result;
        done += (uint64_t)a.result;
        if ((uint64_t)a.result < c) break;
    }
    return (int64_t)done;
}

char *mm_remote_maps_host(pid_t hp, size_t *len_out, int *err)
{
    int fd = connect_host(hp);
    if (fd < 0) { *err = fd; return NULL; }
    struct req q = { OP_MAPS, 0, 0, 0 };
    struct rsp a;
    char *txt = NULL;
    if (full_io(fd, &q, sizeof q, 1) || full_io(fd, &a, sizeof a, 0)) { *err = -LX_ESRCH; goto out; }
    if (a.result < 0) { *err = (int)a.result; goto out; }
    txt = malloc((size_t)a.result + 1);
    if (!txt) { *err = -LX_ENOMEM; goto out; }
    if (full_io(fd, txt, (size_t)a.result, 0)) { free(txt); txt = NULL; *err = -LX_ESRCH; goto out; }
    txt[a.result] = 0;
    if (len_out) *len_out = (size_t)a.result;
    *err = 0;
out:
    close(fd);
    return txt;
}

/* Resolve a guest pid: 0 = this process, > 0 host pid of another process, < 0 error. */
static int64_t resolve(int32_t pid)
{
    lkd_task *t = lkd_current();
    if (t && t->proc && t->proc->pid == pid) return 0;
    lkd_proc_info *info = malloc(sizeof *info);
    if (!info) return -LX_ENOMEM;
    int r = lkd_proc_lookup(pid, info);
    int64_t hp = r < 0 ? r : (info->state == LKD_PS_ZOMBIE ? -LX_ESRCH : info->host_pid);
    free(info);
    if (hp > 0 && hp == getpid()) return 0;
    return hp;
}

static int64_t rw_pid(int32_t pid, uint64_t gva, void *buf, uint64_t n, int write, int force)
{
    int64_t hp = resolve(pid);
    if (hp < 0) return hp;
    if (hp == 0) return mm_local_rw(mm_win.owner, gva, buf, n, write, force);
    return mm_remote_rw_host((pid_t)hp, gva, buf, n, write, force);
}

/* ptrace PEEK/POKE and /proc/<pid>/mem semantics (FOLL_FORCE). */
int64_t lkd_mm_remote_read(int32_t pid, uint64_t gva, void *dst, size_t n) { return rw_pid(pid, gva, dst, n, 0, 1); }
int64_t lkd_mm_remote_write(int32_t pid, uint64_t gva, const void *src, size_t n)
{
    return rw_pid(pid, gva, (void *)src, n, 1, 1);
}

char *lkd_mm_remote_render_maps(int32_t pid, size_t *len_out, int *err)
{
    int e = 0;
    if (!err) err = &e;
    if (len_out) *len_out = 0;
    int64_t hp = resolve(pid);
    if (hp < 0) { *err = (int)hp; return NULL; }
    if (hp == 0) {
        char *t = lkd_mm_render_maps(mm_win.owner, len_out);
        *err = t ? 0 : (mm_win.owner ? -LX_ENOMEM : -LX_ESRCH);
        return t;
    }
    return mm_remote_maps_host((pid_t)hp, len_out, err);
}

/* ---- process_vm_readv / process_vm_writev (mm/process_vm_access.c) ------------------------------- */

#define MAX_RW_COUNT (INT32_MAX & ~(int64_t)(GP - 1))

static int64_t load_iov(uint64_t gva, uint64_t cnt, struct lx_iovec **out)
{
    *out = NULL;
    if (cnt == 0) return 0;
    struct lx_iovec *v = malloc(cnt * sizeof *v);
    if (!v) return -LX_ENOMEM;
    if (lkd_user_read(gva, v, cnt * sizeof *v)) { free(v); return -LX_EFAULT; }
    for (uint64_t i = 0; i < cnt; i++)
        if ((int64_t)v[i].iov_len < 0) { free(v); return -LX_EINVAL; }
    *out = v;
    return 0;
}

static int64_t process_vm_rw(lkd_task *t, const lkd_sysreq *rq, int write)
{
    int32_t pid = LKD_ARGI(0);
    uint64_t lvec = LKD_ARGP(1), lcnt = LKD_ARGU(2), rvec = LKD_ARGP(3), rcnt = LKD_ARGU(4), flags = LKD_ARGU(5);
    if (flags) return -LX_EINVAL;
    if (lcnt > LX_IOV_MAX || rcnt > LX_IOV_MAX) return -LX_EINVAL;
    struct lx_iovec *lv = NULL, *rv = NULL;
    int64_t r = load_iov(lvec, lcnt, &lv);
    if (r) return r;
    uint64_t total = 0;
    for (uint64_t i = 0; i < lcnt; i++) {
        if (lv[i].iov_len > (uint64_t)MAX_RW_COUNT - total) lv[i].iov_len = (uint64_t)MAX_RW_COUNT - total;
        total += lv[i].iov_len;
    }
    if (!total) { free(lv); return 0; }
    if ((r = load_iov(rvec, rcnt, &rv))) { free(lv); return r; }
    int any = 0;
    for (uint64_t i = 0; i < rcnt; i++) if (rv[i].iov_len) any = 1;
    if (!any) { free(lv); free(rv); return 0; }
    int64_t hp = resolve(pid);
    if (hp < 0) { free(lv); free(rv); return hp; }
    r = lkd_ptrace_check_access(t, pid, LKD_PTRACE_MODE_ATTACH);
    if (r == -LX_EACCES) r = -LX_EPERM;
    if (r) { free(lv); free(rv); return r; }

    char *buf = malloc(65536);
    if (!buf) { free(lv); free(rv); return -LX_ENOMEM; }
    uint64_t li = 0, loff = 0, copied = 0;
    r = 0;
    for (uint64_t ri = 0; ri < rcnt && li < lcnt && !r; ri++) {
        uint64_t roff = 0;
        while (roff < rv[ri].iov_len && li < lcnt) {
            while (li < lcnt && loff == lv[li].iov_len) { li++; loff = 0; }
            if (li >= lcnt) break;
            uint64_t c = rv[ri].iov_len - roff;
            if (c > lv[li].iov_len - loff) c = lv[li].iov_len - loff;
            if (c > 65536) c = 65536;
            int64_t n;
            if (write) {
                if (lkd_user_read(lv[li].iov_base + loff, buf, c)) { r = -LX_EFAULT; break; }
                n = hp ? mm_remote_rw_host((pid_t)hp, rv[ri].iov_base + roff, buf, c, 1, 0)
                       : mm_local_rw(mm_win.owner, rv[ri].iov_base + roff, buf, c, 1, 0);
            } else {
                n = hp ? mm_remote_rw_host((pid_t)hp, rv[ri].iov_base + roff, buf, c, 0, 0)
                       : mm_local_rw(mm_win.owner, rv[ri].iov_base + roff, buf, c, 0, 0);
                if (n > 0 && lkd_user_write(lv[li].iov_base + loff, buf, (size_t)n)) { r = -LX_EFAULT; break; }
            }
            if (n < 0) { r = n; break; }
            copied += (uint64_t)n;
            roff += (uint64_t)n;
            loff += (uint64_t)n;
            if ((uint64_t)n < c) { r = -LX_EFAULT; break; } /* remote page not accessible: stop here */
        }
    }
    free(buf);
    free(lv);
    free(rv);
    return copied ? (int64_t)copied : r;
}

LKD_SYSCALL(process_vm_readv) { return process_vm_rw(t, rq, 0); }
LKD_SYSCALL(process_vm_writev) { return process_vm_rw(t, rq, 1); }

void mm_remote_register(void)
{
    LKD_REGISTER(process_vm_readv);
    LKD_REGISTER(process_vm_writev);
}
