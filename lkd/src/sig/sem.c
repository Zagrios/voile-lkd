/* sem.c: System V semaphores: semget, semop, semtimedop, semctl. Models ipc/sem.c and ipc/util.c of
 * Linux v6.12. Owner: sig.
 *
 * Valve's Linux steamclient (tier0 threadtools.cpp) builds thread synchronisation objects on semget and
 * semop. Without them a Steam game that lists servers hangs (Killing Floor under the Linux engine:
 * "Thread synchronization object is unuseable", then no frame).
 *
 * State. Every guest process is its own host process, so the sets live in "sem.shm" of the family
 * directory, mapped MAP_SHARED by every process that uses them, like ntsync.shm: the set table, a pool of
 * semaphores, the undo records (SEM_UNDO) with a pool of adjustments, and the queued operations. One spin
 * lock (holder's host pid, taken over from a dead holder, futex.c) guards it all. Zero-filled memory is
 * an empty table. Guest memory is only copied with the lock released.
 *
 * Waits follow the kernel. An operation that cannot complete queues itself, oldest first, and sleeps on a
 * shared sleeper of futex.c, which signals to its task ring. Whoever changes a semaphore (semop, SETVAL,
 * SETALL, the undo of an exiting process) then performs the queued operations that can now complete, in
 * queue order, writes their result and wakes them (update_queue), so a waiter never loses its turn to a
 * newcomer. IPC_RMID wakes every waiter with EIDRM. A waiter whose host process died is dropped instead
 * of being served. semop is never restarted after a signal handler (signal(7)): an interrupted wait
 * returns EINTR, a timed-out one EAGAIN.
 *
 * Undo. An adjustment record belongs to a guest process (all its threads, CLONE_SYSVSEM) and a set. The
 * exit of the process applies it, capping each value to [0, SEMVMX] as Linux does (sig_sem_proc_exit). A
 * fork child starts without records; execve keeps them. A host process killed outright (SIGKILL, a crash)
 * runs no exit hook: its records are applied the same way once its death is seen, when an operation on
 * the set would block, when a waiter of the set looks again (one of them every SEM_DEAD_CHECK_NS) or when
 * the record pool needs room. A host process that exited but was not waited for yet counts as dead.
 *
 * Limits. Linux's per-set and per-call limits (SEMMSL 32000, SEMOPM 500, SEMVMX 32767). The family holds
 * at most SEM_SETS sets, SEM_POOL semaphores, SEM_UNDOS undo records and SEM_WAITERS queued operations;
 * IPC_INFO reports the first two. A queued operation keeps SEM_INLINE_OPS operations in its entry and
 * longer ones in a pool of SEM_OPS_POOL. A full waiter table, operation pool or undo table answers ENOMEM,
 * where Linux would allocate. The shared sleepers of futex.c (FX_SHM_SLEEPERS for the family, shared with
 * futex and ntsync waits) bound the simultaneous waits too.
 */
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sig_priv.h"

#define LX_IPC_PRIVATE 0
#define LX_IPC_CREAT 01000
#define LX_IPC_EXCL 02000
#define LX_IPC_NOWAIT 04000
#define LX_IPC_RMID 0
#define LX_IPC_SET 1
#define LX_IPC_STAT 2
#define LX_IPC_INFO 3
#define LX_GETPID 11
#define LX_GETVAL 12
#define LX_GETALL 13
#define LX_GETNCNT 14
#define LX_GETZCNT 15
#define LX_SETVAL 16
#define LX_SETALL 17
#define LX_SEM_STAT 18
#define LX_SEM_INFO 19
#define LX_SEM_STAT_ANY 20
#define LX_SEM_UNDO 0x1000
#define CAP_IPC_OWNER 15
#define CAP_SYS_ADMIN 21

#define SEMMSL 32000
#define SEMOPM 500
#define SEMVMX 32767
#define SEMAEM SEMVMX
#define SEMUSZ 20
#define SEM_SETS 1024
#define SEM_POOL 32768
#define SEM_UNDOS 1024
#define SEM_ADJ_POOL 65536
#define SEM_WAITERS 4096
#define SEM_INLINE_OPS 8           /* operations kept in a queued entry; longer ones go to the pool */
#define SEM_OPS_POOL 16384
#define SEM_DEAD_CHECK_NS 250000000ll /* how often a waiter looks for dead undo holders of its set */
#define SEQ_MULT 32768 /* IPCMNI: id = slot + seq * 32768 */
#define SEQ_MAX 65536

struct lx_sembuf { uint16_t sem_num; int16_t sem_op, sem_flg; };
struct lx_ipc64_perm {
    int32_t key;
    uint32_t uid, gid, cuid, cgid;
    uint32_t mode;
    uint16_t seq, pad2;
    uint64_t unused1, unused2;
};
struct lx_semid64_ds { /* arch/x86/include/uapi/asm/sembuf.h, x86-64 */
    struct lx_ipc64_perm sem_perm;
    int64_t sem_otime;
    uint64_t unused1;
    int64_t sem_ctime;
    uint64_t unused2;
    uint64_t sem_nsems, unused3, unused4;
};
struct lx_seminfo { int32_t semmap, semmni, semmns, semmnu, semmsl, semopm, semume, semusz, semvmx, semaem; };
_Static_assert(sizeof(struct lx_sembuf) == 6, "sembuf");
_Static_assert(sizeof(struct lx_ipc64_perm) == 48, "ipc64_perm");
_Static_assert(sizeof(struct lx_semid64_ds) == 104, "semid64_ds");
_Static_assert(sizeof(struct lx_seminfo) == 40, "seminfo");

