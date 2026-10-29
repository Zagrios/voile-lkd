// SPDX-License-Identifier: MIT
// Sampler (FEXDARWIN_SAMPLE=<period in microseconds>): where the guest threads spend their CPU time, at the level of the
// guest instruction. A host thread wakes every period; for each guest thread that is on a CPU it suspends the thread for
// a few microseconds, reads its host PC and, when the PC is in the translated block the thread is running, the guest RIP
// of the instruction (the RIP entries FEXCore keeps for signals). Nothing is changed in the guest and no signal is sent,
// so a packer or an anti-cheat sees nothing. Samples go to FEXDARWIN_SAMPLE_DIR (or FEXDARWIN_DUMP_DIR) as
// sample-<host pid>.bin, with sample-<host pid>.txt naming the threads, the guest modules and the code of the hottest
// blocks. tools/dev/sample-report.py reads both.
#pragma once

namespace Sampler {
// Starts the sampler thread of this process when FEXDARWIN_SAMPLE is set (first process, and the child of a host fork).
void Start();
// The process replaced its image (execve): module names are looked up again.
void NoteExec();
// The process is ending: writes the samples not flushed yet (waits at most 1 s).
void Finish();
} // namespace Sampler
