// Witness: the processor topology CPUID gives in leaf 0BH (extended topology enumeration), which a program uses to tell
// cores from hyper-threads. Subleaf 0 is the SMT level and subleaf 1 the core level, whose EBX counts every logical
// processor of the package; the first subleaf with level type 0 ends the list, and ECX[7:0] echoes the subleaf. Without
// the leaf, a program that falls back on CPUID 1 and 4 sees one core holding all the logical processors. Only checked on
// a GenuineIntel CPUID with the leaf in range: AMD processors describe their topology in other leaves.
// Linux guest only (sched_getaffinity): no WITNESS_HOSTED build. Build with EXEC=1.
#include "rt.h"

#define SYS_sched_getaffinity_ 204

static void cpuid_(u32 leaf, u32 sub, u32* a, u32* b, u32* c, u32* d) {
  __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static int run(void) {
  u8 mask[128];
  u32 a, b, c, d, max;
  i64 got = sys3(SYS_sched_getaffinity_, 0, sizeof(mask), (i64)mask);
  int cpus = 0;
  for (i64 i = 0; i < got; i++)
    for (int bit = 0; bit < 8; bit++) cpus += (mask[i] >> bit) & 1;
  checkb("sched_getaffinity names the processors", cpus > 0);

  cpuid_(0, 0, &max, &b, &c, &d);
  int intel = b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e;
  puts_("  processors="); put_dec(cpus); puts_(" max leaf="); put_hex(max, 8); puts_(intel ? " GenuineIntel\n" : " not GenuineIntel\n");
  if (!intel || max < 0xb) {
    puts_("  no leaf 0BH to check\n");
    return finish("w40_cputopology");
  }

  cpuid_(0xb, 0, &a, &b, &c, &d);
  check64("subleaf 0: ECX[7:0] echoes the subleaf", c & 0xff, 0);
  check64("subleaf 0: level type is SMT", (c >> 8) & 0xff, 1);
  checkb("subleaf 0: EBX counts the logical processors of a core", b >= 1 && (int)b <= cpus);

  cpuid_(0xb, 1, &a, &b, &c, &d);
  check64("subleaf 1: ECX[7:0] echoes the subleaf", c & 0xff, 1);
  check64("subleaf 1: level type is core", (c >> 8) & 0xff, 2);
  check64("subleaf 1: EBX counts every logical processor", b, (u64)cpus);
  u32 shift = a & 0x1f;
  checkb("subleaf 1: EAX[4:0] has room for every processor", shift < 32 && (1ul << shift) >= (u64)cpus);
  checkb("subleaf 1: the x2APIC ID in EDX fits that width", shift < 32 && (u64)d < (1ul << shift));

  cpuid_(0xb, 2, &a, &b, &c, &d);
  check64("subleaf 2: ECX[7:0] echoes the subleaf", c & 0xff, 2);
  check64("subleaf 2: level type 0 ends the list", (c >> 8) & 0xff, 0);
  check64("subleaf 2: EBX is 0", b, 0);

  return finish("w40_cputopology");
}
WITNESS_MAIN(run)
