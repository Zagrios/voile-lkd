/* test_sig_sem.c: System V semaphores (sig module, src/sig/sem.c). Follows ipc/sem.c of Linux v6.12 and
 * semget(2), semop(2), semctl(2): creation and lookup, all-or-nothing operations and their limits, the
 * queue served oldest first, wait-for-zero, GETNCNT and GETZCNT, IPC_RMID ending waits, a signal and a
 * timeout ending a wait, SEM_UNDO applied at process exit and after a host process was killed outright, a
 * set shared across a host fork, a dead waiter dropped, and more waiters than 256. */
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>

#include "test_procfs_util.h"
#include "famsvc.h"
#include "sig.h"
#include "task.h"

#define K_PRIVATE 0
#define K_CREAT 01000
#define K_EXCL 02000
#define K_NOWAIT 04000
#define K_UNDO 0x1000
#define K_RMID 0
#define K_SET 1
#define K_STAT 2
#define K_INFO 3
#define K_GETPID 11
#define K_GETVAL 12
#define K_GETALL 13
#define K_GETNCNT 14
#define K_GETZCNT 15
#define K_SETVAL 16
#define K_SETALL 17
#define K_SEM_STAT 18
#define K_SEM_INFO 19
#define K_SEM_STAT_ANY 20

struct sb { uint16_t num; int16_t op, flg; };
struct perm { int32_t key; uint32_t uid, gid, cuid, cgid, mode; uint16_t seq, pad; uint64_t u1, u2; };
struct ds { struct perm perm; int64_t otime; uint64_t u1; int64_t ctime; uint64_t u2, nsems, u3, u4; };
struct info { int32_t semmap, semmni, semmns, semmnu, semmsl, semopm, semume, semusz, semvmx, semaem; };

/* Guest scratch for syscall arguments: one 8 KiB area per caller, at 64 KiB into the scratch. */
static void *area(int k) { return SB + 65536 + (size_t)k * 8192; }

static int64_t sget(int32_t key, int nsems, int flg)
{
    return SC(T, semget, (uint64_t)(uint32_t)key, (uint64_t)(uint32_t)nsems, (uint64_t)(uint32_t)flg);
}
static int64_t ctl(int id, int num, int cmd, uint64_t arg)
{
    return SC(T, semctl, (uint64_t)(uint32_t)id, (uint64_t)(uint32_t)num, (uint64_t)(uint32_t)cmd, arg);
}
static int64_t val(int id, int num) { return ctl(id, num, K_GETVAL, 0); }

/* n operations (num, op, flg triples) by task t with scratch k; rel_ns < 0: semop, else semtimedop */
static int64_t ops_as(lkd_task *t, int k, int id, int64_t rel_ns, int n, const int16_t *o)
{
    struct sb *s = area(k);
    for (int i = 0; i < n; i++) s[i] = (struct sb){ (uint16_t)o[3 * i], o[3 * i + 1], o[3 * i + 2] };
    if (rel_ns < 0) return SC(t, semop, (uint64_t)(uint32_t)id, P(s), (uint64_t)n);
    struct lx_timespec *ts = (void *)((char *)area(k) + 4096);
    ts->tv_sec = rel_ns / 1000000000ll;
    ts->tv_nsec = rel_ns % 1000000000ll;
    return SC(t, semtimedop, (uint64_t)(uint32_t)id, P(s), (uint64_t)n, P(ts));
}
static int64_t op1(int id, int num, int op, int flg)
{
    const int16_t o[3] = { (int16_t)num, (int16_t)op, (int16_t)flg };
    return ops_as(T, 0, id, -1, 1, o);
}

/* ---- a waiting thread ------------------------------------------------------------------------- */

struct w {
    pthread_t th;
    lkd_task *t;
    int k, id, n;
    int64_t rel;
    const int16_t *o; /* the caller's array, which outlives w_join */
    _Atomic int started, done;
    int64_t ret;
};
static void *w_main(void *p)
{
    struct w *w = p;
    lkd_set_current(w->t);
    atomic_store(&w->started, 1);
    w->ret = ops_as(w->t, w->k, w->id, w->rel, w->n, w->o);
    atomic_store(&w->done, 1);
    return NULL;
}
/* rel_ns as in ops_as */
static void w_launch(struct w *w, int k, int id, int n, const int16_t *o, int64_t rel_ns)
{
    memset(w, 0, sizeof *w);
    w->t = fake_make_thread(T);
    w->k = k; w->id = id; w->n = n; w->o = o; w->rel = rel_ns;
    pthread_create(&w->th, NULL, w_main, w);
    while (!atomic_load(&w->started)) usleep(100);
}
static void w_start(struct w *w, int k, int id, int n, const int16_t *o, int64_t rel_ns)
{
    w_launch(w, k, id, n, o, rel_ns);
    usleep(30000); /* reach the sleep */
}
static void w_join(struct w *w) { pthread_join(w->th, NULL); }

