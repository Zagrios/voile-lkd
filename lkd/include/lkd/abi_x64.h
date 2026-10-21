/* abi_x64.h: the Linux x86-64 ABI as seen by the guest (Linux v6.12 uapi).
 *
 * Naming: constants are LX_<LINUX_NAME>, types are struct lx_<name>. They never collide with the
 * Darwin headers, whose values differ (EAGAIN is 35 on Darwin and 11 on Linux). Handlers return
 * -LX_E*, never -E*. Syscall numbers are LKD_NR_<name> (syscall_nr.h, generated).
 *
 * Every layout below is checked by _Static_assert against the kernel ABI. Layouts that only one
 * module needs (SysV shm, termios variants, timex, io_uring, ...) are defined by that module in its
 * own header with the same conventions; if a second module needs one, it is moved here on request.
 * Reference: https://github.com/torvalds/linux/tree/v6.12 (include/uapi, arch/x86/include/uapi).
 */
#ifndef LKD_ABI_X64_H
#define LKD_ABI_X64_H

#include <stddef.h>
#include <stdint.h>
#include <lkd/syscall_nr.h>

/* ---- x86-64 syscall register convention -------------------------------------------------------
 * number in RAX; arguments RDI, RSI, RDX, R10, R8, R9 (a[0]..a[5]); result in RAX.
 * The SYSCALL instruction itself loads RCX = next RIP and R11 = RFLAGS: the ENGINE does that, not
 * LKD (LKD never touches RCX/R11 of the guest). On a ptrace syscall stop RCX/R11 hold those values.
 * A result in [-4095, -1] is -errno; anything else is a value (pointers above 2^63 are legal).
 */
#define LX_MAX_ERRNO 4095

/* ---- errno (Linux values) --------------------------------------------------------------------- */
#define LX_EPERM 1
#define LX_ENOENT 2
#define LX_ESRCH 3
#define LX_EINTR 4
#define LX_EIO 5
#define LX_ENXIO 6
#define LX_E2BIG 7
#define LX_ENOEXEC 8
#define LX_EBADF 9
#define LX_ECHILD 10
#define LX_EAGAIN 11
#define LX_ENOMEM 12
#define LX_EACCES 13
#define LX_EFAULT 14
#define LX_ENOTBLK 15
#define LX_EBUSY 16
#define LX_EEXIST 17
#define LX_EXDEV 18
#define LX_ENODEV 19
#define LX_ENOTDIR 20
#define LX_EISDIR 21
#define LX_EINVAL 22
#define LX_ENFILE 23
#define LX_EMFILE 24
#define LX_ENOTTY 25
#define LX_ETXTBSY 26
#define LX_EFBIG 27
#define LX_ENOSPC 28
#define LX_ESPIPE 29
#define LX_EROFS 30
#define LX_EMLINK 31
#define LX_EPIPE 32
#define LX_EDOM 33
#define LX_ERANGE 34
#define LX_EDEADLK 35
#define LX_ENAMETOOLONG 36
#define LX_ENOLCK 37
#define LX_ENOSYS 38
#define LX_ENOTEMPTY 39
#define LX_ELOOP 40
#define LX_EWOULDBLOCK LX_EAGAIN
#define LX_ENOMSG 42
#define LX_EIDRM 43
#define LX_ECHRNG 44
#define LX_EL2NSYNC 45
#define LX_EL3HLT 46
#define LX_EL3RST 47
#define LX_ELNRNG 48
#define LX_EUNATCH 49
#define LX_ENOCSI 50
#define LX_EL2HLT 51
#define LX_EBADE 52
#define LX_EBADR 53
#define LX_EXFULL 54
#define LX_ENOANO 55
#define LX_EBADRQC 56
#define LX_EBADSLT 57
#define LX_EDEADLOCK LX_EDEADLK
#define LX_EBFONT 59
#define LX_ENOSTR 60
#define LX_ENODATA 61
#define LX_ETIME 62
#define LX_ENOSR 63
#define LX_ENONET 64
#define LX_ENOPKG 65
#define LX_EREMOTE 66
#define LX_ENOLINK 67
#define LX_EADV 68
#define LX_ESRMNT 69
#define LX_ECOMM 70
#define LX_EPROTO 71
#define LX_EMULTIHOP 72
#define LX_EDOTDOT 73
#define LX_EBADMSG 74
#define LX_EOVERFLOW 75
#define LX_ENOTUNIQ 76
#define LX_EBADFD 77
#define LX_EREMCHG 78
#define LX_ELIBACC 79
#define LX_ELIBBAD 80
#define LX_ELIBSCN 81
#define LX_ELIBMAX 82
#define LX_ELIBEXEC 83
#define LX_EILSEQ 84
#define LX_ERESTART 85
#define LX_ESTRPIPE 86
#define LX_EUSERS 87
#define LX_ENOTSOCK 88
#define LX_EDESTADDRREQ 89
#define LX_EMSGSIZE 90
#define LX_EPROTOTYPE 91
#define LX_ENOPROTOOPT 92
#define LX_EPROTONOSUPPORT 93
#define LX_ESOCKTNOSUPPORT 94
#define LX_EOPNOTSUPP 95
#define LX_ENOTSUP LX_EOPNOTSUPP
#define LX_EPFNOSUPPORT 96
#define LX_EAFNOSUPPORT 97
#define LX_EADDRINUSE 98
#define LX_EADDRNOTAVAIL 99
#define LX_ENETDOWN 100
#define LX_ENETUNREACH 101
#define LX_ENETRESET 102
#define LX_ECONNABORTED 103
#define LX_ECONNRESET 104
#define LX_ENOBUFS 105
#define LX_EISCONN 106
#define LX_ENOTCONN 107
#define LX_ESHUTDOWN 108
#define LX_ETOOMANYREFS 109
#define LX_ETIMEDOUT 110
#define LX_ECONNREFUSED 111
#define LX_EHOSTDOWN 112
#define LX_EHOSTUNREACH 113
#define LX_EALREADY 114
#define LX_EINPROGRESS 115
#define LX_ESTALE 116
#define LX_EUCLEAN 117
#define LX_ENOTNAM 118
#define LX_ENAVAIL 119
#define LX_EISNAM 120
#define LX_EREMOTEIO 121
#define LX_EDQUOT 122
#define LX_ENOMEDIUM 123
#define LX_EMEDIUMTYPE 124
#define LX_ECANCELED 125
#define LX_ENOKEY 126
#define LX_EKEYEXPIRED 127
#define LX_EKEYREVOKED 128
#define LX_EKEYREJECTED 129
#define LX_EOWNERDEAD 130
#define LX_ENOTRECOVERABLE 131
#define LX_ERFKILL 132
#define LX_EHWPOISON 133
#define LX_ELAST 133

/* Kernel-internal restart codes (include/linux/errno.h). They never reach the guest: the dispatcher
 * turns them into -EINTR or a restart (see lkd.h, LKD_RESTART). */
#define LX_ERESTARTSYS 512
#define LX_ERESTARTNOINTR 513
#define LX_ERESTARTNOHAND 514
#define LX_ENOIOCTLCMD 515
#define LX_ERESTART_RESTARTBLOCK 516

/* ---- open(2), *at(2), file modes -------------------------------------------------------------- */
#define LX_O_ACCMODE 0003
#define LX_O_RDONLY 0000
#define LX_O_WRONLY 0001
#define LX_O_RDWR 0002
#define LX_O_CREAT 0000100
#define LX_O_EXCL 0000200
#define LX_O_NOCTTY 0000400
#define LX_O_TRUNC 0001000
#define LX_O_APPEND 0002000
#define LX_O_NONBLOCK 0004000
#define LX_O_DSYNC 0010000
#define LX_O_ASYNC 0020000
#define LX_O_DIRECT 0040000
/* x86-64 value. glibc passes 0 for it, the kernel sets it on every open (F_GETFL shows 0x8000). */
#define LX_O_LARGEFILE 0100000
#define LX_O_DIRECTORY 0200000
#define LX_O_NOFOLLOW 0400000
#define LX_O_NOATIME 01000000
#define LX_O_CLOEXEC 02000000
#define LX_O_SYNC 04010000
#define LX_O_PATH 010000000
#define LX_O_TMPFILE (020000000 | LX_O_DIRECTORY)

#define LX_AT_FDCWD (-100)
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_EACCESS 0x200
#define LX_AT_REMOVEDIR 0x200
#define LX_AT_SYMLINK_FOLLOW 0x400
#define LX_AT_NO_AUTOMOUNT 0x800
#define LX_AT_EMPTY_PATH 0x1000
#define LX_AT_STATX_SYNC_TYPE 0x6000
#define LX_AT_STATX_SYNC_AS_STAT 0x0000
#define LX_AT_STATX_FORCE_SYNC 0x2000
#define LX_AT_STATX_DONT_SYNC 0x4000
#define LX_AT_RENAME_NOREPLACE 1
#define LX_AT_RENAME_EXCHANGE 2
#define LX_AT_RENAME_WHITEOUT 4

