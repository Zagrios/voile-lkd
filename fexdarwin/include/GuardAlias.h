// SPDX-License-Identifier: MIT
// A transient alias of one host page of guest memory, for reading (and writing) a guarded host page (LKD mm,
// mm_page_guarded) without opening it. The host page is PROT_NONE because some of its 4 KiB guest pages are forbidden
// next to allowed ones; opening it lets every thread read the forbidden ones (a packer's encrypted code) without the
// fault the packer waits for. The alias is another mapping of the same memory (mach_vm_remap, shared, not copied), so it
// sees every store of the guest at once and its protection is its own: the guest's mapping stays PROT_NONE.
#pragma once

#include <cstdint>

namespace GuardAlias {
// The host page at guest address HostPage (aligned to the host page) mapped read/write into this thread's alias Slot (0
// or 1; the engine's translator and fault handler use different ones). Valid until the same slot is mapped again or
// the thread ends. nullptr when macOS refuses.
uint8_t* Map(unsigned Slot, uint64_t HostPage);
} // namespace GuardAlias