struct sem_set {
    int32_t used, key, seq, nsems;
    uint32_t mode, uid, gid, cuid, cgid;
    uint32_t base;             /* first semaphore in the pool */
    int64_t otime, ctime;
    int64_t last_reap_ns;      /* when a waiter last looked for dead undo holders (wait_queued) */
    uint32_t head, tail;       /* queued operations, oldest first (waiter index + 1) */
};
struct sem_val { int32_t val, pid; };
struct sem_undo {
    uint32_t used, set, nsems, adj; /* set: slot; adj: first adjustment in the pool */
    int32_t seq, pid, host_pid, _pad;
};
struct sem_waiter {
    uint32_t used, set, next, prev;
    int32_t status;            /* 1 queued, else the result handed over (0, -LX_EAGAIN, -LX_ERANGE, -LX_EIDRM) */
    uint32_t sleeper;          /* shared sleeper index of futex.c */
    uint32_t undo, nsops;      /* undo: record index + 1, 0 none */
    int32_t pid, host_pid;     /* guest process (sempid) and host process of the waiter */
    uint32_t blocking;         /* the operation it waits for (GETNCNT, GETZCNT) */
    uint32_t ops;              /* nsops > SEM_INLINE_OPS: first entry in the operation pool */
    struct lx_sembuf sops[SEM_INLINE_OPS];
};
_Static_assert(sizeof(struct sem_waiter) == 96, "sem_waiter");
struct sem_shm {
    _Atomic uint32_t lock;
    int32_t next_seq, used_sems;
    uint32_t whint;            /* where waiter_alloc starts looking */
    uint64_t pool_map[SEM_POOL / 64];
    uint64_t adj_map[SEM_ADJ_POOL / 64];
    uint64_t ops_map[SEM_OPS_POOL / 64];
    struct sem_set set[SEM_SETS];
    struct sem_val val[SEM_POOL];
    int16_t adj[SEM_ADJ_POOL];
    struct lx_sembuf ops[SEM_OPS_POOL];
    struct sem_undo undo[SEM_UNDOS];
    struct sem_waiter w[SEM_WAITERS];
};

static struct sem_shm *g_sem;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- the shared file ---------------------------------------------------------------------------- */

static int sem_path(char *path, size_t n)
{
    const char *d = lkd_famsvc_dir();
    return d && snprintf(path, n, "%s/sem.shm", d) < (int)n ? 0 : -1;
}

/* create: make the file when it is missing (a syscall), else map it only if a process already made it
 * (the exit of a process that never used semaphores). */
static struct sem_shm *sem_map(int create)
{
    struct sem_shm *m = __atomic_load_n(&g_sem, __ATOMIC_ACQUIRE);
    if (m) return m;
    char path[1024];
    if (sem_path(path, sizeof path) < 0) return NULL;
    pthread_mutex_lock(&g_map_lock);
    if (!g_sem) {
        int fd = open(path, O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0), 0600);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && (st.st_size >= (off_t)sizeof(struct sem_shm) ||
                                        ftruncate(fd, (off_t)sizeof(struct sem_shm)) == 0)) {
                void *p = mmap(NULL, sizeof(struct sem_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (p != MAP_FAILED) __atomic_store_n(&g_sem, (struct sem_shm *)p, __ATOMIC_RELEASE);
            }
            close(fd);
        }
    }
    pthread_mutex_unlock(&g_map_lock);
    return g_sem;
}

static void lock(void) { sig_spin_lock(&g_sem->lock); }
static void unlock(void) { sig_spin_unlock(&g_sem->lock); }

/* ---- helpers ------------------------------------------------------------------------------------ */

static int32_t guest_pid(lkd_task *t) { return t && t->proc ? t->proc->pid : 0; }
static uint32_t euid(lkd_task *t) { return t && t->proc ? t->proc->cred.euid : 0; }
static uint32_t egid(lkd_task *t) { return t && t->proc ? t->proc->cred.egid : 0; }
static int capable(lkd_task *t, int cap) { return t && t->proc && ((t->proc->cred.cap_eff >> cap) & 1); }
static int64_t now_s(void) { return lkd_guest_clock_ns(LX_CLOCK_REALTIME) / 1000000000ll; }

/* The host process is gone or a zombie. kill(pid, 0) still succeeds on a zombie, and the parent that would
 * wait for it may be the process blocked on its semaphore. proc_pidinfo answers ESRCH for a zombie too,
 * in 0.6 us where sysctl(KERN_PROC_PID) takes 11 us (MEASURED, macOS 27). */
