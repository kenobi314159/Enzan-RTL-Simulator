# SPDX-License-Identifier: GPL-3.0-or-later
#
# Enzan's build-in light theme
#
# Supported commands:
#
#     theme <key> <colour>
#     font <area> <points>
#
# where the colour is anything Qt accepts — `#rrggbb`, `#aarrggbb` or an SVG colour name — and
# the size is in points, at the end of this file.
#
# Any option not defined here is taken from the default build-in theme.
# Use window option View > Theme to manipulate theme files.

# --- Chrome ------------------------------------------------------------------

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

theme wave.beyond.recorded         #090c12

theme wave.zoom.band               #40ffd866

theme wave.highlight.1             #444444
theme wave.highlight.2             #2c5428
theme wave.highlight.3             #5a4b28
theme wave.highlight.4             #512a48
theme wave.highlight.5             #26455a

theme wave.highlight.text          #f2f5f9

# --- Fonts -------------------------------------------------------------------
#
# Unset -> taken from system desktop settings by default.
#
# font window      10
# font wave        11
# font transcript  9
# font source      10
