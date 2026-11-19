/* ntsync.c: /dev/ntsync. Models drivers/misc/ntsync.c (Linux 6.14) and include/uapi/linux/ntsync.h. Owner: sig.
 *
 * Wine 11 (dlls/ntdll/unix/sync.c, server/inproc_sync.c) keeps NT events, mutexes and semaphores in this
 * device when it can open it: wineserver creates the objects and hands each process a descriptor of the
 * objects it uses, and waits and signals then stay in the process instead of being wineserver round trips
 * (about 12 us each under LKD; Rocket League makes tens of thousands a second).
 *
 * State. Every guest process is its own host process, so the device and its objects live in "ntsync.shm" of
 * the family directory, mapped MAP_SHARED by every process that uses them. One spin lock (holder's host pid,
 * taken over from a dead holder, futex.c) guards it all: the role of the kernel's object locks and
 * wait_all_lock together. Zero-filled memory is an empty table.
 *
 * Identity. A guest descriptor of a device or an object is an fd object whose private part names a slot and
 * its generation. Its host side is an anchor: the read end of a host pipe whose write end is closed; its
 * inode is unique while it is open. The slot records the inode and a hash finds the slot again from it, so
 * SCM_RIGHTS carries the anchor and the receiver gets the same object (lkd_ntsync_import). macOS gives the inode
 * to another pipe once the anchor is closed everywhere: a new anchor takes it from a slot that outlived its own
 * (slot_retire).
 *
 * Lifetime. A slot counts the fd objects that refer to it in every process (created, received), plus one per
 * object of a device and one per queued wait. The last release frees it. A host process does not close its
 * guest descriptors when it exits (the host kernel closes its fds), so the exit of its last guest process
 * releases what it still holds (sig_ntsync_host_exit); only a process killed outright leaks its references, until
 * a new anchor gets the inode of the slot's.
 * A fork child's copies are not counted and do not release: the child execs or exits, as Wine's do; a child
 * that keeps using an object after its parent closed it finds it gone (-EINVAL). An anchor received through
 * SCM_RIGHTS must also look like one (no writer, empty), so that an ordinary pipe that happens to get the
 * inode of a leaked anchor is never taken for an object.
 *
 * Waits follow the kernel. A wait first tries to acquire under the lock; otherwise it queues a waiter and
 * sleeps on a shared sleeper of futex.c, which signals to its task ring. Whoever changes an object then
 * acquires it for the waiters it satisfies, oldest first, WAIT_ALL waiters before WAIT_ANY ones
 * (try_wake_all_obj, try_wake_any_obj), writes their result and wakes them: pulsed and auto-reset events and
 * mutexes are handed over exactly. A waiter whose process died is dropped instead of being handed objects.
 */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ntsync.h"
#include "sig_priv.h"

#define NT_OBJS 65536u
#define NT_HASH 131072u /* power of two, twice the slots: linear probing never fills it */
#define NT_WAITERS 2048u
#define NT_MAX_WAIT 64u
#define NT_WAIT_REALTIME 0x1u

/* include/uapi/linux/ntsync.h: _IOW / _IOR / _IOWR('N', nr, type), asm-generic/ioctl.h encoding */
#define NT_IOC(dir, nr, size) (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)'N' << 8) | (uint32_t)(nr))
#define NT_W 1u
#define NT_R 2u
#define NTSYNC_IOC_CREATE_SEM   NT_IOC(NT_W, 0x80, 8)
#define NTSYNC_IOC_SEM_RELEASE  NT_IOC(NT_W | NT_R, 0x81, 4)
#define NTSYNC_IOC_WAIT_ANY     NT_IOC(NT_W | NT_R, 0x82, 40)
#define NTSYNC_IOC_WAIT_ALL     NT_IOC(NT_W | NT_R, 0x83, 40)
#define NTSYNC_IOC_CREATE_MUTEX NT_IOC(NT_W, 0x84, 8)
#define NTSYNC_IOC_MUTEX_UNLOCK NT_IOC(NT_W | NT_R, 0x85, 8)
#define NTSYNC_IOC_MUTEX_KILL   NT_IOC(NT_W, 0x86, 4)
#define NTSYNC_IOC_CREATE_EVENT NT_IOC(NT_W, 0x87, 8)
#define NTSYNC_IOC_EVENT_SET    NT_IOC(NT_R, 0x88, 4)
#define NTSYNC_IOC_EVENT_RESET  NT_IOC(NT_R, 0x89, 4)
#define NTSYNC_IOC_EVENT_PULSE  NT_IOC(NT_R, 0x8a, 4)
#define NTSYNC_IOC_SEM_READ     NT_IOC(NT_R, 0x8b, 8)
#define NTSYNC_IOC_MUTEX_READ   NT_IOC(NT_R, 0x8c, 8)
#define NTSYNC_IOC_EVENT_READ   NT_IOC(NT_R, 0x8d, 8)

