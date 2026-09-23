// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace enzan {
class SourceManager;
}

namespace enzan::control {

class Interpreter;
class Session;

/// Installs the Modelsim-style command vocabulary of DESIGN_GUIDE.md section 8.1.
///
/// The names follow Questa where practical, because that is where the users come from and
/// what their existing scripts are written against: `vlog`, `vcom`, `vsim`, `run`, `restart`,
/// `add wave`, `log`, `force`, `examine`, `radix`, `find`, `quit`. Where Enzan has no
/// equivalent of a Questa concept the command is absent rather than approximated, so a script
/// that relies on one fails at the command instead of producing a different simulation.
///
/// `interpreter`, `session` and `sources` must outlive the interpreter, which is what owns the
/// handlers these install.
void registerCommands(Interpreter& interpreter, Session& session, SourceManager& sources);

} // namespace enzan::control
