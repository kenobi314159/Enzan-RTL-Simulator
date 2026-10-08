// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/TclLibrary.h"

#include <mutex>

#include <tcl.h>

// Section 11.3 asks for Tcl 8.6 or later. Checked here rather than in CMake because CMake's
// FindTCL module reports no version at all, so the header is the only thing that knows.
static_assert(TCL_MAJOR_VERSION > 8 || (TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION >= 6),
              "Enzan requires Tcl 8.6 or later (DESIGN_GUIDE.md section 11.3)");

namespace enzan::gui {

void initialiseTcl() {
    static std::once_flag initialised;
    std::call_once(initialised, [] { Tcl_FindExecutable(nullptr); });
}

} // namespace enzan::gui
