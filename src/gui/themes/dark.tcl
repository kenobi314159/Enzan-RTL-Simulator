# SPDX-License-Identifier: GPL-3.0-or-later
#
# Enzan's default theme.
#
# This file is the only place Enzan's colours are written down. DESIGN_GUIDE.md section 10.5
# requires every colour to come from the theme configuration file and forbids colour literals in
# widget code, so there is no fallback in C++ to fall back to: if a key below is missing, the
# widget asking for it gets nothing.
#
# Section 8.1 makes Tcl the configuration language for the whole tool, with no second format, so
# this is a Tcl script rather than an ini file. It gets one command:
#
#     theme <key> <colour>
#
# where the colour is anything Qt accepts — `#rrggbb`, `#aarrggbb` or an SVG colour name.
#
# To change a colour, copy this file, edit it, and pass it with `enzan-gui --theme <file>`. A
# user theme is layered over this one, so a file that sets three colours changes three colours.
# `enzan-gui --write-theme <file>` writes this text out for you.

# --- Chrome ------------------------------------------------------------------
#
# Dark throughout, as section 10.5 requires. The greys are close together on purpose: a dense
# tool that a user reads for hours should separate its panes with contrast at the edges rather
# than with large steps in brightness across the whole window.

theme window.background            #16181d
theme window.foreground            #d7dae0
theme panel.background             #1b1e24
theme panel.foreground             #d7dae0
theme panel.alternate.background   #1f232a
theme panel.border                 #2b3038
theme header.background            #22262e
theme header.foreground            #9aa3b0
theme selection.background         #2f4f7a
theme selection.foreground         #f2f5f9
theme control.background           #22262e
theme control.foreground           #d7dae0
theme control.border               #343a44
theme accent                       #6cb6ff

# --- Transcript --------------------------------------------------------------
#
# The severity colours are the ones the diagnostic engine's severities map onto. They are
# deliberately close to what a terminal uses, because that is what the same message looks like
# when the kernel is run without a GUI and the two should not appear to disagree.

theme transcript.background        #131519
theme transcript.foreground        #ccd1d9
theme transcript.note              #7f8794
theme transcript.warning           #e3b341
theme transcript.error             #f85149

# --- Waveform view -----------------------------------------------------------
#
# Section 10.5 fixes two of these: a high-contrast black background and light green traces.
# The rest follow from them.
#
# Unknown and high-impedance get their own colours and are not left to look like data. An X that
# reads as a plausible value on screen is the failure mode section 3.9 keeps X around to prevent.

theme wave.background              #000000
theme wave.trace                   #7ee787
theme wave.trace.fill              #1d3325
theme wave.text                    #b9f6ca
theme wave.unknown                 #f85149
theme wave.highimpedance           #58a6ff
theme wave.grid                    #1a1f26
theme wave.row.separator           #141a20
theme wave.row.selected            #10202c
theme wave.ruler.background        #0b0d10
theme wave.ruler.foreground        #8b949e
theme wave.cursor                  #ffd866
theme wave.cursor.locked           #ff8f6b
theme wave.cursor.text             #1a1300

# Past the point the kernel has simulated to. Distinct from the background so that "the design
# has not got there yet" does not read as "this signal is flat".
theme wave.beyond.recorded         #090c12

# The band drawn over the range a right-button drag is about to zoom to.
theme wave.zoom.band               #40ffd866

# The five backgrounds a row can be highlighted with. Dark on purpose: a highlight marks a row
# for the reader's own benefit and must not out-shout the trace drawn on top of it, which is the
# thing they are actually looking at. They differ in hue rather than in brightness so that five
# highlighted rows are told apart at a glance without any of them dominating.
theme wave.highlight.1             #17222e
theme wave.highlight.2             #1c2418
theme wave.highlight.3             #2a2118
theme wave.highlight.4             #261a24
theme wave.highlight.5             #16252a