static int host_dead(int32_t pid)
{
    if (pid <= 0) return 0;
    struct proc_bsdshortinfo si;
    if (proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &si, sizeof si) == (int)sizeof si) return 0;
    if (errno == ESRCH) return 1;
    return kill(pid, 0) < 0 && errno == ESRCH;
}

/* host_dead for the host process of a record or a waiter met in a walk: not this process, nor *alive,
 * the last one the walk saw alive. */
static int dead_other(int32_t pid, int32_t *alive)
{
    if (pid == (int32_t)getpid() || pid == *alive) return 0;
    if (host_dead(pid)) return 1;
    *alive = pid;
    return 0;
}

static int in_group(lkd_task *t, uint32_t g)
{
    if (!t || !t->proc) return 0;
    if (t->proc->cred.egid == g) return 1;
    for (uint32_t i = 0; i < t->proc->cred.ngroups && i < LKD_MAX_GROUPS; i++)
        if (t->proc->cred.groups[i] == g) return 1;
    return 0;
}

/* ipcperms (ipc/util.c): -1 when a requested rwx bit is not granted */
static int ipcperms(lkd_task *t, const struct sem_set *s, int flag)
{
    uint32_t uid = euid(t);
    int requested = (flag >> 6) | (flag >> 3) | flag;
    int granted = (int)s->mode;
    if (uid == s->cuid || uid == s->uid) granted >>= 6;
    else if (in_group(t, s->cgid) || in_group(t, s->gid)) granted >>= 3;
    if ((requested & ~granted & 0007) && !capable(t, CAP_IPC_OWNER)) return -1;
    return 0;
}

/* ipcctl_obtain_check (ipc/util.c): IPC_SET and IPC_RMID belong to the creator, the owner or an admin */
static int owner_or_admin(lkd_task *t, const struct sem_set *s)
{
    uint32_t uid = euid(t);
    return uid == s->uid || uid == s->cuid || capable(t, CAP_SYS_ADMIN);
}

/* The set in the slot of id >= 0, whatever its sequence: ipc_obtain_object_idr keeps the low bits
 * (ipcid_to_idx). */
static struct sem_set *slot_of(int id)
{
    int slot = id % SEQ_MULT;
    return slot < SEM_SETS && g_sem->set[slot].used ? &g_sem->set[slot] : NULL;
}
static struct sem_set *by_id(int id)
{
    struct sem_set *s = slot_of(id);
    return s && s->seq == id / SEQ_MULT ? s : NULL;
}
static int set_id(const struct sem_set *s) { return (int)(s - g_sem->set) + s->seq * SEQ_MULT; }
static uint32_t set_slot(const struct sem_set *s) { return (uint32_t)(s - g_sem->set); }
static struct sem_val *vals(const struct sem_set *s) { return &g_sem->val[s->base]; }

/* Bit maps of the semaphore and adjustment pools: first fit of n consecutive free entries. */
static void bits_set(uint64_t *map, uint32_t i, uint32_t n, int on)
{
    for (; n; n--, i++) {
        if (on) map[i >> 6] |= 1ull << (i & 63);
        else map[i >> 6] &= ~(1ull << (i & 63));
    }
}
static int64_t range_alloc(uint64_t *map, uint32_t size, uint32_t n)
{
    uint32_t run = 0;
    for (uint32_t i = 0; i < size; i++) {
        if ((map[i >> 6] >> (i & 63)) & 1) { run = 0; continue; }
        if (++run == n) {
            uint32_t start = i + 1 - n;
            bits_set(map, start, n, 1);
            return start;
        }
    }
    return -1;
}

/* ---- undo records -------------------------------------------------------------------------------- */

static struct sem_undo *undo_of(uint32_t idx) { return idx ? &g_sem->undo[idx - 1] : NULL; }

static void undo_free(struct sem_undo *u)
{
    bits_set(g_sem->adj_map, u->adj, u->nsems, 0);
    u->used = 0;
}

static int undo_of_set(const struct sem_undo *u, const struct sem_set *s)
{
    return u->used && u->set == set_slot(s) && u->seq == s->seq;
}

static void update_queue(struct sem_set *s);

/* exit_sem (ipc/sem.c): add the adjustments, each value capped to [0, SEMVMX], drop the record, then serve
 * the set's queue. A record's set is live: freeary drops its records first. Lock held. */
static void undo_apply(struct sem_undo *u)
{
    struct sem_set *s = &g_sem->set[u->set];
    for (uint32_t k = 0; k < u->nsems; k++) {
        int16_t a = g_sem->adj[u->adj + k];
        if (!a) continue;
        struct sem_val *v = &vals(s)[k];
        v->val += a;
        if (v->val < 0) v->val = 0;
        if (v->val > SEMVMX) v->val = SEMVMX;
        v->pid = u->pid;
    }
    undo_free(u);
    update_queue(s);
}

/* The records of host processes that died without running their exit (SIGKILL, a crash): apply them as
 * exit_sem would have. s: only the records of that set; NULL: all. Returns how many. Lock held. */
