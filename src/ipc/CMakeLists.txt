# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two components from one directory, and the split is the point rather than a build detail.
#
# `ipc` is the protocol and the transport. Both ends of a connection link it, so it depends on
# `common` and `eir` and nothing else: the GUI has no business linking the simulation kernel,
# and a build file that let it would quietly undo the process separation of DESIGN_GUIDE.md
# section 2.2.
#
# `ipc_server` is the kernel end, which by its nature owns a kernel and a waveform database.
# Only the `enzan` executable links it.

enzan_add_component(
  ipc
  SOURCES Codec.cpp Protocol.cpp Transport.cpp
  HEADERS Codec.h Protocol.h Transport.h
  DEPENDS enzan::common enzan::eir)

# `ipc_server` depends on `control_session` because the simulation belongs to a session: a
# served kernel, a `-do` script and a batch run are one pipeline, and a server that built a
# kernel of its own would mean a command typed at a front end acted on a different simulation
# from the one on screen.
enzan_add_component(
  ipc_server
  SOURCES ChangePublisher.cpp KernelServer.cpp
  HEADERS ChangePublisher.h KernelServer.h
  DEPENDS enzan::control_session enzan::ipc enzan::runtime enzan::wavedb
  PRIVATE_DEPENDS enzan::elab)
