# Request mm-2 (core): guest window mode and loader entry points in mm.h

## 1. Meaning of `lkd_config.guest_window_base` (lkd.h, section 4)

`lkd.h` says that mm reserves the window and sets `lkd_guest_base` in `lkd_init()`, and `CONVENTIONS.md` 2.5 says that `lkd_guest_base` is 0 in the tests. The two hold together only if a setting tells the engine apart from the tests. Implemented behavior (`src/mm/mm.c` lkd_mm_init, `src/mm/window.c`):

- `guest_window_base != 0` (engine): `lkd_init` reserves `guest_window_bytes` (default 2^46) at this base, otherwise at the first free candidate (8 TiB, 16 TiB, 32 TiB, 0x7d80000000), otherwise anywhere aligned on 1 GiB, and sets `lkd_guest_base`. The engine copies `lkd_guest_base` into `FEXCore::GuestBase` AFTER `lkd_init`.
- `guest_window_base == 0` (tests): identity mode, `lkd_guest_base` stays 0, the window (at most 4 GiB) is reserved by the first `lkd_mm_create()` and guest addresses are the host addresses of the window.

Request: write this rule in the comment of `lkd_config.guest_window_base` ("0 = identity test mode; the engine passes a base, 0x80000000000 recommended") instead of "0 = module's choice".

## 2. Functions to add to `src/include/mm.h`

They exist (declared in `src/mm/lkd_mm_ext.h`):

```c
void lkd_mm_set_brk_start(lkd_mm *mm, uint64_t start_brk);   /* the ELF loader after exec */
void lkd_mm_set_stack_start(lkd_mm *mm, uint64_t start_stack); /* address that names [stack] */
void lkd_mm_set_mmap_base(lkd_mm *mm, uint64_t base);          /* top of the downward search */
uint64_t lkd_mm_window_guest_lo(void);                         /* 0 in production */
uint64_t lkd_mm_window_guest_hi(void);                         /* effective TASK_SIZE = window size */
```

Without `lkd_mm_set_brk_start`, `brk` reads `t->proc->layout->start_brk` (set by `lkd_proc_set_layout`) on the first call; if nothing is known, `brk` returns 0 (no heap, glibc then goes through mmap).

## 3. Size of the guest space

With a window of 2^46, the guest sees TASK_SIZE = 0x400000000000 and not 0x7ffffffff000. The high area that the Wine x86-64 preloader tries to reserve (0x7ffffe000000) therefore does not exist for the guest. READ, not measured with Wine: the preloader ignores reservations that fail. To check at the first Wine launch; a larger window (up to 0x77fffe000000 bytes with the 8 TiB base) is possible if `guest_window_bytes` asks for it.


## Handled on 2026-10-02 (integration maintainer)

Point 1: comment of `guest_window_base` rewritten in `include/lkd/lkd.h`. Point 2: the five functions are declared in `src/include/mm.h` (weak replacements in `stubs_mm.c`) and removed from `lkd_mm_ext.h`. Point 3: limit confirmed by the EAC probe campaign (MAP_FIXED at 0x7ffff7ff0000 impossible, see `docs/status/ptrace.md`).