#define LX_F_OK 0
#define LX_X_OK 1
#define LX_W_OK 2
#define LX_R_OK 4

#define LX_SEEK_SET 0
#define LX_SEEK_CUR 1
#define LX_SEEK_END 2
#define LX_SEEK_DATA 3
#define LX_SEEK_HOLE 4

#define LX_S_IFMT 0170000
#define LX_S_IFSOCK 0140000
#define LX_S_IFLNK 0120000
#define LX_S_IFREG 0100000
#define LX_S_IFBLK 0060000
#define LX_S_IFDIR 0040000
#define LX_S_IFCHR 0020000
#define LX_S_IFIFO 0010000
#define LX_S_ISUID 04000
#define LX_S_ISGID 02000
#define LX_S_ISVTX 01000
#define LX_S_ISREG(m) (((m) & LX_S_IFMT) == LX_S_IFREG)
#define LX_S_ISDIR(m) (((m) & LX_S_IFMT) == LX_S_IFDIR)
#define LX_S_ISLNK(m) (((m) & LX_S_IFMT) == LX_S_IFLNK)
#define LX_S_ISCHR(m) (((m) & LX_S_IFMT) == LX_S_IFCHR)
#define LX_S_ISBLK(m) (((m) & LX_S_IFMT) == LX_S_IFBLK)
#define LX_S_ISFIFO(m) (((m) & LX_S_IFMT) == LX_S_IFIFO)
#define LX_S_ISSOCK(m) (((m) & LX_S_IFMT) == LX_S_IFSOCK)

#define LX_DT_UNKNOWN 0
#define LX_DT_FIFO 1
#define LX_DT_CHR 2
#define LX_DT_DIR 4
#define LX_DT_BLK 6
#define LX_DT_REG 8
#define LX_DT_LNK 10
#define LX_DT_SOCK 12

#define LX_UTIME_NOW ((1l << 30) - 1l)
#define LX_UTIME_OMIT ((1l << 30) - 2l)

#define LX_PATH_MAX 4096
#define LX_NAME_MAX 255
#define LX_PIPE_BUF 4096
#define LX_IOV_MAX 1024
#define LX_NGROUPS_MAX 65536

/* ---- fcntl, flock, memfd, ioctl --------------------------------------------------------------- */
#define LX_F_DUPFD 0
#define LX_F_GETFD 1
#define LX_F_SETFD 2
#define LX_F_GETFL 3
#define LX_F_SETFL 4
#define LX_F_GETLK 5
#define LX_F_SETLK 6
#define LX_F_SETLKW 7
#define LX_F_SETOWN 8
#define LX_F_GETOWN 9
#define LX_F_SETSIG 10
#define LX_F_GETSIG 11
#define LX_F_SETOWN_EX 15
#define LX_F_GETOWN_EX 16
#define LX_F_GETOWNER_UIDS 17
#define LX_F_OFD_GETLK 36
#define LX_F_OFD_SETLK 37
#define LX_F_OFD_SETLKW 38
#define LX_F_SETLEASE 1024
#define LX_F_GETLEASE 1025
#define LX_F_NOTIFY 1026
#define LX_F_DUPFD_CLOEXEC 1030
#define LX_F_SETPIPE_SZ 1031
#define LX_F_GETPIPE_SZ 1032
#define LX_F_ADD_SEALS 1033
#define LX_F_GET_SEALS 1034
#define LX_F_GET_RW_HINT 1035
#define LX_F_SET_RW_HINT 1036
#define LX_F_GET_FILE_RW_HINT 1037
#define LX_F_SET_FILE_RW_HINT 1038
#define LX_FD_CLOEXEC 1
#define LX_F_RDLCK 0
#define LX_F_WRLCK 1
#define LX_F_UNLCK 2
#define LX_F_SEAL_SEAL 0x0001
#define LX_F_SEAL_SHRINK 0x0002
#define LX_F_SEAL_GROW 0x0004
#define LX_F_SEAL_WRITE 0x0008
#define LX_F_SEAL_FUTURE_WRITE 0x0010
#define LX_F_SEAL_EXEC 0x0020
#define LX_LOCK_SH 1
#define LX_LOCK_EX 2
#define LX_LOCK_NB 4
#define LX_LOCK_UN 8
#define LX_MFD_CLOEXEC 0x0001u
#define LX_MFD_ALLOW_SEALING 0x0002u
#define LX_MFD_HUGETLB 0x0004u
#define LX_MFD_NOEXEC_SEAL 0x0008u
#define LX_MFD_EXEC 0x0010u

#define LX_TCGETS 0x5401
#define LX_TCSETS 0x5402
#define LX_TCSETSW 0x5403
#define LX_TCSETSF 0x5404
#define LX_TIOCSCTTY 0x540E
#define LX_TIOCGPGRP 0x540F
#define LX_TIOCSPGRP 0x5410
#define LX_TIOCGWINSZ 0x5413
#define LX_TIOCSWINSZ 0x5414
#define LX_TIOCNOTTY 0x5422
#define LX_TIOCGSID 0x5429
#define LX_FIONREAD 0x541B
#define LX_TIOCINQ LX_FIONREAD
#define LX_FIONBIO 0x5421
#define LX_FIONCLEX 0x5450
#define LX_FIOCLEX 0x5451
#define LX_FIOASYNC 0x5452
#define LX_TIOCGPTN 0x80045430u
#define LX_TIOCSPTLCK 0x40045431u
#define LX_SIOCGIFCONF 0x8912
#define LX_SIOCGIFFLAGS 0x8913
#define LX_SIOCGIFADDR 0x8915

/* ---- mmap family ------------------------------------------------------------------------------ */
#define LX_PROT_NONE 0x0
#define LX_PROT_READ 0x1
#define LX_PROT_WRITE 0x2
#define LX_PROT_EXEC 0x4
#define LX_PROT_SEM 0x8
#define LX_PROT_GROWSDOWN 0x01000000
#define LX_PROT_GROWSUP 0x02000000
#define LX_MAP_SHARED 0x01
#define LX_MAP_PRIVATE 0x02
#define LX_MAP_SHARED_VALIDATE 0x03
#define LX_MAP_TYPE 0x0f
#define LX_MAP_FIXED 0x10
#define LX_MAP_ANONYMOUS 0x20
#define LX_MAP_32BIT 0x40
#define LX_MAP_GROWSDOWN 0x0100
#define LX_MAP_DENYWRITE 0x0800
#define LX_MAP_EXECUTABLE 0x1000
#define LX_MAP_LOCKED 0x2000
#define LX_MAP_NORESERVE 0x4000
#define LX_MAP_POPULATE 0x8000
#define LX_MAP_NONBLOCK 0x10000
#define LX_MAP_STACK 0x20000
#define LX_MAP_HUGETLB 0x40000
#define LX_MAP_SYNC 0x80000
#define LX_MAP_FIXED_NOREPLACE 0x100000
#define LX_MAP_UNINITIALIZED 0x4000000
#define LX_MREMAP_MAYMOVE 1
#define LX_MREMAP_FIXED 2
#define LX_MREMAP_DONTUNMAP 4
#define LX_MS_ASYNC 1
#define LX_MS_INVALIDATE 2
#define LX_MS_SYNC 4
#define LX_MCL_CURRENT 1
#define LX_MCL_FUTURE 2
#define LX_MCL_ONFAULT 4
#define LX_MLOCK_ONFAULT 1
#define LX_MADV_NORMAL 0
#define LX_MADV_RANDOM 1
#define LX_MADV_SEQUENTIAL 2
#define LX_MADV_WILLNEED 3
#define LX_MADV_DONTNEED 4
#define LX_MADV_FREE 8
#define LX_MADV_REMOVE 9
#define LX_MADV_DONTFORK 10
#define LX_MADV_DOFORK 11
#define LX_MADV_MERGEABLE 12
#define LX_MADV_UNMERGEABLE 13
#define LX_MADV_HUGEPAGE 14
#define LX_MADV_NOHUGEPAGE 15
#define LX_MADV_DONTDUMP 16
#define LX_MADV_DODUMP 17
#define LX_MADV_WIPEONFORK 18
#define LX_MADV_KEEPONFORK 19
#define LX_MADV_COLD 20
#define LX_MADV_PAGEOUT 21
#define LX_MADV_POPULATE_READ 22
#define LX_MADV_POPULATE_WRITE 23
#define LX_MADV_DONTNEED_LOCKED 24
#define LX_MADV_COLLAPSE 25
#define LX_PAGE_SHIFT 12 /* the guest page size is 4 KiB regardless of the 16 KiB host page */
#define LX_PAGE_SIZE 4096ul
#define LX_PAGE_MASK (~(LX_PAGE_SIZE - 1))

