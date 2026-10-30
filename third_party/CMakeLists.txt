# SPDX-License-Identifier: GPL-3.0-or-later
#
# Vendored dependencies.
#
# Vendoring is reserved for cases where it is unavoidable. Each directory here has a README
# explaining why it qualifies, and each is recorded in docs/third_party.md with its licence,
# upstream URL and pinned revision.

add_subdirectory(fst)

# NVC is a git submodule rather than a vendored copy, so Enzan's build of it lives in a directory
# of its own beside it, where Enzan's files can sit without touching NVC's tree.
if(ENZAN_ENABLE_VHDL)
  add_subdirectory(nvc-analysis)
endif()