/* ---- creation and lookup ------------------------------------------------------------------------ */

LT_TEST(get_rules)
{
    /* ksys_semget: nsems in [0, SEMMSL]; newary: a new set needs nsems > 0 */
    LT_ERRNO(sget(K_PRIVATE, -1, 0600), LX_EINVAL);
    LT_ERRNO(sget(K_PRIVATE, 32001, 0600), LX_EINVAL);
    LT_ERRNO(sget(K_PRIVATE, 0, 0600), LX_EINVAL);
    LT_ERRNO(sget(0x1250, 3, 0600), LX_ENOENT);
    int id = (int)sget(0x1250, 3, K_CREAT | 0600);
    LT_CHECK(id >= 0);
    LT_ERRNO(sget(0x1250, 3, K_CREAT | K_EXCL | 0600), LX_EEXIST);
    LT_ERRNO(sget(0x1250, 4, 0), LX_EINVAL); /* sem_more_checks */
    LT_EQ(sget(0x1250, 0, 0), id);
    LT_EQ(sget(0x1250, 2, K_CREAT | 0600), id);
    int64_t p1 = sget(K_PRIVATE, 1, 0600), p2 = sget(K_PRIVATE, 1, 0600);
    LT_CHECK(p1 >= 0 && p2 >= 0 && p1 != p2 && p1 != id);

    struct ds *d = area(1);
    memset(d, 0xab, sizeof *d);
    LT_EQ(ctl(id, 0, K_STAT, P(d)), 0);
    LT_EQ(d->perm.key, 0x1250);
    LT_EQ(d->perm.mode, 0600);
    LT_EQ(d->perm.uid, T->proc->cred.euid);
    LT_EQ(d->perm.cuid, T->proc->cred.euid);
    LT_EQ(d->nsems, 3);
    LT_EQ(d->otime, 0);
    LT_CHECK(d->ctime > 1700000000);
    for (int i = 0; i < 3; i++) LT_EQ(val(id, i), 0);
    LT_ERRNO(val(id, 3), LX_EINVAL);
    LT_ERRNO(val(id, -1), LX_EINVAL);
    LT_ERRNO(ctl(id, 0, 0x100 | K_STAT, P(d)), LX_EINVAL); /* x86-64 takes no IPC_64 flag */

    /* IPC_SET keeps the other mode bits; another user is held to the mode, and only the owner removes */
    d->perm.mode = 0640;
    LT_EQ(ctl(id, 0, K_SET, P(d)), 0);
    LT_EQ(ctl(id, 0, K_STAT, P(d)), 0);
    LT_EQ(d->perm.mode, 0640);
    d->perm.uid = (uint32_t)-1; /* ipc_update_perm: no valid uid, EINVAL */
    LT_ERRNO(ctl(id, 0, K_SET, P(d)), LX_EINVAL);
    LT_EQ(ctl(id, 0, K_STAT, P(d)), 0);
    LT_EQ(d->perm.uid, T->proc->cred.euid);
    d->perm.gid = (uint32_t)-1;
    LT_ERRNO(ctl(id, 0, K_SET, P(d)), LX_EINVAL);
    struct lkd_creds saved = T->proc->cred;
    T->proc->cred.euid = 4242;
    T->proc->cred.egid = 4242;
    T->proc->cred.ngroups = 0;
    T->proc->cred.cap_eff = 0;
    LT_ERRNO(val(id, 0), LX_EACCES);
    LT_ERRNO(op1(id, 0, 1, 0), LX_EACCES);
    LT_ERRNO(sget(0x1250, 3, 0600), LX_EACCES);
    LT_ERRNO(ctl(id, 0, K_RMID, 0), LX_EPERM);
    T->proc->cred = saved;

    LT_EQ(ctl(id, 0, K_RMID, 0), 0);
    LT_ERRNO(val(id, 0), LX_EINVAL);
    LT_ERRNO(op1(id, 0, 1, 0), LX_EINVAL);
    LT_ERRNO(ctl(id, 0, K_RMID, 0), LX_EINVAL);
    LT_ERRNO(sget(0x1250, 3, 0600), LX_ENOENT);
    int again = (int)sget(0x1250, 3, K_CREAT | 0600);
    LT_CHECK(again >= 0 && again != id); /* a new sequence number: the old id stays invalid */
    LT_EQ(ctl(again, 0, K_RMID, 0), 0);
    LT_EQ(ctl((int)p1, 0, K_RMID, 0), 0);
    LT_EQ(ctl((int)p2, 0, K_RMID, 0), 0);
}