struct nt_wait_args { uint64_t timeout, objs; uint32_t count, index, flags, owner, alert, pad; };
_Static_assert(sizeof(struct nt_wait_args) == 40, "struct ntsync_wait_args");

enum { NT_FREE = 0, NT_DEV, NT_SEM, NT_MUTEX, NT_EVENT };

struct nt_obj {
    uint32_t type, gen, refs, dev; /* dev: slot of the object's device (1-based), 0 for a device */
    uint64_t ino;                  /* anchor identity */
    uint32_t a, b, c;              /* sem: count, max; mutex: owner, count, abandoned; event: manual, signaled */
    uint32_t nwait;                /* queued waits that name it (objects and alerts) */
    uint32_t next_free, _pad;
};

struct nt_waiter {
    uint32_t used, all, count, alert; /* alert: slot (1-based) or 0 */
    uint32_t owner, sleeper;          /* sleeper: shared sleeper index of futex.c */
    uint32_t done, index;             /* done: a waker acquired for it; index: its result */
    int32_t err, pid;                 /* err: 0 or -LX_EOWNERDEAD; pid: host pid of the waiting process */
    uint32_t obj[NT_MAX_WAIT];        /* slots (1-based) */
};

struct nt_hent { uint64_t ino; uint32_t slot, _pad; };

struct nt_shm {
    _Atomic uint32_t lock;
    uint32_t high, free_head, nactive, whint, _pad;
    uint32_t active[NT_WAITERS];      /* queued waiters (index + 1), oldest first */
    struct nt_hent hash[NT_HASH];
    struct nt_obj o[NT_OBJS];
    struct nt_waiter w[NT_WAITERS];
};

struct nt_file { uint32_t slot, gen; pid_t pid; int anchor; struct nt_file *prev, *next; };

static struct nt_shm *g_nt;
/* The descriptors of this host process, released at its exit (sig_ntsync_host_exit). */
static pthread_mutex_t g_files_lock = PTHREAD_MUTEX_INITIALIZER;
static struct nt_file *g_files;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
static const lkd_fd_ops g_dev_ops, g_obj_ops;

/* ---- the shared file ---------------------------------------------------------------------------- */

static struct nt_shm *nt_map(void)
{
    struct nt_shm *m = __atomic_load_n(&g_nt, __ATOMIC_ACQUIRE);
    if (m) return m;
    pthread_mutex_lock(&g_map_lock);
    if (!g_nt) {
        const char *d = lkd_famsvc_dir();
        char path[1024];
        if (d && snprintf(path, sizeof path, "%s/ntsync.shm", d) < (int)sizeof path) {
            int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0 && (st.st_size >= (off_t)sizeof(struct nt_shm) ||
                                            ftruncate(fd, (off_t)sizeof(struct nt_shm)) == 0)) {
                    void *p = mmap(NULL, sizeof(struct nt_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    if (p != MAP_FAILED) __atomic_store_n(&g_nt, (struct nt_shm *)p, __ATOMIC_RELEASE);
                }
                close(fd);
            }
        }
    }
    pthread_mutex_unlock(&g_map_lock);
    return g_nt;
}

int lkd_ntsync_available(void)
{
    const char *e = getenv("LKD_NTSYNC");
    if (e && !strcmp(e, "0")) return 0;
    return lkd_famsvc_dir() != NULL;
}

static void lock(void) { sig_spin_lock(&g_nt->lock); }
static void unlock(void) { sig_spin_unlock(&g_nt->lock); }
static struct nt_obj *obj(uint32_t slot) { return &g_nt->o[slot - 1]; }
static struct nt_waiter *waiter(uint32_t i) { return &g_nt->w[i - 1]; }

/* ---- anchors and the inode hash (locked) -------------------------------------------------------- */

static uint32_t hash_home(uint64_t ino) { return (uint32_t)((ino * 0x9e3779b97f4a7c15ull) >> 40) & (NT_HASH - 1); }

static void hash_put(uint64_t ino, uint32_t slot)
{
    for (uint32_t i = hash_home(ino);; i = (i + 1) & (NT_HASH - 1))
        if (!g_nt->hash[i].ino) { g_nt->hash[i].ino = ino; g_nt->hash[i].slot = slot; return; }
}

