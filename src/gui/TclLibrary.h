// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace enzan::gui {

/// Initialises the Tcl library, once per process, before anything in it is used.
///
/// Tcl asks to be told about the executable before anything else. It is what initialises the
/// library's encodings and its allocator, and skipping it leaves a handful of things quietly not
/// working. The theme's interpreter and the signal list's parser both need it, so it is done here
/// rather than by whichever of them happens to run first.
void initialiseTcl();

} // namespace enzan::gui
