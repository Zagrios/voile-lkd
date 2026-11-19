/* test_sig_ntsync.c: /dev/ntsync (sig module, src/sig/ntsync.c). Follows the kernel selftests
 * (tools/testing/selftests/drivers/ntsync/ntsync.c, Linux 6.14): object state and errors, NT_WAIT_ANY,
 * NT_WAIT_ALL, alerts, wakes across threads, a signal ending a wait, sharing across a host fork and through
 * SCM_RIGHTS anchors, slots reused after close, and the release at a group exit. */
#include <pthread.h>
#include <sys/wait.h>

#include "test_procfs_util.h"
#include "ntsync.h"
#include "sig.h"

#define NT_IOC(dir, nr, size) (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)'N' << 8) | (uint32_t)(nr))
#define CREATE_SEM   NT_IOC(1, 0x80, 8)
#define SEM_RELEASE  NT_IOC(3, 0x81, 4)
#define NT_WAIT_ANY     NT_IOC(3, 0x82, 40)
#define NT_WAIT_ALL     NT_IOC(3, 0x83, 40)
#define CREATE_MUTEX NT_IOC(1, 0x84, 8)
#define MUTEX_UNLOCK NT_IOC(3, 0x85, 8)
#define MUTEX_KILL   NT_IOC(1, 0x86, 4)
#define CREATE_EVENT NT_IOC(1, 0x87, 8)
#define EVENT_SET    NT_IOC(2, 0x88, 4)
#define EVENT_RESET  NT_IOC(2, 0x89, 4)
#define EVENT_PULSE  NT_IOC(2, 0x8a, 4)
#define SEM_READ     NT_IOC(2, 0x8b, 8)
#define MUTEX_READ   NT_IOC(2, 0x8c, 8)
#define EVENT_READ   NT_IOC(2, 0x8d, 8)

struct wait_args { uint64_t timeout, objs; uint32_t count, index, flags, owner, alert, pad; };

static int DEV;

void sig_ntsync_host_exit(void); /* sig private: the last guest process of the host process exits */

/* Guest scratch for ioctl arguments, in the bulk part of the fixture's scratch: one 4 KiB area per caller. */
static void *area(int k) { return SB + 65536 + (size_t)k * 4096; }

static int64_t io(lkd_task *t, int fd, uint32_t cmd, void *arg) { return SC(t, ioctl, (uint64_t)fd, cmd, P(arg)); }

static int create_sem(uint32_t count, uint32_t max)
{
    uint32_t *a = area(0); a[0] = count; a[1] = max;
    return (int)io(T, DEV, CREATE_SEM, a);
}
static int create_mutex(uint32_t owner, uint32_t count)
{
    uint32_t *a = area(0); a[0] = owner; a[1] = count;
    return (int)io(T, DEV, CREATE_MUTEX, a);
}
static int create_event(uint32_t manual, uint32_t signaled)
{
    uint32_t *a = area(0); a[0] = manual; a[1] = signaled;
    return (int)io(T, DEV, CREATE_EVENT, a);
}
static int64_t read2(int fd, uint32_t cmd, uint32_t *x, uint32_t *y)
{
    uint32_t *a = area(0); a[0] = a[1] = 0xdeadbeef;
    int64_t r = io(T, fd, cmd, a);
    *x = a[0]; *y = a[1];
    return r;
}
static int64_t op1(int fd, uint32_t cmd, uint32_t in, uint32_t *out)
{
    uint32_t *a = area(0); a[0] = in;
    int64_t r = io(T, fd, cmd, a);
    if (out) *out = a[0];
    return r;
}
static int64_t unlock_mutex(int fd, uint32_t owner, uint32_t *prev)
{
    uint32_t *a = area(0); a[0] = owner; a[1] = 0xdeadbeef;
    int64_t r = io(T, fd, MUTEX_UNLOCK, a);
    if (prev) *prev = a[1];
    return r;
}

/* A wait from task t with its own scratch k: objects, relative timeout in ns (-1 infinite, on the
 * monotonic clock), owner, alert fd (0 none). Returns the ioctl result and the index. */
