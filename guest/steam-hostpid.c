/* steam-hostpid.so: an LD_PRELOAD library for x86-64 Linux programs run by voile-lkd.
 *
 * getpid() called from Valve's steamclient.so returns the macOS pid of the host process that runs the caller
 * (LKD's /proc/self/lkd_host_pid). Every other caller gets the guest pid, as without the library.
 *
 * Why: steamclient.so sends getpid() with each IPC message to Obelisk's Steam engine on macOS, which resets the pipe
 * 2 to 3 s later unless that pid is a live macOS process. Guest pids are LKD's own (2, 3, ...). Wine must keep them:
 * ntdll gives getpid() to wineserver (init_first_thread, unix_pid), which signals the process's threads with
 * tgkill(unix_pid, unix_tid) and reads it with process_vm_readv. So the library looks at who calls, not at which
 * process it is in. steamclient.so calls getpid through its PLT with a call instruction (22 sites, no tail call, in
 * build 1788652215), so the return address is inside steamclient.so.
 *
 * steamclient.so also signals the pid it got: syscall(SYS_tgkill, getpid(), tid, 0) tells it whether one of its threads
 * lives, and kill(pid, 0) whether a process it shares memory with does. The guest knows no macOS pid (ESRCH), so every
 * thread looked dead. A game server, whose engine runs in the game's process (SteamGameServer_Init, as Rocket League's
 * Steam build calls), then freed what its threads still used and crashed. So the library also gives the guest pid
 * back to the signals steamclient.so sends to the macOS pid it was given.
 *
 * Outside LKD (no lkd_host_pid file) the guest pid is the real pid and is returned unchanged. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The guest pid in the high half, the pid to return in the low half. One word, so that a thread never pairs the guest
 * pid of one process with the host pid of another (a fork copies the cache; guest pids are never reused). */
static _Atomic uint64_t g_cache;

static pid_t read_host_pid(void)
{
    int fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/lkd_host_pid", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[16];
    long n = syscall(SYS_read, fd, buf, sizeof buf - 1);
    syscall(SYS_close, fd);
    if (n <= 0) return -1;
    long v = 0;
    for (long i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) v = v * 10 + (buf[i] - '0');
    return v > 0 && v <= 0x7fffffff ? (pid_t)v : -1;
}

static int from_steamclient(void *caller)
{
    struct dl_find_object o;
    if (_dl_find_object(caller, &o) != 0 || !o.dlfo_link_map || !o.dlfo_link_map->l_name) return 0;
    const char *name = o.dlfo_link_map->l_name;
    const char *base = strrchr(name, '/');
    return strcmp(base ? base + 1 : name, "steamclient.so") == 0;
}

pid_t getpid(void)
{
    pid_t pid = (pid_t)syscall(SYS_getpid);
    if (!from_steamclient(__builtin_return_address(0))) return pid;
    uint64_t c = atomic_load_explicit(&g_cache, memory_order_relaxed);
    if ((pid_t)(c >> 32) == pid) return (pid_t)(uint32_t)c;
    pid_t host = read_host_pid();
    if (host <= 0) host = pid;
    atomic_store_explicit(&g_cache, (uint64_t)(uint32_t)pid << 32 | (uint32_t)host, memory_order_relaxed);
    return host;
}

/* The guest pid of this process when pid is the macOS pid getpid() gave steamclient.so in it, else pid. */
static pid_t guest_pid(pid_t pid)
{
    pid_t self = (pid_t)syscall(SYS_getpid);
    uint64_t c = atomic_load_explicit(&g_cache, memory_order_relaxed);
    return pid > 0 && (pid_t)(c >> 32) == self && (pid_t)(uint32_t)c == pid ? self : pid;
}

long syscall(long number, ...)
{
    static long (*next)(long, ...);
    if (!next) next = (long (*)(long, ...))dlsym(RTLD_NEXT, "syscall");
    va_list ap;
    va_start(ap, number);
    long a[6];
    for (int i = 0; i < 6; i++) a[i] = va_arg(ap, long);
    va_end(ap);
    if (number == SYS_tgkill && from_steamclient(__builtin_return_address(0))) a[0] = guest_pid((pid_t)a[0]);
    return next(number, a[0], a[1], a[2], a[3], a[4], a[5]);
}

int kill(pid_t pid, int sig)
{
    static int (*next)(pid_t, int);
    if (!next) next = (int (*)(pid_t, int))dlsym(RTLD_NEXT, "kill");
    if (from_steamclient(__builtin_return_address(0))) pid = guest_pid(pid);
    return next(pid, sig);
}