/* ---- operations --------------------------------------------------------------------------------- */

LT_TEST(op_rules)
{
    int id = (int)sget(K_PRIVATE, 2, 0600);
    LT_CHECK(id >= 0);
    const int16_t big[3] = { 0, 1, 0 };
    LT_ERRNO(ops_as(T, 0, id, -1, 0, big), LX_EINVAL);
    LT_ERRNO(SC(T, semop, (uint64_t)id, P(area(0)), 501), LX_E2BIG); /* SEMOPM */
    LT_ERRNO(op1(id, 2, 1, 0), LX_EFBIG);
    LT_ERRNO(op1(-1, 0, 1, 0), LX_EINVAL);

    LT_ERRNO(ctl(id, 0, K_SETVAL, 32768), LX_ERANGE);
    LT_ERRNO(ctl(id, 0, K_SETVAL, (uint64_t)(uint32_t)-1), LX_ERANGE);
    LT_EQ(ctl(id, 0, K_SETVAL, 2), 0);
    LT_EQ(val(id, 0), 2);
    LT_EQ(op1(id, 0, -1, 0), 0);
    LT_EQ(val(id, 0), 1);
    LT_EQ(ctl(id, 0, K_GETPID, 0), T->proc->pid);
    LT_ERRNO(op1(id, 0, -2, K_NOWAIT), LX_EAGAIN);
    LT_ERRNO(op1(id, 0, 0, K_NOWAIT), LX_EAGAIN); /* wait for zero on 1 */
    LT_EQ(val(id, 0), 1);

    /* all or nothing: the increment of semaphore 0 is undone when semaphore 1 would block */
    const int16_t both[6] = { 0, 1, 0, 1, -1, K_NOWAIT };
    LT_ERRNO(ops_as(T, 0, id, -1, 2, both), LX_EAGAIN);
    LT_EQ(val(id, 0), 1);
    /* operations apply in order: 1 + 5 - 6 = 0 succeeds */
    const int16_t seq[6] = { 0, 5, 0, 0, -6, 0 };
    LT_EQ(ops_as(T, 0, id, -1, 2, seq), 0);
    LT_EQ(val(id, 0), 0);
    /* SEMVMX */
    LT_EQ(ctl(id, 0, K_SETVAL, 32767), 0);
    LT_ERRNO(op1(id, 0, 1, 0), LX_ERANGE);
    LT_EQ(val(id, 0), 32767);
    /* the adjustment of SEM_UNDO stays within [-SEMAEM-1, SEMAEM] */
    LT_EQ(op1(id, 0, -32767, K_UNDO), 0);
    LT_EQ(op1(id, 0, 1, 0), 0);
    LT_ERRNO(op1(id, 0, -1, K_UNDO), LX_ERANGE);
    LT_EQ(val(id, 0), 1);

    /* SETALL checks every value first; GETALL */
    uint16_t *a = area(1);
    a[0] = 7; a[1] = 32768;
    LT_ERRNO(ctl(id, 0, K_SETALL, P(a)), LX_ERANGE);
    LT_EQ(val(id, 0), 1);
    a[0] = 7; a[1] = 9;
    LT_EQ(ctl(id, 0, K_SETALL, P(a)), 0);
    a[0] = a[1] = 0;
    LT_EQ(ctl(id, 0, K_GETALL, P(a)), 0);
    LT_EQ(a[0], 7);
    LT_EQ(a[1], 9);
    LT_ERRNO(ctl(id, 0, K_GETALL, 8), LX_EFAULT);

    /* semtimedop: a valid relative timeout, then EAGAIN when it passes */
    struct sb *s = area(2);
    struct lx_timespec *ts = (void *)((char *)area(2) + 4096);
    s[0] = (struct sb){ 1, -10, 0 };
    ts->tv_sec = 0; ts->tv_nsec = 1000000000;
    LT_ERRNO(SC(T, semtimedop, (uint64_t)id, P(s), 1, P(ts)), LX_EINVAL);
    ts->tv_sec = -1; ts->tv_nsec = 0;
    LT_ERRNO(SC(T, semtimedop, (uint64_t)id, P(s), 1, P(ts)), LX_EINVAL);
    const int16_t ten[3] = { 1, -10, 0 };
    LT_ERRNO(ops_as(T, 0, id, 0, 1, ten), LX_EAGAIN);
    int64_t t0 = lkd_host_mono_ns();
    LT_ERRNO(ops_as(T, 0, id, 50000000ll, 1, ten), LX_EAGAIN);
    int64_t dt = lkd_host_mono_ns() - t0;
    LT_CHECK(dt >= 50000000ll && dt < 1000000000ll);
    LT_EQ(val(id, 1), 9);
    LT_EQ(ctl(id, 1, K_GETNCNT, 0), 0);

    struct ds *d = area(3);
    LT_EQ(ctl(id, 0, K_STAT, P(d)), 0);
    LT_CHECK(d->otime > 1700000000);
    LT_EQ(ctl(id, 0, K_RMID, 0), 0);
}

