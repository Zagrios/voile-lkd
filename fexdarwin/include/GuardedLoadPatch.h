// SPDX-License-Identifier: MIT
// Translated loads that keep faulting on a guarded host page (LKD mm, mm_page_guarded) read the page's file instead.
//
// A guarded host page is PROT_NONE on the host because some of its 4 KiB guest pages are PROT_NONE next to readable
// ones, so every read of the readable ones costs a fault (DarwinSignalDelegator::ResolveGuardedFault). THEIA (THE
// FINALS) hashes the game image with SHA-256 and reads the constants of that loop from a page next to a still encrypted
// one: 16 faults per 64-byte block, about a minute of faults, and its watchdog kills the game first.
//
// After PatchAfter such faults, the load's host instruction becomes a branch to a stub (GuardedLoadStub.h) that reads
// the bytes from the file view LKD keeps for the page (lkd_mm_guard_aliases), with no fault, while the guest's mappings
// stay as they were when the stub was described. Anything else runs the original load, which faults as before and
// describes the stub again. The guest sees nothing: no signal, no protection change.
//
// Faults inside a stub: the original load at its end faults with the registers of the original site, and the handler
// continues from the site (OnFault moves the PC there). Any other fault in a stub (the file view unmapped under it)
// restores the registers, puts the original instruction back and retries it from the site.
#pragma once

#include <cstdint>
#include <sys/ucontext.h>

namespace GuardedLoadPatch {
// The instruction FEX emitted at host address Pc: the original load when Pc was patched.
uint32_t InstructionAt(uint64_t Pc);

// First step of the SIGSEGV/SIGBUS handler. True when the fault was a stub's and is recovered: return from the handler.
// A fault of the original load at a stub's end moves the PC to its site and returns false: handle it from there.
bool OnFault(ucontext_t* Uc);

// The load Insn at host address Pc of translated code read guest address Addr of a guarded page through its file view.
void NoteFileRead(uint64_t Pc, uint32_t Insn, uint64_t Addr);

// exec: the code buffers start over.
void Reset();
} // namespace GuardedLoadPatch