static uint32_t hash_get(uint64_t ino)
{
    for (uint32_t i = hash_home(ino);; i = (i + 1) & (NT_HASH - 1)) {
        if (!g_nt->hash[i].ino) return 0;
        if (g_nt->hash[i].ino == ino) return g_nt->hash[i].slot;
    }
}

/* Removes the entry of slot under ino, unless a newer slot took the inode (slot_retire). Linear probing deletion
 * without tombstones: shift back the entries whose home lies before the hole. */
static void hash_del(uint64_t ino, uint32_t slot)
{
    uint32_t i = hash_home(ino);
    while (g_nt->hash[i].ino && (g_nt->hash[i].ino != ino || g_nt->hash[i].slot != slot)) i = (i + 1) & (NT_HASH - 1);
    if (!g_nt->hash[i].ino) return;
    for (uint32_t j = i;;) {
        j = (j + 1) & (NT_HASH - 1);
        if (!g_nt->hash[j].ino) break;
        uint32_t k = hash_home(g_nt->hash[j].ino);
        if (((j - k) & (NT_HASH - 1)) >= ((j - i) & (NT_HASH - 1))) { g_nt->hash[i] = g_nt->hash[j]; i = j; }
    }
    g_nt->hash[i].ino = 0;
    g_nt->hash[i].slot = 0;
}

/* A new anchor: the read end of a pipe whose write end is closed. */
static int anchor_new(uint64_t *ino)
{
    int p[2];
    if (pipe(p) < 0) return (int)LKD_HOST_ERR();
    close(p[1]);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    struct stat st;
    if (fstat(p[0], &st) < 0 || !st.st_ino) { close(p[0]); return -LX_ENFILE; }
    *ino = st.st_ino;
    return p[0];
}

/* ---- slots (locked) ----------------------------------------------------------------------------- */

static void slot_put(uint32_t s);

/* The anchor of a new slot is open, and macOS gives a pipe's inode again once the pipe is closed: a slot that
 * still names this inode lost every descriptor with references left (a process that died without releasing its
 * own, a wait still queued on it). A receiver of the new anchor would find that slot, and wait on an object
 * nobody signals. So the inode goes to the new slot alone, and the old one keeps only what its queued waits hold. */
static void slot_retire(uint64_t ino)
{
    uint32_t s = hash_get(ino);
    if (!s) return;
    struct nt_obj *o = obj(s);
    hash_del(ino, s);
    if (o->type == NT_DEV || o->refs <= o->nwait) return; /* a device goes with its objects */
    o->refs = o->nwait + 1;
    slot_put(s);
}

static uint32_t slot_alloc(uint32_t type, uint64_t ino)
{
    slot_retire(ino);
    uint32_t s = g_nt->free_head;
    if (s) g_nt->free_head = obj(s)->next_free;
    else if (g_nt->high < NT_OBJS) s = ++g_nt->high;
    else return 0;
    struct nt_obj *o = obj(s);
    uint32_t gen = o->gen + 1;
    memset(o, 0, sizeof *o);
    o->type = type;
    o->gen = gen ? gen : 1;
    o->refs = 1;
    o->ino = ino;
    hash_put(ino, s);
    return s;
}

static void slot_put(uint32_t s)
{
    struct nt_obj *o = obj(s);
    if (o->type == NT_FREE || !o->refs || --o->refs) return;
    hash_del(o->ino, s);
    uint32_t dev = o->dev;
    o->type = NT_FREE;
    o->next_free = g_nt->free_head;
    g_nt->free_head = s;
    if (dev) slot_put(dev);
}

/* The live object of slot/gen with the expected type (NT_DEV, an object type, or 0 for any object). */
static struct nt_obj *live(uint32_t s, uint32_t gen, uint32_t type)
{
    if (!s || s > NT_OBJS) return NULL;
    struct nt_obj *o = obj(s);
    if (o->type == NT_FREE || o->gen != gen) return NULL;
    if (type ? o->type != type : o->type == NT_DEV) return NULL;
    return o;
}

/* ---- signaled state and acquisition (locked) ---------------------------------------------------- */

static int is_signaled(const struct nt_obj *o, uint32_t owner)
{
    switch (o->type) {
    case NT_SEM: return o->a > 0;
    case NT_MUTEX: return (!o->a || o->a == owner) && o->b < UINT32_MAX;
    case NT_EVENT: return o->b != 0;
    }
    return 0;
}

