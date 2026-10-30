// SPDX-License-Identifier: GPL-3.0-or-later
//
// The routines of `vhpi_user.h`, as an application calls them.
//
// Each one finds the process's context and forwards to it; the work is in Vhpi.cpp. Every
// routine the standard declares is defined here, the unsupported ones included, so that an
// application naming one it never calls still loads. The `enzan` executable exports these
// beside VPI's and nothing else.
//
// Called with no context — before one exists, or from an application's own clean-up after the
// session has gone — every routine fails quietly rather than dereferencing nothing.

#include "pli/PliContext.h"
#include "pli/PliModel.h"

#include "vhpi_user.h"

#include <cstdarg>
#include <cstdio>
#include <string>

// The names, signatures and C linkage are the standard's, not Enzan's to choose.
// NOLINTBEGIN(readability-identifier-naming, cppcoreguidelines-pro-type-vararg,
//             cert-dcl50-cpp, modernize-avoid-variadic-functions)

namespace {

using enzan::pli::PliContext;

/// The context, or null. Each routine forgets the previous call's error as it begins, which
/// `vhpi_check_error` reports on.
PliContext* context() {
    return PliContext::current();
}

/// Reports `routine` as one Enzan does not support, through `vhpi_check_error`.
void unsupported(const char* routine) {
    if (PliContext* current = context(); current != nullptr) {
        current->clearError();
        current->unsupported(routine);
    }
}

} // namespace

namespace enzan::pli {

void keepVhpiRoutinesLinked() noexcept {}

} // namespace enzan::pli

extern "C" {

vhpiHandleT vhpi_register_cb(vhpiCbDataT* cb_data_p, int32_t flags) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiRegisterCallback(cb_data_p, flags) : nullptr;
}

int vhpi_remove_cb(vhpiHandleT cb_obj) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiRemoveCallback(cb_obj) : -1;
}

int vhpi_disable_cb(vhpiHandleT /*cb_obj*/) {
    unsupported("vhpi_disable_cb");
    return -1;
}

int vhpi_enable_cb(vhpiHandleT /*cb_obj*/) {
    unsupported("vhpi_enable_cb");
    return -1;
}

int vhpi_get_cb_info(vhpiHandleT /*object*/, vhpiCbDataT* /*cb_data_p*/) {
    unsupported("vhpi_get_cb_info");
    return -1;
}

vhpiHandleT vhpi_handle_by_name(const char* name, vhpiHandleT scope) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiHandleByName(name, scope) : nullptr;
}

vhpiHandleT vhpi_handle_by_index(vhpiOneToManyT itRel, vhpiHandleT parent, int32_t indx) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiHandleByIndex(itRel, parent, indx) : nullptr;
}

vhpiHandleT vhpi_handle(vhpiOneToOneT type, vhpiHandleT referenceHandle) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiHandle(type, referenceHandle) : nullptr;
}

vhpiHandleT vhpi_iterator(vhpiOneToManyT type, vhpiHandleT referenceHandle) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiIterator(type, referenceHandle) : nullptr;
}

vhpiHandleT vhpi_scan(vhpiHandleT iterator) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiScan(iterator) : nullptr;
}

vhpiIntT vhpi_get(vhpiIntPropertyT property, vhpiHandleT object) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiGet(property, object) : vhpiUndefined;
}

const vhpiCharT* vhpi_get_str(vhpiStrPropertyT property, vhpiHandleT object) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiGetStr(property, object) : nullptr;
}

vhpiRealT vhpi_get_real(vhpiRealPropertyT /*property*/, vhpiHandleT /*object*/) {
    unsupported("vhpi_get_real");
    return 0.0;
}

vhpiPhysT vhpi_get_phys(vhpiPhysPropertyT property, vhpiHandleT object) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiGetPhys(property, object) : vhpiPhysT{};
}

int vhpi_protected_call(vhpiHandleT /*varHdl*/, vhpiUserFctT /*userFct*/, void* /*userData*/) {
    unsupported("vhpi_protected_call");
    return -1;
}