/* ---- the queue ---------------------------------------------------------------------------------- */

LT_TEST(queue_order)
{
    int id = (int)sget(K_PRIVATE, 2, 0600);
    LT_CHECK(id >= 0);
    struct w a, b, z;
    /* waiters are served oldest first, and a newcomer does not take a value meant for a waiter */
    const int16_t dec[3] = { 0, -1, 0 };
    w_start(&a, 1, id, 1, dec, -1);
    w_start(&b, 2, id, 1, dec, -1);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), 2);
    LT_EQ(ctl(id, 0, K_GETZCNT, 0), 0);
    LT_EQ(op1(id, 0, 1, 0), 0);
    usleep(30000);
    LT_EQ(atomic_load(&a.done), 1);
    LT_EQ(atomic_load(&b.done), 0);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), 1);
    LT_EQ(op1(id, 0, 1, 0), 0);
    LT_ERRNO(op1(id, 0, -1, K_NOWAIT), LX_EAGAIN);
    w_join(&a); w_join(&b);
    LT_EQ(a.ret, 0); LT_EQ(b.ret, 0);
    LT_EQ(val(id, 0), 0);
    LT_EQ(ctl(id, 0, K_GETPID, 0), b.t->proc->pid);

    /* wait-for-zero */
    LT_EQ(ctl(id, 0, K_SETVAL, 2), 0);
    const int16_t zero[3] = { 0, 0, 0 };
    w_start(&z, 3, id, 1, zero, -1);
    LT_EQ(ctl(id, 0, K_GETZCNT, 0), 1);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), 0);
    LT_EQ(op1(id, 0, -2, 0), 0);
    w_join(&z);
    LT_EQ(z.ret, 0);
    LT_EQ(ctl(id, 0, K_GETZCNT, 0), 0);

    /* check_restart: completing a newer waiter unblocks an older one, both complete */
    const int16_t take1[3] = { 1, -1, 0 };
    const int16_t pass[6] = { 0, -1, 0, 1, 1, 0 };
    w_start(&a, 1, id, 1, take1, -1);
    w_start(&b, 2, id, 2, pass, -1);
    LT_EQ(ctl(id, 1, K_GETNCNT, 0), 1);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), 1);
    LT_EQ(op1(id, 0, 1, 0), 0);
    w_join(&a); w_join(&b);
    LT_EQ(a.ret, 0); LT_EQ(b.ret, 0);
    LT_EQ(val(id, 0), 0);
    LT_EQ(val(id, 1), 0);

    /* SETVAL serves the queue too */
    w_start(&a, 1, id, 1, dec, -1);
    LT_EQ(ctl(id, 0, K_SETVAL, 1), 0);
    w_join(&a);
    LT_EQ(a.ret, 0);
    LT_EQ(val(id, 0), 0);

    /* IPC_RMID ends the waits with EIDRM */
    w_start(&a, 1, id, 1, dec, -1);
    LT_EQ(ctl(id, 0, K_RMID, 0), 0);
    w_join(&a);
    LT_ERRNO(a.ret, LX_EIDRM);
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
    int id = (int)sget(K_PRIVATE, 1, 0600);
    struct lx_sigaction h = { 0x6000, LX_SA_RESTORER | LX_SA_RESTART, 0x2000, 0 };
    LT_EQ(SC(T, rt_sigaction, LX_SIGUSR1, P(&h), 0, 8), 0);
    pthread_t th;
    struct intr a = { T, 20 };
    pthread_create(&th, NULL, intr_main, &a);
    int64_t t0 = lkd_host_mono_ns();
    /* semop is never restarted after a handler, even with SA_RESTART (signal(7)) */
    LT_ERRNO(op1(id, 0, -1, 0), LX_EINTR);
    int64_t dt = lkd_host_mono_ns() - t0;
    pthread_join(th, NULL);
    LT_CHECK(dt < 45000000);
    printf("MEASURE semop interrupted after %.2f ms (signal sent at 20 ms)\n", dt / 1e6);
    LT_EQ(lkd_signal_next(T), 1);
    lkd_sig_set_mask(T, 0);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), 0);
    LT_EQ(op1(id, 0, 1, 0), 0);
    LT_EQ(val(id, 0), 1); /* nobody took it */
    LT_EQ(ctl(id, 0, K_RMID, 0), 0);
}

