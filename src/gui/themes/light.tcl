# SPDX-License-Identifier: GPL-3.0-or-later
#
# Enzan's light theme, built in beside the default dark one.
#
# It sets every key the dark theme sets, so that it stands on its own rather than borrowing from
# the dark one: a colour left to the dark theme would be a dark patch in a light window. The
# commands are the dark theme's, `theme <key> <colour>` and `font <area> <points>`, and each key
# means what it means there; the colours keep the same distinctions on a white ground.
#
# Choose it from the window's View > Theme menu, or with `theme -builtin light` in
# ~/.config/enzan/config. New Theme, with this theme in use, writes a copy of it to edit.

# --- Chrome ------------------------------------------------------------------
#
# White panels on a pale grey window, so that the panes are told apart by their edges, as in the
# dark theme.

theme window.background            #f6f7f9
theme window.foreground            #1f2328
theme panel.background             #ffffff
theme panel.foreground             #1f2328
theme panel.alternate.background   #f6f8fa
theme panel.border                 #d0d7de
theme header.background            #eceff3
theme header.foreground            #545d68
theme selection.background         #cce0fa
theme selection.foreground         #0b1726
theme control.background           #f0f2f5
theme control.foreground           #1f2328
theme control.border               #b8c0c9
theme accent                       #0969da

# --- Transcript --------------------------------------------------------------
#
# The severities darkened until they read as text on white: an amber light enough to look like
# the terminal's is too light to read there.

theme transcript.background        #ffffff
theme transcript.foreground        #24292f
theme transcript.note              #656d76
theme transcript.warning           #8a5a00
theme transcript.error             #c4202b

# --- Source window -----------------------------------------------------------

theme source.background            #ffffff
theme source.foreground            #1f2328
theme source.gutter.background     #f6f8fa
theme source.gutter.foreground     #7d8590
theme source.current.line          #fff5c2
theme source.current.marker        #0969da
theme source.breakpoint            #c4202b
theme source.breakpoint.disabled   #f2b8bd

# --- Waveform view -----------------------------------------------------------
#
# The default theme's black background and light green traces, turned over: a white background
# and dark green traces. Unknown and high impedance keep their colours of their own, red and
# blue, darkened to read on white.

theme wave.background              #ffffff
theme wave.trace                   #1a7f37
theme wave.trace.fill              #b7e4c0
theme wave.text                    #0f5323
theme wave.unknown                 #c4202b
theme wave.highimpedance           #0969da
theme wave.grid                    #e6eaee
theme wave.row.separator           #eef0f3
theme wave.row.selected            #dceafc
theme wave.ruler.background        #f3f4f6
theme wave.ruler.foreground        #545d68
theme wave.cursor                  #8a5a00
theme wave.cursor.locked           #b33c00
theme wave.cursor.text             #ffffff

# A shade off white, so that time the kernel has not simulated yet does not read as a flat signal.
theme wave.beyond.recorded         #eceef1

theme wave.zoom.band               #338a5a00

# Pale on purpose, as the dark theme's are dark on purpose, and told apart by hue.
theme wave.highlight.1             #e3e3e3
theme wave.highlight.2             #d3efcf
theme wave.highlight.3             #f4e6c1
theme wave.highlight.4             #f1d8ec
theme wave.highlight.5             #cde7ee

# A shade darker than the panel's text, as the dark theme's is a shade brighter.
theme wave.highlight.text          #14181d

# --- Fonts -------------------------------------------------------------------
#
# None is set, as in the dark theme, so text is the desktop's size. These would set them:
#
# font window      10
# font wave        11
# font transcript  9
# font source      10