static int undo_reap_dead(const struct sem_set *s)
{
    int32_t alive = 0;
    int n = 0;
    for (uint32_t i = 0; i < SEM_UNDOS; i++) {
        struct sem_undo *u = &g_sem->undo[i];
        if (!u->used || (s && !undo_of_set(u, s)) || !dead_other(u->host_pid, &alive)) continue;
        undo_apply(u);
        n++;
    }
    return n;
}

/* A new record of process pid of this host process for set s. 0 when the table or the pool is full. */
static uint32_t undo_alloc(struct sem_set *s, int32_t pid)
{
    uint32_t i = 0;
    while (i < SEM_UNDOS && g_sem->undo[i].used) i++;
    if (i == SEM_UNDOS) return 0;
    int64_t adj = range_alloc(g_sem->adj_map, SEM_ADJ_POOL, (uint32_t)s->nsems);
    if (adj < 0) return 0;
    g_sem->undo[i] = (struct sem_undo){ .used = 1, .set = set_slot(s), .nsems = (uint32_t)s->nsems,
                                        .adj = (uint32_t)adj, .seq = s->seq, .pid = pid,
                                        .host_pid = (int32_t)getpid() };
    memset(&g_sem->adj[adj], 0, (size_t)s->nsems * sizeof g_sem->adj[0]);
    return i + 1;
}

/* find_alloc_undo (ipc/sem.c): the record of process pid for set s, made on first use. When full, the
 * records of dead host processes make room. 0 when still full. */
static uint32_t undo_find_alloc(struct sem_set *s, int32_t pid)
{
    int32_t host = (int32_t)getpid();
    for (uint32_t i = 0; i < SEM_UNDOS; i++) {
        struct sem_undo *u = &g_sem->undo[i];
        if (undo_of_set(u, s) && u->pid == pid && u->host_pid == host) return i + 1;
    }
    uint32_t i = undo_alloc(s, pid);
    if (!i && undo_reap_dead(NULL)) i = undo_alloc(s, pid);
    return i;
}

/* SETVAL and SETALL clear the adjustments of every process for the semaphores they set. */
static void undo_clear(const struct sem_set *s, int semnum)
{
    for (uint32_t i = 0; i < SEM_UNDOS; i++) {
        struct sem_undo *u = &g_sem->undo[i];
        if (!undo_of_set(u, s)) continue;
        if (semnum >= 0) g_sem->adj[u->adj + (uint32_t)semnum] = 0;
        else memset(&g_sem->adj[u->adj], 0, (size_t)u->nsems * sizeof g_sem->adj[0]);
    }
}

/* ---- operations and the queue -------------------------------------------------------------------- */

/* perform_atomic_semop_slow (ipc/sem.c): apply the operations in order, or none of them. Returns 0 done,
 * 1 would block (*blocking: index of the operation), -LX_EAGAIN (would block with IPC_NOWAIT) or
 * -LX_ERANGE (above SEMVMX, or an adjustment outside [-SEMAEM-1, SEMAEM]). */
static int perform(const struct sem_set *s, const struct lx_sembuf *sops, uint32_t nsops, struct sem_undo *un,
                   int32_t pid, uint32_t *blocking)
{
    struct sem_val *v = vals(s);
    int16_t *adj = un ? &g_sem->adj[un->adj] : NULL;
    uint32_t i;
    int r;
    for (i = 0; i < nsops; i++) {
        const struct lx_sembuf *op = &sops[i];
        int32_t result = v[op->sem_num].val;
        if (!op->sem_op && result) goto would_block;
        result += op->sem_op;
        if (result < 0) goto would_block;
        if (result > SEMVMX) { r = -LX_ERANGE; goto revert; }
        if (op->sem_flg & LX_SEM_UNDO) {
            int u = adj[op->sem_num] - op->sem_op;
            if (u < -SEMAEM - 1 || u > SEMAEM) { r = -LX_ERANGE; goto revert; }
            adj[op->sem_num] = (int16_t)u;
        }
        v[op->sem_num].val = result;
    }
    for (i = 0; i < nsops; i++) v[sops[i].sem_num].pid = pid;
    return 0;
would_block:
    *blocking = i;
    r = (sops[i].sem_flg & LX_IPC_NOWAIT) ? -LX_EAGAIN : 1;
revert:
    while (i-- > 0) {
        v[sops[i].sem_num].val -= sops[i].sem_op;
        if (sops[i].sem_flg & LX_SEM_UNDO) adj[sops[i].sem_num] += sops[i].sem_op;
    }
    return r;
}

static struct sem_waiter *wt(uint32_t i) { return &g_sem->w[i - 1]; }
static struct lx_sembuf *wops(struct sem_waiter *w)
{
    return w->nsops > SEM_INLINE_OPS ? &g_sem->ops[w->ops] : w->sops;
}

static void q_append(struct sem_set *s, uint32_t i)
{
    struct sem_waiter *w = wt(i);
    w->next = 0;
    w->prev = s->tail;
    if (s->tail) wt(s->tail)->next = i;
    else s->head = i;
    s->tail = i;
}