static int64_t wait_as(lkd_task *t, int k, int all, const int *fds, uint32_t n, int64_t rel_ns, uint32_t owner,
                       int alert, uint32_t *index)
{
    struct wait_args *a = area(k);
    uint32_t *objs = (uint32_t *)((char *)area(k) + 64);
    for (uint32_t i = 0; i < n; i++) objs[i] = (uint32_t)fds[i];
    memset(a, 0, sizeof *a);
    a->timeout = rel_ns < 0 ? UINT64_MAX : (uint64_t)(lkd_guest_clock_ns(LX_CLOCK_MONOTONIC) + rel_ns);
    a->objs = P(objs);
    a->count = n;
    a->index = 0xdeadbeef;
    a->owner = owner;
    a->alert = (uint32_t)alert;
    int64_t r = io(t, DEV, all ? NT_WAIT_ALL : NT_WAIT_ANY, a);
    if (index) *index = a->index;
    return r;
}
static int64_t wait_any(const int *fds, uint32_t n, uint32_t owner, uint32_t *index)
{
    return wait_as(T, 0, 0, fds, n, 0, owner, 0, index);
}
static int64_t wait_all(const int *fds, uint32_t n, uint32_t owner, uint32_t *index)
{
    return wait_as(T, 0, 1, fds, n, 0, owner, 0, index);
}

/* ---- object state ------------------------------------------------------------------------------- */

LT_TEST(device_node)
{
    struct lx_stat st;
    LT_EQ(SC(T, newfstatat, AT_CWD, gstr("/dev/ntsync"), P(area(10)), 0), 0);
    memcpy(&st, area(10), sizeof st);
    LT_EQ(st.st_mode, LX_S_IFCHR | 0666);
    LT_EQ(st.st_rdev, (10u << 8) | 126u);
    DEV = (int)gopen("/dev/ntsync", LX_O_RDONLY | LX_O_CLOEXEC);
    LT_CHECK(DEV >= 0);
}

LT_TEST(semaphore_state)
{
    LT_ERRNO(create_sem(3, 2), LX_EINVAL);
    int sem = create_sem(2, 2);
    LT_CHECK(sem >= 0);
    uint32_t c, m, idx, prev;
    LT_EQ(read2(sem, SEM_READ, &c, &m), 0);
    LT_EQ(c, 2); LT_EQ(m, 2);
    LT_ERRNO(op1(sem, SEM_RELEASE, 1, &prev), LX_EOVERFLOW);
    LT_EQ(wait_any(&sem, 1, 123, &idx), 0);
    LT_EQ(idx, 0);
    LT_EQ(wait_any(&sem, 1, 123, &idx), 0);
    LT_ERRNO(wait_any(&sem, 1, 123, &idx), LX_ETIMEDOUT);
    LT_EQ(read2(sem, SEM_READ, &c, &m), 0);
    LT_EQ(c, 0);
    LT_EQ(op1(sem, SEM_RELEASE, 2, &prev), 0);
    LT_EQ(prev, 0);
    LT_ERRNO(op1(sem, SEM_RELEASE, 1, &prev), LX_EOVERFLOW);
    LT_ERRNO(op1(sem, EVENT_SET, 0, &prev), LX_EINVAL);   /* wrong object type */
    LT_ERRNO(io(T, DEV, SEM_READ, area(0)), LX_ENOTTY);   /* object ioctl on the device */
    LT_EQ(SC(T, close, (uint64_t)sem), 0);
}

