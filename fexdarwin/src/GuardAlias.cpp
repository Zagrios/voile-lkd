// SPDX-License-Identifier: MIT
#include "GuardAlias.h"

#include <FEXCore/Utils/GuestBase.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace GuardAlias {
namespace {
constexpr mach_vm_size_t Hps = FEXCore::Utils::FEX_HOST_PAGE_SIZE;
constexpr unsigned Slots = 2;

// The thread's slots: address space the aliases are mapped over, given back when the thread ends.
struct ThreadSlots {
  mach_vm_address_t Base {};
  ~ThreadSlots() {
    if (Base) mach_vm_deallocate(mach_task_self(), Base, Hps * Slots);
  }
};
thread_local ThreadSlots Mine;
} // namespace

uint8_t* Map(unsigned Slot, uint64_t HostPage) {
  if (Slot >= Slots) return nullptr;
  if (!Mine.Base && mach_vm_allocate(mach_task_self(), &Mine.Base, Hps * Slots, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
    Mine.Base = 0;
    return nullptr;
  }
  mach_vm_address_t Alias = Mine.Base + Slot * Hps;
  vm_prot_t Cur, Max;
  if (mach_vm_remap(mach_task_self(), &Alias, Hps, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                    reinterpret_cast<mach_vm_address_t>(FEXCore::G2H(HostPage)), FALSE, &Cur, &Max, VM_INHERIT_NONE) != KERN_SUCCESS) {
    return nullptr;
  }
  if (mach_vm_protect(mach_task_self(), Alias, Hps, FALSE, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) return nullptr;
  return reinterpret_cast<uint8_t*>(Alias);
}
} // namespace GuardAlias