static void q_remove(struct sem_set *s, uint32_t i)
{
    struct sem_waiter *w = wt(i);
    if (w->prev) wt(w->prev)->next = w->next;
    else s->head = w->next;
    if (w->next) wt(w->next)->prev = w->prev;
    else s->tail = w->prev;
    w->next = w->prev = 0;
}

/* A free entry for nsops operations, with room in the operation pool when they do not fit inline. 0 when
 * full. */
static uint32_t waiter_alloc(uint32_t nsops)
{
    for (uint32_t k = 0; k < SEM_WAITERS; k++) {
        uint32_t i = (g_sem->whint + k) % SEM_WAITERS;
        struct sem_waiter *w = &g_sem->w[i];
        if (w->used) continue;
        int64_t ops = 0;
        if (nsops > SEM_INLINE_OPS && (ops = range_alloc(g_sem->ops_map, SEM_OPS_POOL, nsops)) < 0) return 0;
        w->used = 1;
        w->nsops = nsops;
        w->ops = (uint32_t)ops;
        g_sem->whint = (i + 1) % SEM_WAITERS;
        return i + 1;
    }
    return 0;
}

static void waiter_free(struct sem_waiter *w)
{
    if (w->nsops > SEM_INLINE_OPS) bits_set(g_sem->ops_map, w->ops, w->nsops, 0);
    w->used = 0;
}

/* update_queue and do_smart_update (ipc/sem.c): complete the queued operations that can proceed now,
 * oldest first. A completed operation that changed a value can unblock an older one: scan again from
 * the head (check_restart). Lock held. */
static void update_queue(struct sem_set *s)
{
    int32_t alive = 0;
again:
    for (uint32_t i = s->head, next; i; i = next) {
        struct sem_waiter *w = wt(i);
        next = w->next;
        if (dead_other(w->host_pid, &alive)) { q_remove(s, i); waiter_free(w); continue; }
        const struct lx_sembuf *ops = wops(w);
        int r = perform(s, ops, w->nsops, undo_of(w->undo), w->pid, &w->blocking);
        if (r > 0) continue;
        q_remove(s, i);
        w->status = r;
        sig_shared_sleeper_wake(w->sleeper);
        if (r) continue;
        s->otime = now_s();
        for (uint32_t k = 0; k < w->nsops; k++)
            if (ops[k].sem_op) goto again;
    }
}

/* count_semcnt (ipc/sem.c): queued operations blocked on semnum, by decrement or by wait-for-zero */
static int count_semcnt(const struct sem_set *s, int semnum, int zero)
{
    int n = 0;
    for (uint32_t i = s->head; i; i = wt(i)->next) {
        struct sem_waiter *w = wt(i);
        const struct lx_sembuf *op = &wops(w)[w->blocking];
        if (op->sem_num == semnum && (zero ? op->sem_op == 0 : op->sem_op < 0)) n++;
    }
    return n;
}

/* freeary (ipc/sem.c): wake every waiter with EIDRM, drop the undo records, free the set. Lock held. */
static void freeary(struct sem_set *s)
{
    for (uint32_t i = 0; i < SEM_UNDOS; i++)
        if (undo_of_set(&g_sem->undo[i], s)) undo_free(&g_sem->undo[i]);
    int32_t alive = 0;
    for (uint32_t i = s->head, next; i; i = next) {
        struct sem_waiter *w = wt(i);
        next = w->next;
        w->next = w->prev = 0;
        if (dead_other(w->host_pid, &alive)) { waiter_free(w); continue; }
        w->status = -LX_EIDRM;
        sig_shared_sleeper_wake(w->sleeper);
    }
    s->head = s->tail = 0;
    bits_set(g_sem->pool_map, s->base, (uint32_t)s->nsems, 0);
    g_sem->used_sems -= s->nsems;
    s->used = 0;
}

/* ---- syscalls ------------------------------------------------------------------------------------ */

LKD_SYSCALL(semget)
{
    int32_t key = (int32_t)LKD_ARGI(0);
    int nsems = LKD_ARGI(1), flg = LKD_ARGI(2);
    if (nsems < 0 || nsems > SEMMSL) return -LX_EINVAL;
    if (!sem_map(1)) return -LX_ENOMEM;
    lock();
    struct sem_set *s = NULL;
    if (key != LX_IPC_PRIVATE)
        for (int i = 0; i < SEM_SETS; i++)
            if (g_sem->set[i].used && g_sem->set[i].key == key) { s = &g_sem->set[i]; break; }
    int64_t r;
    if (s) { /* ipcget_public: EEXIST, then sem_more_checks, then the permissions */
        if ((flg & LX_IPC_CREAT) && (flg & LX_IPC_EXCL)) r = -LX_EEXIST;
        else if (nsems > s->nsems) r = -LX_EINVAL;
        else if (ipcperms(t, s, flg)) r = -LX_EACCES;
        else r = set_id(s);
        unlock();
        return r;
    }
    if (key != LX_IPC_PRIVATE && !(flg & LX_IPC_CREAT)) { unlock(); return -LX_ENOENT; }
    /* newary */
    if (!nsems) { unlock(); return -LX_EINVAL; }
    if (g_sem->used_sems + nsems > SEM_POOL) { unlock(); return -LX_ENOSPC; }
    for (int i = 0; i < SEM_SETS && !s; i++)
        if (!g_sem->set[i].used) s = &g_sem->set[i];
    int64_t base = s ? range_alloc(g_sem->pool_map, SEM_POOL, (uint32_t)nsems) : -1;
    if (base < 0) { unlock(); return -LX_ENOSPC; }
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->key = key;
    s->seq = g_sem->next_seq;
    g_sem->next_seq = (g_sem->next_seq + 1) % SEQ_MAX;
    s->nsems = nsems;
    s->base = (uint32_t)base;
    s->mode = (uint32_t)flg & 0777;
    s->uid = s->cuid = euid(t);
    s->gid = s->cgid = egid(t);
    s->ctime = now_s();
    memset(vals(s), 0, (size_t)nsems * sizeof(struct sem_val));
    g_sem->used_sems += nsems;
    r = set_id(s);
    unlock();
    return r;
}

