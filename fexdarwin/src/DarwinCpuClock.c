// SPDX-License-Identifier: MIT
// The host's highest CPU clock, for the guest's /proc/cpuinfo and cpufreq files (lkd_procfs_set_cpu_max_khz). Apple
// silicon has no sysctl for it. The power manager's DVFS tables (IODeviceTree:/arm-io/pmgr) hold a (frequency,
// millivolts) pair of 32-bit words per state, the fastest last. The CPU clusters' tables are voltage-statesN-sram, N
// depending on the chip (5 for the P cores up to M4), in Hz up to M3 and in kHz from M4 on; the GPU's -sram tables are
// in Hz and slower than the CPU. So the top state of every -sram table is taken in kHz by its size, and the fastest wins.
// IORegistryEntryGetProperty needs no CoreFoundation, which a forked guest process, the first to ask, may not use.
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint32_t fexdarwin_cpu_max_khz(void) {
  io_registry_entry_t pmgr = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/arm-io/pmgr");
  uint32_t best = 0;
  if (!pmgr) return 0;
  for (unsigned n = 0; n < 64; n++) {
    io_name_t name;
    io_struct_inband_t buf;
    uint32_t size = sizeof(buf);
    snprintf(name, sizeof(name), "voltage-states%u-sram", n);
    if (IORegistryEntryGetProperty(pmgr, name, buf, &size) != KERN_SUCCESS || size < 8) continue;
    uint32_t top;
    memcpy(&top, buf + (size & ~7u) - 8, 4);
    uint32_t khz = top >= 100000000 ? top / 1000 : top;
    if (khz >= 100000 && khz <= 10000000 && khz > best) best = khz;
  }
  IOObjectRelease(pmgr);
  return best;
}
