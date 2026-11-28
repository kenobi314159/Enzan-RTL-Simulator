// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

namespace enzan {
class DiagnosticEngine;
}

namespace enzan::runtime {
class ExecutionBackend;
class Image;
} // namespace enzan::runtime

namespace enzan::jit {

/// Compiles every body used by `image` before execution, or reports why it cannot.
/// The image and diagnostics must outlive the returned backend.
std::unique_ptr<runtime::ExecutionBackend> makeBackend(const runtime::Image& image,
                                                       DiagnosticEngine& diagnostics);

} // namespace enzan::jit
