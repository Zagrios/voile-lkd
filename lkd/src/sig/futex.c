/* futex.c: futex, futex_waitv, futex_wake, futex_wait, futex_requeue, robust lists. Owner: sig.
 * Models kernel/futex/{syscalls,waitwake,requeue,pi,core}.c v6.12.
 *
 * Two tables of waiters with the same layout (buckets of index-linked entries):
 *   - the private table, in process memory, for keys of kind LKD_FK_PRIVATE (os_unfair_lock buckets);
 *   - the shared table, a MAP_SHARED file "futex.shm" in the family directory, for LKD_FK_SHARED keys
 *     (backing object and offset, equal in every process of the family). Its bucket locks are spin
 *     words holding the holder's host pid; a waiter that finds the holder dead takes the lock over.
 *     Zero-filled memory is an empty table, so the file needs no initialisation.
 * A waiting thread owns one sleeper. Private-only waits sleep on the task's wake pipe (so
 * lkd_task_wake, signals and kicks end them like every other LKD wait). A wait that involves a shared
 * key sleeps with os_sync_wait_on_address(SHARED) on a 64-bit word of a sleeper slot in the shared
 * file: wakers of any process set the low half, signal generation bumps the high half (sig_ring_futex),
 * and the sleep is cut in 50 ms slices so that an interruption nobody rang (another module's
 * lkd_task_wake) is still seen. */
#include <fcntl.h>
#include <os/lock.h>
#include <os/os_sync_wait_on_address.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sig_priv.h"

#define FX_NB 256
#define FX_PRIV_ENTRIES 4096
#define FX_SHM_ENTRIES 8192
#define FX_SHM_SLEEPERS 2048
#define FX_SLICE_NS 50000000ll
#define FX_WAITV_MAX 128
#define FX_ROBUST_LIMIT 2048

enum { FXS_QUEUED = 0, FXS_WOKEN = 1, FXS_OWNER = 2 };

struct fx_sleeper {
    _Atomic uint64_t word;   /* low 32: 0 waiting, else vec+1 of the waking entry; high 32: ring count */
    int32_t host_pid, tid;
    _Atomic uint32_t used;
    uint32_t shared;         /* lives in the shared file: wake with os_sync SHARED */
    uint64_t task;           /* lkd_task * of the waiter, meaningful in host_pid only */
};

struct fx_entry {
    uint32_t next, prev;     /* 1-based indices; 0 = none */
    uint32_t bucket;
    _Atomic uint32_t used;
    uint8_t key_kind, pi, state, _p;
    uint32_t bitset;
    uint32_t vec;
    uint32_t _pad;
    uint64_t key_a, key_b;
    uint64_t uaddr;
    int32_t tid, host_pid;
    uint64_t sleeper;        /* private table: host pointer; shared table: sleeper index + 1 */
};

struct fx_bucket { _Atomic uint32_t lock; uint32_t head, tail, count; };

struct fx_shm {
    struct fx_bucket b[FX_NB];
    _Atomic uint32_t alloc_lock;
    uint32_t ehint, shint, _pad;
    struct fx_entry e[FX_SHM_ENTRIES];
    struct fx_sleeper s[FX_SHM_SLEEPERS];
};

struct fx_priv {
    os_unfair_lock locks[FX_NB];
    struct fx_bucket b[FX_NB];
    os_unfair_lock alloc_lock;
    uint32_t ehint;
    struct fx_entry e[FX_PRIV_ENTRIES];
};

struct fx_tab {
    int shared;
    struct fx_bucket *b;
    struct fx_entry *e;
    uint32_t ne;
};

static struct fx_priv g_priv;
static struct fx_tab g_ptab = { 0, g_priv.b, g_priv.e, FX_PRIV_ENTRIES };
static struct fx_shm *g_shm;
static struct fx_tab g_stab;
static pthread_mutex_t g_shm_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_mypid;

static int (*g_key_fn)(lkd_task *, uint64_t, int, struct lkd_futex_key *);

void lkd_futex_set_key_fn(int (*fn)(lkd_task *t, uint64_t gva, int private_hint, struct lkd_futex_key *key))
{
    g_key_fn = fn;
}

static int fx_key(lkd_task *t, uint64_t gva, int private_hint, struct lkd_futex_key *k)
{
    if (gva & 3) return -LX_EINVAL;
    memset(k, 0, sizeof *k);
    return g_key_fn ? g_key_fn(t, gva, private_hint, k) : lkd_mm_futex_key(t, gva, private_hint, k);
}

static uint32_t mypid(void)
{
    if (!g_mypid) g_mypid = (uint32_t)getpid();
    return g_mypid;
}

/* ---- shared table ----------------------------------------------------------------------------- */

static struct fx_tab *shm_tab(void)
{
    if (__atomic_load_n(&g_shm, __ATOMIC_ACQUIRE)) return &g_stab;
    pthread_mutex_lock(&g_shm_lock);
    if (!g_shm) {
        const char *d = lkd_famsvc_dir();
        char path[1024];
        if (d && snprintf(path, sizeof path, "%s/futex.shm", d) < (int)sizeof path) {
            int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0 && (st.st_size >= (off_t)sizeof(struct fx_shm) ||
                                            ftruncate(fd, (off_t)sizeof(struct fx_shm)) == 0)) {
                    void *p = mmap(NULL, sizeof(struct fx_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    if (p != MAP_FAILED) {
                        g_stab.shared = 1;
                        g_stab.b = ((struct fx_shm *)p)->b;
                        g_stab.e = ((struct fx_shm *)p)->e;
                        g_stab.ne = FX_SHM_ENTRIES;
                        __atomic_store_n(&g_shm, (struct fx_shm *)p, __ATOMIC_RELEASE);
                    }
                }
                close(fd);
            }
        }
    }
    pthread_mutex_unlock(&g_shm_lock);
    return g_shm ? &g_stab : NULL;
}

static void spin_lock(_Atomic uint32_t *l)
{
    uint32_t me = mypid();
    for (unsigned n = 0;; n++) {
        uint32_t z = 0;
        if (atomic_compare_exchange_weak(l, &z, me)) return;
        if (n < 128) continue;
        if ((n & 1023) == 1023 && z && z != me && kill((pid_t)z, 0) < 0 && errno == ESRCH) {
            /* the holder died inside a critical section: take the lock over */
            if (atomic_compare_exchange_strong(l, &z, me)) return;
        }
        if (n < 4096) sched_yield();
        else usleep(50);
    }
}

