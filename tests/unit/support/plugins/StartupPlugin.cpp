// SPDX-License-Identifier: GPL-3.0-or-later
//
// A VPI application as small as one can be: two startup routines and an entry point, each
// saying through `vpi_printf` that it ran, and callbacks that say when the simulation starts
// and ends. The routines it calls are resolved against the test executable when the library is
// loaded, exactly as an application's are resolved against `enzan`.

#include "vpi_user.h"

namespace {

void say(const char* text) {
    vpi_printf(const_cast<PLI_BYTE8*>(text));
}

PLI_INT32 announce(p_cb_data data) {
    say(data->reason == cbStartOfSimulation ? "start of simulation\n" : "end of simulation\n");
    return 0;
}

void firstRoutine() {
    say("first startup routine ran\n");

    s_cb_data callback{};
    callback.cb_rtn = announce;
    callback.reason = cbStartOfSimulation;
    vpi_register_cb(&callback);
    callback.reason = cbEndOfSimulation;
    vpi_register_cb(&callback);
}

void secondRoutine() {
    say("second startup routine ran\n");
}

} // namespace

extern "C" {

// The names are the standard's, which is the point of them.
// NOLINTBEGIN(readability-identifier-naming)

void (*vlog_startup_routines[])() = {firstRoutine, secondRoutine, nullptr};

void alternate_entry() {
    say("alternate entry ran\n");
}

// NOLINTEND(readability-identifier-naming)
}