/* ---- signals ---------------------------------------------------------------------------------- */
#define LX_SIGHUP 1
#define LX_SIGINT 2
#define LX_SIGQUIT 3
#define LX_SIGILL 4
#define LX_SIGTRAP 5
#define LX_SIGABRT 6
#define LX_SIGIOT LX_SIGABRT
#define LX_SIGBUS 7
#define LX_SIGFPE 8
#define LX_SIGKILL 9
#define LX_SIGUSR1 10
#define LX_SIGSEGV 11
#define LX_SIGUSR2 12
#define LX_SIGPIPE 13
#define LX_SIGALRM 14
#define LX_SIGTERM 15
#define LX_SIGSTKFLT 16
#define LX_SIGCHLD 17
#define LX_SIGCONT 18
#define LX_SIGSTOP 19
#define LX_SIGTSTP 20
#define LX_SIGTTIN 21
#define LX_SIGTTOU 22
#define LX_SIGURG 23
#define LX_SIGXCPU 24
#define LX_SIGXFSZ 25
#define LX_SIGVTALRM 26
#define LX_SIGPROF 27
#define LX_SIGWINCH 28
#define LX_SIGIO 29
#define LX_SIGPOLL LX_SIGIO
#define LX_SIGPWR 30
#define LX_SIGSYS 31
#define LX_SIGRTMIN 32 /* kernel value; glibc reserves 32 and 33 and exposes 34 */
#define LX_SIGRTMAX 64
#define LX_NSIG 64
#define LX_SIGSET_BYTES 8 /* sigsetsize argument of rt_sig* */
#define LX_SIGBIT(sig) (1ULL << ((sig) - 1))
#define LX_SIG_VALID(sig) ((sig) >= 1 && (sig) <= LX_NSIG)
#define LX_SIG_UNBLOCKABLE (LX_SIGBIT(LX_SIGKILL) | LX_SIGBIT(LX_SIGSTOP))

#define LX_SA_NOCLDSTOP 0x00000001u
#define LX_SA_NOCLDWAIT 0x00000002u
#define LX_SA_SIGINFO 0x00000004u
#define LX_SA_UNSUPPORTED 0x00000400u
#define LX_SA_EXPOSE_TAGBITS 0x00000800u
#define LX_SA_RESTORER 0x04000000u
#define LX_SA_ONSTACK 0x08000000u
#define LX_SA_RESTART 0x10000000u
#define LX_SA_NODEFER 0x40000000u
#define LX_SA_RESETHAND 0x80000000u
#define LX_SIG_BLOCK 0
#define LX_SIG_UNBLOCK 1
#define LX_SIG_SETMASK 2
#define LX_SIG_DFL 0ul
#define LX_SIG_IGN 1ul
#define LX_SIG_ERR (~0ul)
#define LX_SS_ONSTACK 1
#define LX_SS_DISABLE 2
#define LX_SS_AUTODISARM (1u << 31)
#define LX_MINSIGSTKSZ 2048
#define LX_SIGSTKSZ 8192

#define LX_SI_USER 0
#define LX_SI_KERNEL 0x80
#define LX_SI_QUEUE (-1)
#define LX_SI_TIMER (-2)
#define LX_SI_MESGQ (-3)
#define LX_SI_ASYNCIO (-4)
#define LX_SI_SIGIO (-5)
#define LX_SI_TKILL (-6)
#define LX_SI_DETHREAD (-7)
#define LX_ILL_ILLOPC 1
#define LX_ILL_ILLOPN 2
#define LX_ILL_ILLADR 3
#define LX_ILL_ILLTRP 4
#define LX_ILL_PRVOPC 5
#define LX_FPE_INTDIV 1
#define LX_FPE_INTOVF 2
#define LX_FPE_FLTDIV 3
#define LX_FPE_FLTOVF 4
#define LX_FPE_FLTUND 5
#define LX_FPE_FLTRES 6
#define LX_FPE_FLTINV 7
#define LX_SEGV_MAPERR 1
#define LX_SEGV_ACCERR 2
#define LX_BUS_ADRALN 1
#define LX_BUS_ADRERR 2
#define LX_BUS_OBJERR 3
#define LX_TRAP_BRKPT 1
#define LX_TRAP_TRACE 2
#define LX_TRAP_HWBKPT 4
#define LX_CLD_EXITED 1
#define LX_CLD_KILLED 2
#define LX_CLD_DUMPED 3
#define LX_CLD_TRAPPED 4
#define LX_CLD_STOPPED 5
#define LX_CLD_CONTINUED 6
#define LX_POLL_IN 1
#define LX_POLL_OUT 2
#define LX_POLL_MSG 3
#define LX_POLL_ERR 4
#define LX_POLL_PRI 5
#define LX_POLL_HUP 6

/* Kernel struct sigaction (rt_sigaction): 32 bytes. mask is the 64-bit kernel sigset. */
struct lx_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

/* siginfo_t, 128 bytes. The union starts at offset 16 (after signo, errno, code and 4 pad bytes). */
struct lx_siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t _pad0;
    union {
        struct { int32_t pid; uint32_t uid; } kill;
        struct { int32_t tid; int32_t overrun; uint64_t sigval; int32_t sys_private; } timer;
        struct { int32_t pid; uint32_t uid; uint64_t sigval; } rt; /* sigqueue: pid, uid, value */
        struct { int32_t pid; uint32_t uid; int32_t status; int32_t _pad; int64_t utime; int64_t stime; } chld;
        struct { uint64_t addr; int16_t addr_lsb; int16_t _pad[3]; uint64_t lower; uint64_t upper; } fault;
        struct { int64_t band; int32_t fd; } poll;
        struct { uint64_t call_addr; int32_t syscall; uint32_t arch; } sys;
        int32_t _si_pad[28];
    } u;
};

struct lx_stack_t { /* stack_t */
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t _pad;
    uint64_t ss_size;
};

/* sigcontext (x86-64), 256 bytes. */
struct lx_sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
    uint64_t eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate; /* guest pointer to struct lx_fpstate, 0 if none */
    uint64_t reserved1[8];
};

/* Kernel ucontext (x86-64), 312 bytes. uc_sigmask is the 64-bit kernel sigset. */
struct lx_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct lx_stack_t uc_stack;
    struct lx_sigcontext uc_mcontext;
    uint64_t uc_sigmask;
};
#define LX_UC_FP_XSTATE 0x1
#define LX_UC_SIGCONTEXT_SS 0x2
#define LX_UC_STRICT_RESTORE_SS 0x4

/* rt_sigframe as built on the guest stack: pretcode (return address = sa_restorer), ucontext, siginfo.
 * The xsave/fxsave area (fpstate) lives above it at a 64-byte aligned address. The red zone (128
 * bytes below RSP) is skipped before building the frame. */
struct lx_rt_sigframe {
    uint64_t pretcode;
    struct lx_ucontext uc;
    struct lx_siginfo info;
};
#define LX_RED_ZONE 128
#define LX_FP_XSTATE_MAGIC1 0x46505853u
#define LX_FP_XSTATE_MAGIC2 0x46505845u

enum lx_sig_default { LX_SIGDFL_TERM, LX_SIGDFL_CORE, LX_SIGDFL_STOP, LX_SIGDFL_CONT, LX_SIGDFL_IGN };

/* Default action of a signal (signal(7)). Real-time signals terminate. */
static inline enum lx_sig_default lx_sig_default_action(int sig)
{
    switch (sig) {
    case LX_SIGQUIT: case LX_SIGILL: case LX_SIGTRAP: case LX_SIGABRT: case LX_SIGBUS:
    case LX_SIGFPE: case LX_SIGSEGV: case LX_SIGXCPU: case LX_SIGXFSZ: case LX_SIGSYS:
        return LX_SIGDFL_CORE;
    case LX_SIGSTOP: case LX_SIGTSTP: case LX_SIGTTIN: case LX_SIGTTOU:
        return LX_SIGDFL_STOP;
    case LX_SIGCONT:
        return LX_SIGDFL_CONT;
    case LX_SIGCHLD: case LX_SIGURG: case LX_SIGWINCH:
        return LX_SIGDFL_IGN;
    default:
        return LX_SIGDFL_TERM;
    }
}