LT_TEST(mutex_state)
{
    LT_ERRNO(create_mutex(123, 0), LX_EINVAL);
    LT_ERRNO(create_mutex(0, 1), LX_EINVAL);
    int mx = create_mutex(123, 2);
    LT_CHECK(mx >= 0);
    uint32_t o, c, idx, prev;
    LT_EQ(read2(mx, MUTEX_READ, &o, &c), 0);
    LT_EQ(o, 123); LT_EQ(c, 2);
    LT_EQ(wait_any(&mx, 1, 123, &idx), 0);             /* recursive acquisition by the owner */
    LT_ERRNO(wait_any(&mx, 1, 456, &idx), LX_ETIMEDOUT);
    LT_ERRNO(unlock_mutex(mx, 456, &prev), LX_EPERM);
    LT_ERRNO(unlock_mutex(mx, 0, &prev), LX_EINVAL);
    LT_EQ(unlock_mutex(mx, 123, &prev), 0);
    LT_EQ(prev, 3);
    LT_EQ(unlock_mutex(mx, 123, &prev), 0);
    LT_EQ(unlock_mutex(mx, 123, &prev), 0);
    LT_EQ(prev, 1);
    LT_ERRNO(unlock_mutex(mx, 123, &prev), LX_EPERM);  /* now unowned */
    LT_EQ(read2(mx, MUTEX_READ, &o, &c), 0);
    LT_EQ(o, 0); LT_EQ(c, 0);
    LT_EQ(wait_any(&mx, 1, 456, &idx), 0);
    LT_ERRNO(op1(mx, MUTEX_KILL, 123, NULL), LX_EPERM);
    LT_EQ(op1(mx, MUTEX_KILL, 456, NULL), 0);          /* abandoned */
    LT_ERRNO(read2(mx, MUTEX_READ, &o, &c), LX_EOWNERDEAD);
    LT_EQ(o, 0); LT_EQ(c, 0);
    LT_ERRNO(wait_any(&mx, 1, 789, &idx), LX_EOWNERDEAD);
    LT_EQ(idx, 0);
    LT_EQ(read2(mx, MUTEX_READ, &o, &c), 0);           /* the abandonment is reported once */
    LT_EQ(o, 789); LT_EQ(c, 1);
    LT_EQ(SC(T, close, (uint64_t)mx), 0);
}

LT_TEST(event_state)
{
    int man = create_event(1, 0), aut = create_event(0, 1);
    LT_CHECK(man >= 0 && aut >= 0);
    uint32_t m, s, idx, prev;
    LT_EQ(read2(man, EVENT_READ, &m, &s), 0);
    LT_EQ(m, 1); LT_EQ(s, 0);
    LT_EQ(op1(man, EVENT_SET, 0, &prev), 0);
    LT_EQ(prev, 0);
    LT_EQ(op1(man, EVENT_SET, 0, &prev), 0);
    LT_EQ(prev, 1);
    LT_EQ(wait_any(&man, 1, 1, &idx), 0);
    LT_EQ(wait_any(&man, 1, 1, &idx), 0);              /* a manual event stays signaled */
    LT_EQ(op1(man, EVENT_RESET, 0, &prev), 0);
    LT_EQ(prev, 1);
    LT_ERRNO(wait_any(&man, 1, 1, &idx), LX_ETIMEDOUT);
    LT_EQ(op1(man, EVENT_PULSE, 0, &prev), 0);        /* nobody waits: back to reset */
    LT_EQ(prev, 0);
    LT_EQ(read2(man, EVENT_READ, &m, &s), 0);
    LT_EQ(s, 0);
    LT_EQ(wait_any(&aut, 1, 1, &idx), 0);              /* an auto-reset event is consumed */
    LT_ERRNO(wait_any(&aut, 1, 1, &idx), LX_ETIMEDOUT);
    LT_EQ(read2(aut, EVENT_READ, &m, &s), 0);
    LT_EQ(m, 0); LT_EQ(s, 0);
    SC(T, close, (uint64_t)man);
    SC(T, close, (uint64_t)aut);
}

/* ---- waits -------------------------------------------------------------------------------------- */