static void spin_unlock(_Atomic uint32_t *l) { atomic_store_explicit(l, 0, memory_order_release); }

static void bucket_lock(struct fx_tab *T, uint32_t i)
{
    if (T->shared) spin_lock(&T->b[i].lock);
    else os_unfair_lock_lock(&g_priv.locks[i]);
}

static void bucket_unlock(struct fx_tab *T, uint32_t i)
{
    if (T->shared) spin_unlock(&T->b[i].lock);
    else os_unfair_lock_unlock(&g_priv.locks[i]);
}

/* Lock two buckets of possibly different tables in a global order (private before shared, then by
 * index); the same bucket is locked once. */
static void lock2(struct fx_tab *T1, uint32_t b1, struct fx_tab *T2, uint32_t b2)
{
    if (T1 == T2 && b1 == b2) { bucket_lock(T1, b1); return; }
    if (T1->shared > T2->shared || (T1 == T2 && b1 > b2)) { bucket_lock(T2, b2); bucket_lock(T1, b1); }
    else { bucket_lock(T1, b1); bucket_lock(T2, b2); }
}

static void unlock2(struct fx_tab *T1, uint32_t b1, struct fx_tab *T2, uint32_t b2)
{
    bucket_unlock(T1, b1);
    if (!(T1 == T2 && b1 == b2)) bucket_unlock(T2, b2);
}

static uint32_t hash_key(const struct lkd_futex_key *k)
{
    uint64_t h = k->a * 0x9e3779b97f4a7c15ull ^ (k->b >> 2) * 0xc2b2ae3d27d4eb4full ^ k->kind;
    h ^= h >> 29;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 32;
    return (uint32_t)(h % FX_NB);
}

static struct fx_tab *tab_for(const struct lkd_futex_key *k)
{
    return k->kind == LKD_FK_SHARED ? shm_tab() : &g_ptab;
}

/* ---- entries and sleepers --------------------------------------------------------------------- */

static void alloc_lock(struct fx_tab *T)
{
    if (T->shared) spin_lock(&g_shm->alloc_lock);
    else os_unfair_lock_lock(&g_priv.alloc_lock);
}
static void alloc_unlock(struct fx_tab *T)
{
    if (T->shared) spin_unlock(&g_shm->alloc_lock);
    else os_unfair_lock_unlock(&g_priv.alloc_lock);
}

static uint32_t entry_alloc(struct fx_tab *T)
{
    uint32_t *hint = T->shared ? &g_shm->ehint : &g_priv.ehint;
    uint32_t r = 0;
    alloc_lock(T);
    for (uint32_t k = 0; k < T->ne; k++) {
        uint32_t i = (*hint + k) % T->ne;
        if (!atomic_load(&T->e[i].used)) {
            memset(&T->e[i], 0, sizeof T->e[i]);
            atomic_store(&T->e[i].used, 1);
            *hint = (i + 1) % T->ne;
            r = i + 1;
            break;
        }
    }
    alloc_unlock(T);
    return r;
}

static void entry_free(struct fx_tab *T, uint32_t idx)
{
    if (idx) atomic_store(&T->e[idx - 1].used, 0);
}

static struct fx_sleeper *shm_sleeper_alloc(void)
{
    struct fx_sleeper *s = NULL;
    spin_lock(&g_shm->alloc_lock);
    for (uint32_t k = 0; k < FX_SHM_SLEEPERS; k++) {
        uint32_t i = (g_shm->shint + k) % FX_SHM_SLEEPERS;
        if (!atomic_load(&g_shm->s[i].used)) {
            s = &g_shm->s[i];
            atomic_store(&s->word, 0);
            atomic_store(&s->used, 1);
            g_shm->shint = (i + 1) % FX_SHM_SLEEPERS;
            break;
        }
    }
    spin_unlock(&g_shm->alloc_lock);
    return s;
}

static uint64_t sleeper_ref(struct fx_tab *T, struct fx_sleeper *s)
{
    if (T->shared) return (uint64_t)(s - g_shm->s) + 1;
    return (uint64_t)(uintptr_t)s;
}

static struct fx_sleeper *sleeper_of(struct fx_tab *T, const struct fx_entry *e)
{
    if (T->shared) return e->sleeper && e->sleeper <= FX_SHM_SLEEPERS ? &g_shm->s[e->sleeper - 1] : NULL;
    return (struct fx_sleeper *)(uintptr_t)e->sleeper;
}

/* Set the waking index (first waker wins) and wake the sleeper. Called with the entry's bucket lock
 * held, which keeps a stack sleeper alive: its owner unqueues under the same lock before returning. */