/* ---- processes ---------------------------------------------------------------------------------- */

static void put64(int fd, int64_t v) { (void)!write(fd, &v, sizeof v); }
static int64_t get64(int fd)
{
    int64_t v = -12345;
    if (read(fd, &v, sizeof v) != (ssize_t)sizeof v) return -12345;
    return v;
}

LT_TEST(undo_at_exit)
{
    int id = (int)sget(K_PRIVATE, 1, 0600);
    LT_EQ(ctl(id, 0, K_SETVAL, 5), 0);
    int pfd[2];
    LT_EQ(pipe(pfd), 0);
    fflush(NULL);
    int64_t pid = SC(T, clone, LX_SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        alarm(20);
        lkd_task *c = lkd_current();
        const int16_t take[3] = { 0, -2, K_UNDO }, give[3] = { 0, 1, 0 }, twice[3] = { 0, -1, K_UNDO };
        put64(pfd[1], ops_as(c, 1, id, -1, 1, take));
        put64(pfd[1], ops_as(c, 1, id, -1, 1, give));
        put64(pfd[1], ops_as(c, 1, id, -1, 1, twice));
        SC(c, exit_group, 0);
        int st = 0;
        if (lkd_task_release(c, &st)) _exit(st);
        _exit(99);
    }
    LT_CHECK(pid > 0);
    LT_EQ(get64(pfd[0]), 0);
    LT_EQ(get64(pfd[0]), 0);
    LT_EQ(get64(pfd[0]), 0);
    int32_t *status = area(1);
    *status = -1;
    LT_EQ(SC(T, wait4, (uint64_t)pid, P(status), 0, 0), pid);
    LT_EQ(*status, 0);
    /* 5 - 2 + 1 - 1 while it ran; its exit gives back the 3 taken with SEM_UNDO, not the 1 it gave */
    LT_EQ(val(id, 0), 6);
    LT_EQ(ctl(id, 0, K_GETPID, 0), pid);
    close(pfd[0]); close(pfd[1]);
    LT_EQ(ctl(id, 0, K_RMID, 0), 0);
}

