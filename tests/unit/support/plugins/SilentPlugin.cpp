// SPDX-License-Identifier: GPL-3.0-or-later
//
// A shared library that is not a VPI application: it defines no `vlog_startup_routines`, so
// loading it without naming an entry has to be reported rather than quietly doing nothing.

extern "C" {

// NOLINTNEXTLINE(readability-identifier-naming) — a C entry point, named as C names them
int silent_function() {
    return 0;
}
}