/* signalfd_siginfo, 128 bytes. */
struct lx_signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t ssi_errno;
    int32_t ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t ssi_status;
    int32_t ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint16_t _pad2;
    int32_t ssi_syscall;
    uint64_t ssi_call_addr;
    uint32_t ssi_arch;
    uint8_t _pad[28];
};

/* ---- clone, wait, exit ------------------------------------------------------------------------ */
#define LX_CSIGNAL 0x000000ffull
#define LX_CLONE_VM 0x00000100ull
#define LX_CLONE_FS 0x00000200ull
#define LX_CLONE_FILES 0x00000400ull
#define LX_CLONE_SIGHAND 0x00000800ull
#define LX_CLONE_PIDFD 0x00001000ull
#define LX_CLONE_PTRACE 0x00002000ull
#define LX_CLONE_VFORK 0x00004000ull
#define LX_CLONE_PARENT 0x00008000ull
#define LX_CLONE_THREAD 0x00010000ull
#define LX_CLONE_NEWNS 0x00020000ull
#define LX_CLONE_SYSVSEM 0x00040000ull
#define LX_CLONE_SETTLS 0x00080000ull
#define LX_CLONE_PARENT_SETTID 0x00100000ull
#define LX_CLONE_CHILD_CLEARTID 0x00200000ull
#define LX_CLONE_DETACHED 0x00400000ull
#define LX_CLONE_UNTRACED 0x00800000ull
#define LX_CLONE_CHILD_SETTID 0x01000000ull
#define LX_CLONE_NEWCGROUP 0x02000000ull
#define LX_CLONE_NEWUTS 0x04000000ull
#define LX_CLONE_NEWIPC 0x08000000ull
#define LX_CLONE_NEWUSER 0x10000000ull
#define LX_CLONE_NEWPID 0x20000000ull
#define LX_CLONE_NEWNET 0x40000000ull
#define LX_CLONE_IO 0x80000000ull
#define LX_CLONE_CLEAR_SIGHAND 0x100000000ull
#define LX_CLONE_INTO_CGROUP 0x200000000ull
#define LX_CLONE_NEWTIME 0x00000080ull

struct lx_clone_args { /* clone3, CLONE_ARGS_SIZE_VER2 = 88 */
    uint64_t flags, pidfd, child_tid, parent_tid, exit_signal;
    uint64_t stack, stack_size, tls, set_tid, set_tid_size, cgroup;
};
#define LX_CLONE_ARGS_SIZE_VER0 64
#define LX_CLONE_ARGS_SIZE_VER1 80
#define LX_CLONE_ARGS_SIZE_VER2 88

#define LX_WNOHANG 0x00000001
#define LX_WUNTRACED 0x00000002
#define LX_WSTOPPED LX_WUNTRACED
#define LX_WEXITED 0x00000004
#define LX_WCONTINUED 0x00000008
#define LX_WNOWAIT 0x01000000
#define LX_WNOTHREAD 0x20000000
#define LX_WALL 0x40000000
#define LX_WCLONE 0x80000000u
#define LX_P_ALL 0
#define LX_P_PID 1
#define LX_P_PGID 2
#define LX_P_PIDFD 3
/* Wait status encoding (bits/waitstatus.h; same as the kernel's). */
#define LX_WEXITSTATUS(s) (((s) & 0xff00) >> 8)
#define LX_WTERMSIG(s) ((s) & 0x7f)
#define LX_WSTOPSIG(s) LX_WEXITSTATUS(s)
#define LX_WIFEXITED(s) (LX_WTERMSIG(s) == 0)
#define LX_WIFSIGNALED(s) (((signed char)(((s) & 0x7f) + 1) >> 1) > 0)
#define LX_WIFSTOPPED(s) (((s) & 0xff) == 0x7f)
#define LX_WIFCONTINUED(s) ((s) == 0xffff)
#define LX_WCOREDUMP(s) ((s) & 0x80)
#define LX_W_EXITCODE(ret, sig) (((ret) << 8) | (sig))
#define LX_W_STOPCODE(sig) (((sig) << 8) | 0x7f)
#define LX_W_CONTINUED 0xffff
/* ptrace event stop: ((event << 8) | SIGTRAP) << 8 | 0x7f. */
#define LX_W_PTRACE_EVENT(event, sig) ((((event) << 8) | (sig)) << 8 | 0x7f)

/* ---- time, resources, system information ------------------------------------------------------ */
struct lx_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct lx_timeval { int64_t tv_sec; int64_t tv_usec; };
struct lx_itimerspec { struct lx_timespec it_interval, it_value; };
struct lx_itimerval { struct lx_timeval it_interval, it_value; };
struct lx_timezone { int32_t tz_minuteswest; int32_t tz_dsttime; };
struct lx_tms { int64_t tms_utime, tms_stime, tms_cutime, tms_cstime; };
struct lx_rlimit { uint64_t rlim_cur, rlim_max; };
struct lx_rusage {
    struct lx_timeval ru_utime, ru_stime;
    int64_t ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap,
            ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};
struct lx_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
struct lx_sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs;
    uint16_t _pad;
    uint32_t _pad2;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
    uint32_t _pad3;
};
struct lx_robust_list_head { uint64_t list; int64_t futex_offset; uint64_t list_op_pending; };
struct lx_sched_param { int32_t sched_priority; };
struct lx_iovec { uint64_t iov_base; uint64_t iov_len; };
struct lx_futex_waitv { uint64_t val; uint64_t uaddr; uint32_t flags; uint32_t _reserved; };
struct lx_cap_header { uint32_t version; int32_t pid; };
struct lx_cap_data { uint32_t effective, permitted, inheritable; };

#define LX_CLOCK_REALTIME 0
#define LX_CLOCK_MONOTONIC 1
#define LX_CLOCK_PROCESS_CPUTIME_ID 2
#define LX_CLOCK_THREAD_CPUTIME_ID 3
#define LX_CLOCK_MONOTONIC_RAW 4
#define LX_CLOCK_REALTIME_COARSE 5
#define LX_CLOCK_MONOTONIC_COARSE 6
#define LX_CLOCK_BOOTTIME 7
#define LX_CLOCK_REALTIME_ALARM 8
#define LX_CLOCK_BOOTTIME_ALARM 9
#define LX_CLOCK_TAI 11
#define LX_TIMER_ABSTIME 1
#define LX_ITIMER_REAL 0
#define LX_ITIMER_VIRTUAL 1
#define LX_ITIMER_PROF 2

#define LX_RLIMIT_CPU 0
#define LX_RLIMIT_FSIZE 1
#define LX_RLIMIT_DATA 2
#define LX_RLIMIT_STACK 3
#define LX_RLIMIT_CORE 4
#define LX_RLIMIT_RSS 5
#define LX_RLIMIT_NPROC 6
#define LX_RLIMIT_NOFILE 7
#define LX_RLIMIT_MEMLOCK 8
#define LX_RLIMIT_AS 9
#define LX_RLIMIT_LOCKS 10
#define LX_RLIMIT_SIGPENDING 11
#define LX_RLIMIT_MSGQUEUE 12
#define LX_RLIMIT_NICE 13
#define LX_RLIMIT_RTPRIO 14
#define LX_RLIMIT_RTTIME 15
#define LX_RLIM_NLIMITS 16
#define LX_RLIM_INFINITY (~0ull)
#define LX_RUSAGE_SELF 0
#define LX_RUSAGE_CHILDREN (-1)
#define LX_RUSAGE_THREAD 1

#define LX_SCHED_OTHER 0
#define LX_SCHED_FIFO 1
#define LX_SCHED_RR 2
#define LX_SCHED_BATCH 3
#define LX_SCHED_IDLE 5
#define LX_SCHED_DEADLINE 6
#define LX_GRND_NONBLOCK 1
#define LX_GRND_RANDOM 2
#define LX_GRND_INSECURE 4
#define LX_PER_LINUX 0
#define LX_PER_LINUX32 8