int vhpi_get_value(vhpiHandleT expr, vhpiValueT* value_p) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiGetValue(expr, value_p) : -1;
}

int vhpi_put_value(vhpiHandleT object, vhpiValueT* value_p, vhpiPutValueModeT mode) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiPutValue(object, value_p, mode) : -1;
}

int vhpi_schedule_transaction(vhpiHandleT /*drivHdl*/, vhpiValueT* /*value_p*/,
                              uint32_t /*numValues*/, vhpiTimeT* /*delayp*/,
                              vhpiDelayModeT /*delayMode*/, vhpiTimeT* /*pulseRejp*/) {
    unsupported("vhpi_schedule_transaction");
    return -1;
}

int vhpi_format_value(const vhpiValueT* /*in_value_p*/, vhpiValueT* /*out_value_p*/) {
    unsupported("vhpi_format_value");
    return -1;
}

void vhpi_get_time(vhpiTimeT* time_p, long* cycles) {
    if (PliContext* current = context(); current != nullptr) current->vhpiGetTime(time_p, cycles);
}

int vhpi_get_next_time(vhpiTimeT* /*time_p*/) {
    unsupported("vhpi_get_next_time");
    return -1;
}

int vhpi_control(vhpiSimControlT command, ...) {
    // A status may follow vhpiFinish and vhpiStop; Enzan ends the same way whatever it is, so
    // it is not read, which also keeps clear of `va_start` after an enumeration, whose promotion
    // makes that undefined.
    PliContext* current = context();
    return current != nullptr ? current->vhpiControl(command) : -1;
}

int vhpi_vprintf(const char* format, va_list args) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiPrint(enzan::pli::formatted(format, args)) : 0;
}

int vhpi_printf(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int printed = vhpi_vprintf(format, arguments);
    va_end(arguments);
    return printed;
}

int vhpi_is_printable(char ch) {
    return ch >= ' ' && ch <= '~' ? 1 : 0;
}

int vhpi_assert(vhpiSeverityT /*severity*/, char* formatmsg, ...) {
    // Reported as the design's own assertions would be is more than VHPI promises here; it is
    // printed, which is what an application asking for it needs to see.
    va_list arguments;
    va_start(arguments, formatmsg);
    const int printed = vhpi_vprintf(formatmsg, arguments);
    va_end(arguments);
    return printed >= 0 ? 0 : -1;
}

int vhpi_compare_handles(vhpiHandleT handle1, vhpiHandleT handle2) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiCompareHandles(handle1, handle2) : 0;
}

int vhpi_check_error(vhpiErrorInfoT* error_info_p) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiCheckError(error_info_p) : 0;
}

int vhpi_release_handle(vhpiHandleT object) {
    PliContext* current = context();
    return current != nullptr ? current->vhpiReleaseHandle(object) : -1;
}

vhpiHandleT vhpi_create(vhpiClassKindT /*kind*/, vhpiHandleT /*handle1*/, vhpiHandleT /*handle2*/) {
    unsupported("vhpi_create");
    return nullptr;
}

vhpiHandleT vhpi_register_foreignf(vhpiForeignDataT* /*foreignDatap*/) {
    unsupported("vhpi_register_foreignf");
    return nullptr;
}

int vhpi_get_foreignf_info(vhpiHandleT /*hdl*/, vhpiForeignDataT* /*foreignDatap*/) {
    unsupported("vhpi_get_foreignf_info");
    return -1;
}

size_t vhpi_get_data(int32_t /*id*/, void* /*dataLoc*/, size_t /*numBytes*/) {
    unsupported("vhpi_get_data");
    return 0;
}

size_t vhpi_put_data(int32_t /*id*/, void* /*dataLoc*/, size_t /*numBytes*/) {
    unsupported("vhpi_put_data");
    return 0;
}

} // extern "C"

// NOLINTEND(readability-identifier-naming, cppcoreguidelines-pro-type-vararg, cert-dcl50-cpp,
//           modernize-avoid-variadic-functions)
