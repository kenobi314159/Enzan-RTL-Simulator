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

theme wave.beyond.recorded         #eceef1

theme wave.zoom.band               #338a5a00

theme wave.highlight.1             #e3e3e3
theme wave.highlight.2             #d3efcf
theme wave.highlight.3             #f4e6c1
theme wave.highlight.4             #f1d8ec
theme wave.highlight.5             #cde7ee

theme wave.highlight.text          #14181d

# --- Fonts -------------------------------------------------------------------
#
# Unset -> taken from system desktop settings by default.
#
# font window      10
# font wave        11
# font transcript  9
# font source      10