#define LX_PR_SET_PDEATHSIG 1
#define LX_PR_GET_PDEATHSIG 2
#define LX_PR_GET_DUMPABLE 3
#define LX_PR_SET_DUMPABLE 4
#define LX_PR_GET_KEEPCAPS 7
#define LX_PR_SET_KEEPCAPS 8
#define LX_PR_SET_NAME 15
#define LX_PR_GET_NAME 16
#define LX_PR_GET_SECCOMP 21
#define LX_PR_SET_SECCOMP 22
#define LX_PR_CAPBSET_READ 23
#define LX_PR_CAPBSET_DROP 24
#define LX_PR_GET_SECUREBITS 27
#define LX_PR_SET_SECUREBITS 28
#define LX_PR_SET_TIMERSLACK 29
#define LX_PR_GET_TIMERSLACK 30
#define LX_PR_SET_CHILD_SUBREAPER 36
#define LX_PR_GET_CHILD_SUBREAPER 37
#define LX_PR_SET_NO_NEW_PRIVS 38
#define LX_PR_GET_NO_NEW_PRIVS 39
#define LX_PR_GET_TID_ADDRESS 40
#define LX_PR_SET_THP_DISABLE 41
#define LX_PR_GET_THP_DISABLE 42
#define LX_PR_CAP_AMBIENT 47
#define LX_PR_SET_VMA 0x53564d41
#define LX_PR_SET_MDWE 65
#define LX_PR_GET_MDWE 66
#define LX_ARCH_SET_GS 0x1001
#define LX_ARCH_SET_FS 0x1002
#define LX_ARCH_GET_FS 0x1003
#define LX_ARCH_GET_GS 0x1004
#define LX_ARCH_GET_CPUID 0x1011
#define LX_ARCH_SET_CPUID 0x1012
#define LX_ARCH_CET_STATUS 0x3001

#define LX_FUTEX_WAIT 0
#define LX_FUTEX_WAKE 1
#define LX_FUTEX_FD 2
#define LX_FUTEX_REQUEUE 3
#define LX_FUTEX_CMP_REQUEUE 4
#define LX_FUTEX_WAKE_OP 5
#define LX_FUTEX_LOCK_PI 6
#define LX_FUTEX_UNLOCK_PI 7
#define LX_FUTEX_TRYLOCK_PI 8
#define LX_FUTEX_WAIT_BITSET 9
#define LX_FUTEX_WAKE_BITSET 10
#define LX_FUTEX_WAIT_REQUEUE_PI 11
#define LX_FUTEX_CMP_REQUEUE_PI 12
#define LX_FUTEX_LOCK_PI2 13
#define LX_FUTEX_PRIVATE_FLAG 128
#define LX_FUTEX_CLOCK_REALTIME 256
#define LX_FUTEX_CMD_MASK (~(LX_FUTEX_PRIVATE_FLAG | LX_FUTEX_CLOCK_REALTIME))
#define LX_FUTEX_BITSET_MATCH_ANY 0xffffffffu
#define LX_FUTEX_WAITERS 0x80000000u
#define LX_FUTEX_OWNER_DIED 0x40000000u
#define LX_FUTEX_TID_MASK 0x3fffffffu

/* Auxiliary vector types. */
#define LX_AT_NULL 0
#define LX_AT_IGNORE 1
#define LX_AT_EXECFD 2
#define LX_AT_PHDR 3
#define LX_AT_PHENT 4
#define LX_AT_PHNUM 5
#define LX_AT_PAGESZ 6
#define LX_AT_BASE 7
#define LX_AT_FLAGS 8
#define LX_AT_ENTRY 9
#define LX_AT_NOTELF 10
#define LX_AT_UID 11
#define LX_AT_EUID 12
#define LX_AT_GID 13
#define LX_AT_EGID 14
#define LX_AT_PLATFORM 15
#define LX_AT_HWCAP 16
#define LX_AT_CLKTCK 17
#define LX_AT_SECURE 23
#define LX_AT_BASE_PLATFORM 24
#define LX_AT_RANDOM 25
#define LX_AT_HWCAP2 26
#define LX_AT_EXECFN 31
#define LX_AT_SYSINFO_EHDR 33
#define LX_AT_MINSIGSTKSZ 51

/* ---- stat family ------------------------------------------------------------------------------ */
/* struct stat on x86-64: 144 bytes. */
struct lx_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t _pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_atime_sec, st_atime_nsec;
    int64_t st_mtime_sec, st_mtime_nsec;
    int64_t st_ctime_sec, st_ctime_nsec;
    int64_t _unused[3];
};

struct lx_statx_timestamp { int64_t tv_sec; uint32_t tv_nsec; int32_t _pad; };
struct lx_statx { /* 256 bytes */
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t _spare0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct lx_statx_timestamp stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align, stx_dio_offset_align;
    uint64_t stx_subvol;
    uint32_t stx_atomic_write_unit_min, stx_atomic_write_unit_max, stx_atomic_write_segments_max;
    uint32_t _spare1;
    uint64_t _spare3[9];
};
#define LX_STATX_TYPE 0x0001u
#define LX_STATX_MODE 0x0002u
#define LX_STATX_NLINK 0x0004u
#define LX_STATX_UID 0x0008u
#define LX_STATX_GID 0x0010u
#define LX_STATX_ATIME 0x0020u
#define LX_STATX_MTIME 0x0040u
#define LX_STATX_CTIME 0x0080u
#define LX_STATX_INO 0x0100u
#define LX_STATX_SIZE 0x0200u
#define LX_STATX_BLOCKS 0x0400u
#define LX_STATX_BASIC_STATS 0x07ffu
#define LX_STATX_BTIME 0x0800u
#define LX_STATX_MNT_ID 0x1000u

struct lx_statfs { /* x86-64: 120 bytes */
    int64_t f_type, f_bsize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t f_fsid[2];
    int64_t f_namelen, f_frsize, f_flags;
    int64_t f_spare[4];
};

struct lx_linux_dirent64 { /* getdents64 record header; d_name follows, record padded to 8 */
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};
struct lx_linux_dirent { /* legacy getdents: d_ino, d_off, d_reclen, name, ..., d_type in the last byte */
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    char d_name[];
};

struct lx_flock { /* x86-64 struct flock, 32 bytes */
    int16_t l_type;
    int16_t l_whence;
    int32_t _pad0;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
    int32_t _pad1;
};
struct lx_termios { /* kernel termios, 36 bytes */
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line;
    uint8_t c_cc[19];
};
struct lx_winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

/* ---- polling, event objects ------------------------------------------------------------------- */
#define LX_POLLIN 0x0001
#define LX_POLLPRI 0x0002
#define LX_POLLOUT 0x0004
#define LX_POLLERR 0x0008
#define LX_POLLHUP 0x0010
#define LX_POLLNVAL 0x0020
#define LX_POLLRDNORM 0x0040
#define LX_POLLRDBAND 0x0080
#define LX_POLLWRNORM 0x0100
#define LX_POLLWRBAND 0x0200
#define LX_POLLMSG 0x0400
#define LX_POLLRDHUP 0x2000
struct lx_pollfd { int32_t fd; int16_t events; int16_t revents; };

#define LX_EPOLLIN 0x00000001u
#define LX_EPOLLPRI 0x00000002u
#define LX_EPOLLOUT 0x00000004u
#define LX_EPOLLERR 0x00000008u
#define LX_EPOLLHUP 0x00000010u
#define LX_EPOLLNVAL 0x00000020u
#define LX_EPOLLRDNORM 0x00000040u
#define LX_EPOLLRDBAND 0x00000080u
#define LX_EPOLLWRNORM 0x00000100u
#define LX_EPOLLWRBAND 0x00000200u
#define LX_EPOLLMSG 0x00000400u
#define LX_EPOLLRDHUP 0x00002000u
#define LX_EPOLLEXCLUSIVE (1u << 28)
#define LX_EPOLLWAKEUP (1u << 29)
#define LX_EPOLLONESHOT (1u << 30)
#define LX_EPOLLET (1u << 31)
#define LX_EPOLL_CTL_ADD 1
#define LX_EPOLL_CTL_DEL 2
#define LX_EPOLL_CTL_MOD 3
#define LX_EPOLL_CLOEXEC LX_O_CLOEXEC
/* x86-64 epoll_event is packed: 12 bytes, data at offset 4. */
struct __attribute__((packed)) lx_epoll_event { uint32_t events; uint64_t data; };

#define LX_EFD_SEMAPHORE 1
#define LX_EFD_CLOEXEC LX_O_CLOEXEC
#define LX_EFD_NONBLOCK LX_O_NONBLOCK
#define LX_TFD_CLOEXEC LX_O_CLOEXEC
#define LX_TFD_NONBLOCK LX_O_NONBLOCK
#define LX_TFD_TIMER_ABSTIME 1
#define LX_TFD_TIMER_CANCEL_ON_SET 2
#define LX_SFD_CLOEXEC LX_O_CLOEXEC
#define LX_SFD_NONBLOCK LX_O_NONBLOCK
#define LX_IN_CLOEXEC LX_O_CLOEXEC
#define LX_IN_NONBLOCK LX_O_NONBLOCK
#define LX_IN_ACCESS 0x00000001u
#define LX_IN_MODIFY 0x00000002u
#define LX_IN_ATTRIB 0x00000004u
#define LX_IN_CLOSE_WRITE 0x00000008u
#define LX_IN_CLOSE_NOWRITE 0x00000010u
#define LX_IN_OPEN 0x00000020u
#define LX_IN_MOVED_FROM 0x00000040u
#define LX_IN_MOVED_TO 0x00000080u
#define LX_IN_CREATE 0x00000100u
#define LX_IN_DELETE 0x00000200u
#define LX_IN_DELETE_SELF 0x00000400u
#define LX_IN_MOVE_SELF 0x00000800u
#define LX_IN_UNMOUNT 0x00002000u
#define LX_IN_Q_OVERFLOW 0x00004000u
#define LX_IN_IGNORED 0x00008000u
#define LX_IN_ONLYDIR 0x01000000u
#define LX_IN_DONT_FOLLOW 0x02000000u
#define LX_IN_EXCL_UNLINK 0x04000000u
#define LX_IN_MASK_CREATE 0x10000000u
#define LX_IN_MASK_ADD 0x20000000u
#define LX_IN_ISDIR 0x40000000u
#define LX_IN_ONESHOT 0x80000000u
struct lx_inotify_event { int32_t wd; uint32_t mask, cookie, len; char name[]; };