LT_TEST(wait_any_and_alert)
{
    int sem = create_sem(0, 3), mx = create_mutex(0, 0), ev = create_event(1, 0), alert = create_event(0, 0);
    int objs[3] = { sem, mx, ev };
    uint32_t idx, prev;
    LT_EQ(wait_any(objs, 3, 9, &idx), 0);              /* the free mutex */
    LT_EQ(idx, 1);
    LT_EQ(op1(ev, EVENT_SET, 0, &prev), 0);
    LT_EQ(wait_any(objs, 3, 9, &idx), 0);              /* mutex owned by 9: still taken, first in order */
    LT_EQ(idx, 1);
    LT_EQ(wait_any(objs, 3, 10, &idx), 0);             /* owner 10: the event */
    LT_EQ(idx, 2);
    LT_EQ(op1(ev, EVENT_RESET, 0, &prev), 0);
    LT_ERRNO(wait_as(T, 0, 0, objs, 3, 0, 10, alert, &idx), LX_ETIMEDOUT);
    LT_EQ(op1(alert, EVENT_SET, 0, &prev), 0);
    LT_EQ(wait_as(T, 0, 0, objs, 3, 0, 10, alert, &idx), 0);
    LT_EQ(idx, 3);                                      /* index == count: the alert */
    LT_ERRNO(wait_as(T, 0, 0, objs, 3, 0, 10, alert, &idx), LX_ETIMEDOUT); /* consumed */
    LT_ERRNO(wait_as(T, 0, 0, objs, 3, 0, 10, sem, &idx), LX_EINVAL);       /* alert must be an event */
    int bad[1] = { DEV };
    LT_ERRNO(wait_any(bad, 1, 1, &idx), LX_EINVAL);    /* the device is not an object */
    struct wait_args *a = area(0);
    memset(a, 0, sizeof *a);
    a->count = 65;
    LT_ERRNO(io(T, DEV, NT_WAIT_ANY, a), LX_EINVAL);
    a->count = 0; a->pad = 1;
    LT_ERRNO(io(T, DEV, NT_WAIT_ANY, a), LX_EINVAL);
    a->pad = 0; a->flags = 2;
    LT_ERRNO(io(T, DEV, NT_WAIT_ANY, a), LX_EINVAL);
    SC(T, close, (uint64_t)sem); SC(T, close, (uint64_t)mx); SC(T, close, (uint64_t)ev); SC(T, close, (uint64_t)alert);
}

LT_TEST(wait_all_rules)
{
    int sem = create_sem(1, 3), mx = create_mutex(0, 0), ev = create_event(0, 0);
    int objs[3] = { sem, mx, ev };
    uint32_t idx, prev, c, m, o;
    LT_ERRNO(wait_all(objs, 3, 5, &idx), LX_ETIMEDOUT);   /* the event is not set: nothing acquired */
    LT_EQ(read2(sem, SEM_READ, &c, &m), 0);
    LT_EQ(c, 1);
    LT_EQ(op1(ev, EVENT_SET, 0, &prev), 0);
    LT_EQ(wait_all(objs, 3, 5, &idx), 0);
    LT_EQ(idx, 0);
    LT_EQ(read2(sem, SEM_READ, &c, &m), 0);
    LT_EQ(c, 0);
    LT_EQ(read2(mx, MUTEX_READ, &o, &c), 0);
    LT_EQ(o, 5); LT_EQ(c, 1);
    LT_EQ(read2(ev, EVENT_READ, &m, &c), 0);
    LT_EQ(c, 0);
    int dup2[2] = { sem, sem };
    LT_ERRNO(wait_all(dup2, 2, 5, &idx), LX_EINVAL);   /* NT_WAIT_ALL names an object once */
    LT_ERRNO(wait_any(dup2, 2, 5, &idx), LX_ETIMEDOUT); /* NT_WAIT_ANY may repeat it */
    SC(T, close, (uint64_t)sem); SC(T, close, (uint64_t)mx); SC(T, close, (uint64_t)ev);
}

/* ---- wakes across threads ----------------------------------------------------------------------- */

struct w {
    lkd_task *t;
    int k, all, alert;
    int fds[4];
    uint32_t n, owner, index;
    int64_t rel, ret;
    _Atomic int started, done;
    pthread_t th;
};
static void *w_main(void *p)
{
    struct w *w = p;
    lkd_set_current(w->t);
    atomic_store(&w->started, 1);
    w->ret = wait_as(w->t, w->k, w->all, w->fds, w->n, w->rel, w->owner, w->alert, &w->index);
    atomic_store(&w->done, 1);
    return NULL;
}
static void w_start(struct w *w, int k, int all, const int *fds, uint32_t n, uint32_t owner, int64_t rel)
{
    memset(w, 0, sizeof *w);
    w->t = fake_make_thread(T);
    w->k = k; w->all = all; w->n = n; w->owner = owner; w->rel = rel;
    memcpy(w->fds, fds, n * sizeof fds[0]);
    pthread_create(&w->th, NULL, w_main, w);
    while (!atomic_load(&w->started)) usleep(100);
    usleep(30000); /* reach the sleep */
}
static void w_join(struct w *w) { pthread_join(w->th, NULL); }

