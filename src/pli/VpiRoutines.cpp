// SPDX-License-Identifier: GPL-3.0-or-later
//
// The routines of `vpi_user.h`, as an application calls them.
//
// Each one finds the process's context and forwards to it; the work is in PliContext.cpp.
// Every routine the standard declares is defined here, the unsupported ones included, so that
// an application naming one it never calls still loads. The `enzan` executable exports these
// and nothing else.
//
// Called with no context — before one exists, or from an application's own clean-up after the
// session has gone — every routine fails quietly rather than dereferencing nothing.

#include "pli/PliContext.h"
#include "pli/PliModel.h"

#include "vpi_user.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <string>

// The names, signatures and C linkage are the standard's, not Enzan's to choose, and the
// variadic ones forward a `va_list` to `vsnprintf` because that is the interface they define.
// NOLINTBEGIN(readability-identifier-naming, cppcoreguidelines-pro-type-vararg,
//             cert-dcl50-cpp, modernize-avoid-variadic-functions)

namespace {

using enzan::pli::PliContext;

/// The context, with the previous call's error forgotten, or null.
PliContext* context() {
    PliContext* current = PliContext::current();
    if (current != nullptr) current->clearError();
    return current;
}

/// The one multichannel descriptor there is: the transcript.
constexpr PLI_UINT32 TranscriptChannel = VPI_MCD_STDOUT;

} // namespace

namespace enzan::pli {

void keepRoutinesLinked() noexcept {}

// `vpi_printf`, `vhpi_printf` and their relatives are printf: the format is the application's, and
// it is a variable because that is the routine's whole contract. -Wformat-nonliteral exists to
// catch a format that should have been a literal, which this one never could be.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"

std::string formatted(const char* text, va_list arguments) {
    if (text == nullptr) return {};

    va_list measuring;
    va_copy(measuring, arguments);
    const int length = std::vsnprintf(nullptr, 0, text, measuring);
    va_end(measuring);
    if (length <= 0) return {};

    std::string result(static_cast<std::size_t>(length) + 1, '\0');
    std::vsnprintf(result.data(), result.size(), text, arguments);
    result.resize(static_cast<std::size_t>(length));
    return result;
}

#pragma GCC diagnostic pop

} // namespace enzan::pli