/* ---- sockets ---------------------------------------------------------------------------------- */
#define LX_AF_UNSPEC 0
#define LX_AF_UNIX 1
#define LX_AF_LOCAL 1
#define LX_AF_INET 2
#define LX_AF_INET6 10
#define LX_AF_NETLINK 16
#define LX_AF_PACKET 17
#define LX_SOCK_STREAM 1
#define LX_SOCK_DGRAM 2
#define LX_SOCK_RAW 3
#define LX_SOCK_RDM 4
#define LX_SOCK_SEQPACKET 5
#define LX_SOCK_TYPE_MASK 0xf
#define LX_SOCK_NONBLOCK LX_O_NONBLOCK
#define LX_SOCK_CLOEXEC LX_O_CLOEXEC
#define LX_SHUT_RD 0
#define LX_SHUT_WR 1
#define LX_SHUT_RDWR 2
#define LX_SOMAXCONN 4096
#define LX_SOL_SOCKET 1
#define LX_SOL_IP 0
#define LX_SOL_TCP 6
#define LX_SOL_UDP 17
#define LX_SOL_IPV6 41
#define LX_IPPROTO_IP 0
#define LX_IPPROTO_ICMP 1
#define LX_IPPROTO_TCP 6
#define LX_IPPROTO_UDP 17
#define LX_IPPROTO_IPV6 41
#define LX_IPPROTO_ICMPV6 58
#define LX_SO_DEBUG 1
#define LX_SO_REUSEADDR 2
#define LX_SO_TYPE 3
#define LX_SO_ERROR 4
#define LX_SO_DONTROUTE 5
#define LX_SO_BROADCAST 6
#define LX_SO_SNDBUF 7
#define LX_SO_RCVBUF 8
#define LX_SO_KEEPALIVE 9
#define LX_SO_OOBINLINE 10
#define LX_SO_NO_CHECK 11
#define LX_SO_PRIORITY 12
#define LX_SO_LINGER 13
#define LX_SO_BSDCOMPAT 14
#define LX_SO_REUSEPORT 15
#define LX_SO_PASSCRED 16
#define LX_SO_PEERCRED 17
#define LX_SO_RCVLOWAT 18
#define LX_SO_SNDLOWAT 19
#define LX_SO_RCVTIMEO 20
#define LX_SO_SNDTIMEO 21
#define LX_SO_TIMESTAMP 29
#define LX_SO_ACCEPTCONN 30
#define LX_SO_PEERSEC 31
#define LX_SO_SNDBUFFORCE 32
#define LX_SO_RCVBUFFORCE 33
#define LX_SO_PASSSEC 34
#define LX_SO_PROTOCOL 38
#define LX_SO_DOMAIN 39
#define LX_TCP_NODELAY 1
#define LX_TCP_MAXSEG 2
#define LX_TCP_CORK 3
#define LX_TCP_KEEPIDLE 4
#define LX_TCP_KEEPINTVL 5
#define LX_TCP_KEEPCNT 6
#define LX_TCP_USER_TIMEOUT 18
#define LX_IP_TOS 1
#define LX_IP_TTL 2
#define LX_IP_PKTINFO 8
#define LX_IP_MULTICAST_IF 32
#define LX_IP_MULTICAST_TTL 33
#define LX_IP_MULTICAST_LOOP 34
#define LX_IP_ADD_MEMBERSHIP 35
#define LX_IP_DROP_MEMBERSHIP 36
#define LX_IPV6_UNICAST_HOPS 16
#define LX_IPV6_MULTICAST_IF 17
#define LX_IPV6_MULTICAST_HOPS 18
#define LX_IPV6_MULTICAST_LOOP 19
#define LX_IPV6_V6ONLY 26
#define LX_IP_RECVERR 11
#define LX_IPV6_RECVERR 25
#define LX_IP_MTU_DISCOVER 10
#define LX_IPV6_MTU_DISCOVER 23
#define LX_IPV6_DONTFRAG 62
#define LX_IP_PMTUDISC_DONT 0        /* the IPV6_PMTUDISC_* values are the same */
#define LX_IP_PMTUDISC_WANT 1
#define LX_IP_PMTUDISC_DO 2
#define LX_IP_PMTUDISC_PROBE 3
#define LX_IP_PMTUDISC_OMIT 5
#define LX_SO_EE_ORIGIN_NONE 0
#define LX_SO_EE_ORIGIN_LOCAL 1
#define LX_SO_EE_ORIGIN_ICMP 2
#define LX_SO_EE_ORIGIN_ICMP6 3
/* include/uapi/linux/errqueue.h; the offender sockaddr follows it in the IP_RECVERR control message */
struct lx_sock_extended_err {
    uint32_t ee_errno;
    uint8_t ee_origin, ee_type, ee_code, ee_pad;
    uint32_t ee_info, ee_data;
};
#define LX_SCM_RIGHTS 1
#define LX_SCM_CREDENTIALS 2
#define LX_MSG_OOB 0x1
#define LX_MSG_PEEK 0x2
#define LX_MSG_DONTROUTE 0x4
#define LX_MSG_CTRUNC 0x8
#define LX_MSG_PROXY 0x10
#define LX_MSG_TRUNC 0x20
#define LX_MSG_DONTWAIT 0x40
#define LX_MSG_EOR 0x80
#define LX_MSG_WAITALL 0x100
#define LX_MSG_FIN 0x200
#define LX_MSG_SYN 0x400
#define LX_MSG_CONFIRM 0x800
#define LX_MSG_RST 0x1000
#define LX_MSG_ERRQUEUE 0x2000
#define LX_MSG_NOSIGNAL 0x4000
#define LX_MSG_MORE 0x8000
#define LX_MSG_WAITFORONE 0x10000
#define LX_MSG_CMSG_CLOEXEC 0x40000000

struct lx_sockaddr { uint16_t sa_family; char sa_data[14]; };
struct lx_sockaddr_in { uint16_t sin_family; uint16_t sin_port; uint32_t sin_addr; uint8_t sin_zero[8]; };
struct lx_sockaddr_in6 { uint16_t sin6_family; uint16_t sin6_port; uint32_t sin6_flowinfo; uint8_t sin6_addr[16]; uint32_t sin6_scope_id; };
struct lx_sockaddr_un { uint16_t sun_family; char sun_path[108]; };
struct lx_sockaddr_nl { uint16_t nl_family; uint16_t nl_pad; uint32_t nl_pid; uint32_t nl_groups; };
struct lx_sockaddr_storage { uint16_t ss_family; char _data[126]; } __attribute__((aligned(8)));
struct lx_ucred { int32_t pid; uint32_t uid; uint32_t gid; };
struct lx_linger { int32_t l_onoff; int32_t l_linger; };
struct lx_msghdr { /* 56 bytes */
    uint64_t msg_name;
    int32_t msg_namelen;
    int32_t _pad0;
    uint64_t msg_iov;
    uint64_t msg_iovlen;
    uint64_t msg_control;
    uint64_t msg_controllen;
    int32_t msg_flags;
    int32_t _pad1;
};
struct lx_cmsghdr { uint64_t cmsg_len; int32_t cmsg_level; int32_t cmsg_type; };
struct lx_mmsghdr { struct lx_msghdr msg_hdr; uint32_t msg_len; uint32_t _pad; };
#define LX_CMSG_ALIGN(len) (((len) + sizeof(uint64_t) - 1) & ~(sizeof(uint64_t) - 1))
#define LX_CMSG_LEN(len) (sizeof(struct lx_cmsghdr) + (len))
#define LX_CMSG_SPACE(len) (sizeof(struct lx_cmsghdr) + LX_CMSG_ALIGN(len))

