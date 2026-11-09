// Witness: 32-bit code in a 64-bit process, the way Wine's new WoW64 runs 32-bit Windows programs
// (dlls/wow64cpu/cpu.c, dlls/ntdll/unix/signal_x86_64.c). Code segment 0x23 entered and left with wow64cpu's transfers
// (iretq, ljmp through memory, lcall through memory as in CrossOver's Rosetta thunk, lretq), x87 code in 32-bit mode,
// and int $0x80 set_thread_area from 64-bit code, which gives each thread's 32-bit TEB a GDT TLS selector for %fs
// (alloc_fs_sel), with the checks of arch/x86/kernel/tls.c. Also get_thread_area, an entry cleared, the reload of a
// segment register that holds the changed entry, the entry kept by a forked child and gone after an execve
// (flush_thread), and SIGSEGV for any other int $0x80, as on a kernel without IA32 emulation.
// Build with EXEC=1: the 32-bit code, its stack and the user_desc must be below 4 GiB. Linux guest only (int $0x80,
// iretq to selector 0x23, an execve of itself): no WITNESS_HOSTED build.
#include "rt.h"

// 32-bit code and the 64-bit trampolines into it. call32(entry) enters 32-bit mode at entry with iretq on a stack below
// 4 GiB; the 32-bit code stores its result in result32 and jumps back through far_ret64. call32_ljmp() enters with
// ljmp *(%r14) instead, leaves with lcall (the 64-bit side pops the far return address), comes back with lretq and
// leaves for good with ljmp.
extern u32 call32(u64 entry);
extern u32 call32_ljmp(void);
extern char c32_x87[], c32_fs[], c32_fs_area[], c32_i80_end[];
u32 sel32, desc32;
u32 result32;
__asm__(
  ".text\n"
  ".globl call32\n"
  "call32:\n"
  "  push %rbx\n  push %rbp\n  push %r12\n  push %r13\n  push %r14\n  push %r15\n"
  "  mov %rsp, saved64(%rip)\n"
  "  lea stack32_top(%rip), %rax\n"
  "  pushq $0x2b\n  pushq %rax\n  pushfq\n  pushq $0x23\n  pushq %rdi\n"
  "  iretq\n"
  "ret64:\n"
  "  mov saved64(%rip), %rsp\n"
  "  mov result32(%rip), %eax\n"
  "  pop %r15\n  pop %r14\n  pop %r13\n  pop %r12\n  pop %rbp\n  pop %rbx\n"
  "  ret\n"
  ".globl call32_ljmp\n"
  "call32_ljmp:\n"
  "  push %rbx\n  push %rbp\n  push %r12\n  push %r13\n  push %r14\n  push %r15\n"
  "  mov %rsp, saved64(%rip)\n"
  "  lea stack32_top(%rip), %rsp\n"
  "  lea far_c32_lcall(%rip), %r14\n"
  "  ljmp *(%r14)\n"
  "back64_lcall:\n"
  "  add $8, %esp\n"                       // the 32-bit CS:EIP the lcall pushed
  "  pushq $0x23\n"
  "  lea c32_after(%rip), %rax\n"
  "  pushq %rax\n"
  "  lretq\n"
  ".code32\n"
  ".globl c32_x87\n"
  "c32_x87:\n"                             // 1 + 50000 additions with a square root each, then fistp: 50001
  "  mov $50000, %ecx\n"
  "  fld1\n"
  "1:fld1\n"
  "  faddp\n"
  "  fld %st(0)\n"
  "  fsqrt\n"
  "  fstp %st(0)\n"
  "  loop 1b\n"
  "  fistpl result32\n"
  "  ljmp *far_ret64\n"
  ".globl c32_fs\n"
  "c32_fs:\n"                              // %fs:0 through the selector in sel32, then the null selector again
  "  movl sel32, %eax\n"
  "  mov %ax, %fs\n"
  "  movl %fs:0, %eax\n"
  "  movl %eax, result32\n"
  "  xor %eax, %eax\n"
  "  mov %ax, %fs\n"
  "  ljmp *far_ret64\n"
  ".globl c32_fs_area\n"
  "c32_fs_area:\n"                         // %fs loaded from sel32, int $0x80 set_thread_area(desc32), then %fs:0
  "  movl sel32, %eax\n"
  "  mov %ax, %fs\n"
  "  movl $243, %eax\n"
  "  movl desc32, %ebx\n"
  "  int $0x80\n"
  ".globl c32_i80_end\n"
  "c32_i80_end:\n"
  "  movl %fs:0, %eax\n"
  "  movl %eax, result32\n"
  "  xor %eax, %eax\n"
  "  mov %ax, %fs\n"
  "  ljmp *far_ret64\n"
  "c32_lcall:\n"
  "  movl $7, result32\n"
  "  lcall *far_back64_lcall\n"
  "c32_after:\n"
  "  addl $35, result32\n"
  "  ljmp *far_ret64\n"
  ".code64\n"
  ".data\n"
  ".p2align 3\n"
  "far_ret64: .long ret64\n .word 0x33\n"
  "far_back64_lcall: .long back64_lcall\n .word 0x33\n"
  "far_c32_lcall: .long c32_lcall\n .word 0x23\n"
  ".bss\n"
  ".p2align 4\n"
  "stack32: .skip 16384\n"
  "stack32_top:\n"
  "saved64: .quad 0\n"
  ".text\n");

