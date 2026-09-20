# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test suites (DESIGN_GUIDE.md section 14).

include(GoogleTest)

add_subdirectory(unit)

# add_subdirectory(golden)       # M1 — self-checking HDL cases
# add_subdirectory(conformance)  # M5 — sv-tests, ivtest harnesses