/* ---- ptrace ----------------------------------------------------------------------------------- */
#define LX_PTRACE_TRACEME 0
#define LX_PTRACE_PEEKTEXT 1
#define LX_PTRACE_PEEKDATA 2
#define LX_PTRACE_PEEKUSER 3
#define LX_PTRACE_POKETEXT 4
#define LX_PTRACE_POKEDATA 5
#define LX_PTRACE_POKEUSER 6
#define LX_PTRACE_CONT 7
#define LX_PTRACE_KILL 8
#define LX_PTRACE_SINGLESTEP 9
#define LX_PTRACE_GETREGS 12
#define LX_PTRACE_SETREGS 13
#define LX_PTRACE_GETFPREGS 14
#define LX_PTRACE_SETFPREGS 15
#define LX_PTRACE_ATTACH 16
#define LX_PTRACE_DETACH 17
#define LX_PTRACE_GETFPXREGS 18
#define LX_PTRACE_SETFPXREGS 19
#define LX_PTRACE_SYSCALL 24
#define LX_PTRACE_GET_THREAD_AREA 25
#define LX_PTRACE_SET_THREAD_AREA 26
#define LX_PTRACE_ARCH_PRCTL 30
#define LX_PTRACE_SYSEMU 31
#define LX_PTRACE_SYSEMU_SINGLESTEP 32
#define LX_PTRACE_SINGLEBLOCK 33
#define LX_PTRACE_SETOPTIONS 0x4200
#define LX_PTRACE_GETEVENTMSG 0x4201
#define LX_PTRACE_GETSIGINFO 0x4202
#define LX_PTRACE_SETSIGINFO 0x4203
#define LX_PTRACE_GETREGSET 0x4204
#define LX_PTRACE_SETREGSET 0x4205
#define LX_PTRACE_SEIZE 0x4206
#define LX_PTRACE_INTERRUPT 0x4207
#define LX_PTRACE_LISTEN 0x4208
#define LX_PTRACE_PEEKSIGINFO 0x4209
#define LX_PTRACE_GETSIGMASK 0x420a
#define LX_PTRACE_SETSIGMASK 0x420b
#define LX_PTRACE_SECCOMP_GET_FILTER 0x420c
#define LX_PTRACE_SECCOMP_GET_METADATA 0x420d
#define LX_PTRACE_GET_SYSCALL_INFO 0x420e
#define LX_PTRACE_GET_RSEQ_CONFIGURATION 0x420f
#define LX_PTRACE_SET_SYSCALL_USER_DISPATCH_CONFIG 0x4210
#define LX_PTRACE_GET_SYSCALL_USER_DISPATCH_CONFIG 0x4211
#define LX_PTRACE_O_TRACESYSGOOD 0x00000001
#define LX_PTRACE_O_TRACEFORK 0x00000002
#define LX_PTRACE_O_TRACEVFORK 0x00000004
#define LX_PTRACE_O_TRACECLONE 0x00000008
#define LX_PTRACE_O_TRACEEXEC 0x00000010
#define LX_PTRACE_O_TRACEVFORKDONE 0x00000020
#define LX_PTRACE_O_TRACEEXIT 0x00000040
#define LX_PTRACE_O_TRACESECCOMP 0x00000080
#define LX_PTRACE_O_EXITKILL 0x00100000
#define LX_PTRACE_O_SUSPEND_SECCOMP 0x00200000
#define LX_PTRACE_O_MASK 0x003000ff
#define LX_PTRACE_EVENT_FORK 1
#define LX_PTRACE_EVENT_VFORK 2
#define LX_PTRACE_EVENT_CLONE 3
#define LX_PTRACE_EVENT_EXEC 4
#define LX_PTRACE_EVENT_VFORK_DONE 5
#define LX_PTRACE_EVENT_EXIT 6
#define LX_PTRACE_EVENT_SECCOMP 7
#define LX_PTRACE_EVENT_STOP 128
#define LX_PTRACE_SYSCALL_INFO_NONE 0
#define LX_PTRACE_SYSCALL_INFO_ENTRY 1
#define LX_PTRACE_SYSCALL_INFO_EXIT 2
#define LX_PTRACE_SYSCALL_INFO_SECCOMP 3
#define LX_AUDIT_ARCH_X86_64 0xc000003eu

/* seccomp(2) (include/uapi/linux/seccomp.h) and the classic BPF it runs (include/uapi/linux/filter.h,
 * bpf_common.h). */
#define LX_SECCOMP_MODE_DISABLED 0
#define LX_SECCOMP_MODE_STRICT 1
#define LX_SECCOMP_MODE_FILTER 2
#define LX_SECCOMP_SET_MODE_STRICT 0
#define LX_SECCOMP_SET_MODE_FILTER 1
#define LX_SECCOMP_GET_ACTION_AVAIL 2
#define LX_SECCOMP_GET_NOTIF_SIZES 3
#define LX_SECCOMP_FILTER_FLAG_TSYNC (1u << 0)
#define LX_SECCOMP_FILTER_FLAG_LOG (1u << 1)
#define LX_SECCOMP_FILTER_FLAG_SPEC_ALLOW (1u << 2)
#define LX_SECCOMP_FILTER_FLAG_NEW_LISTENER (1u << 3)
#define LX_SECCOMP_FILTER_FLAG_TSYNC_ESRCH (1u << 4)
#define LX_SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV (1u << 5)
#define LX_SECCOMP_RET_KILL_PROCESS 0x80000000u
#define LX_SECCOMP_RET_KILL_THREAD 0x00000000u
#define LX_SECCOMP_RET_TRAP 0x00030000u
#define LX_SECCOMP_RET_ERRNO 0x00050000u
#define LX_SECCOMP_RET_USER_NOTIF 0x7fc00000u
#define LX_SECCOMP_RET_TRACE 0x7ff00000u
#define LX_SECCOMP_RET_LOG 0x7ffc0000u
#define LX_SECCOMP_RET_ALLOW 0x7fff0000u
#define LX_SECCOMP_RET_ACTION_FULL 0xffff0000u
#define LX_SECCOMP_RET_DATA 0x0000ffffu
#define LX_SYS_SECCOMP 1 /* si_code of the SIGSYS a SECCOMP_RET_TRAP sends */
#define LX_BPF_MAXINSNS 4096
#define LX_BPF_MEMWORDS 16

struct lx_sock_filter {
    uint16_t code;
    uint8_t jt, jf;
    uint32_t k;
};
struct lx_sock_fprog {
    uint16_t len;
    uint8_t _pad[6];
    uint64_t filter; /* guest address of len struct lx_sock_filter */
};
struct lx_seccomp_data {
    int32_t nr;
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t args[6];
};
struct lx_seccomp_notif_sizes {
    uint16_t seccomp_notif, seccomp_notif_resp, seccomp_data;
};
_Static_assert(sizeof(struct lx_sock_filter) == 8, "sock_filter");
_Static_assert(sizeof(struct lx_sock_fprog) == 16, "sock_fprog");
_Static_assert(sizeof(struct lx_seccomp_data) == 64, "seccomp_data");
#define LX_NT_PRSTATUS 1
#define LX_NT_PRFPREG 2
#define LX_NT_PRPSINFO 3
#define LX_NT_PRXFPREG 0x46e62b7fu
#define LX_NT_X86_XSTATE 0x202
#define LX_NT_X86_IO_BITMAP 0x201
#define LX_NT_SIGINFO 0x53494749
#define LX_PTRACE_USER_REGS_BYTES 216
#define LX_PTRACE_FPREGS_BYTES 512
#define LX_PTRACE_USER_STRUCT_BYTES 928 /* sizeof(struct user) on x86-64, PEEKUSER/POKEUSER range */

/* struct user_regs_struct (x86-64): 27 words, 216 bytes. Index = word number = offsetof / 8. */
struct lx_user_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rax, rcx, rdx, rsi, rdi, orig_rax;
    uint64_t rip, cs, eflags, rsp, ss;
    uint64_t fs_base, gs_base, ds, es, fs, gs;
};
#define LX_REG_R15 0
#define LX_REG_R14 1
#define LX_REG_R13 2
#define LX_REG_R12 3
#define LX_REG_RBP 4
#define LX_REG_RBX 5
#define LX_REG_R11 6
#define LX_REG_R10 7
#define LX_REG_R9 8
#define LX_REG_R8 9
#define LX_REG_RAX 10
#define LX_REG_RCX 11
#define LX_REG_RDX 12
#define LX_REG_RSI 13
#define LX_REG_RDI 14
#define LX_REG_ORIG_RAX 15
#define LX_REG_RIP 16
#define LX_REG_CS 17
#define LX_REG_EFLAGS 18
#define LX_REG_RSP 19
#define LX_REG_SS 20
#define LX_REG_FS_BASE 21
#define LX_REG_GS_BASE 22
#define LX_REG_DS 23
#define LX_REG_ES 24
#define LX_REG_FS 25
#define LX_REG_GS 26
#define LX_NUM_USER_REGS 27