LT_TEST(shared_across_fork)
{
    int id = (int)sget(K_PRIVATE, 2, 0600);
    fflush(NULL);
    pid_t c = lkd_fork_host(T, 0);
    if (c == 0) {
        alarm(20);
        const int16_t wait0[3] = { 0, -1, 0 }, post1[3] = { 1, 1, 0 };
        int64_t r = ops_as(T, 1, id, 5000000000ll, 1, wait0); /* posted by the parent */
        int64_t r2 = ops_as(T, 1, id, -1, 1, post1);
        _exit(r == 0 && r2 == 0 ? 0 : 10);
    }
    LT_CHECK(c > 0);
    usleep(50000);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), 1);
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(op1(id, 0, 1, 0), 0);
    const int16_t wait1[3] = { 1, -1, 0 };
    LT_EQ(ops_as(T, 0, id, 5000000000ll, 1, wait1), 0);
    printf("MEASURE semop round trip parent -> child -> parent %.1f us\n", (lkd_host_mono_ns() - t0) / 1e3);
    int st = 0;
    LT_EQ(waitpid(c, &st, 0), c);
    LT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* a waiter whose host process died is dropped, not served */
    fflush(NULL);
    c = lkd_fork_host(T, 0);
    if (c == 0) {
        alarm(20);
        const int16_t wait0[3] = { 0, -1, 0 };
        ops_as(T, 1, id, -1, 1, wait0);
        _exit(10);
    }
    LT_CHECK(c > 0);
    usleep(50000);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), 1);
    kill(c, SIGKILL);
    LT_EQ(waitpid(c, &st, 0), c);
    LT_EQ(op1(id, 0, 1, 0), 0);
    LT_EQ(val(id, 0), 1);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), 0);
    LT_EQ(ctl(id, 0, K_RMID, 0), 0);
}

/* A host process of the family that takes lockop on set id, writes the result to fd, then waits to be
 * killed. */
static pid_t spawn_holder(int id, const int16_t *lockop, int fd)
{
    fflush(NULL);
    pid_t c = lkd_fork_host(T, 0);
    if (c == 0) {
        alarm(20);
        put64(fd, ops_as(T, 1, id, -1, 1, lockop));
        pause();
        _exit(10);
    }
    return c;
}

/* exit_sem runs in do_exit (kernel/exit.c), before the process is a zombie: a SEM_UNDO lock comes back
 * however its holder dies. Here the holder's host process is killed outright, so no exit hook runs. */
LT_TEST(undo_after_host_kill)
{
    int id = (int)sget(K_PRIVATE, 1, 0600);
    LT_EQ(ctl(id, 0, K_SETVAL, 1), 0);
    const int16_t lockop[3] = { 0, -1, K_UNDO };
    int pfd[2];
    LT_EQ(pipe(pfd), 0);

    /* a waiter already sleeps when the holder dies; nobody waits for the holder (it stays a zombie) */
    pid_t c = spawn_holder(id, lockop, pfd[1]);
    LT_CHECK(c > 0);
    LT_EQ(get64(pfd[0]), 0);
    LT_EQ(val(id, 0), 0);
    struct w a;
    w_start(&a, 2, id, 1, lockop, 5000000000ll);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), 1);
    int64_t t0 = lkd_host_mono_ns();
    kill(c, SIGKILL);
    w_join(&a);
    int64_t dt = lkd_host_mono_ns() - t0;
    LT_EQ(a.ret, 0);
    LT_CHECK(dt < 1000000000ll);
    printf("MEASURE waiter served %.1f ms after the holder's host was killed\n", dt / 1e6);
    LT_EQ(val(id, 0), 0);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), 0);
    int st = 0;
    LT_EQ(waitpid(c, &st, 0), c);
    LT_CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    LT_EQ(op1(id, 0, 1, K_UNDO), 0); /* the waiter's process gives the lock back */
    LT_EQ(val(id, 0), 1);

    /* nobody waits when the holder dies: the next operation that would block applies its record first,
     * an IPC_NOWAIT one too */
    c = spawn_holder(id, lockop, pfd[1]);
    LT_CHECK(c > 0);
    LT_EQ(get64(pfd[0]), 0);
    LT_EQ(val(id, 0), 0);
    const int16_t trylock[3] = { 0, -1, K_UNDO | K_NOWAIT };
    LT_ERRNO(ops_as(T, 0, id, -1, 1, trylock), LX_EAGAIN);
    kill(c, SIGKILL);
    siginfo_t si;
    LT_EQ(waitid(P_PID, (id_t)c, &si, WEXITED | WNOWAIT), 0); /* dead, not reaped */
    LT_EQ(ops_as(T, 0, id, -1, 1, trylock), 0);
    LT_EQ(val(id, 0), 0);
    LT_EQ(waitpid(c, &st, 0), c);
    LT_EQ(op1(id, 0, 1, K_UNDO), 0);
    LT_EQ(val(id, 0), 1);
    close(pfd[0]); close(pfd[1]);
    LT_EQ(ctl(id, 0, K_RMID, 0), 0);
}

