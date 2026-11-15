/* famsvc.h: the family IPC service. OWNER: sig (implementation in src/sig/famsvc.c).
 *
 * Purpose. Several modules must act on another host process of the same runtime family: sig queues a
 * signal there (Wine's server sends SIGUSR1 to client threads), mm reads and writes guest memory
 * (ptrace PEEK/POKE, /proc/<pid>/mem, process_vm_*), procfs lists descriptors and task state, ptrace
 * posts tracer commands. One service serves them all.
 *
 * Model. Every host process of the family runs one host service thread (not a guest thread: it has no
 * lkd_task, lkd_current() is NULL there, every host signal is blocked in it). It listens on a private
 * AF_UNIX stream socket "<dir>/<host pid>.s" inside a 0700 family directory. Modules register opcodes
 * at init (lkd_famsvc_register); a thread of any process of the family calls an opcode of another
 * process synchronously with a timeout (lkd_famsvc_call). A request and its response each carry a byte
 * payload (at most LKD_FAMSVC_MAX_PAYLOAD) and at most LKD_FAMSVC_MAX_FDS host descriptors
 * (SCM_RIGHTS). The receiver learns the caller's host pid from the kernel (LOCAL_PEERPID) and refuses
 * a peer with another uid (getpeereid).
 *
 * Directory. The environment variable LKD_FAMSVC_DIR names it. The first process of a family creates it
 * (mkdtemp under $TMPDIR, or /tmp when the path would not fit sun_path) and exports the variable; fork
 * children inherit it; a host re-exec of the runtime keeps it when the engine keeps the environment
 * (it must keep LKD_FAMSVC_DIR and LKD_FAMSVC_FD, as it keeps LKD_FAMILY_FD). Tests may set
 * LKD_FAMSVC_DIR to a directory of their own before lkd_init. The directory also holds the family's
 * shared tables, the "*.shm" files (futex.shm, ntsync.shm, sem.shm). The last process that leaves
 * (lkd_famsvc_shutdown with no peer socket left) removes them and the directory.
 *
 * Lifetime. Started by lkd_sig_init (so by lkd_init), restarted in the fork child by lkd_sig_fork_child
 * (new socket for the new host pid; inherited connections are closed), adopted after a host re-exec:
 * the listening socket is deliberately NOT close-on-exec and LKD_FAMSVC_FD="<fd>:<host pid>" names it,
 * so connections that arrive during the exec wait in the backlog and the new image serves them. A
 * process that exits without lkd_famsvc_shutdown leaves a stale socket file that refuses connections:
 * callers then get -LX_ESRCH.
 *
 * Handlers. A handler runs on the service thread, one request at a time per process, with no LKD lock
 * held. It must answer quickly (no blocking wait for a guest thread), must not take a lock that a guest
 * thread may hold across a blocking wait, and must NOT call lkd_famsvc_call itself (two services that
 * call each other would only end at the timeout). It reads req->data/len, may take ownership of a
 * received descriptor by setting req->fds[i] = -1 (the others are closed after the handler returns),
 * and fills resp->status (>= 0 or -LX_E*), resp->data (malloc'ed, freed by famsvc after sending) and
 * resp->fds (closed by famsvc after sending).
 *
 * Opcode numbers are a fixed ABI between the processes of a family (all run the same runtime build).
 */
#ifndef LKD_FAMSVC_H
#define LKD_FAMSVC_H

#include "lkd_internal.h"

enum {
    LKD_FAMSVC_OP_PING = 1,               /* famsvc: echoes the payload, status = receiver host pid */
    /* sig: 16..31 */
    LKD_FAMSVC_OP_QUEUE_SIGNAL = 16,      /* queue a signal in the receiver (kill, tgkill, sigqueue) */
    LKD_FAMSVC_OP_CHILD_EVENT = 17,       /* SIGCHLD to a parent living in the receiver */
    LKD_FAMSVC_OP_SIG_STATUS = 18,        /* lkd_sig_status of a task of the receiver */
    /* mm: 32..47 */
    LKD_FAMSVC_OP_READ_MEM = 32,
    LKD_FAMSVC_OP_WRITE_MEM = 33,
    LKD_FAMSVC_OP_RENDER_MAPS = 34,
    /* procfs and fs: 48..63 */
    LKD_FAMSVC_OP_LIST_FDS = 48,
    LKD_FAMSVC_OP_TASK_STATUS = 49,
    /* ptrace: 64..95 */
    LKD_FAMSVC_OP_PTRACE_FIRST = 64,
    LKD_FAMSVC_OP_PTRACE_LAST = 95,
    /* task: 96..111 */
    LKD_FAMSVC_OP_TASK_FIRST = 96,
    LKD_FAMSVC_OP_TASK_LAST = 111,
    /* tests: 120..127 (unit tests register their own handlers here) */
    LKD_FAMSVC_OP_TEST_FIRST = 120,
    LKD_FAMSVC_OP_MAX = 128
};