static void acquire(struct nt_obj *o, uint32_t owner, int32_t *err)
{
    switch (o->type) {
    case NT_SEM: o->a--; break;
    case NT_MUTEX:
        if (o->c) { *err = -LX_EOWNERDEAD; o->c = 0; }
        o->a = owner;
        o->b++;
        break;
    case NT_EVENT:
        if (!o->a) o->b = 0;
        break;
    }
}

/* ---- waiters (locked) --------------------------------------------------------------------------- */

static uint32_t waiter_alloc(void)
{
    for (uint32_t k = 0; k < NT_WAITERS; k++) {
        uint32_t i = (g_nt->whint + k) % NT_WAITERS;
        if (!g_nt->w[i].used) {
            memset(&g_nt->w[i], 0, sizeof g_nt->w[i]);
            g_nt->w[i].used = 1;
            g_nt->whint = (i + 1) % NT_WAITERS;
            g_nt->active[g_nt->nactive++] = i + 1;
            return i + 1;
        }
    }
    return 0;
}

static void waiter_free(uint32_t wi)
{
    struct nt_waiter *w = waiter(wi);
    for (uint32_t i = 0; i < w->count; i++) { obj(w->obj[i])->nwait--; slot_put(w->obj[i]); }
    if (w->alert) { obj(w->alert)->nwait--; slot_put(w->alert); }
    for (uint32_t k = 0; k < g_nt->nactive; k++)
        if (g_nt->active[k] == wi) {
            memmove(&g_nt->active[k], &g_nt->active[k + 1], (g_nt->nactive - k - 1) * sizeof g_nt->active[0]);
            g_nt->nactive--;
            break;
        }
    w->used = 0;
}

/* A waiter about to be handed objects: still alive? A dead one is dropped (its sleeper is gone with it). */
static int waiter_alive(uint32_t wi)
{
    struct nt_waiter *w = waiter(wi);
    if (w->pid == (int32_t)getpid() || kill((pid_t)w->pid, 0) == 0 || errno != ESRCH) return 1;
    waiter_free(wi);
    return 0;
}

static void finish(struct nt_waiter *w, uint32_t index, int32_t err)
{
    w->done = 1;
    w->index = index;
    w->err = err;
    sig_shared_sleeper_wake(w->sleeper);
}

static int names(const struct nt_waiter *w, uint32_t s)
{
    for (uint32_t i = 0; i < w->count; i++)
        if (w->obj[i] == s) return (int)i;
    return -1;
}

/* try_wake_all: every object signaled for the owner, then all are acquired at once. */
static int try_wake_all(uint32_t wi)
{
    struct nt_waiter *w = waiter(wi);
    for (uint32_t i = 0; i < w->count; i++)
        if (!is_signaled(obj(w->obj[i]), w->owner)) return 0;
    if (!waiter_alive(wi)) return 1;
    int32_t err = 0;
    for (uint32_t i = 0; i < w->count; i++) acquire(obj(w->obj[i]), w->owner, &err);
    finish(w, 0, err);
    return 1;
}

/* Object s changed: hand it to the waiters it satisfies, WAIT_ALL ones first, oldest first. The active list
 * shrinks when a dead waiter is dropped: walk it again from the start then (done waiters are skipped). */
static void wake_obj(uint32_t s)
{
    struct nt_obj *o = obj(s);
    if (!o->nwait) return;
again_all:
    for (uint32_t k = 0; k < g_nt->nactive; k++) {
        uint32_t wi = g_nt->active[k];
        struct nt_waiter *w = waiter(wi);
        if (!w->all || w->done || names(w, s) < 0) continue;
        uint32_t before = g_nt->nactive;
        try_wake_all(wi);
        if (g_nt->nactive != before) goto again_all;
    }
again_any:
    for (uint32_t k = 0; k < g_nt->nactive && o->nwait; k++) {
        uint32_t wi = g_nt->active[k];
        struct nt_waiter *w = waiter(wi);
        if (w->done) continue;
        int pos = w->all ? -1 : names(w, s);
        if (pos < 0 && w->alert == s) pos = (int)w->count;
        if (pos < 0 || !is_signaled(o, w->owner)) continue;
        if (!waiter_alive(wi)) goto again_any;
        int32_t err = 0;
        acquire(o, w->owner, &err);
        finish(w, (uint32_t)pos, err);
    }
}