// 64-bit code that may fault: the signal handler goes on at the label after the instruction, so an engine that gets it
// wrong gives FAIL lines instead of dying. int80_call(nr, arg) is int $0x80 with nr in eax and arg in ebx.
extern char i80_at[], i80_end[];
__asm__(".text\n"
  ".globl int80_call\nint80_call: push %rbx\n mov %edi, %eax\n mov %esi, %ebx\n.globl i80_at\ni80_at: int $0x80\n"
  ".globl i80_end\ni80_end: pop %rbx\n ret\n");
i32 int80_call(u32 nr, u32 arg);

#define SIGILL_ 4
#define SIGSEGV_ 11
#define SA_SIGINFO_ 4
#define SA_RESTORER_ 0x04000000
#define SA_NODEFER_ 0x40000000
struct ksigaction { u64 handler, flags, restorer, mask; };
struct stack_t_ { u64 sp; i32 flags; i32 pad; u64 size; };
struct sigcontext_ { u64 r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags; u16 cs, gs, fs, ss; u64 err, trapno, oldmask, cr2, fpstate, reserved[8]; };
struct ucontext_ { u64 flags, link; struct stack_t_ stack; struct sigcontext_ mc; u64 sigmask; };
__asm__(".text\n.globl restorer\nrestorer:\n\tmovl $15, %eax\n\tsyscall\n");
void restorer(void);

static volatile int last_sig;
static volatile u64 last_rip, last_err, last_trapno, resume_rip;
static void on_fault(int sig, void* si, struct ucontext_* uc) {
  (void)si;
  last_sig = sig; last_rip = uc->mc.rip; last_err = uc->mc.err; last_trapno = uc->mc.trapno;
  if (!resume_rip) {
    puts_("FAIL unexpected signal "); put_dec(sig); puts_(" at rip "); put_hex(uc->mc.rip, 12); puts_("\n");
    exit_group(3);
  }
  uc->mc.rip = resume_rip;
}

// struct user_desc (asm/ldt.h); flags 0x41 = seg_32bit | useable, as alloc_fs_sel passes.
struct user_desc { u32 entry_number, base_addr, limit, flags; };
static struct user_desc desc;
static u32 teb_a = 0x7eb00001, teb_b = 0x7eb00002;

static i32 int80(u32 nr, u32 arg) {
  last_sig = 0; last_rip = 0; last_err = 0; last_trapno = 0; resume_rip = (u64)i80_end;
  i32 r = int80_call(nr, arg);
  resume_rip = 0;
  return r;
}
enum { IA32_getpid = 20, IA32_set_thread_area = 243, IA32_get_thread_area = 244 };
#define SYS_fork_ 57
#define SYS_execve_ 59
#define SYS_wait4_ 61

// The limit is ~0, as alloc_fs_sel passes it; the descriptor keeps its low 20 bits.
static i32 set_area(u32 entry, u32 base, u32 flags) {
  desc = (struct user_desc){entry, base, 0xffffffffu, flags};
  return int80(IA32_set_thread_area, (u32)(u64)&desc);
}
// Base of TLS entry e, or 0 when it is empty.
static u32 tls_base(u32 e) {
  desc = (struct user_desc){e, 0, 0, 0};
  int80(IA32_get_thread_area, (u32)(u64)&desc);
  return desc.base_addr;
}

// Exit status of a child that runs fn (fork), or of this program run again with one argument (fork and execve).
static int child_status(int (*fn)(void)) {
  i64 pid = sys0(SYS_fork_);
  if (pid == 0) {
    if (fn) exit_group(fn());
    static const char* argv[] = {"/bin-exec/w39_wow64", "exec-child", 0};
    sys3(SYS_execve_, (i64)argv[0], (i64)argv, 0);
    exit_group(99);
  }
  int st = 0;
  if (pid < 0 || sys4(SYS_wait4_, pid, (i64)&st, 0, 0) != pid || (st & 0x7f)) return -1;
  return (st >> 8) & 0xff;
}
static u32 g_entry;
static int forked_child(void) { return tls_base(g_entry) == (u32)(u64)&teb_a ? 7 : 9; }
// The execve child (one argument): no TLS entry.
static int exec_child(void) {
  for (u32 e = 12; e <= 14; e++) if (tls_base(e)) return 9;
  return 7;
}