/* Sleep until the queued operation wi is served, its set goes, a signal or the deadline. Nobody wakes a
 * waiter when a holder dies without its exit, so the waiters of a set apply the undo records of its dead
 * holders themselves, one of them every SEM_DEAD_CHECK_NS. Returns the result of the operation. */
static int64_t wait_queued(lkd_task *t, uint32_t wi, struct fx_sleeper *sl, int64_t deadline)
{
    struct sem_waiter *w = wt(wi);
    struct sem_set *s = &g_sem->set[w->set];
    int64_t r;
    for (;;) {
        int64_t until = lkd_host_mono_ns() + SEM_DEAD_CHECK_NS;
        if (deadline >= 0 && deadline < until) until = deadline;
        int sr = sig_shared_sleep(t, sl, until);
        lock();
        int64_t now = lkd_host_mono_ns();
        int early = sr == 0 && (deadline < 0 || now < deadline);
        if (early && w->status == 1 && now - s->last_reap_ns >= SEM_DEAD_CHECK_NS) {
            s->last_reap_ns = now;
            undo_reap_dead(s);
        }
        if (w->status != 1) { /* served, or the set was removed */
            r = w->status;
            break;
        }
        if (sr < 0 || (sr == 0 && !early)) {
            r = sr < 0 ? -LX_EINTR : -LX_EAGAIN;
            q_remove(s, wi); /* still queued: its set still exists */
            break;
        }
        /* Woken with the status still 1: a stray wake (a late waker of the sleeper's previous owner).
         * Every real wake comes under the lock after the status changes, so none is lost by clearing it
         * and sleeping again. */
        if (sr > 0) sig_shared_sleeper_rearm(sl);
        unlock();
    }
    waiter_free(w);
    unlock();
    return r;
}

/* do_semtimedop (ipc/sem.c). utimeout: a relative struct timespec, 0 to wait without limit. */
static int64_t do_semtimedop(lkd_task *t, int semid, uint64_t usops, uint64_t nsops, uint64_t utimeout)
{
    if (nsops < 1 || semid < 0) return -LX_EINVAL;
    if (nsops > SEMOPM) return -LX_E2BIG;
    struct lx_sembuf sops[SEMOPM];
    if (lkd_user_read(usops, sops, nsops * sizeof sops[0])) return -LX_EFAULT;
    int64_t deadline = -1;
    if (utimeout) {
        int64_t rel;
        int e = sig_read_timeout(utimeout, &rel);
        if (e) return e;
        deadline = lkd_host_mono_ns() + rel;
    }
    uint32_t max = 0;
    int undos = 0, alter = 0;
    for (uint32_t i = 0; i < nsops; i++) {
        if (sops[i].sem_num >= max) max = sops[i].sem_num;
        if (sops[i].sem_flg & LX_SEM_UNDO) undos = 1;
        if (sops[i].sem_op) alter = 1;
    }
    if (!sem_map(1)) return -LX_ENOMEM;
    int32_t pid = guest_pid(t);
    struct fx_sleeper *sl = NULL;
    int reaped = 0;
    int64_t r;
    for (;;) {
        lock();
        struct sem_set *s = by_id(semid);
        uint32_t un = 0;
        if (!s) r = -LX_EINVAL;
        else if (max >= (uint32_t)s->nsems) r = -LX_EFBIG;
        else if (ipcperms(t, s, alter ? 0222 : 0444)) r = -LX_EACCES;
        else if (undos && !(un = undo_find_alloc(s, pid))) r = -LX_ENOMEM;
        else r = 0;
        if (r) { unlock(); break; }
        uint32_t blocking = 0;
        r = perform(s, sops, (uint32_t)nsops, undo_of(un), pid, &blocking);
        /* would block: the holder may be a process that died without its exit (looked for once a call) */
        if ((r > 0 || r == -LX_EAGAIN) && !reaped) {
            reaped = 1;
            if (undo_reap_dead(s)) r = perform(s, sops, (uint32_t)nsops, undo_of(un), pid, &blocking);
        }
        if (r <= 0) {
            if (!r) {
                s->otime = now_s();
                if (alter) update_queue(s);
            }
            unlock();
            break;
        }
        if (deadline >= 0 && lkd_host_mono_ns() >= deadline) { unlock(); r = -LX_EAGAIN; break; }
        if (!sl) {
            /* the sleeper comes from futex.c's table, under its own lock: take one, then try again */
            unlock();
            if (!(sl = sig_shared_sleeper_get(t))) { r = -LX_ENOMEM; break; }
            continue;
        }
        uint32_t wi = waiter_alloc((uint32_t)nsops);
        if (!wi) { unlock(); r = -LX_ENOMEM; break; }
        struct sem_waiter *w = wt(wi);
        w->set = set_slot(s);
        w->status = 1;
        w->sleeper = sig_shared_sleeper_index(sl);
        w->undo = un;
        w->pid = pid;
        w->host_pid = (int32_t)getpid();
        w->blocking = blocking;
        memcpy(wops(w), sops, nsops * sizeof sops[0]);
        q_append(s, wi);
        unlock();
        r = wait_queued(t, wi, sl, deadline);
        break;
    }
    if (sl) sig_shared_sleeper_put(sl);
    return r;
}

