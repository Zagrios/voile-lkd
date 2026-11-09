// Witness: the ways out of a translated block through FEXCore's SYSCALL and CPUID handlers, and the 0F 01 /5 row.
// Upstream FEXCore ends a block at every CPUID and looks up the next block in the L1 cache after a SYSCALL or a CPUID;
// the fork's rules must hold on that path too. With TF set, the trap comes after each instruction, also when the next
// block is already in the L1 (HELLDIVERS 2's protector single-steps both). Only 0F 01 E8 is SERIALIZE, and only when
// CPUID 7 says so: E9..ED, RDPKRU/WRPKRU without OSPKE and the unprefixed /5 memory form are #UD (SIGILL), which the same
// protector probes for. A CPUID at 0xFFFFFFFE in a compatibility mode code segment continues where EIP wraps, at 0, and
// faults there (AION 2's NCGuard). A signal return to RIP 0 faults at 0 (Themida raises exceptions that way), also when
// the L1 entry of address 0 holds a block that a write to its code invalidated.
// Linux guest only: raw rt_sigaction and an iretq to selector 0x23, no WITNESS_HOSTED build.
#include "rt.h"

#define SIGILL_ 4
#define SIGTRAP_ 5
#define SIGSEGV_ 11
#define SA_SIGINFO_ 4
#define SA_ONSTACK_ 0x08000000
#define SA_RESTORER_ 0x04000000
#define SA_NODEFER_ 0x40000000
enum { PROT_RWX = 7, MAP_PRIVATE = 2, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20 };

struct ksigaction { u64 handler, flags, restorer, mask; };
struct stack_t_ { u64 sp; i32 flags; i32 pad; u64 size; };
struct sigcontext_ { u64 r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags; u16 cs, gs, fs, ss; u64 err, trapno, oldmask, cr2, fpstate, reserved[8]; };
struct ucontext_ { u64 flags, link; struct stack_t_ stack; struct sigcontext_ mc; u64 sigmask; };
struct siginfo_ { i32 signo, errno_, code, pad; u64 addr; u8 rest[104]; };

__asm__(".text\n.globl restorer\nrestorer:\n\tmovl $15, %eax\n\tsyscall\n");
void restorer(void);

static void set_handler(int sig, void* fn) {
  struct ksigaction a = {(u64)fn, SA_SIGINFO_ | SA_RESTORER_ | SA_NODEFER_ | SA_ONSTACK_, (u64)restorer, 0};
  sys4(SYS_rt_sigaction, sig, (i64)&a, 0, 8);
}

// SIGTRAP: record where each single-step trap lands; a trap outside [lo, hi) clears TF.
static volatile u64 trap_rip[32];
static volatile int ntrap;
static volatile u64 lo, hi;
static void on_trap(int sig, struct siginfo_* si, struct ucontext_* uc) {
  (void)sig; (void)si;
  if (ntrap < 32) trap_rip[ntrap] = uc->mc.rip;
  ntrap++;
  if (uc->mc.rip < lo || uc->mc.rip >= hi) uc->mc.eflags &= ~0x100UL;
}

// SIGILL and SIGSEGV: record the fault and continue at resume_rip (back in the 64-bit code segment when resume_rsp is set).
static volatile int last_sig;
static volatile u64 last_rip, last_addr, resume_rip, resume_rsp;
static volatile u16 last_cs;
static void on_fault(int sig, struct siginfo_* si, struct ucontext_* uc) {
  last_sig = sig; last_rip = uc->mc.rip; last_addr = si->addr; last_cs = uc->mc.cs;
  uc->mc.rip = resume_rip;
  if (resume_rsp) { uc->mc.rsp = resume_rsp; uc->mc.cs = 0x33; uc->mc.ss = 0x2b; }
}

// SIGTRAP from int3: return to RIP 0.
static void on_int3(int sig, struct siginfo_* si, struct ucontext_* uc) { (void)sig; (void)si; uc->mc.rip = 0; }

// rdi != 0 sets TF before the instruction ahead of CPUID or SYSCALL. The labels give the expected trap addresses.
extern char c_go[], c_cpuid[], c_n1[], c_end[];
extern char s_go[], s_sys[], s_n1[], s_n2[], s_end[];
__asm__(".text\n"
  ".globl step_cpuid\nstep_cpuid:\n push %rbx\n test %rdi,%rdi\n jz c_go\n pushfq\n orq $0x100,(%rsp)\n popfq\n"
  ".globl c_go\nc_go: xor %eax,%eax\n"
  ".globl c_cpuid\nc_cpuid: cpuid\n"
  ".globl c_n1\nc_n1: nop\n nop\n nop\n"
  ".globl c_end\nc_end: nop\n pop %rbx\n ret\n"
  ".globl step_sys\nstep_sys:\n test %rdi,%rdi\n jz s_go\n pushfq\n orq $0x100,(%rsp)\n popfq\n"
  ".globl s_go\ns_go: mov $39,%eax\n"
  ".globl s_sys\ns_sys: syscall\n"
  ".globl s_n1\ns_n1: nop\n.globl s_n2\ns_n2: nop\n nop\n"
  ".globl s_end\ns_end: nop\n ret\n");
