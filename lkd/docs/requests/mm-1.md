# Request mm-1 (core): read the configuration from a module

The mm module must read `guest_window_bytes` and `guest_window_base` from `lkd_config`. The only function that gives them is `lkd_config_get()` declared in `src/core/core_priv.h`, reserved for the common kernel. Meanwhile, `src/mm/mm_priv.h` redeclares the prototype `const lkd_config *lkd_config_get(void);`.

Request: declare `lkd_config_get()` in `src/include/lkd_internal.h` (read-only for the modules).


## Handled on 2026-10-02 (integration maintainer)

Done (lead, `lkd_config_get` in `lkd_internal.h`).
