// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/Assert.h"

#include <cstdio>
#include <cstdlib>

namespace enzan::detail {

void assertionFailed(const char* expression, const char* message, const char* file, unsigned line,
                     const char* function) {
    // Written with stdio rather than iostreams, and without allocating: an
    // assertion may fire while the heap or a stream is in a state that cannot be
    // used, and a failure handler that itself crashes destroys the evidence.
    std::fprintf(stderr,
                 "\nenzan: internal assertion failed\n"
                 "  condition: %s\n"
                 "  reason:    %s\n"
                 "  at:        %s:%u in %s\n"
                 "\nThis is a defect in Enzan, not in the design being simulated.\n"
                 "Please report it, quoting the lines above and `enzan --version`.\n",
                 expression, message, file, line, function);
    std::fflush(stderr);
    std::abort();
}

} // namespace enzan::detail