void step_cpuid(u64 tf);
void step_sys(u64 tf);

static void print_traps(void) {
  puts_("     traps:");
  for (int i = 0; i < ntrap && i < 8; i++) { puts_(" "); put_hex(trap_rip[i], 12); }
  puts_("\n");
}

// Each probe runs one 0F 01 /5 encoding at its _at label and returns from its _end label.
extern char e8_at[], e8_end[], e9_at[], e9_end[], ee_at[], ee_end[], mem_at[], mem_end[];
__asm__(".text\n"
  ".globl op_e8\nop_e8:\n.globl e8_at\ne8_at: .byte 0x0f,0x01,0xe8\n.globl e8_end\ne8_end: ret\n"
  ".globl op_e9\nop_e9:\n.globl e9_at\ne9_at: .byte 0x0f,0x01,0xe9\n nop\n.globl e9_end\ne9_end: ret\n"
  ".globl op_ee\nop_ee: xor %ecx,%ecx\n.globl ee_at\nee_at: .byte 0x0f,0x01,0xee\n nop\n.globl ee_end\nee_end: ret\n"
  ".globl op_mem\nop_mem:\n.globl mem_at\nmem_at: .byte 0x0f,0x01,0x2d,0x00,0x00,0x00,0x00\n nop\n.globl mem_end\nmem_end: ret\n");
void op_e8(void); void op_e9(void); void op_ee(void); void op_mem(void);

static void probe(const char* name, void (*fn)(void), const char* at, const char* end, int want_sig) {
  last_sig = 0; last_rip = 0; resume_rip = (u64)end; resume_rsp = 0;
  fn();
  int ok = last_sig == want_sig && (!want_sig || last_rip == (u64)at);
  checkb(name, ok);
  if (!ok) { puts_("     sig="); put_dec(last_sig); puts_(" rip="); put_hex(last_rip, 12); puts_("\n"); }
}

static u8 altstack[65536] __attribute__((aligned(16)));