/* A wait that can be satisfied now (no waiter queued yet): 1 with index and err, else 0. */
static int try_now(int all, const uint32_t *s, uint32_t count, uint32_t alert, uint32_t owner, uint32_t *index,
                   int32_t *err)
{
    if (all) {
        int ok = count > 0;
        for (uint32_t i = 0; ok && i < count; i++) ok = is_signaled(obj(s[i]), owner);
        if (ok) {
            for (uint32_t i = 0; i < count; i++) acquire(obj(s[i]), owner, err);
            *index = 0;
            return 1;
        }
    } else {
        for (uint32_t i = 0; i < count; i++)
            if (is_signaled(obj(s[i]), owner)) { acquire(obj(s[i]), owner, err); *index = i; return 1; }
    }
    if (alert && is_signaled(obj(alert), owner)) { acquire(obj(alert), owner, err); *index = count; return 1; }
    return 0;
}

/* ---- guest descriptors -------------------------------------------------------------------------- */

static lkd_fdobj *file_new(uint32_t slot, uint32_t gen, uint64_t ino, int anchor, int dev)
{
    struct nt_file *f = malloc(sizeof *f);
    if (!f) return NULL;
    f->slot = slot;
    f->gen = gen;
    f->pid = getpid();
    f->anchor = anchor;
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_DEV, dev ? &g_dev_ops : &g_obj_ops, -1, LX_O_RDWR, f);
    if (!o) { free(f); return NULL; }
    pthread_mutex_lock(&g_files_lock);
    f->prev = NULL;
    f->next = g_files;
    if (g_files) g_files->prev = f;
    g_files = f;
    pthread_mutex_unlock(&g_files_lock);
    o->ino = ino;
    return o;
}

/* Slot and generation behind guest fd `fd` when it is an ntsync object (not a device). */
static int fd_object(lkd_task *t, int fd, uint32_t *slot, uint32_t *gen)
{
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    if (!o) return -LX_EINVAL;
    int r = -LX_EINVAL;
    if (o->ops == &g_obj_ops) {
        struct nt_file *f = o->priv;
        *slot = f->slot;
        *gen = f->gen;
        r = 0;
    }
    lkd_fdobj_put(o);
    return r;
}

static void nt_release(lkd_fdobj *o)
{
    struct nt_file *f = o->priv;
    if (!f) return;
    /* Out of the list and out of the slot under the list's lock: the process's exit, which another thread may run
     * meanwhile (sig_ntsync_host_exit), then releases the slot there or leaves it to here, never neither. */
    pthread_mutex_lock(&g_files_lock);
    if (f->prev) f->prev->next = f->next;
    else if (g_files == f) g_files = f->next;
    if (f->next) f->next->prev = f->prev;
    if (f->pid == getpid() && nt_map()) {
        lock();
        if (f->slot && f->slot <= NT_OBJS && obj(f->slot)->gen == f->gen) slot_put(f->slot);
        unlock();
    }
    pthread_mutex_unlock(&g_files_lock);
    close(f->anchor);
    free(f);
    o->priv = NULL;
}

static int nt_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "%s", o->ops == &g_dev_ops ? "/dev/ntsync" : "anon_inode:ntsync");
    return n < 0 ? -LX_EINVAL : ((size_t)n >= cap ? (int)cap : n);
}

static int nt_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    (void)t;
    memset(out, 0, sizeof *out);
    out->st_ino = o->ino;
    out->st_nlink = 1;
    out->st_blksize = 4096;
    if (o->ops == &g_dev_ops) {
        out->st_mode = LX_S_IFCHR | 0666;
        out->st_rdev = (10u << 8) | 126u; /* misc major 10 */
    } else {
        out->st_mode = 0600; /* anon inode: no file type bits */
    }
    return 0;
}

/* ---- device ioctls ------------------------------------------------------------------------------ */

static int64_t create(lkd_task *t, struct nt_file *dev, uint32_t type, uint32_t a, uint32_t b, uint32_t c)
{
    uint64_t ino;
    int anchor = anchor_new(&ino);
    if (anchor < 0) return anchor;
    lock();
    if (!live(dev->slot, dev->gen, NT_DEV)) { unlock(); close(anchor); return -LX_EINVAL; }
    uint32_t s = slot_alloc(type, ino);
    if (!s) { unlock(); close(anchor); return -LX_ENOMEM; }
    struct nt_obj *o = obj(s);
    o->dev = dev->slot;
    obj(dev->slot)->refs++;
    o->a = a;
    o->b = b;
    o->c = c;
    uint32_t gen = o->gen;
    unlock();
    lkd_fdobj *f = file_new(s, gen, ino, anchor, 0);
    if (!f) {
        lock();
        slot_put(s);
        unlock();
        close(anchor);
        return -LX_ENOMEM;
    }
    int fd = lkd_fd_alloc(t, f, 0, 1); /* O_CLOEXEC, as anon_inode_getfd in ntsync_*_create */
    lkd_fdobj_put(f);
    return fd;
}