LT_TEST(wakes_across_threads)
{
    int ev = create_event(0, 0), sem = create_sem(0, 2), mx = create_mutex(7, 1);
    uint32_t prev;
    struct w a, b;
    /* an auto-reset event set once wakes one waiter, oldest first, and stays reset */
    w_start(&a, 1, 0, &ev, 1, 1, -1);
    w_start(&b, 2, 0, &ev, 1, 2, 2000000000ll);
    LT_EQ(op1(ev, EVENT_SET, 0, &prev), 0);
    usleep(30000);
    LT_EQ(atomic_load(&a.done), 1);
    LT_EQ(atomic_load(&b.done), 0);
    LT_EQ(op1(ev, EVENT_SET, 0, &prev), 0);
    w_join(&a); w_join(&b);
    LT_EQ(a.ret, 0); LT_EQ(b.ret, 0);
    uint32_t m, s;
    LT_EQ(read2(ev, EVENT_READ, &m, &s), 0);
    LT_EQ(s, 0);
    /* pulse wakes the current waiter and leaves the event reset */
    w_start(&a, 1, 0, &ev, 1, 1, -1);
    LT_EQ(op1(ev, EVENT_PULSE, 0, &prev), 0);
    w_join(&a);
    LT_EQ(a.ret, 0);
    LT_EQ(read2(ev, EVENT_READ, &m, &s), 0);
    LT_EQ(s, 0);
    /* a semaphore release of 2 wakes two waiters */
    w_start(&a, 1, 0, &sem, 1, 1, -1);
    w_start(&b, 2, 0, &sem, 1, 2, -1);
    LT_EQ(op1(sem, SEM_RELEASE, 2, &prev), 0);
    w_join(&a); w_join(&b);
    LT_EQ(a.ret, 0); LT_EQ(b.ret, 0);
    /* a mutex released by its owner is handed to the waiter */
    w_start(&a, 1, 0, &mx, 1, 8, -1);
    LT_EQ(unlock_mutex(mx, 7, &prev), 0);
    w_join(&a);
    LT_EQ(a.ret, 0);
    uint32_t o, c;
    LT_EQ(read2(mx, MUTEX_READ, &o, &c), 0);
    LT_EQ(o, 8); LT_EQ(c, 1);
    /* NT_WAIT_ALL wakes only when the last object is signaled, and takes them all */
    int both[2] = { ev, sem };
    w_start(&a, 1, 1, both, 2, 3, -1);
    LT_EQ(op1(sem, SEM_RELEASE, 1, &prev), 0);
    usleep(30000);
    LT_EQ(atomic_load(&a.done), 0);
    LT_EQ(op1(ev, EVENT_SET, 0, &prev), 0);
    w_join(&a);
    LT_EQ(a.ret, 0);
    LT_EQ(read2(sem, SEM_READ, &c, &m), 0);
    LT_EQ(c, 0);
    /* a killed mutex is handed over as abandoned */
    w_start(&a, 1, 0, &mx, 1, 9, -1);
    LT_EQ(op1(mx, MUTEX_KILL, 8, NULL), 0);
    w_join(&a);
    LT_ERRNO(a.ret, LX_EOWNERDEAD);
    LT_EQ(a.index, 0);
    /* a timeout ends a wait */
    int64_t t0 = lkd_host_mono_ns();
    w_start(&a, 1, 0, &ev, 1, 1, 100000000ll);
    w_join(&a);
    LT_ERRNO(a.ret, LX_ETIMEDOUT);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 100000000ll);
    SC(T, close, (uint64_t)ev); SC(T, close, (uint64_t)sem); SC(T, close, (uint64_t)mx);
}

struct intr { lkd_task *t; int delay_ms; };
static void *intr_main(void *p)
{
    struct intr *a = p;
    usleep((useconds_t)a->delay_ms * 1000);
    lkd_sig_send_tid(a->t, a->t->proc->pid, a->t->tid, LX_SIGUSR1, NULL);
    return NULL;
}