extern "C" {

vpiHandle vpi_register_cb(p_cb_data cb_data_p) {
    PliContext* current = context();
    return current != nullptr ? current->registerCallback(cb_data_p) : nullptr;
}

PLI_INT32 vpi_remove_cb(vpiHandle cb_obj) {
    PliContext* current = context();
    return current != nullptr ? current->removeCallback(cb_obj) : 0;
}

void vpi_get_cb_info(vpiHandle object, p_cb_data cb_data_p) {
    if (PliContext* current = context(); current != nullptr)
        current->getCallbackInfo(object, cb_data_p);
}

vpiHandle vpi_register_systf(p_vpi_systf_data /*systf_data_p*/) {
    // A user system task needs the frontend to compile a call to it, which it does not.
    if (PliContext* current = context(); current != nullptr)
        current->unsupported("vpi_register_systf");
    return nullptr;
}

void vpi_get_systf_info(vpiHandle /*object*/, p_vpi_systf_data /*systf_data_p*/) {
    if (PliContext* current = context(); current != nullptr)
        current->unsupported("vpi_get_systf_info");
}

vpiHandle vpi_handle_by_name(PLI_BYTE8* name, vpiHandle scope) {
    PliContext* current = context();
    return current != nullptr ? current->handleByName(name, scope) : nullptr;
}

vpiHandle vpi_handle_by_index(vpiHandle object, PLI_INT32 indx) {
    PliContext* current = context();
    return current != nullptr ? current->handleByIndex(object, indx) : nullptr;
}

vpiHandle vpi_handle_by_multi_index(vpiHandle /*obj*/, PLI_INT32 /*num_index*/,
                                    PLI_INT32* /*index_array*/) {
    if (PliContext* current = context(); current != nullptr)
        current->unsupported("vpi_handle_by_multi_index");
    return nullptr;
}

vpiHandle vpi_handle(PLI_INT32 type, vpiHandle refHandle) {
    PliContext* current = context();
    return current != nullptr ? current->handle(type, refHandle) : nullptr;
}

vpiHandle vpi_handle_multi(PLI_INT32 /*type*/, vpiHandle /*refHandle1*/, vpiHandle /*refHandle2*/,
                           ...) {
    if (PliContext* current = context(); current != nullptr)
        current->unsupported("vpi_handle_multi");
    return nullptr;
}

vpiHandle vpi_iterate(PLI_INT32 type, vpiHandle refHandle) {
    PliContext* current = context();
    return current != nullptr ? current->iterate(type, refHandle) : nullptr;
}

vpiHandle vpi_scan(vpiHandle iterator) {
    PliContext* current = context();
    return current != nullptr ? current->scan(iterator) : nullptr;
}

PLI_INT32 vpi_get(PLI_INT32 property, vpiHandle object) {
    PliContext* current = context();
    return current != nullptr ? current->get(property, object) : vpiUndefined;
}

PLI_INT64 vpi_get64(PLI_INT32 property, vpiHandle object) {
    PliContext* current = context();
    return current != nullptr ? current->get(property, object) : vpiUndefined;
}

PLI_BYTE8* vpi_get_str(PLI_INT32 property, vpiHandle object) {
    PliContext* current = context();
    return current != nullptr ? current->getString(property, object) : nullptr;
}

void vpi_get_delays(vpiHandle /*object*/, p_vpi_delay /*delay_p*/) {
    if (PliContext* current = context(); current != nullptr) current->unsupported("vpi_get_delays");
}

void vpi_put_delays(vpiHandle /*object*/, p_vpi_delay /*delay_p*/) {
    if (PliContext* current = context(); current != nullptr) current->unsupported("vpi_put_delays");
}

void vpi_get_value(vpiHandle expr, p_vpi_value value_p) {
    if (PliContext* current = context(); current != nullptr) current->getValue(expr, value_p);
}

vpiHandle vpi_put_value(vpiHandle object, p_vpi_value value_p, p_vpi_time time_p, PLI_INT32 flags) {
    PliContext* current = context();
    return current != nullptr ? current->putValue(object, value_p, time_p, flags) : nullptr;
}

void vpi_get_value_array(vpiHandle /*expr*/, p_vpi_arrayvalue /*arrayvalue_p*/,
                         PLI_INT32* /*index_p*/, PLI_UINT32 /*num*/) {
    if (PliContext* current = context(); current != nullptr)
        current->unsupported("vpi_get_value_array");
}

void vpi_put_value_array(vpiHandle /*object*/, p_vpi_arrayvalue /*arrayvalue_p*/,
                         PLI_INT32* /*index_p*/, PLI_UINT32 /*num*/) {
    if (PliContext* current = context(); current != nullptr)
        current->unsupported("vpi_put_value_array");
}

void vpi_get_time(vpiHandle object, p_vpi_time time_p) {
    if (PliContext* current = context(); current != nullptr) current->getTime(object, time_p);
}

PLI_UINT32 vpi_mcd_open(PLI_BYTE8* /*fileName*/) {
    // Only the transcript channel exists. Opening files is what `$fopen` does in a design,
    // and Enzan does not implement that either.
    if (PliContext* current = context(); current != nullptr) current->unsupported("vpi_mcd_open");
    return 0;
}

PLI_UINT32 vpi_mcd_close(PLI_UINT32 mcd) {
    // Closing the transcript is refused, as the standard requires, by reporting it as still
    // open; any other channel was never opened.
    if (PliContext* current = context(); current != nullptr && mcd != TranscriptChannel)
        current->unsupported("vpi_mcd_close of a channel other than the transcript");
    return mcd;
}

PLI_BYTE8* vpi_mcd_name(PLI_UINT32 cd) {
    static std::array<PLI_BYTE8, 7> name = {'s', 't', 'd', 'o', 'u', 't', '\0'};
    static_cast<void>(context());
    return cd == TranscriptChannel ? name.data() : nullptr;
}

PLI_INT32 vpi_mcd_vprintf(PLI_UINT32 mcd, PLI_BYTE8* format, va_list ap) {
    PliContext* current = context();
    if (current == nullptr || (mcd & TranscriptChannel) == 0) return 0;
    return current->print(enzan::pli::formatted(format, ap));
}

PLI_INT32 vpi_mcd_printf(PLI_UINT32 mcd, PLI_BYTE8* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const PLI_INT32 written = vpi_mcd_vprintf(mcd, format, arguments);
    va_end(arguments);
    return written;
}

PLI_INT32 vpi_mcd_flush(PLI_UINT32 mcd) {
    PliContext* current = context();
    if (current == nullptr || (mcd & TranscriptChannel) == 0) return 1;
    return current->flush();
}

PLI_INT32 vpi_vprintf(PLI_BYTE8* format, va_list ap) {
    PliContext* current = context();
    return current != nullptr ? current->print(enzan::pli::formatted(format, ap)) : 0;
}

PLI_INT32 vpi_printf(PLI_BYTE8* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const PLI_INT32 written = vpi_vprintf(format, arguments);
    va_end(arguments);
    return written;
}

PLI_INT32 vpi_flush(void) {
    PliContext* current = context();
    return current != nullptr ? current->flush() : 1;
}

PLI_INT32 vpi_compare_objects(vpiHandle object1, vpiHandle object2) {
    PliContext* current = context();
    return current != nullptr ? current->compareObjects(object1, object2) : 0;
}

PLI_INT32 vpi_chk_error(p_vpi_error_info error_info_p) {
    // The one routine that does not forget the last error: reporting it is its whole purpose.
    PliContext* current = PliContext::current();
    return current != nullptr ? current->checkError(error_info_p) : 0;
}

PLI_INT32 vpi_free_object(vpiHandle object) {
    PliContext* current = context();
    return current != nullptr ? current->freeObject(object) : 0;
}

PLI_INT32 vpi_release_handle(vpiHandle object) {
    PliContext* current = context();
    return current != nullptr ? current->freeObject(object) : 0;
}

PLI_INT32 vpi_get_vlog_info(p_vpi_vlog_info vlog_info_p) {
    PliContext* current = context();
    return current != nullptr ? current->getVlogInfo(vlog_info_p) : 0;
}

PLI_INT32 vpi_get_data(PLI_INT32 /*id*/, PLI_BYTE8* /*dataLoc*/, PLI_INT32 /*numOfBytes*/) {
    // Save and restart are not supported yet.
    if (PliContext* current = context(); current != nullptr) current->unsupported("vpi_get_data");
    return 0;
}

PLI_INT32 vpi_put_data(PLI_INT32 /*id*/, PLI_BYTE8* /*dataLoc*/, PLI_INT32 /*numOfBytes*/) {
    if (PliContext* current = context(); current != nullptr) current->unsupported("vpi_put_data");
    return 0;
}

void* vpi_get_userdata(vpiHandle /*obj*/) {
    // User data belongs to a user system task, which `vpi_register_systf` cannot create.
    if (PliContext* current = context(); current != nullptr)
        current->unsupported("vpi_get_userdata");
    return nullptr;
}

PLI_INT32 vpi_put_userdata(vpiHandle /*obj*/, void* /*userdata*/) {
    if (PliContext* current = context(); current != nullptr)
        current->unsupported("vpi_put_userdata");
    return 0;
}

PLI_INT32 vpi_control(PLI_INT32 operation, ...) {
    // The diagnostic level that may follow `vpiFinish` and `vpiStop` selects what a simulator
    // prints as it stops, and Enzan prints the same thing whichever is asked for.
    PliContext* current = context();
    return current != nullptr ? current->control(operation) : 0;
}

} // extern "C"

// NOLINTEND(readability-identifier-naming, cppcoreguidelines-pro-type-vararg, cert-dcl50-cpp,
//           modernize-avoid-variadic-functions)