LKD_SYSCALL(semop)
{
    return do_semtimedop(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGU(2), 0);
}

LKD_SYSCALL(semtimedop)
{
    return do_semtimedop(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGU(2), LKD_ARGP(3));
}

static int64_t semctl_info(int cmd, uint64_t buf)
{
    struct lx_seminfo in = { 0 };
    in.semmni = SEM_SETS;
    in.semmns = SEM_POOL;
    in.semmsl = SEMMSL;
    in.semopm = SEMOPM;
    in.semvmx = SEMVMX;
    in.semmnu = SEM_POOL;
    in.semmap = SEM_POOL;
    in.semume = SEMOPM;
    int used = 0, max = 0; /* ipc_get_maxidx, 0 without sets */
    lock();
    for (int i = 0; i < SEM_SETS; i++)
        if (g_sem->set[i].used) { used++; max = i; }
    if (cmd == LX_SEM_INFO) {
        in.semusz = used;
        in.semaem = g_sem->used_sems;
    } else {
        in.semusz = SEMUSZ;
        in.semaem = SEMAEM;
    }
    unlock();
    if (lkd_user_write(buf, &in, sizeof in)) return -LX_EFAULT;
    return max;
}

static int64_t semctl_stat(lkd_task *t, int semid, int cmd, uint64_t buf)
{
    struct lx_semid64_ds d;
    memset(&d, 0, sizeof d);
    int64_t r;
    lock();
    struct sem_set *s = cmd == LX_IPC_STAT ? by_id(semid) : slot_of(semid); /* SEM_STAT*: an index */
    if (!s) r = -LX_EINVAL;
    else if (cmd != LX_SEM_STAT_ANY && ipcperms(t, s, 0444)) r = -LX_EACCES;
    else {
        d.sem_perm.key = s->key;
        d.sem_perm.uid = s->uid;
        d.sem_perm.gid = s->gid;
        d.sem_perm.cuid = s->cuid;
        d.sem_perm.cgid = s->cgid;
        d.sem_perm.mode = s->mode;
        d.sem_perm.seq = (uint16_t)s->seq;
        d.sem_otime = s->otime;
        d.sem_ctime = s->ctime;
        d.sem_nsems = (uint64_t)s->nsems;
        r = cmd == LX_IPC_STAT ? 0 : set_id(s);
    }
    unlock();
    if (r < 0) return r;
    if (lkd_user_write(buf, &d, sizeof d)) return -LX_EFAULT;
    return r;
}

/* SETVAL and SETALL: n values from first, then the adjustments of every process for them are cleared and
 * the queue served. Lock held. */
static void set_vals(struct sem_set *s, int first, int n, const uint16_t *src, int32_t pid)
{
    for (int i = 0; i < n; i++) vals(s)[first + i] = (struct sem_val){ .val = src[i], .pid = pid };
    undo_clear(s, n == 1 ? first : -1);
    s->ctime = now_s();
    update_queue(s);
}

/* GETALL and SETALL (semctl_main, ipc/sem.c). The buffer is allocated with the lock released; a set
 * removed meanwhile is EIDRM, as in Linux. */
static int64_t semctl_all(lkd_task *t, int semid, int cmd, uint64_t arg)
{
    int64_t r = 0;
    int n = 0;
    lock();
    struct sem_set *s = by_id(semid);
    if (!s) r = -LX_EINVAL;
    else if (ipcperms(t, s, cmd == LX_SETALL ? 0222 : 0444)) r = -LX_EACCES;
    else n = s->nsems;
    unlock();
    if (r) return r;
    size_t size = (size_t)n * sizeof(uint16_t);
    uint16_t *io = malloc(size);
    if (!io) return -LX_ENOMEM;
    if (cmd == LX_SETALL) {
        if (lkd_user_read(arg, io, size)) r = -LX_EFAULT;
        for (int i = 0; !r && i < n; i++)
            if (io[i] > SEMVMX) r = -LX_ERANGE;
    }
    if (!r) {
        lock();
        if (!(s = by_id(semid))) r = -LX_EIDRM; /* the same id is the same set, with the same n */
        else if (cmd == LX_SETALL) set_vals(s, 0, n, io, guest_pid(t));
        else for (int i = 0; i < n; i++) io[i] = (uint16_t)vals(s)[i].val;
        unlock();
    }
    if (!r && cmd == LX_GETALL && lkd_user_write(arg, io, size)) r = -LX_EFAULT;
    free(io);
    return r;
}