static void sleeper_wake(struct fx_sleeper *s, uint32_t value)
{
    if (!s) return;
    uint64_t w = atomic_load(&s->word);
    for (;;) {
        if ((uint32_t)w) return;
        if (atomic_compare_exchange_weak(&s->word, &w, (w & 0xffffffff00000000ull) | value)) break;
    }
    if (s->shared) os_sync_wake_by_address_any((void *)&s->word, 8, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
    else lkd_task_wake((lkd_task *)(uintptr_t)s->task);
}

/* ---- shared sleepers for other waits of the family (ntsync.c) ---------------------------------- */

static int fx_sleep(lkd_task *t, struct fx_sleeper *s, int64_t deadline);

struct fx_sleeper *sig_shared_sleeper_get(lkd_task *t)
{
    if (!shm_tab()) return NULL;
    struct fx_sleeper *s = shm_sleeper_alloc();
    if (!s) return NULL;
    s->shared = 1;
    s->host_pid = (int32_t)mypid();
    s->tid = t->tid;
    s->task = (uint64_t)(uintptr_t)t;
    return s;
}

uint32_t sig_shared_sleeper_index(struct fx_sleeper *s) { return (uint32_t)(s - g_shm->s) + 1; }

void sig_shared_sleeper_wake(uint32_t index)
{
    if (!index || index > FX_SHM_SLEEPERS || !shm_tab()) return;
    sleeper_wake(&g_shm->s[index - 1], 1);
}

int sig_shared_sleep(lkd_task *t, struct fx_sleeper *s, int64_t deadline) { return fx_sleep(t, s, deadline); }

void sig_shared_sleeper_put(struct fx_sleeper *s) { atomic_store(&s->used, 0); }

void sig_shared_sleeper_rearm(struct fx_sleeper *s) { atomic_fetch_and(&s->word, 0xffffffff00000000ull); }

void sig_spin_lock(_Atomic uint32_t *l) { spin_lock(l); }
void sig_spin_unlock(_Atomic uint32_t *l) { spin_unlock(l); }

void sig_ring_futex(lkd_task *t)
{
    struct sig_task *st = __atomic_load_n((struct sig_task **)&t->pending.queue, __ATOMIC_ACQUIRE);
    if (!st) return;
    struct fx_sleeper *s = atomic_load(&st->fx);
    if (!s || !s->shared) return;
    atomic_fetch_add(&s->word, 1ull << 32);
    os_sync_wake_by_address_all((void *)&s->word, 8, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

/* lkd_task_wake (core) calls this after writing the wake pipe (request sig-1): a wake from any module
 * (ptrace, task) also ends a shared-key futex sleep, which does not poll the pipe. */
void lkd_sig_wake_hook(lkd_task *t) { sig_ring_futex(t); }

/* ---- bucket lists ----------------------------------------------------------------------------- */

static void list_append(struct fx_tab *T, uint32_t b, uint32_t idx)
{
    struct fx_bucket *bk = &T->b[b];
    struct fx_entry *e = &T->e[idx - 1];
    e->bucket = b;
    e->next = 0;
    e->prev = bk->tail;
    if (bk->tail) T->e[bk->tail - 1].next = idx;
    else bk->head = idx;
    bk->tail = idx;
    bk->count++;
}

static void list_remove(struct fx_tab *T, uint32_t idx)
{
    struct fx_entry *e = &T->e[idx - 1];
    struct fx_bucket *bk = &T->b[e->bucket];
    if (e->prev) T->e[e->prev - 1].next = e->next;
    else bk->head = e->next;
    if (e->next) T->e[e->next - 1].prev = e->prev;
    else bk->tail = e->prev;
    e->next = e->prev = 0;
    if (bk->count) bk->count--;
}

static int key_match(const struct fx_entry *e, const struct lkd_futex_key *k)
{
    return e->key_kind == k->kind && e->key_a == k->a && e->key_b == k->b;
}

/* Dequeue and wake up to nr matching waiters (bitset filter; PI waiters are an error: -EINVAL).
 * nr <= 0 still wakes one (futex_wake: "++ret >= nr_wake"). Bucket locked by the caller. */
static int wake_locked(struct fx_tab *T, uint32_t b, const struct lkd_futex_key *k, int nr, uint32_t bitset, int use_bitset)
{
    int ret = 0;
    uint32_t i = T->b[b].head;
    while (i) {
        struct fx_entry *e = &T->e[i - 1];
        uint32_t next = e->next;
        if (key_match(e, k)) {
            if (e->pi) return -LX_EINVAL;
            if (!use_bitset || (e->bitset & bitset)) {
                list_remove(T, i);
                e->state = FXS_WOKEN;
                sleeper_wake(sleeper_of(T, e), e->vec + 1);
                if (++ret >= nr) break;
            }
        }
        i = next;
    }
    return ret;
}

/* ---- guest word access ------------------------------------------------------------------------ */

static int get_word(uint64_t gva, uint32_t *v)
{
    return lkd_user_read(gva, v, sizeof *v) < 0 ? -LX_EFAULT : 0;
}

/* Atomic compare-and-swap on a guest word, fault-safe (core, request sig-1): a concurrent munmap gives
 * -EFAULT instead of a host crash. */
static int cmpxchg_word(uint64_t gva, uint32_t *expected, uint32_t desired)
{
    int r = lkd_user_cmpxchg32(gva, expected, desired);
    return r == -LX_EINVAL ? -LX_EFAULT : r;
}

/* ---- sleeping --------------------------------------------------------------------------------- */

/* Returns 1 woken, 0 deadline, -1 interrupted. */
static int fx_sleep(lkd_task *t, struct fx_sleeper *s, int64_t deadline)
{
    struct sig_task *st = NULL;
    if (s->shared) {
        struct lkd_sigproc *sp = sig_proc(t->proc);
        sig_lock(sp);
        st = sig_task_of(t);
        sig_unlock(sp);
        atomic_store(&st->fx, s);
    }
    int r;
    for (;;) {
        uint64_t w = atomic_load(&s->word);
        if ((uint32_t)w) { r = 1; break; }
        if (lkd_sig_interrupted(t)) { r = -1; break; }
        int64_t now = lkd_host_mono_ns();
        if (deadline >= 0 && now >= deadline) { r = 0; break; }
        if (!s->shared) {
            int ms = -1;
            if (deadline >= 0) {
                int64_t left = deadline - now;
                ms = (int)((left + 999999) / 1000000);
            }
            struct pollfd p = { .fd = t->wake_rd, .events = POLLIN, .revents = 0 };
            int pr = poll(&p, 1, ms);
            if (pr > 0) { char buf[64]; while (read(t->wake_rd, buf, sizeof buf) > 0) { } }
        } else {
            int64_t slice = FX_SLICE_NS;
            if (deadline >= 0 && deadline - now < slice) slice = deadline - now;
            if (slice < 1000) slice = 1000;
            (void)os_sync_wait_on_address_with_timeout((void *)&s->word, w, 8, OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                                       OS_CLOCK_MACH_ABSOLUTE_TIME, (uint64_t)slice);
        }
    }
    if (st) atomic_store(&st->fx, NULL);
    return r;
}

/* ---- waits ------------------------------------------------------------------------------------ */

struct fx_w {
    uint64_t uaddr;
    uint32_t val;
    uint32_t bitset;
    int private_hint;
    struct lkd_futex_key key;
    struct fx_tab *T;
    uint32_t b;
    uint32_t idx;
};

/* Remove entry idx of T whatever bucket requeue moved it to. Returns its state. */
static int unqueue(struct fx_tab *T, uint32_t idx)
{
    for (;;) {
        uint32_t b = __atomic_load_n(&T->e[idx - 1].bucket, __ATOMIC_ACQUIRE);
        bucket_lock(T, b);
        if (T->e[idx - 1].bucket != b) { bucket_unlock(T, b); continue; }
        int st = T->e[idx - 1].state;
        if (st == FXS_QUEUED) list_remove(T, idx);
        bucket_unlock(T, b);
        return st;
    }
}

/* futex_wait_multiple: queue every futex, compare its value under its bucket lock, sleep. Returns the
 * index of a woken futex (>= 0), -EAGAIN, -ETIMEDOUT, -ERESTARTSYS (interrupted), -EFAULT, -EINVAL,
 * -ENOMEM. */
static int64_t wait_multi(lkd_task *t, struct fx_w *w, int n, int64_t deadline)
{
    int any_shared = 0;
    for (int i = 0; i < n; i++) {
        int e = fx_key(t, w[i].uaddr, w[i].private_hint, &w[i].key);
        if (e < 0) return e;
        w[i].T = tab_for(&w[i].key);
        if (!w[i].T) return -LX_ENOMEM;
        w[i].b = hash_key(&w[i].key);
        w[i].idx = 0;
        any_shared |= w[i].T->shared;
    }
    struct fx_sleeper local, *s;
    if (any_shared) {
        if (!shm_tab()) return -LX_ENOMEM;
        s = shm_sleeper_alloc();
        if (!s) return -LX_ENOMEM;
        s->shared = 1;
    } else {
        memset(&local, 0, sizeof local);
        s = &local;
        s->shared = 0;
    }
    s->host_pid = (int32_t)mypid();
    s->tid = t->tid;
    s->task = (uint64_t)(uintptr_t)t;

    int64_t ret = 0;
    int queued = 0;
    for (int i = 0; i < n; i++) {
        uint32_t idx = entry_alloc(w[i].T);
        if (!idx) { ret = -LX_ENOMEM; break; }
        struct fx_entry *e = &w[i].T->e[idx - 1];
        e->key_kind = w[i].key.kind;
        e->key_a = w[i].key.a;
        e->key_b = w[i].key.b;
        e->bitset = w[i].bitset;
        e->vec = (uint32_t)i;
        e->uaddr = w[i].uaddr;
        e->tid = t->tid;
        e->host_pid = (int32_t)mypid();
        e->sleeper = sleeper_ref(w[i].T, s);
        e->state = FXS_QUEUED;
        bucket_lock(w[i].T, w[i].b);
        uint32_t cur;
        int ge = get_word(w[i].uaddr, &cur);
        if (ge < 0 || cur != w[i].val) {
            bucket_unlock(w[i].T, w[i].b);
            entry_free(w[i].T, idx);
            ret = ge < 0 ? ge : -LX_EAGAIN;
            break;
        }
        list_append(w[i].T, w[i].b, idx);
        bucket_unlock(w[i].T, w[i].b);
        w[i].idx = idx;
        queued = i + 1;
    }
    int r = 1;
    if (ret == 0) r = fx_sleep(t, s, deadline);
    for (int i = 0; i < queued; i++) {
        (void)unqueue(w[i].T, w[i].idx);
        entry_free(w[i].T, w[i].idx);
    }
    uint32_t woke = (uint32_t)atomic_load(&s->word);
    if (any_shared) atomic_store(&s->used, 0);
    if (woke) return (int64_t)(woke - 1); /* a wake beats a timeout, a signal or a later mismatch */
    if (ret) return ret;
    return r == 0 ? -LX_ETIMEDOUT : -LX_ERESTARTSYS;
}

/* ---- timeouts --------------------------------------------------------------------------------- */

static int read_ts(uint64_t uts, struct lx_timespec *ts)
{
    if (lkd_user_read(uts, ts, sizeof *ts) < 0) return -LX_EFAULT;
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000) return -LX_EINVAL;
    return 0;
}

static int64_t ts_ns(const struct lx_timespec *ts)
{
    int64_t s = ts->tv_sec > 100000000000ll ? 100000000000ll : ts->tv_sec;
    return s * 1000000000ll + ts->tv_nsec;
}

/* Absolute guest time on CLOCK_REALTIME or CLOCK_MONOTONIC to a deadline on lkd_host_mono_ns, through the
 * guest clocks of the task module (docs/requests/sig-2.md section 3). A deadline is converted once: a later
 * change of the wall clock does not move it. */
static int64_t abs_deadline(const struct lx_timespec *ts, int realtime)
{
    /* the guest clocks of clock_gettime (task), not the host clock of the deadlines (sig-2 point 3) */
    int64_t now = lkd_guest_clock_ns(realtime ? LX_CLOCK_REALTIME : LX_CLOCK_MONOTONIC);
    int64_t rel = ts_ns(ts) - now;
    if (rel < 0) rel = 0;
    return lkd_host_mono_ns() + rel;
}

/* ---- operations ------------------------------------------------------------------------------- */

static int64_t futex_wait_restart(lkd_task *t);

static int64_t do_wait(lkd_task *t, uint64_t uaddr, int private_hint, uint32_t val, int64_t deadline, uint32_t bitset)
{
    if (!bitset) return -LX_EINVAL;
    struct fx_w w;
    memset(&w, 0, sizeof w);
    w.uaddr = uaddr;
    w.val = val;
    w.bitset = bitset;
    w.private_hint = private_hint;
    int64_t r = wait_multi(t, &w, 1, deadline);
    if (r >= 0) return 0;
    if (r == -LX_ERESTARTSYS && deadline >= 0) {
        t->restart.fn = futex_wait_restart;
        t->restart.arg[0] = uaddr;
        t->restart.arg[1] = (uint64_t)private_hint;
        t->restart.arg[2] = val;
        t->restart.arg[3] = (uint64_t)deadline;
        t->restart.arg[4] = bitset;
        return -LX_ERESTART_RESTARTBLOCK;
    }
    return r;
}

static int64_t futex_wait_restart(lkd_task *t)
{
    uint64_t a0 = t->restart.arg[0], a1 = t->restart.arg[1], a2 = t->restart.arg[2], a3 = t->restart.arg[3], a4 = t->restart.arg[4];
    t->restart.fn = NULL;
    return do_wait(t, a0, (int)a1, (uint32_t)a2, (int64_t)a3, (uint32_t)a4);
}

static int64_t do_wake(lkd_task *t, uint64_t uaddr, int private_hint, int nr, uint32_t bitset, int strict)
{
    if (!bitset) return -LX_EINVAL;
    struct lkd_futex_key k;
    int e = fx_key(t, uaddr, private_hint, &k);
    if (e < 0) return e;
    if (strict && !nr) return 0;
    struct fx_tab *T = tab_for(&k);
    if (!T) return 0;
    uint32_t b = hash_key(&k);
    /* No unlocked "waiters pending" shortcut: a waiter reads the futex word before it is counted in the
     * bucket, so a waker that stores the word and then reads a zero count could miss a waiter that read
     * the old value (lost wake-up, MESURE: test_sig_futex mutex_contention hung about once in 20 runs).
     * Linux counts the waiter before reading the word (futex_q_lock: hb_waiters_inc then smp_mb). */
    bucket_lock(T, b);
    int r = wake_locked(T, b, &k, nr, bitset, 1);
    bucket_unlock(T, b);
    return r;
}

static int64_t do_requeue(lkd_task *t, uint64_t u1, int ph1, uint64_t u2, int ph2, int nr_wake, int nr_requeue, const uint32_t *cmpval)
{
    if (nr_wake < 0 || nr_requeue < 0) return -LX_EINVAL;
    struct lkd_futex_key k1, k2;
    int e = fx_key(t, u1, ph1, &k1);
    if (e < 0) return e;
    e = fx_key(t, u2, ph2, &k2);
    if (e < 0) return e;
    struct fx_tab *T1 = tab_for(&k1), *T2 = tab_for(&k2);
    if (!T1 || !T2) return -LX_ENOMEM;
    uint32_t b1 = hash_key(&k1), b2 = hash_key(&k2);
    lock2(T1, b1, T2, b2);
    if (cmpval) {
        uint32_t cur;
        if (get_word(u1, &cur) < 0) { unlock2(T1, b1, T2, b2); return -LX_EFAULT; }
        if (cur != *cmpval) { unlock2(T1, b1, T2, b2); return -LX_EAGAIN; }
    }
    int task_count = 0, ret = 0;
    uint32_t i = T1->b[b1].head;
    while (i) {
        struct fx_entry *en = &T1->e[i - 1];
        uint32_t next = en->next;
        if (task_count - nr_wake >= nr_requeue) break;
        if (key_match(en, &k1)) {
            if (en->pi) { ret = -LX_EINVAL; break; }
            if (++task_count <= nr_wake || T1 != T2) {
                /* A move across tables would leave a private sleeper unreachable from other
                 * processes: such a waiter is woken instead (a spurious wakeup futex users allow). */
                list_remove(T1, i);
                en->state = FXS_WOKEN;
                sleeper_wake(sleeper_of(T1, en), en->vec + 1);
            } else {
                list_remove(T1, i);
                en->key_kind = k2.kind;
                en->key_a = k2.a;
                en->key_b = k2.b;
                list_append(T2, b2, i);
            }
        }
        i = next;
    }
    unlock2(T1, b1, T2, b2);
    return ret ? ret : task_count;
}

static int64_t do_wake_op(lkd_task *t, uint64_t u1, uint64_t u2, int ph, int nr_wake, int nr_wake2, uint32_t encoded)
{
    struct lkd_futex_key k1, k2;
    int e = fx_key(t, u1, ph, &k1);
    if (e < 0) return e;
    e = fx_key(t, u2, ph, &k2);
    if (e < 0) return e;
    struct fx_tab *T1 = tab_for(&k1), *T2 = tab_for(&k2);
    if (!T1 || !T2) return -LX_ENOMEM;
    uint32_t b1 = hash_key(&k1), b2 = hash_key(&k2);
    unsigned op = (encoded & 0x70000000u) >> 28, cmp = (encoded & 0x0f000000u) >> 24;
    int32_t oparg = (int32_t)((encoded & 0x00fff000u) << 8) >> 20;
    int32_t cmparg = (int32_t)((encoded & 0x00000fffu) << 20) >> 20;
    if (encoded & (8u << 28)) oparg = (int32_t)(1u << (oparg & 31));
    if (op > 4) return -LX_ENOSYS;
    lock2(T1, b1, T2, b2);
    uint32_t old;
    if (get_word(u2, &old) < 0) { unlock2(T1, b1, T2, b2); return -LX_EFAULT; }
    for (;;) {
        uint32_t nv;
        switch (op) {
        case 0: nv = (uint32_t)oparg; break;
        case 1: nv = old + (uint32_t)oparg; break;
        case 2: nv = old | (uint32_t)oparg; break;
        case 3: nv = old & ~(uint32_t)oparg; break;
        default: nv = old ^ (uint32_t)oparg; break;
        }
        int c = cmpxchg_word(u2, &old, nv);
        if (c < 0) { unlock2(T1, b1, T2, b2); return c; }
        if (c == 0) break;
    }
    int32_t o = (int32_t)old;
    int op_ret;
    switch (cmp) {
    case 0: op_ret = o == cmparg; break;
    case 1: op_ret = o != cmparg; break;
    case 2: op_ret = o < cmparg; break;
    case 3: op_ret = o <= cmparg; break;
    case 4: op_ret = o > cmparg; break;
    case 5: op_ret = o >= cmparg; break;
    default: unlock2(T1, b1, T2, b2); return -LX_ENOSYS;
    }
    int ret = wake_locked(T1, b1, &k1, nr_wake, 0, 0);
    if (ret >= 0 && op_ret > 0) {
        int r2 = wake_locked(T2, b2, &k2, nr_wake2, 0, 0);
        ret = r2 < 0 ? r2 : ret + r2;
    }
    unlock2(T1, b1, T2, b2);
    return ret;
}

/* ---- PI futexes (no priority inheritance: ownership protocol only) ---------------------------- */

static int owner_alive(int32_t tid)
{
    lkd_tid_info ti;
    if (lkd_tid_lookup(tid, &ti) == 0 && ti.state != LKD_PS_ZOMBIE && ti.state != LKD_PS_DEAD) return 1;
    lkd_task *x = lkd_task_lookup(tid);
    if (x) { int alive = !(atomic_load(&x->flags) & LKD_TF_EXITING); lkd_task_put(x); return alive; }
    return 0;
}

static int has_pi_waiter(struct fx_tab *T, uint32_t b, const struct lkd_futex_key *k)
{
    for (uint32_t i = T->b[b].head; i; i = T->e[i - 1].next)
        if (T->e[i - 1].pi && key_match(&T->e[i - 1], k)) return 1;
    return 0;
}

static int64_t do_lock_pi(lkd_task *t, uint64_t uaddr, int ph, int64_t deadline, int trylock)
{
    struct lkd_futex_key k;
    int e = fx_key(t, uaddr, ph, &k);
    if (e < 0) return e;
    struct fx_tab *T = tab_for(&k);
    if (!T) return -LX_ENOMEM;
    uint32_t b = hash_key(&k);
    uint32_t tid = (uint32_t)t->tid;
    for (;;) {
        bucket_lock(T, b);
        uint32_t uval;
        if (get_word(uaddr, &uval) < 0) { bucket_unlock(T, b); return -LX_EFAULT; }
        if ((uval & LX_FUTEX_TID_MASK) == tid) { bucket_unlock(T, b); return -LX_EDEADLK; }
        if (!(uval & LX_FUTEX_TID_MASK)) {
            uint32_t nv = (uval & LX_FUTEX_OWNER_DIED) | tid | (has_pi_waiter(T, b, &k) ? LX_FUTEX_WAITERS : 0);
            int c = cmpxchg_word(uaddr, &uval, nv);
            bucket_unlock(T, b);
            if (c < 0) return c;
            if (c == 0) return 0;
            continue;
        }
        if (!owner_alive((int32_t)(uval & LX_FUTEX_TID_MASK))) {
            /* handle_exit_race: the owner is gone and the word did not change */
            bucket_unlock(T, b);
            return -LX_ESRCH;
        }
        if (trylock) { bucket_unlock(T, b); return -LX_EWOULDBLOCK; }
        if (!(uval & LX_FUTEX_WAITERS)) {
            int c = cmpxchg_word(uaddr, &uval, uval | LX_FUTEX_WAITERS);
            if (c != 0) { bucket_unlock(T, b); if (c < 0) return c; continue; }
        }
        bucket_unlock(T, b);
        break;
    }
    /* queue as a PI waiter and sleep until the owner hands the lock over */
    struct fx_sleeper local, *s;
    if (T->shared) { s = shm_sleeper_alloc(); if (!s) return -LX_ENOMEM; s->shared = 1; }
    else { memset(&local, 0, sizeof local); s = &local; }
    s->host_pid = (int32_t)mypid();
    s->tid = t->tid;
    s->task = (uint64_t)(uintptr_t)t;
    uint32_t idx = entry_alloc(T);
    if (!idx) { if (T->shared) atomic_store(&s->used, 0); return -LX_ENOMEM; }
    struct fx_entry *en = &T->e[idx - 1];
    en->key_kind = k.kind; en->key_a = k.a; en->key_b = k.b;
    en->pi = 1; en->bitset = LX_FUTEX_BITSET_MATCH_ANY; en->uaddr = uaddr;
    en->tid = t->tid; en->host_pid = (int32_t)mypid(); en->sleeper = sleeper_ref(T, s);
    int64_t ret;
    for (;;) {
        bucket_lock(T, b);
        uint32_t uval;
        if (get_word(uaddr, &uval) < 0) { bucket_unlock(T, b); ret = -LX_EFAULT; break; }
        if (!(uval & LX_FUTEX_TID_MASK)) {
            /* released meanwhile: take it */
            uint32_t nv = (uval & LX_FUTEX_OWNER_DIED) | (uint32_t)t->tid | (has_pi_waiter(T, b, &k) ? LX_FUTEX_WAITERS : 0);
            int c = cmpxchg_word(uaddr, &uval, nv);
            bucket_unlock(T, b);
            if (c < 0) { ret = c; break; }
            if (c == 0) { ret = 0; break; }
            continue;
        }
        if (!(uval & LX_FUTEX_WAITERS)) {
            int c = cmpxchg_word(uaddr, &uval, uval | LX_FUTEX_WAITERS);
            if (c != 0) { bucket_unlock(T, b); if (c < 0) { ret = c; break; } continue; }
        }
        en->state = FXS_QUEUED;
        atomic_store(&s->word, atomic_load(&s->word) & 0xffffffff00000000ull);
        list_append(T, b, idx);
        bucket_unlock(T, b);
        int r = fx_sleep(t, s, deadline);
        bucket_lock(T, b);
        int st = en->state;
        if (st == FXS_QUEUED) list_remove(T, idx);
        bucket_unlock(T, b);
        if (st == FXS_OWNER) { ret = 0; break; }
        if (st == FXS_QUEUED) {
            if (r == 0) { ret = -LX_ETIMEDOUT; break; }
            if (r < 0) { ret = -LX_ERESTARTNOINTR; break; } /* futex_lock_pi: -EINTR becomes restart */
        }
        /* woken without ownership (robust death handoff races): try again */
    }
    entry_free(T, idx);
    if (T->shared) atomic_store(&s->used, 0);
    return ret;
}

/* Hand the lock to the first PI waiter of k (bucket locked). Returns 1 if handed over. */
static int pi_handoff_locked(struct fx_tab *T, uint32_t b, const struct lkd_futex_key *k, uint64_t uaddr,
                             uint32_t uval, uint32_t keep_bits, int *err)
{
    for (uint32_t i = T->b[b].head; i; i = T->e[i - 1].next) {
        struct fx_entry *en = &T->e[i - 1];
        if (!en->pi || !key_match(en, k)) continue;
        uint32_t nv = (uint32_t)en->tid | LX_FUTEX_WAITERS | keep_bits;
        int c = cmpxchg_word(uaddr, &uval, nv);
        if (c != 0) { *err = c < 0 ? c : -LX_EAGAIN; return 0; }
        list_remove(T, i);
        en->state = FXS_OWNER;
        sleeper_wake(sleeper_of(T, en), en->vec + 1);
        return 1;
    }
    return 0;
}

static int64_t do_unlock_pi(lkd_task *t, uint64_t uaddr, int ph)
{
    struct lkd_futex_key k;
    int e = fx_key(t, uaddr, ph, &k);
    if (e < 0) return e;
    struct fx_tab *T = tab_for(&k);
    if (!T) return -LX_ENOMEM;
    uint32_t b = hash_key(&k);
    for (int tries = 0; tries < 1000; tries++) {
        bucket_lock(T, b);
        uint32_t uval;
        if (get_word(uaddr, &uval) < 0) { bucket_unlock(T, b); return -LX_EFAULT; }
        if ((uval & LX_FUTEX_TID_MASK) != (uint32_t)t->tid) { bucket_unlock(T, b); return -LX_EPERM; }
        int err = 0;
        if (pi_handoff_locked(T, b, &k, uaddr, uval, 0, &err)) { bucket_unlock(T, b); return 0; }
        if (err == -LX_EFAULT) { bucket_unlock(T, b); return err; }
        if (err) { bucket_unlock(T, b); continue; }
        int c = cmpxchg_word(uaddr, &uval, 0);
        bucket_unlock(T, b);
        if (c < 0) return c;
        if (c == 0) return 0;
    }
    return -LX_EAGAIN;
}

/* ---- the futex syscall ------------------------------------------------------------------------ */

static int cmd_has_timeout(int cmd)
{
    return cmd == LX_FUTEX_WAIT || cmd == LX_FUTEX_LOCK_PI || cmd == LX_FUTEX_LOCK_PI2 ||
           cmd == LX_FUTEX_WAIT_BITSET || cmd == LX_FUTEX_WAIT_REQUEUE_PI;
}

LKD_SYSCALL(futex)
{
    uint64_t uaddr = LKD_ARGP(0);
    int op = LKD_ARGI(1);
    uint32_t val = LKD_ARGUI(2);
    uint64_t utime = LKD_ARGU(3);
    uint64_t uaddr2 = LKD_ARGP(4);
    uint32_t val3 = LKD_ARGUI(5);
    int cmd = op & LX_FUTEX_CMD_MASK;
    int ph = (op & LX_FUTEX_PRIVATE_FLAG) != 0;
    int clockrt = (op & LX_FUTEX_CLOCK_REALTIME) != 0;
    if (clockrt && cmd != LX_FUTEX_WAIT_BITSET && cmd != LX_FUTEX_WAIT_REQUEUE_PI && cmd != LX_FUTEX_LOCK_PI2)
        return -LX_ENOSYS;
    int64_t deadline = -1;
    if (utime && cmd_has_timeout(cmd)) {
        struct lx_timespec ts;
        int e = read_ts(utime, &ts);
        if (e < 0) return e;
        if (cmd == LX_FUTEX_WAIT) deadline = lkd_host_mono_ns() + ts_ns(&ts);
        else if (cmd == LX_FUTEX_LOCK_PI) deadline = abs_deadline(&ts, 1);
        else deadline = abs_deadline(&ts, clockrt);
    }
    int val2 = (int)(uint32_t)utime;
    switch (cmd) {
    case LX_FUTEX_WAIT: return do_wait(t, uaddr, ph, val, deadline, LX_FUTEX_BITSET_MATCH_ANY);
    case LX_FUTEX_WAIT_BITSET: return do_wait(t, uaddr, ph, val, deadline, val3);
    case LX_FUTEX_WAKE: return do_wake(t, uaddr, ph, (int)val, LX_FUTEX_BITSET_MATCH_ANY, 0);
    case LX_FUTEX_WAKE_BITSET: return do_wake(t, uaddr, ph, (int)val, val3, 0);
    case LX_FUTEX_REQUEUE: return do_requeue(t, uaddr, ph, uaddr2, ph, (int)val, val2, NULL);
    case LX_FUTEX_CMP_REQUEUE: return do_requeue(t, uaddr, ph, uaddr2, ph, (int)val, val2, &val3);
    case LX_FUTEX_WAKE_OP: return do_wake_op(t, uaddr, uaddr2, ph, (int)val, val2, val3);
    case LX_FUTEX_LOCK_PI:
    case LX_FUTEX_LOCK_PI2: return do_lock_pi(t, uaddr, ph, deadline, 0);
    case LX_FUTEX_TRYLOCK_PI: return do_lock_pi(t, uaddr, ph, -1, 1);
    case LX_FUTEX_UNLOCK_PI: return do_unlock_pi(t, uaddr, ph);
    case LX_FUTEX_WAIT_REQUEUE_PI:
    case LX_FUTEX_CMP_REQUEUE_PI:
        return -LX_ENOSYS; /* documented gap: glibc >= 2.25 condvars do not use requeue-PI */
    default:
        return -LX_ENOSYS;
    }
}

/* ---- futex2 ----------------------------------------------------------------------------------- */

#define FUTEX2_SIZE_U32 0x02u
#define FUTEX2_SIZE_MASK 0x03u
#define FUTEX2_PRIVATE 128u
#define FUTEX2_VALID_MASK (FUTEX2_SIZE_MASK | FUTEX2_PRIVATE)

static int futex2_flags_ok(uint32_t flags)
{
    if (flags & ~FUTEX2_VALID_MASK) return 0;
    return (flags & FUTEX2_SIZE_MASK) == FUTEX2_SIZE_U32;
}

static int futex2_timeout(uint64_t uts, int clockid, int64_t *deadline)
{
    *deadline = -1;
    if (!uts) return 0;
    if (clockid != LX_CLOCK_REALTIME && clockid != LX_CLOCK_MONOTONIC) return -LX_EINVAL;
    struct lx_timespec ts;
    int e = read_ts(uts, &ts);
    if (e < 0) return e;
    *deadline = abs_deadline(&ts, clockid == LX_CLOCK_REALTIME);
    return 0;
}

static int parse_waitv(uint64_t uw, unsigned n, struct fx_w *w)
{
    for (unsigned i = 0; i < n; i++) {
        struct lx_futex_waitv a;
        if (lkd_user_read(uw + i * sizeof a, &a, sizeof a) < 0) return -LX_EFAULT;
        if ((a.flags & ~FUTEX2_VALID_MASK) || a._reserved) return -LX_EINVAL;
        if (!futex2_flags_ok(a.flags)) return -LX_EINVAL;
        if (a.val >> 32) return -LX_EINVAL;
        memset(&w[i], 0, sizeof w[i]);
        w[i].uaddr = a.uaddr;
        w[i].val = (uint32_t)a.val;
        w[i].bitset = LX_FUTEX_BITSET_MATCH_ANY;
        w[i].private_hint = (a.flags & FUTEX2_PRIVATE) != 0;
    }
    return 0;
}

LKD_SYSCALL(futex_waitv)
{
    uint64_t uw = LKD_ARGP(0);
    unsigned n = LKD_ARGUI(1);
    unsigned flags = LKD_ARGUI(2);
    if (flags) return -LX_EINVAL;
    if (!n || n > FX_WAITV_MAX || !uw) return -LX_EINVAL;
    int64_t deadline;
    int e = futex2_timeout(LKD_ARGP(3), LKD_ARGI(4), &deadline);
    if (e < 0) return e;
    struct fx_w *w = calloc(n, sizeof *w);
    if (!w) return -LX_ENOMEM;
    e = parse_waitv(uw, n, w);
    int64_t r = e < 0 ? e : wait_multi(t, w, (int)n, deadline);
    free(w);
    return r;
}

LKD_SYSCALL(futex_wake)
{
    uint64_t mask = LKD_ARGU(1);
    int nr = LKD_ARGI(2);
    unsigned flags = LKD_ARGUI(3);
    if (!futex2_flags_ok(flags)) return -LX_EINVAL;
    if (mask >> 32) return -LX_EINVAL;
    return do_wake(t, LKD_ARGP(0), (flags & FUTEX2_PRIVATE) != 0, nr, (uint32_t)mask, 1);
}

LKD_SYSCALL(futex_wait)
{
    uint64_t val = LKD_ARGU(1), mask = LKD_ARGU(2);
    unsigned flags = LKD_ARGUI(3);
    if (!futex2_flags_ok(flags)) return -LX_EINVAL;
    if ((val >> 32) || (mask >> 32)) return -LX_EINVAL;
    int64_t deadline;
    int e = futex2_timeout(LKD_ARGP(4), LKD_ARGI(5), &deadline);
    if (e < 0) return e;
    int64_t r = do_wait(t, LKD_ARGP(0), (flags & FUTEX2_PRIVATE) != 0, (uint32_t)val, deadline, (uint32_t)mask);
    /* futex_wait(2) has no restart block: an interrupted wait with a timeout ends with EINTR (the
     * __futex_wait path returns -ERESTARTSYS only without timeout). */
    if (r == -LX_ERESTART_RESTARTBLOCK) { t->restart.fn = NULL; return -LX_ERESTARTSYS; }
    return r;
}

LKD_SYSCALL(futex_requeue)
{
    uint64_t uw = LKD_ARGP(0);
    unsigned flags = LKD_ARGUI(1);
    int nr_wake = LKD_ARGI(2), nr_requeue = LKD_ARGI(3);
    if (flags) return -LX_EINVAL;
    if (!uw) return -LX_EINVAL;
    struct fx_w w[2];
    int e = parse_waitv(uw, 2, w);
    if (e < 0) return e;
    uint32_t cmpval = w[0].val;
    return do_requeue(t, w[0].uaddr, w[0].private_hint, w[1].uaddr, w[1].private_hint, nr_wake, nr_requeue, &cmpval);
}

/* ---- API for task (futex.h) ------------------------------------------------------------------- */

int lkd_futex_wake_addr(lkd_task *t, uint64_t gva, int nr, int private_hint)
{
    int64_t r = do_wake(t, gva, private_hint, nr, LX_FUTEX_BITSET_MATCH_ANY, 0);
    return (int)r;
}

/* handle_futex_death (kernel/futex/core.c) */
static int handle_death(lkd_task *t, uint64_t uaddr, int pi, int pending_op)
{
    for (int tries = 0; tries < 100; tries++) {
        uint32_t uval;
        if (get_word(uaddr, &uval) < 0) return -1;
        if (pending_op && !pi && !uval) {
            (void)do_wake(t, uaddr, 0, 1, LX_FUTEX_BITSET_MATCH_ANY, 0);
            return 0;
        }
        if ((uval & LX_FUTEX_TID_MASK) != (uint32_t)t->tid) return 0;
        uint32_t mval = (uval & LX_FUTEX_WAITERS) | LX_FUTEX_OWNER_DIED;
        if (pi && (uval & LX_FUTEX_WAITERS)) {
            /* exit_pi_state_list: the first PI waiter becomes the owner, OWNER_DIED kept */
            struct lkd_futex_key k;
            if (fx_key(t, uaddr, 0, &k) == 0) {
                struct fx_tab *T = tab_for(&k);
                if (T) {
                    uint32_t b = hash_key(&k);
                    bucket_lock(T, b);
                    int err = 0;
                    int done = pi_handoff_locked(T, b, &k, uaddr, uval, LX_FUTEX_OWNER_DIED, &err);
                    bucket_unlock(T, b);
                    if (done) return 0;
                    if (err == -LX_EFAULT) return -1;
                    if (err) continue;
                }
            }
        }
        int c = cmpxchg_word(uaddr, &uval, mval);
        if (c < 0) return -1;
        if (c > 0) continue;
        if (!pi && (uval & LX_FUTEX_WAITERS)) (void)do_wake(t, uaddr, 0, 1, LX_FUTEX_BITSET_MATCH_ANY, 0);
        return 0;
    }
    return 0;
}

void lkd_futex_exit_robust_list(lkd_task *t)
{
    uint64_t head = t->robust_list_head;
    if (!head) return;
    struct lx_robust_list_head h;
    if (lkd_user_read(head, &h, sizeof h) < 0) return;
    uint64_t entry = h.list & ~1ull;
    int pi = (int)(h.list & 1);
    uint64_t pending = h.list_op_pending & ~1ull;
    int pip = (int)(h.list_op_pending & 1);
    unsigned limit = FX_ROBUST_LIMIT;
    while (entry != head) {
        uint64_t next;
        if (lkd_user_read(entry, &next, sizeof next) < 0) return;
        if (entry != pending && handle_death(t, entry + (uint64_t)h.futex_offset, pi, 0) < 0) return;
        entry = next & ~1ull;
        pi = (int)(next & 1);
        if (!--limit) break;
    }
    if (pending) (void)handle_death(t, pending + (uint64_t)h.futex_offset, pip, 1);
}

/* ---- module glue ------------------------------------------------------------------------------ */

void sig_futex_fork_child(void)
{
    /* Every private waiter belonged to a thread that does not exist in the child. */
    memset(&g_priv, 0, sizeof g_priv); /* zero is OS_UNFAIR_LOCK_INIT */
    g_mypid = (uint32_t)getpid();
    lkd_fork_reinit_mutex(&g_shm_lock, 0);
}

void sig_futex_register(void)
{
    LKD_REGISTER(futex);
    LKD_REGISTER(futex_waitv);
    LKD_REGISTER(futex_wake);
    LKD_REGISTER(futex_wait);
    LKD_REGISTER(futex_requeue);
}