/* struct user_fpregs_struct (FXSAVE image), 512 bytes. */
struct lx_user_fpregs {
    uint16_t cwd, swd, ftw, fop;
    uint64_t rip, rdp;
    uint32_t mxcsr, mxcr_mask;
    uint32_t st_space[32];
    uint32_t xmm_space[64];
    uint32_t padding[24];
};

/* Legacy fxsave/xsave area header as embedded in the signal frame (fpstate), 512 bytes + xsave header. */
struct lx_fpx_sw_bytes { uint32_t magic1, extended_size; uint64_t xfeatures; uint32_t xstate_size; uint32_t _padding[7]; };

struct lx_ptrace_syscall_info {
    uint8_t op;
    uint8_t _pad[3];
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t stack_pointer;
    union {
        struct { uint64_t nr; uint64_t args[6]; } entry;
        struct { int64_t rval; uint8_t is_error; } exit;
        struct { uint64_t nr; uint64_t args[6]; uint32_t ret_data; } seccomp;
    } u;
};
struct lx_ptrace_peeksiginfo_args { uint64_t off; uint32_t flags; int32_t nr; };

/* ---- layout checks ---------------------------------------------------------------------------- */
_Static_assert(sizeof(struct lx_sigaction) == 32, "kernel sigaction");
_Static_assert(sizeof(struct lx_siginfo) == 128, "siginfo");
_Static_assert(offsetof(struct lx_siginfo, u) == 16, "siginfo union offset");
_Static_assert(offsetof(struct lx_siginfo, u.chld.status) == 24, "si_status");
_Static_assert(offsetof(struct lx_siginfo, u.fault.addr) == 16, "si_addr");
_Static_assert(sizeof(struct lx_stack_t) == 24, "stack_t");
_Static_assert(sizeof(struct lx_sigcontext) == 256, "sigcontext");
_Static_assert(offsetof(struct lx_sigcontext, rip) == 128, "sigcontext.rip");
_Static_assert(offsetof(struct lx_sigcontext, eflags) == 136, "sigcontext.eflags");
_Static_assert(offsetof(struct lx_sigcontext, err) == 152, "sigcontext.err");
_Static_assert(offsetof(struct lx_sigcontext, cr2) == 176, "sigcontext.cr2");
_Static_assert(offsetof(struct lx_sigcontext, fpstate) == 184, "sigcontext.fpstate");
_Static_assert(offsetof(struct lx_ucontext, uc_stack) == 16, "ucontext.uc_stack");
_Static_assert(offsetof(struct lx_ucontext, uc_mcontext) == 40, "ucontext.uc_mcontext");
_Static_assert(offsetof(struct lx_ucontext, uc_sigmask) == 296, "ucontext.uc_sigmask");
_Static_assert(sizeof(struct lx_ucontext) == 304, "kernel struct ucontext (glibc ucontext_t is larger)");
_Static_assert(offsetof(struct lx_rt_sigframe, uc) == 8, "rt_sigframe.uc");
_Static_assert(offsetof(struct lx_rt_sigframe, info) == 312, "rt_sigframe.info");
_Static_assert(sizeof(struct lx_rt_sigframe) == 440, "rt_sigframe");
_Static_assert(sizeof(struct lx_signalfd_siginfo) == 128, "signalfd_siginfo");
_Static_assert(sizeof(struct lx_clone_args) == LX_CLONE_ARGS_SIZE_VER2, "clone_args");
_Static_assert(sizeof(struct lx_timespec) == 16 && sizeof(struct lx_timeval) == 16, "time");
_Static_assert(sizeof(struct lx_itimerspec) == 32 && sizeof(struct lx_itimerval) == 32, "itimer");
_Static_assert(sizeof(struct lx_tms) == 32, "tms");
_Static_assert(sizeof(struct lx_rlimit) == 16, "rlimit");
_Static_assert(sizeof(struct lx_rusage) == 144, "rusage");
_Static_assert(sizeof(struct lx_utsname) == 390, "utsname");
_Static_assert(sizeof(struct lx_sysinfo) == 112, "sysinfo");
_Static_assert(offsetof(struct lx_sysinfo, totalhigh) == 88, "sysinfo.totalhigh");
_Static_assert(sizeof(struct lx_robust_list_head) == 24, "robust_list_head");
_Static_assert(sizeof(struct lx_iovec) == 16, "iovec");
_Static_assert(sizeof(struct lx_futex_waitv) == 24, "futex_waitv");
_Static_assert(sizeof(struct lx_stat) == 144, "stat");
_Static_assert(offsetof(struct lx_stat, st_mode) == 24, "stat.st_mode");
_Static_assert(offsetof(struct lx_stat, st_rdev) == 40, "stat.st_rdev");
_Static_assert(offsetof(struct lx_stat, st_size) == 48, "stat.st_size");
_Static_assert(offsetof(struct lx_stat, st_atime_sec) == 72, "stat.st_atime");
_Static_assert(offsetof(struct lx_stat, st_mtime_sec) == 88, "stat.st_mtime");
_Static_assert(offsetof(struct lx_stat, st_ctime_sec) == 104, "stat.st_ctime");
_Static_assert(sizeof(struct lx_statx) == 256, "statx");
_Static_assert(offsetof(struct lx_statx, stx_atime) == 64, "statx.stx_atime");
_Static_assert(offsetof(struct lx_statx, stx_mtime) == 112, "statx.stx_mtime");
_Static_assert(offsetof(struct lx_statx, stx_rdev_major) == 128, "statx.stx_rdev_major");
_Static_assert(offsetof(struct lx_statx, stx_subvol) == 160, "statx.stx_subvol");
_Static_assert(sizeof(struct lx_statfs) == 120, "statfs");
_Static_assert(offsetof(struct lx_linux_dirent64, d_name) == 19, "dirent64");
_Static_assert(sizeof(struct lx_flock) == 32, "flock");
_Static_assert(sizeof(struct lx_termios) == 36, "termios");
_Static_assert(sizeof(struct lx_pollfd) == 8, "pollfd");
_Static_assert(sizeof(struct lx_epoll_event) == 12, "epoll_event packed");
_Static_assert(offsetof(struct lx_epoll_event, data) == 4, "epoll_event.data");
_Static_assert(sizeof(struct lx_inotify_event) == 16, "inotify_event");
_Static_assert(sizeof(struct lx_sockaddr_in) == 16 && sizeof(struct lx_sockaddr_in6) == 28, "sockaddr_in*");
_Static_assert(sizeof(struct lx_sockaddr_un) == 110 && sizeof(struct lx_sockaddr_nl) == 12, "sockaddr_un/nl");
_Static_assert(sizeof(struct lx_sockaddr_storage) == 128, "sockaddr_storage");
_Static_assert(sizeof(struct lx_ucred) == 12, "ucred");
_Static_assert(sizeof(struct lx_msghdr) == 56, "msghdr");
_Static_assert(sizeof(struct lx_cmsghdr) == 16, "cmsghdr");
_Static_assert(sizeof(struct lx_mmsghdr) == 64, "mmsghdr");
_Static_assert(sizeof(struct lx_user_regs) == 216, "user_regs_struct");
_Static_assert(offsetof(struct lx_user_regs, rax) == 80, "user_regs.rax");
_Static_assert(offsetof(struct lx_user_regs, orig_rax) == 120, "user_regs.orig_rax");
_Static_assert(offsetof(struct lx_user_regs, rip) == 128, "user_regs.rip");
_Static_assert(offsetof(struct lx_user_regs, rsp) == 152, "user_regs.rsp");
_Static_assert(offsetof(struct lx_user_regs, fs_base) == 168, "user_regs.fs_base");
_Static_assert(offsetof(struct lx_user_regs, gs) == 208, "user_regs.gs");
_Static_assert(sizeof(struct lx_user_fpregs) == 512, "user_fpregs_struct");
_Static_assert(offsetof(struct lx_user_fpregs, mxcsr) == 24, "fpregs.mxcsr");
_Static_assert(offsetof(struct lx_user_fpregs, xmm_space) == 160, "fpregs.xmm_space");
_Static_assert(sizeof(struct lx_ptrace_syscall_info) == 88, "ptrace_syscall_info");
_Static_assert(sizeof(struct lx_ptrace_peeksiginfo_args) == 16, "peeksiginfo_args");

#endif /* LKD_ABI_X64_H */