#define LKD_FAMSVC_MAX_PAYLOAD (8u << 20)   /* bytes per request or response */
#define LKD_FAMSVC_MAX_FDS 16               /* descriptors per request or response */
#define LKD_FAMSVC_TIMEOUT_MS 5000          /* default when a caller passes timeout_ms <= 0 */

/* One request or response. */
typedef struct lkd_famsvc_msg {
    uint32_t op;
    int32_t status;           /* response: the handler's result (>= 0 or -LX_E*); request: 0 */
    int32_t target_pid;       /* guest ids the caller addresses (for the handler; 0 = unspecified) */
    int32_t target_tid;
    int32_t caller_pid;       /* guest ids of the calling task, filled from lkd_current() when 0 */
    int32_t caller_tid;
    uint32_t caller_uid;      /* euid of the calling guest process, filled from lkd_current() */
    int32_t caller_host_pid;  /* receiver side only: from LOCAL_PEERPID, never taken from the wire */
    void *data;               /* payload */
    size_t len;
    int fds[LKD_FAMSVC_MAX_FDS];
    int nfds;
} lkd_famsvc_msg;

typedef void (*lkd_famsvc_handler)(lkd_famsvc_msg *req, lkd_famsvc_msg *resp);

/* Register the handler of op, once, from lkd_<module>_init (single-threaded). Returns 0, -LX_EINVAL
 * (op out of range or fn NULL), -LX_EEXIST (op taken). An opcode without a handler answers -LX_ENOSYS. */
int lkd_famsvc_register(uint32_t op, lkd_famsvc_handler fn);

/* Synchronous call to the service of host process host_pid (any process of the family, the caller's
 * own included). ANY thread except the service thread, takes no LKD lock, blocks up to timeout_ms
 * (<= 0: LKD_FAMSVC_TIMEOUT_MS). req->data/len/fds are sent and stay the caller's. On return resp is
 * filled: resp->data (malloc'ed or NULL; release with lkd_famsvc_msg_free) and resp->fds belong to the
 * caller. Returns resp->status when the call reached a handler, else a transport error:
 *   -LX_ESRCH     no service for that pid (process gone, or not a family member)
 *   -LX_ETIMEDOUT no answer in time (the connection is dropped; a late answer is discarded)
 *   -LX_EPROTO    malformed answer
 *   -LX_E2BIG     payload or descriptor count above the limits
 *   -LX_EINVAL    called from the service thread itself, or bad arguments */
int lkd_famsvc_call(int32_t host_pid, lkd_famsvc_msg *req, lkd_famsvc_msg *resp, int timeout_ms);

/* Same, addressed by guest pid: the host pid comes from the family registry (lkd_proc_lookup). Returns
 * -LX_ESRCH when the registry has no live process with that pid (a zombie has no service). */
int lkd_famsvc_call_pid(int32_t guest_pid, lkd_famsvc_msg *req, lkd_famsvc_msg *resp, int timeout_ms);

/* Release what lkd_famsvc_call put in a response: free(data), close(fds). Safe on a zeroed msg. */
void lkd_famsvc_msg_free(lkd_famsvc_msg *m);

/* Service control. lkd_famsvc_start is idempotent (lkd_sig_init calls it) and returns 0 or -errno.
 * lkd_famsvc_shutdown stops the thread, unlinks this process' socket and removes the family directory
 * when it holds no other socket; the sig module calls it from lkd_sig_proc_exit. */
int lkd_famsvc_start(void);
void lkd_famsvc_shutdown(void);
int lkd_famsvc_running(void);

/* The family directory (creates it on first use), or NULL with errno set. Stable for the process. */
const char *lkd_famsvc_dir(void);

/* Fork hooks, called by the sig module's own fork hooks (prepare takes the client lock, child closes the
 * inherited listening socket and connections and starts a new service for the child's host pid). */
void lkd_famsvc_fork_prepare(void);
void lkd_famsvc_fork_parent(void);
void lkd_famsvc_fork_child(void);

/* Statistics for tests and /proc diagnostics. */
struct lkd_famsvc_stats { uint64_t calls, served, connects, timeouts, adopted; };
void lkd_famsvc_get_stats(struct lkd_famsvc_stats *out);

#endif /* LKD_FAMSVC_H */