static int run(void) {
  puts_("w39_wow64: 32-bit code segment, WoW64 transfers, int $0x80 set_thread_area\n");
  checkb("the witness runs below 4 GiB (EXEC=1)", (u64)&desc < (1ull << 32) && (u64)c32_x87 < (1ull << 32));
  struct ksigaction sa = {(u64)on_fault, SA_SIGINFO_ | SA_RESTORER_ | SA_NODEFER_, (u64)restorer, 0};
  sys4(SYS_rt_sigaction, SIGSEGV_, (i64)&sa, 0, 8);
  sys4(SYS_rt_sigaction, SIGILL_, (i64)&sa, 0, 8);

  check64("iretq into 32-bit code, x87 there, ljmp back", call32((u64)c32_x87), 50001);
  check64("ljmp *(%r14) in, lcall out, lretq in, ljmp out", call32_ljmp(), 42);

  // set_thread_area and get_thread_area as arch/x86/kernel/tls.c serves them.
  check64("int $0x80 set_thread_area(-1) succeeds", (u64)(i64)set_area((u32)-1, (u32)(u64)&teb_a, 0x41), 0);
  u32 entry = desc.entry_number;
  checkb("it allocated a TLS entry of the GDT (12 to 14)", entry >= 12 && entry <= 14);
  sel32 = (entry << 3) | 3;
  check64("%fs:0 in 32-bit code reads the TEB", call32((u64)c32_fs), teb_a);

  // A new thread reuses the selector with its own TEB (alloc_fs_sel(fs32_sel >> 3, teb)).
  check64("set_thread_area(entry) rewrites it", (u64)(i64)set_area(entry, (u32)(u64)&teb_b, 0x41), 0);
  check64("%fs:0 reads the new base", call32((u64)c32_fs), teb_b);

  desc = (struct user_desc){entry, 0, 0, 0};
  check64("get_thread_area succeeds", (u64)(i64)int80(IA32_get_thread_area, (u32)(u64)&desc), 0);
  check64("get_thread_area: base", desc.base_addr, (u32)(u64)&teb_b);
  check64("get_thread_area: limit", desc.limit, 0xfffff);
  check64("get_thread_area: seg_32bit and useable", desc.flags, 0x41);

  // int $0x80 from 32-bit code, while %fs holds the entry it rewrites: do_set_thread_area reloads %fs.
  desc = (struct user_desc){entry, (u32)(u64)&teb_a, 0xffffffffu, 0x41};
  desc32 = (u32)(u64)&desc;
  resume_rip = (u64)c32_i80_end;
  check64("int $0x80 set_thread_area in 32-bit code reloads the %fs that holds the entry", call32((u64)c32_fs_area), teb_a);
  resume_rip = 0;

  check64("seg_not_present is EINVAL", (u64)(i64)set_area(entry, (u32)(u64)&teb_a, 0x41 | 0x20), (u64)-22);
  desc = (struct user_desc){(u32)-1, (u32)(u64)&teb_a, 0xffffffffu, 0x40}; // a 16-bit segment
  check64("a 16-bit segment is EINVAL", (u64)(i64)int80(IA32_set_thread_area, (u32)(u64)&desc), (u64)-22);
  check64("... checked before an entry is allocated and written back", desc.entry_number, 0xffffffffu);
  check64("lm is accepted", (u64)(i64)set_area(entry, (u32)(u64)&teb_a, 0x41 | 0x80), 0);
  desc = (struct user_desc){entry, 0, 0, 0};
  int80(IA32_get_thread_area, (u32)(u64)&desc);
  check64("... and not set (get_thread_area: no lm)", desc.flags, 0x41);
  desc = (struct user_desc){entry, 0, 0, 0x80}; // LDT_zero, which ignores lm
  check64("an all-zero descriptor with lm clears the entry",
          (u64)(i64)int80(IA32_set_thread_area, (u32)(u64)&desc) | tls_base(entry), 0);
  desc = (struct user_desc){entry, 0, 0, 0x28}; // LDT_empty: read_exec_only | seg_not_present
  check64("an empty descriptor clears the entry", (u64)(i64)int80(IA32_set_thread_area, (u32)(u64)&desc), 0);
  desc = (struct user_desc){entry, 0, 0, 0};
  int80(IA32_get_thread_area, (u32)(u64)&desc);
  check64("get_thread_area: the cleared entry reads back empty", ((u64)desc.base_addr << 32) | desc.flags, 0x28);
  check64("an entry outside 12 to 14 is EINVAL", (u64)(i64)set_area(11, (u32)(u64)&teb_a, 0x41), (u64)-22);

  int80(IA32_getpid, 0);
  checkb("int $0x80 getpid: SIGSEGV at the int, #GP with IDT vector 0x80 (no IA32 emulation)",
         last_sig == SIGSEGV_ && last_rip == (u64)i80_at && last_trapno == 13 && last_err == 0x402);

  set_area((u32)-1, (u32)(u64)&teb_a, 0x41);
  g_entry = desc.entry_number;
  check64("a forked child keeps the TLS entry", (u64)child_status(forked_child), 7);
  check64("after execve: no TLS entry", (u64)child_status(0), 7);
  return finish("w39_wow64");
}

__attribute__((used, noreturn)) static void start_c(u64* initial_sp) {
  exit_group(initial_sp[0] > 1 ? exec_child() : run());
  __builtin_unreachable();
}
__asm__(".text\n.globl _start\n_start:\n\txor %ebp,%ebp\n\tmov %rsp,%rdi\n\tand $-16,%rsp\n\tcall start_c\n\thlt\n");
