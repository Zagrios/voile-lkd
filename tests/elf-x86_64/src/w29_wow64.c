// Witness: 32-bit code in a 64-bit process, the way Wine's new WoW64 runs 32-bit Windows programs
// (dlls/wow64cpu/cpu.c, dlls/ntdll/unix/signal_x86_64.c). Code segment 0x23 entered and left with wow64cpu's transfers
// (iretq, ljmp through memory, lcall through memory as in CrossOver's Rosetta thunk, lretq), x87 code in 32-bit mode,
// and int $0x80 set_thread_area from 64-bit code, which gives each thread's 32-bit TEB a GDT TLS selector for %fs
// (alloc_fs_sel). Also get_thread_area, an entry cleared, ENOSYS for a 32-bit call the engine does not serve, the entry
// kept by a forked child and gone after an execve (flush_thread).
// Build with EXEC=1: the 32-bit code, its stack and the user_desc must be below 4 GiB.
#include "rt.h"

// 32-bit code and the 64-bit trampolines into it. call32(entry) enters 32-bit mode at entry with iretq on a stack below
// 4 GiB; the 32-bit code stores its result in result32 and jumps back through far_ret64. call32_ljmp() enters with
// ljmp *(%r14) instead, leaves with lcall (the 64-bit side pops the far return address), comes back with lretq and
// leaves for good with ljmp.
extern u32 call32(u64 entry);
extern u32 call32_ljmp(void);
extern char c32_x87[], c32_fs[];
u32 sel32;
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

// struct user_desc (asm/ldt.h); flags 0x41 = seg_32bit | useable, as alloc_fs_sel passes.
struct user_desc { u32 entry_number, base_addr, limit, flags; };
static struct user_desc desc;
static u32 teb_a = 0x7eb00001, teb_b = 0x7eb00002;

static i32 int80(u32 nr, u32 arg) {
  i32 r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(arg) : "memory");
  return r;
}
enum { IA32_getpid = 20, IA32_set_thread_area = 243, IA32_get_thread_area = 244 };
#define SYS_fork_ 57
#define SYS_execve_ 59
#define SYS_wait4_ 61

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
    static const char* argv[] = {"/bin-exec/w29_wow64", "exec-child", 0};
    sys3(SYS_execve_, (i64)argv[0], (i64)argv, 0);
    exit_group(99);
  }
  int st = 0;
  if (pid < 0 || sys4(SYS_wait4_, pid, (i64)&st, 0, 0) != pid || (st & 0x7f)) return -1;
  return (st >> 8) & 0xff;
}
static u32 g_entry;
static int forked_child(void) { return tls_base(g_entry) == (u32)(u64)&teb_a ? 7 : 9; }
static int exec_child(void) {
  for (u32 e = 12; e <= 14; e++) if (tls_base(e)) return 9;
  return 7;
}

static int run(void) {
  puts_("w29_wow64: 32-bit code segment, WoW64 transfers, int $0x80 set_thread_area\n");
  checkb("the witness runs below 4 GiB (EXEC=1)", (u64)&desc < (1ull << 32) && (u64)c32_x87 < (1ull << 32));

  check64("iretq into 32-bit code, x87 there, ljmp back", call32((u64)c32_x87), 50001);
  check64("ljmp *(%r14) in, lcall out, lretq in, ljmp out", call32_ljmp(), 42);

  desc = (struct user_desc){(u32)-1, (u32)(u64)&teb_a, 0xffffffffu, 0x41};
  check64("int $0x80 set_thread_area(-1) succeeds", (u64)(i64)int80(IA32_set_thread_area, (u32)(u64)&desc), 0);
  u32 entry = desc.entry_number;
  checkb("it allocated a TLS entry of the GDT (12 to 14)", entry >= 12 && entry <= 14);
  sel32 = (entry << 3) | 3;
  check64("%fs:0 in 32-bit code reads the TEB", call32((u64)c32_fs), teb_a);

  // A new thread reuses the selector with its own TEB (alloc_fs_sel(fs32_sel >> 3, teb)).
  desc = (struct user_desc){entry, (u32)(u64)&teb_b, 0xffffffffu, 0x41};
  check64("set_thread_area(entry) rewrites it", (u64)(i64)int80(IA32_set_thread_area, (u32)(u64)&desc), 0);
  check64("%fs:0 reads the new base", call32((u64)c32_fs), teb_b);

  struct user_desc got = {entry, 0, 0, 0};
  desc = got;
  check64("get_thread_area succeeds", (u64)(i64)int80(IA32_get_thread_area, (u32)(u64)&desc), 0);
  check64("get_thread_area: base", desc.base_addr, (u32)(u64)&teb_b);
  check64("get_thread_area: seg_32bit and useable", desc.flags & 0x41, 0x41);

  desc = (struct user_desc){entry, 0, 0, 0x28}; // LDT_empty: read_exec_only | seg_not_present
  check64("an empty descriptor clears the entry", (u64)(i64)int80(IA32_set_thread_area, (u32)(u64)&desc), 0);
  desc = (struct user_desc){entry, 0, 0, 0};
  int80(IA32_get_thread_area, (u32)(u64)&desc);
  check64("get_thread_area: the cleared entry has no base", desc.base_addr, 0);
  desc = (struct user_desc){11, (u32)(u64)&teb_a, 0xffffffffu, 0x41};
  check64("an entry outside 12 to 14 is EINVAL", (u64)(i64)int80(IA32_set_thread_area, (u32)(u64)&desc), (u64)-22);

  check64("a 32-bit call the engine does not serve is ENOSYS", (u64)(i64)int80(IA32_getpid, 0), (u64)-38);

  desc = (struct user_desc){(u32)-1, (u32)(u64)&teb_a, 0xffffffffu, 0x41};
  int80(IA32_set_thread_area, (u32)(u64)&desc);
  g_entry = desc.entry_number;
  check64("a forked child keeps the TLS entry", (u64)child_status(forked_child), 7);
  check64("after execve no TLS entry is left", (u64)child_status(0), 7);
  return finish("w29_wow64");
}

// The execve child (one argument) only reports whether the TLS entries are empty.
#ifdef WITNESS_HOSTED
WITNESS_MAIN(run)
#else
__attribute__((used, noreturn)) static void start_c(u64* initial_sp) {
  exit_group(initial_sp[0] > 1 ? exec_child() : run());
  __builtin_unreachable();
}
__asm__(".text\n.globl _start\n_start:\n\txor %ebp,%ebp\n\tmov %rsp,%rdi\n\tand $-16,%rsp\n\tcall start_c\n\thlt\n");
#endif
