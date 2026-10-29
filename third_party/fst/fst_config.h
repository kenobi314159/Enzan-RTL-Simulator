/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Build configuration for the vendored FST library.
 *
 * fstapi.c includes whatever FST_CONFIG_INCLUDE names, defaulting to GTKWave's autoconf
 * <config.h>. Enzan does not use autoconf, so this file stands in for it and is selected with
 * -DFST_CONFIG_INCLUDE="<fst_config.h>" from third_party/CMakeLists.txt.
 *
 * This is Enzan's file, not upstream's, which is why it carries Enzan's licence rather than
 * MIT. Everything else in this directory is vendored unmodified; see docs/third_party.md.
 */

#ifndef ENZAN_FST_CONFIG_H
#define ENZAN_FST_CONFIG_H

/* Linux provides both. Enzan targets Linux only, so these are asserted rather than probed; a
 * port would have to probe them, and would have far more to change than this file. */
#define HAVE_ALLOCA_H 1
#define HAVE_FSEEKO 1
#define HAVE_REALPATH 1

/*
 * Judy arrays are deliberately *not* enabled.
 *
 * libJudy is LGPL, and upstream's own comment notes that the Jenkins hash alternative is
 * functionally equivalent and appears slightly faster. Taking on an extra dependency with a
 * different licence for no measured gain is the wrong trade.
 */
/* #undef _WAVE_HAVE_JUDY */

/*
 * The parallel writer is deliberately *not* enabled.
 *
 * It spawns a thread that compresses blocks while simulation continues. The kernel is
 * single-threaded by design and determinism is a hard requirement; introducing a background
 * thread into the one component whose output is byte-compared against a reference simulator
 * would put that at risk for a gain nobody has measured. `bench/` exists so that this can be
 * revisited with a number rather than a hunch.
 */
/* #undef HAVE_LIBPTHREAD */

#endif /* ENZAN_FST_CONFIG_H */