LT_TEST(signal_ends_wait)
{
    struct lx_sigaction h = { 0x6000, LX_SA_RESTORER, 0x2000, 0 };
    LT_EQ(SC(T, rt_sigaction, LX_SIGUSR1, P(&h), 0, 8), 0);
    int ev = create_event(0, 0);
    pthread_t th;
    struct intr a = { T, 20 };
    pthread_create(&th, NULL, intr_main, &a);
    int64_t t0 = lkd_host_mono_ns();
    uint32_t idx;
    /* the ioctl is not restarted for a handler without SA_RESTART: EINTR, on which Wine waits again */
    LT_ERRNO(wait_as(T, 0, 0, &ev, 1, -1, 1, 0, &idx), LX_EINTR);
    int64_t dt = lkd_host_mono_ns() - t0;
    pthread_join(th, NULL);
    LT_CHECK(dt < 45000000);
    printf("MEASURE ntsync wait interrupted after %.2f ms (signal sent at 20 ms)\n", dt / 1e6);
    LT_EQ(lkd_signal_next(T), 1);
    lkd_sig_set_mask(T, 0);
    /* the event set meanwhile was not consumed by the interrupted wait */
    uint32_t prev, m, s;
    LT_EQ(op1(ev, EVENT_SET, 0, &prev), 0);
    LT_EQ(read2(ev, EVENT_READ, &m, &s), 0);
    LT_EQ(s, 1);
    SC(T, close, (uint64_t)ev);
}

/* ---- sharing across processes -------------------------------------------------------------------- */

LT_TEST(shared_across_fork)
{
    int ev = create_event(0, 0), back = create_event(0, 0);
    fflush(NULL);
    pid_t c = lkd_fork_host(T, 0);
    if (c == 0) {
        alarm(20);
        uint32_t idx, prev;
        int64_t r = wait_as(T, 1, 0, &ev, 1, 5000000000ll, 1, 0, &idx); /* set by the parent */
        int64_t r2 = op1(back, EVENT_SET, 0, &prev);
        _exit(r == 0 && r2 == 0 ? 0 : 10);
    }
    LT_CHECK(c > 0);
    usleep(50000);
    uint32_t prev, idx;
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(op1(ev, EVENT_SET, 0, &prev), 0);
    LT_EQ(wait_as(T, 0, 0, &back, 1, 5000000000ll, 2, 0, &idx), 0);
    printf("MEASURE ntsync round trip parent -> child -> parent %.1f us\n", (lkd_host_mono_ns() - t0) / 1e3);
    int st = 0;
    LT_EQ(waitpid(c, &st, 0), c);
    LT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    /* the child's inherited descriptors did not release the objects */
    uint32_t m, s;
    LT_EQ(read2(ev, EVENT_READ, &m, &s), 0);
    SC(T, close, (uint64_t)ev); SC(T, close, (uint64_t)back);
}