static int64_t wait_objs(lkd_task *t, struct nt_file *dev, uint64_t arg, int all)
{
    struct nt_wait_args a;
    if (lkd_user_read(arg, &a, sizeof a) < 0) return -LX_EFAULT;
    if (a.count > NT_MAX_WAIT || a.pad || (a.flags & ~NT_WAIT_REALTIME)) return -LX_EINVAL;
    uint32_t fds[NT_MAX_WAIT], s[NT_MAX_WAIT], gen[NT_MAX_WAIT], alert = 0, agen = 0;
    if (a.count && lkd_user_read(a.objs, fds, a.count * sizeof fds[0]) < 0) return -LX_EFAULT;
    for (uint32_t i = 0; i < a.count; i++)
        if (fd_object(t, (int)fds[i], &s[i], &gen[i]) < 0) return -LX_EINVAL;
    if (a.alert && fd_object(t, (int)a.alert, &alert, &agen) < 0) return -LX_EINVAL;

    int64_t deadline = -1;
    if (a.timeout != UINT64_MAX) {
        int64_t now = lkd_guest_clock_ns((a.flags & NT_WAIT_REALTIME) ? LX_CLOCK_REALTIME : LX_CLOCK_MONOTONIC);
        int64_t left = a.timeout > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)a.timeout - now;
        if (left < 0) left = 0;
        deadline = left > INT64_MAX / 2 ? -1 : lkd_host_mono_ns() + left;
    }

    struct fx_sleeper *sl = sig_shared_sleeper_get(t);
    if (!sl) return -LX_ENOMEM;
    lock();
    int64_t ret = 0;
    for (uint32_t i = 0; i < a.count && !ret; i++) {
        struct nt_obj *o = live(s[i], gen[i], 0);
        if (!o || o->dev != dev->slot) ret = -LX_EINVAL;
        for (uint32_t j = 0; all && !ret && j < i; j++)
            if (s[j] == s[i]) ret = -LX_EINVAL; /* WAIT_ALL names each object once */
    }
    if (!ret && alert) {
        struct nt_obj *o = live(alert, agen, NT_EVENT);
        if (!o || o->dev != dev->slot) ret = -LX_EINVAL;
    }
    if (!live(dev->slot, dev->gen, NT_DEV)) ret = -LX_EINVAL;
    if (ret) { unlock(); sig_shared_sleeper_put(sl); return ret; }

    uint32_t index = 0;
    int32_t err = 0;
    if (try_now(all, s, a.count, alert, a.owner, &index, &err)) {
        unlock();
        sig_shared_sleeper_put(sl);
        if (lkd_user_write(arg + offsetof(struct nt_wait_args, index), &index, sizeof index) < 0) return -LX_EFAULT;
        return err;
    }
    if (deadline >= 0 && lkd_host_mono_ns() >= deadline) { unlock(); sig_shared_sleeper_put(sl); return -LX_ETIMEDOUT; }
    uint32_t wi = waiter_alloc();
    if (!wi) { unlock(); sig_shared_sleeper_put(sl); return -LX_ENOMEM; }
    struct nt_waiter *w = waiter(wi);
    w->all = all;
    w->count = a.count;
    w->alert = alert;
    w->owner = a.owner;
    w->pid = (int32_t)getpid();
    w->sleeper = sig_shared_sleeper_index(sl);
    for (uint32_t i = 0; i < a.count; i++) { w->obj[i] = s[i]; obj(s[i])->nwait++; obj(s[i])->refs++; }
    if (alert) { obj(alert)->nwait++; obj(alert)->refs++; }
    unlock();

    int r = sig_shared_sleep(t, sl, deadline);

    lock();
    int done = w->done;
    if (done) { index = w->index; err = w->err; }
    waiter_free(wi);
    unlock();
    sig_shared_sleeper_put(sl);
    if (!done) return r < 0 ? -LX_ERESTARTSYS : -LX_ETIMEDOUT;
    if (lkd_user_write(arg + offsetof(struct nt_wait_args, index), &index, sizeof index) < 0) return -LX_EFAULT;
    return err;
}