/* semctl_main (ipc/sem.c): GETVAL, GETPID, GETNCNT and GETZCNT read one semaphore */
static int64_t semctl_main(lkd_task *t, int semid, int semnum, int cmd)
{
    int64_t r = 0;
    lock();
    struct sem_set *s = by_id(semid);
    if (!s) r = -LX_EINVAL;
    else if (ipcperms(t, s, 0444)) r = -LX_EACCES;
    else if (semnum < 0 || semnum >= s->nsems) r = -LX_EINVAL;
    else switch (cmd) {
    case LX_GETVAL: r = vals(s)[semnum].val; break;
    case LX_GETPID: r = vals(s)[semnum].pid; break;
    case LX_GETNCNT: r = count_semcnt(s, semnum, 0); break;
    case LX_GETZCNT: r = count_semcnt(s, semnum, 1); break;
    }
    unlock();
    return r;
}

static int64_t semctl_setval(lkd_task *t, int semid, int semnum, int val)
{
    if (val > SEMVMX || val < 0) return -LX_ERANGE;
    lock();
    struct sem_set *s = by_id(semid);
    int64_t r = 0;
    if (!s) r = -LX_EINVAL;
    else if (semnum < 0 || semnum >= s->nsems) r = -LX_EINVAL;
    else if (ipcperms(t, s, 0222)) r = -LX_EACCES;
    else set_vals(s, semnum, 1, &(uint16_t){ (uint16_t)val }, guest_pid(t));
    unlock();
    return r;
}

LKD_SYSCALL(semctl)
{
    int semid = LKD_ARGI(0), semnum = LKD_ARGI(1), cmd = LKD_ARGI(2);
    uint64_t arg = LKD_ARGU(3); /* union semun, by value */
    if (semid < 0) return -LX_EINVAL;
    if (!sem_map(1)) return -LX_ENOMEM;
    switch (cmd) {
    case LX_IPC_INFO:
    case LX_SEM_INFO:
        return semctl_info(cmd, arg);
    case LX_IPC_STAT:
    case LX_SEM_STAT:
    case LX_SEM_STAT_ANY:
        return semctl_stat(t, semid, cmd, arg);
    case LX_GETALL:
    case LX_SETALL:
        return semctl_all(t, semid, cmd, arg);
    case LX_GETVAL:
    case LX_GETPID:
    case LX_GETNCNT:
    case LX_GETZCNT:
        return semctl_main(t, semid, semnum, cmd);
    case LX_SETVAL:
        return semctl_setval(t, semid, semnum, (int)(int32_t)arg); /* little-endian: the low half */
    case LX_IPC_SET:
    case LX_IPC_RMID: {
        struct lx_semid64_ds d;
        if (cmd == LX_IPC_SET && lkd_user_read(arg, &d, sizeof d)) return -LX_EFAULT;
        int64_t r = 0;
        lock();
        struct sem_set *s = by_id(semid);
        if (!s) r = -LX_EINVAL;
        else if (!owner_or_admin(t, s)) r = -LX_EPERM;
        else if (cmd == LX_IPC_RMID) freeary(s);
        else if (d.sem_perm.uid == UINT32_MAX || d.sem_perm.gid == UINT32_MAX) r = -LX_EINVAL; /* !uid_valid */
        else { /* ipc_update_perm */
            s->uid = d.sem_perm.uid;
            s->gid = d.sem_perm.gid;
            s->mode = (s->mode & ~0777u) | (d.sem_perm.mode & 0777u);
            s->ctime = now_s();
        }
        unlock();
        return r;
    }
    }
    return -LX_EINVAL;
}

/* exit_sem (ipc/sem.c): the guest process of t exits; apply and drop its adjustments. */
void sig_sem_proc_exit(lkd_task *t)
{
    if (!t || !t->proc || !sem_map(0)) return;
    int32_t pid = t->proc->pid, host = (int32_t)getpid();
    lock();
    for (uint32_t i = 0; i < SEM_UNDOS; i++) {
        struct sem_undo *u = &g_sem->undo[i];
        if (u->used && u->pid == pid && u->host_pid == host) undo_apply(u);
    }
    unlock();
}

/* In a fork child: the map lock may have been held by a thread that does not exist here. */
void sig_sem_fork_child(void) { lkd_fork_reinit_mutex(&g_map_lock, 0); }

void sig_sem_register(void)
{
    LKD_REGISTER(semget);
    LKD_REGISTER(semop);
    LKD_REGISTER(semtimedop);
    LKD_REGISTER(semctl);
}
