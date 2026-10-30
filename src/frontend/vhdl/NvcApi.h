// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// NVC's own interface, for the files of this directory that drive NVC and walk its trees, and for
// no other component.
//
// These are the C++ copies of NVC's headers that third_party/nvc-analysis generates at configure
// time, not NVC's originals: those are C that C++ rejects in three places, and the copies change
// only those. NVC is C, so its declarations have C linkage.

extern "C" {
#include "common.h"
#include "diag.h"
#include "ident.h"
#include "lib.h"
#include "option.h"
#include "phase.h"
#include "scan.h"
#include "thread.h"
#include "tree.h"
#include "type.h"
#include "util.h"
}

// NVC's thread.h spells its atomics as macros named like the standard library's functions, which
// break <atomic> and <memory> when either is included after this. Nothing outside NVC uses them.
#undef atomic_add
#undef atomic_fetch_add
#undef atomic_load
#undef atomic_store
#undef atomic_xchg
#undef atomic_cas
