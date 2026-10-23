# Request procfs-3 to the project lead (fexdarwin engine)

Date: 2026-10-02. Author: procfs module.

1. CPUID provider. `/proc/cpuinfo` and `/sys/devices/system/cpu/cpuN/cache/` are built from the CPUID that the guest executes, through a function that the engine registers once before the first guest code:
   `lkd_procfs_set_cpuid(fn)` with `int fn(uint32_t leaf, uint32_t subleaf, int cpu, struct lkd_cpuid_regs *out)`. In `LkdGlue.cpp`: `CTX->RunCPUIDFunction(leaf, subleaf)` for all leaves, and `CTX->RunCPUIDFunctionName(leaf, subleaf, cpu)` for 0x80000002 to 0x80000004 (brand string per CPU). The leaves read: 0, 1, 4 (subleaves up to type 0), 6, 7 (0 and 1), 0xD (1), 0x10, 0x15, 0x80000001, 0x80000007, 0x80000008, 0x8000000A, 0x8000001F. As long as the provider is missing, `open("/proc/cpuinfo")` answers `-ENOSYS` (logged once) and `/sys/.../cache` and `clocksource0` do not exist: no invented content.
   FEX is not hidden: the `hypervisor` bit of CPUID shows in `flags` (FEX clears it with `HideHypervisorBit`), the brand comes from the MIDR.
2. Layout after exec. `ops->exec_image` must call `lkd_proc_set_layout` (request `task-3.md` point 1): without it `/proc/<pid>/auxv` is empty and `start_code`, `start_stack`, `arg_start`... of `stat` are 0.
3. `/proc/self/exe` returns `proc->exe_path` (guest path set by task at exec). A direct `execve("/proc/self/exe")` goes through `lkd_vfs_to_host`, which answers `-ENOENT` or `-EXDEV` for a magic link: not checked with the engine (task).
4. Family service. procfs registers `LKD_FAMSVC_OP_TASK_STATUS` (49). The engine must keep `LKD_FAMSVC_DIR` and `LKD_FAMSVC_FD` at host exec (already requested by sig).