static int64_t dev_ioctl(lkd_task *t, lkd_fdobj *fo, uint32_t cmd, uint64_t arg)
{
    struct nt_file *dev = fo->priv;
    uint32_t v[2];
    switch (cmd) {
    case NTSYNC_IOC_CREATE_SEM:
        if (lkd_user_read(arg, v, 8) < 0) return -LX_EFAULT;
        if (v[0] > v[1]) return -LX_EINVAL;
        return create(t, dev, NT_SEM, v[0], v[1], 0);
    case NTSYNC_IOC_CREATE_MUTEX:
        if (lkd_user_read(arg, v, 8) < 0) return -LX_EFAULT;
        if (!v[0] != !v[1]) return -LX_EINVAL;
        return create(t, dev, NT_MUTEX, v[0], v[1], 0);
    case NTSYNC_IOC_CREATE_EVENT:
        if (lkd_user_read(arg, v, 8) < 0) return -LX_EFAULT;
        return create(t, dev, NT_EVENT, !!v[0], !!v[1], 0);
    case NTSYNC_IOC_WAIT_ANY: return wait_objs(t, dev, arg, 0);
    case NTSYNC_IOC_WAIT_ALL: return wait_objs(t, dev, arg, 1);
    }
    return -LX_ENOTTY;
}

/* ---- object ioctls ------------------------------------------------------------------------------ */

static int64_t obj_ioctl(lkd_task *t, lkd_fdobj *fo, uint32_t cmd, uint64_t arg)
{
    (void)t;
    struct nt_file *f = fo->priv;
    uint32_t type = 0, v[2] = { 0, 0 }, out[2];
    int nout = 0;
    switch (cmd) {
    case NTSYNC_IOC_SEM_RELEASE: case NTSYNC_IOC_SEM_READ: type = NT_SEM; break;
    case NTSYNC_IOC_MUTEX_UNLOCK: case NTSYNC_IOC_MUTEX_KILL: case NTSYNC_IOC_MUTEX_READ: type = NT_MUTEX; break;
    case NTSYNC_IOC_EVENT_SET: case NTSYNC_IOC_EVENT_RESET: case NTSYNC_IOC_EVENT_PULSE:
    case NTSYNC_IOC_EVENT_READ: type = NT_EVENT; break;
    default: return -LX_ENOTTY;
    }
    if (cmd == NTSYNC_IOC_SEM_RELEASE || cmd == NTSYNC_IOC_MUTEX_KILL) {
        if (lkd_user_read(arg, v, 4) < 0) return -LX_EFAULT;
    } else if (cmd == NTSYNC_IOC_MUTEX_UNLOCK) {
        if (lkd_user_read(arg, v, 8) < 0) return -LX_EFAULT;
    }
    if ((cmd == NTSYNC_IOC_MUTEX_UNLOCK || cmd == NTSYNC_IOC_MUTEX_KILL) && !v[0]) return -LX_EINVAL; /* owner 0 */
    lock();
    struct nt_obj *o = live(f->slot, f->gen, type);
    int64_t ret = 0;
    if (!o) { unlock(); return -LX_EINVAL; }
    switch (cmd) {
    case NTSYNC_IOC_SEM_RELEASE:
        if (v[0] > o->b - o->a) { ret = -LX_EOVERFLOW; break; }
        out[0] = o->a;
        nout = 1;
        o->a += v[0];
        wake_obj(f->slot);
        break;
    case NTSYNC_IOC_MUTEX_UNLOCK:
        if (o->a != v[0]) { ret = -LX_EPERM; break; }
        out[0] = v[0];
        out[1] = o->b;
        nout = 2;
        if (!--o->b) o->a = 0;
        wake_obj(f->slot);
        break;
    case NTSYNC_IOC_MUTEX_KILL:
        if (o->a != v[0]) { ret = -LX_EPERM; break; }
        o->c = 1;
        o->a = 0;
        o->b = 0;
        wake_obj(f->slot);
        break;
    case NTSYNC_IOC_EVENT_SET:
    case NTSYNC_IOC_EVENT_RESET:
    case NTSYNC_IOC_EVENT_PULSE:
        out[0] = o->b;
        nout = 1;
        o->b = cmd != NTSYNC_IOC_EVENT_RESET;
        if (o->b) wake_obj(f->slot);
        if (cmd == NTSYNC_IOC_EVENT_PULSE) o->b = 0;
        break;
    case NTSYNC_IOC_SEM_READ: out[0] = o->a; out[1] = o->b; nout = 2; break;
    case NTSYNC_IOC_MUTEX_READ:
        out[0] = o->a;
        out[1] = o->b;
        nout = 2;
        if (o->c) ret = -LX_EOWNERDEAD; /* the values are copied out all the same */
        break;
    case NTSYNC_IOC_EVENT_READ: out[0] = o->a; out[1] = o->b; nout = 2; break;
    }
    unlock();
    if (nout && (ret == 0 || ret == -LX_EOWNERDEAD) && lkd_user_write(arg, out, (size_t)nout * 4) < 0) return -LX_EFAULT;
    return ret;
}

