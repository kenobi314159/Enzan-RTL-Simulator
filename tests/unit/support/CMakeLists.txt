# SPDX-License-Identifier: GPL-3.0-or-later
#
# Header-only helpers shared by the unit tests. Every test binary links this, so a helper that
# three fixtures had each written for themselves has one home instead.

add_library(enzan_test_support INTERFACE)
add_library(enzan::test_support ALIAS enzan_test_support)

# The parent directory, so that an include reads "support/Literals.h" and says where it comes
# from rather than looking like a header of the component under test.
target_include_directories(enzan_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/..")

target_link_libraries(enzan_test_support INTERFACE enzan::eir GTest::gtest)
