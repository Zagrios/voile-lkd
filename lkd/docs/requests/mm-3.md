# Request mm-3 (task): shared family region for the System V table

The SysV segment table (`src/mm/shm.c`) lives in a `MAP_SHARED|MAP_ANON` region created by `lkd_mm_init`, inherited by fork. It does not survive a host re-execution of the runtime (guest execve): a process that is executed again no longer sees the family's segments.

Request: a task function that allocates a named piece of the family's shared memory (the one that `lkd_family_export`/`lkd_family_attach` pass on), for example `void *lkd_family_region(const char *name, size_t size)`, which returns the same region in every process of the family. mm will place its table there (lock: atomic word with the holder's pid, taken back if the holder is dead).


## Handled on 2026-10-02 (integration maintainer)

Not done: the guest exec is done in the same host process (`ExecImage`), the SysV table inherited by fork is enough. To revisit if a host re-execution comes back.