/* Linux queues one blocked operation per task, with up to SEMOPM operations each (do_semtimedop) */
LT_TEST(many_waiters)
{
    enum { N = 288 }; /* more than 256, under the 300 threads of a test process */
    static struct w ws[N];
    int id = (int)sget(K_PRIVATE, 2, 0600);
    LT_CHECK(id >= 0);
    const int16_t dec[3] = { 0, -1, 0 };
    for (int i = 0; i < N; i++) w_launch(&ws[i], 8 + i, id, 1, dec, 20000000000ll);
    for (int k = 0; k < 500 && ctl(id, 0, K_GETNCNT, 0) < N; k++) usleep(10000);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), N);
    int done = 0;
    for (int i = 0; i < N; i++) done += atomic_load(&ws[i].done);
    LT_EQ(done, 0);
    LT_EQ(ctl(id, 0, K_SETVAL, N), 0);
    int ok = 0;
    for (int i = 0; i < N; i++) {
        w_join(&ws[i]);
        ok += ws[i].ret == 0;
    }
    LT_EQ(ok, N);
    LT_EQ(val(id, 0), 0);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), 0);

    /* SEMOPM operations queued at once: wait on semaphore 1, then add 1 to semaphore 0 499 times */
    static int16_t big[3 * 500];
    big[0] = 1; big[1] = -1; big[2] = 0;
    for (int i = 1; i < 500; i++) { big[3 * i] = 0; big[3 * i + 1] = 1; big[3 * i + 2] = 0; }
    struct w b;
    w_start(&b, 8, id, 500, big, 20000000000ll);
    LT_EQ(ctl(id, 1, K_GETNCNT, 0), 1);
    LT_EQ(ctl(id, 0, K_GETNCNT, 0), 0);
    LT_EQ(val(id, 0), 0);
    LT_EQ(op1(id, 1, 1, 0), 0);
    w_join(&b);
    LT_EQ(b.ret, 0);
    LT_EQ(val(id, 0), 499);
    LT_EQ(val(id, 1), 0);
    LT_EQ(ctl(id, 0, K_RMID, 0), 0);
}

/* ---- information -------------------------------------------------------------------------------- */

LT_TEST(info_and_stat)
{
    int id = (int)sget(0x2310, 4, K_CREAT | 0600);
    LT_CHECK(id >= 0);
    struct info *in = area(1);
    memset(in, 0, sizeof *in);
    int64_t maxidx = ctl(0, 0, K_INFO, P(in));
    LT_CHECK(maxidx >= id % 32768);
    LT_EQ(in->semmsl, 32000);
    LT_EQ(in->semopm, 500);
    LT_EQ(in->semvmx, 32767);
    LT_EQ(in->semaem, 32767);
    LT_EQ(in->semusz, 20);
    LT_EQ(in->semmni, 1024);
    LT_EQ(in->semmns, 32768);
    memset(in, 0, sizeof *in);
    LT_EQ(ctl(0, 0, K_SEM_INFO, P(in)), maxidx);
    LT_EQ(in->semusz, 1);  /* sets in use */
    LT_EQ(in->semaem, 4);  /* semaphores in use */
    struct ds *d = area(2);
    LT_EQ(ctl(id % 32768, 0, K_SEM_STAT, P(d)), id);
    LT_EQ(d->nsems, 4);
    LT_EQ(ctl(id % 32768, 0, K_SEM_STAT_ANY, P(d)), id);
    LT_EQ(ctl(id % 32768 + 7 * 32768, 0, K_SEM_STAT, P(d)), id); /* ipcid_to_idx keeps the low 15 bits */
    LT_ERRNO(ctl(1023, 0, K_SEM_STAT, P(d)), LX_EINVAL);
    LT_ERRNO(ctl(id, 0, K_STAT, 8), LX_EFAULT);
    LT_ERRNO(ctl(id, 0, 99, 0), LX_EINVAL);
    LT_EQ(ctl(id, 0, K_RMID, 0), 0);
}

int main(void)
{
    procfs_fixture();
    if (!lkd_famsvc_dir()) { printf("SKIP no family directory\n"); return lt_finish(); }
    LT_RUN(get_rules);
    LT_RUN(op_rules);
    LT_RUN(queue_order);
    LT_RUN(signal_ends_wait);
    LT_RUN(undo_at_exit);
    LT_RUN(shared_across_fork);
    LT_RUN(undo_after_host_kill);
    LT_RUN(many_waiters);
    LT_RUN(info_and_stat);
    return lt_finish();
}