static int run(void) {
  puts_("w38_cpuidsys: TF over CPUID and SYSCALL, the 0F 01 /5 row, CPUID at the 4 GiB wrap, a return to RIP 0\n");
  struct stack_t_ ss = {(u64)altstack, 0, 0, sizeof(altstack)};
  sys2(SYS_sigaltstack, (i64)&ss, 0);
  set_handler(SIGTRAP_, (void*)on_trap);
  set_handler(SIGILL_, (void*)on_fault);
  set_handler(SIGSEGV_, (void*)on_fault);

  // 1. Single step over CPUID once the L1 holds the block after it.
  for (int i = 0; i < 4; i++) step_cpuid(0);
  ntrap = 0; lo = (u64)c_go; hi = (u64)c_end + 1;
  step_cpuid(1);
  checkb("TF: trap at cpuid, after xor", ntrap > 0 && trap_rip[0] == (u64)c_cpuid);
  checkb("TF: trap right after cpuid", ntrap > 1 && trap_rip[1] == (u64)c_n1);
  if (!(ntrap > 1 && trap_rip[0] == (u64)c_cpuid && trap_rip[1] == (u64)c_n1)) print_traps();

  // 2. Single step over SYSCALL once the L1 holds the block after it. Linux reports the trap right after it, or one
  // instruction later, depending on how the kernel returns to user mode.
  for (int i = 0; i < 4; i++) step_sys(0);
  ntrap = 0; lo = (u64)s_go; hi = (u64)s_end + 1;
  step_sys(1);
  checkb("TF: trap at syscall, after mov", ntrap > 0 && trap_rip[0] == (u64)s_sys);
  checkb("TF: next trap after syscall", ntrap > 1 && (trap_rip[1] == (u64)s_n1 || trap_rip[1] == (u64)s_n2));
  if (!(ntrap > 1 && trap_rip[0] == (u64)s_sys && (trap_rip[1] == (u64)s_n1 || trap_rip[1] == (u64)s_n2))) print_traps();

  // 3. The 0F 01 /5 row: SERIALIZE as CPUID 7 reports it, everything else #UD at the instruction.
  u32 a7 = 7, b7, c7 = 0, d7;
  __asm__ volatile("cpuid" : "+a"(a7), "=b"(b7), "+c"(c7), "=d"(d7));
  int serialize = (d7 >> 14) & 1, ospke = (c7 >> 4) & 1;
  probe(serialize ? "0f 01 e8: SERIALIZE runs (CPUID 7 has it)" : "0f 01 e8: #UD (CPUID 7 has no SERIALIZE)", op_e8, e8_at, e8_end,
        serialize ? 0 : SIGILL_);
  probe("0f 01 e9: reserved, #UD", op_e9, e9_at, e9_end, SIGILL_);
  if (!ospke) probe("0f 01 ee: RDPKRU without OSPKE, #UD", op_ee, ee_at, ee_end, SIGILL_);
  probe("0f 01 2d disp32: /5 memory form without prefix, #UD", op_mem, mem_at, mem_end, SIGILL_);

  // 4. CPUID at 0xFFFFFFFE in a compatibility mode code segment (selector 0x23): EIP wraps to 0, which faults.
  i64 m = sys6(SYS_mmap, 0xFFFFC000L, 0x4000, PROT_RWX, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
  check64("mmap at 0xFFFFC000", (u64)m, 0xFFFFC000UL);
  if (m == 0xFFFFC000L) {
    volatile u8* p = (volatile u8*)0xFFFFFFFEUL;
    p[0] = 0x0f; p[1] = 0xa2;
    last_sig = 0; last_rip = 1;
    __asm__ volatile(
      "leaq 1f(%%rip), %%rax\n\tmovq %%rax, %[rr]\n\tmovq %%rsp, %[rs]\n\t"
      "pushq $0x2b\n\tpushq %%rsp\n\tpushfq\n\tpushq $0x23\n\tmovl $0xFFFFFFFE, %%eax\n\tpushq %%rax\n\txor %%eax,%%eax\n\tiretq\n"
      "1:\n" : [rr] "=m"(resume_rip), [rs] "=m"(resume_rsp) : : "rax", "rbx", "rcx", "rdx", "memory");
    resume_rsp = 0;
    checkb("compat CPUID at 0xFFFFFFFE: SIGSEGV at EIP 0", last_sig == SIGSEGV_ && last_rip == 0 && last_addr == 0);
    check64("compat CPUID at 0xFFFFFFFE: fault in selector 0x23", last_cs, 0x23);
    if (last_rip != 0) { puts_("     sig="); put_dec(last_sig); puts_(" rip="); put_hex(last_rip, 16); puts_("\n"); }
    sys2(SYS_munmap, 0xFFFFC000L, 0x4000);
  }

  // 5. A signal handler sets RIP to 0; rt_sigreturn returns there through the system call path.
  set_handler(SIGTRAP_, (void*)on_int3);
  last_sig = 0; last_rip = 1;
  __asm__ volatile("leaq 1f(%%rip), %%rax\n\tmovq %%rax, %[rr]\n\tint3\n\tnop\n1:\n" : [rr] "=m"(resume_rip) : : "rax", "memory");
  checkb("rt_sigreturn to RIP 0: SIGSEGV at 0", last_sig == SIGSEGV_ && last_rip == 0);

  // 6. The same return while the L1 entry of address 0 holds an invalidated block. An invalidation sets the entry's
  // guest address to 0 and leaves its host address on the old translation, so an L1 match on RIP 0 would run that
  // translation. The code at 0x10000000 (L1 index 0 at every L1 size) runs until it is in the L1; then one asm
  // statement rewrites its immediate and raises the int3, so no other block takes entry 0 in between. The old
  // translation would load 0x1234 and return to label 1 through the address pushed for its RET, without a signal.
  i64 m0 = sys6(SYS_mmap, 0x10000000L, 0x4000, PROT_RWX, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
  u32 eax = 0;
  last_sig = 0; last_rip = 1;
  if (m0 == 0x10000000L) {
    static const u8 mov_ret[] = {0xb8, 0x34, 0x12, 0x00, 0x00, 0xc3}; // mov $0x1234, %eax; ret
    volatile u8* code = (volatile u8*)0x10000000UL;
    for (int i = 0; i < 6; i++) code[i] = mov_ret[i];
    for (int i = 0; i < 8; i++) ((u32(*)(void))0x10000000UL)();
    // Below the red zone, resume_rsp holds the stack pointer before the push: on_fault restores it, and the old
    // translation's RET pops back to it.
    __asm__ volatile(
      "leaq 1f(%%rip), %%rax\n\tmovq %%rax, %[rr]\n\tleaq -128(%%rsp), %%rsp\n\tmovq %%rsp, %[rs]\n\t"
      "pushq %%rax\n\txorl %%eax, %%eax\n\tmovb $0x78, 0x10000001\n\tint3\n"
      "1:\n\tleaq 128(%%rsp), %%rsp\n"
      : [rr] "=m"(resume_rip), [rs] "=m"(resume_rsp), "=a"(eax) : : "memory");
    resume_rsp = 0;
    sys2(SYS_munmap, 0x10000000L, 0x4000);
  }
  int stale_ok = m0 == 0x10000000L && last_sig == SIGSEGV_ && last_rip == 0;
  checkb("rt_sigreturn to RIP 0 over an invalidated L1 entry: SIGSEGV at 0", stale_ok);
  if (!stale_ok) {
    puts_("     mmap="); put_hex((u64)m0, 16); puts_(" sig="); put_dec(last_sig); puts_(" rip="); put_hex(last_rip, 16);
    puts_(" eax="); put_hex(eax, 8); puts_("\n");
  }

  return finish("w38_cpuidsys");
}
WITNESS_MAIN(run)