static const lkd_fd_ops g_dev_ops = {
    .name = "ntsync", .ioctl = dev_ioctl, .fstat = nt_fstat, .describe = nt_describe, .release = nt_release,
};
static const lkd_fd_ops g_obj_ops = {
    .name = "ntsync-object", .ioctl = obj_ioctl, .fstat = nt_fstat, .describe = nt_describe, .release = nt_release,
};

/* ---- open, SCM_RIGHTS --------------------------------------------------------------------------- */

int lkd_ntsync_open_device(lkd_task *t, uint32_t lx_flags, lkd_fdobj **out)
{
    (void)t;
    if (!lkd_ntsync_available() || !nt_map()) return -LX_ENODEV;
    uint64_t ino;
    int anchor = anchor_new(&ino);
    if (anchor < 0) return anchor;
    lock();
    uint32_t s = slot_alloc(NT_DEV, ino);
    uint32_t gen = s ? obj(s)->gen : 0;
    unlock();
    if (!s) { close(anchor); return -LX_ENOMEM; }
    lkd_fdobj *o = file_new(s, gen, ino, anchor, 1);
    if (!o) {
        lock();
        slot_put(s);
        unlock();
        close(anchor);
        return -LX_ENOMEM;
    }
    atomic_store(&o->status_flags, (lx_flags & (LX_O_ACCMODE | LX_O_NONBLOCK)) | LX_O_LARGEFILE);
    o->gpath = strdup("/dev/ntsync");
    *out = o;
    return 0;
}

int lkd_ntsync_export(lkd_fdobj *o, int *hfds, int cap)
{
    if (o->ops != &g_dev_ops && o->ops != &g_obj_ops) return 0;
    if (cap < 1) return -LX_ETOOMANYREFS;
    struct nt_file *f = o->priv;
    hfds[0] = fcntl(f->anchor, F_DUPFD_CLOEXEC, 0);
    return hfds[0] < 0 ? (int)LKD_HOST_ERR() : 1;
}

int lkd_ntsync_import(int host_fd, lkd_fdobj **out)
{
    struct stat st;
    if (fstat(host_fd, &st) < 0 || !S_ISFIFO(st.st_mode) || !st.st_ino || !nt_map()) return 0;
    struct pollfd p = { .fd = host_fd, .events = POLLIN, .revents = 0 };
    int avail = 0;
    if (poll(&p, 1, 0) != 1 || !(p.revents & POLLHUP) || ioctl(host_fd, FIONREAD, &avail) < 0 || avail) return 0;
    lock();
    uint32_t s = hash_get(st.st_ino);
    struct nt_obj *o = s ? obj(s) : NULL;
    if (!o || o->type == NT_FREE) { unlock(); return 0; }
    o->refs++;
    uint32_t gen = o->gen;
    int dev = o->type == NT_DEV;
    unlock();
    fcntl(host_fd, F_SETFD, FD_CLOEXEC);
    lkd_fdobj *f = file_new(s, gen, st.st_ino, host_fd, dev);
    if (!f) {
        lock();
        if (obj(s)->gen == gen) slot_put(s);
        unlock();
        return 0;
    }
    if (dev) f->gpath = strdup("/dev/ntsync");
    *out = f;
    return 1;
}

/* ---- process life ------------------------------------------------------------------------------- */

/* The last guest process of this host process exits: release the slots its descriptors still hold (the host
 * kernel will close the anchors). */
void sig_ntsync_host_exit(void)
{
    if (!g_nt) return;
    pid_t me = getpid();
    pthread_mutex_lock(&g_files_lock);
    lock();
    for (struct nt_file *f = g_files; f; f = f->next) {
        if (f->pid != me) continue;
        if (f->slot && f->slot <= NT_OBJS && obj(f->slot)->gen == f->gen) slot_put(f->slot);
        f->pid = 0;
    }
    unlock();
    pthread_mutex_unlock(&g_files_lock);
}

/* In a fork child: the list lock may have been held by a thread that does not exist here. */
void sig_ntsync_fork_child(void) { pthread_mutex_init(&g_files_lock, NULL); }
