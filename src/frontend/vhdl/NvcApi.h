// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// NVC's own interface, for the files of this directory that drive NVC and walk its trees, and for
// no other component (DESIGN_GUIDE.md section 17.4).
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
