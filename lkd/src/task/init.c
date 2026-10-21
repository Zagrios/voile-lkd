/* init.c: registration of the task module's syscalls.
 *
 * Deliberately not registered (the dispatcher answers -ENOSYS, counted): rseq (glibc falls back),
 * pidfd_open (no pidfds), modify_ldt (no LDT in the engine), timer_create/timer_settime/timer_gettime/timer_getoverrun/timer_delete (POSIX timers not
 * implemented yet), ioprio_get/ioprio_set. See docs/status/task.md. */
#include "task_priv.h"

void task_ids_register(void);
void task_sys_register(void);
void task_time_register(void);
void task_clone_register(void);
void task_wait_register(void);
void task_exec_register(void);
void task_wake_register(void);

void lkd_task_init(void)
{
    fam_ready();
    task_ids_register();
    task_sys_register();
    task_time_register();
    task_clone_register();
    task_wait_register();
    task_exec_register();
    task_wake_register();
    task_seccomp_register();
}