LT_TEST(anchors_and_slot_reuse)
{
    int ev = create_event(1, 0);
    lkd_fdobj *o = lkd_fd_get(T, ev, NULL);
    LT_CHECK(o != NULL);
    int h[1];
    LT_EQ(lkd_ntsync_export(o, h, 1), 1);
    lkd_fdobj_put(o);
    int h2 = dup(h[0]);
    lkd_fdobj *got = NULL;
    LT_EQ(lkd_ntsync_import(h[0], &got), 1);           /* the same object, as a receiver gets it */
    int ev2 = lkd_fd_alloc(T, got, 0, 1);
    lkd_fdobj_put(got);
    LT_CHECK(ev2 >= 0);
    uint32_t prev, m, s;
    LT_EQ(op1(ev, EVENT_SET, 0, &prev), 0);
    LT_EQ(read2(ev2, EVENT_READ, &m, &s), 0);
    LT_EQ(s, 1);
    LT_EQ(SC(T, close, (uint64_t)ev), 0);               /* still alive through ev2 */
    LT_EQ(read2(ev2, EVENT_READ, &m, &s), 0);
    LT_EQ(SC(T, close, (uint64_t)ev2), 0);              /* gone: its anchor names nothing now */
    LT_EQ(lkd_ntsync_import(h2, &got), 0);
    close(h2);
    int p[2];
    LT_EQ(pipe(p), 0);
    LT_EQ(lkd_ntsync_import(p[0], &got), 0);            /* an ordinary pipe is not an object */
    close(p[0]); close(p[1]);
    /* a host process that exits releases the references it still holds (it does not close its fds) */
    int ev3 = create_event(1, 0);
    o = lkd_fd_get(T, ev3, NULL);
    LT_EQ(lkd_ntsync_export(o, h, 1), 1);
    lkd_fdobj_put(o);
    h2 = dup(h[0]);
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        lkd_fdobj *in = NULL;
        int ok = lkd_ntsync_import(h[0], &in) == 1;      /* the child holds a counted reference */
        sig_ntsync_host_exit();
        _exit(ok ? 0 : 1);
    }
    close(h[0]);
    int st = 0;
    LT_EQ(waitpid(c, &st, 0), c);
    LT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    LT_EQ(SC(T, close, (uint64_t)ev3), 0);
    LT_EQ(lkd_ntsync_import(h2, &got), 0);              /* freed: the child's reference went with it */
    close(h2);
    /* slots are reused: far more objects over time than the table holds at once */
    int64_t t0 = lkd_host_mono_ns();
    for (int i = 0; i < 200000; i++) {
        int e = create_event(0, 1);
        if (e < 0) { LT_CHECK(e >= 0); break; }
        SC(T, close, (uint64_t)e);
    }
    printf("MEASURE ntsync create+close %.2f us\n", (lkd_host_mono_ns() - t0) / 1e3 / 200000);
    int e = create_event(0, 0);
    t0 = lkd_host_mono_ns();
    for (int i = 0; i < 200000; i++) op1(e, EVENT_SET, 0, &prev);
    printf("MEASURE ntsync EVENT_SET %.3f us\n", (lkd_host_mono_ns() - t0) / 1e3 / 200000);
    SC(T, close, (uint64_t)e);
}

/* A process ends at exit_group while another of its threads is still there: its host process exits at once, and
 * what it held is released as after a last thread. */
LT_TEST(group_exit_releases)
{
    int ev = create_event(1, 0);
    lkd_fdobj *o = lkd_fd_get(T, ev, NULL);
    LT_CHECK(o != NULL);
    int h[1];
    LT_EQ(lkd_ntsync_export(o, h, 1), 1);
    lkd_fdobj_put(o);
    int h2 = dup(h[0]);
    fflush(NULL);
    int64_t pid = SC(T, clone, LX_SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        alarm(20);
        lkd_task *c = lkd_current();
        lkd_fdobj *in = NULL;
        int ok = lkd_ntsync_import(h[0], &in) == 1;      /* the child holds a counted reference */
        ok = ok && fake_make_thread(c) != NULL;
        SC(c, exit_group, (uint64_t)(ok ? 0 : 1));
        int st = 0;
        if (lkd_task_release(c, &st)) _exit(st);
        _exit(99);
    }
    LT_CHECK(pid > 0);
    close(h[0]);
    int32_t *status = area(1);
    *status = -1;
    LT_EQ(SC(T, wait4, (uint64_t)pid, P(status), 0, 0), pid);
    LT_EQ(*status, 0);
    LT_EQ(SC(T, close, (uint64_t)ev), 0);
    lkd_fdobj *got = NULL;
    LT_EQ(lkd_ntsync_import(h2, &got), 0);              /* freed: the child's reference went at its exit */
    close(h2);
}

int main(void)
{
    procfs_fixture();
    if (!lkd_ntsync_available()) { printf("SKIP no family directory\n"); return lt_finish(); }
    LT_RUN(device_node);
    LT_RUN(semaphore_state);
    LT_RUN(mutex_state);
    LT_RUN(event_state);
    LT_RUN(wait_any_and_alert);
    LT_RUN(wait_all_rules);
    LT_RUN(wakes_across_threads);
    LT_RUN(signal_ends_wait);
    LT_RUN(shared_across_fork);
    LT_RUN(anchors_and_slot_reuse);
    LT_RUN(group_exit_releases);
    return lt_finish();
}
