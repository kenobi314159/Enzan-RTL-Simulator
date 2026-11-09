# SPDX-License-Identifier: GPL-3.0-or-later
#
# Enzan's built-in dark theme.
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

theme transcript.background        #131519
theme transcript.foreground        #ccd1d9
theme transcript.note              #7f8794
theme transcript.warning           #e3b341
theme transcript.error             #f85149

# The colours of a terminal's colour codes in a program's output. On this theme's background
# each has to be as readable as text, so black is a grey.
theme transcript.terminal.black          #8b949e
theme transcript.terminal.red            #ff7b72
theme transcript.terminal.green          #3fb950
theme transcript.terminal.yellow         #d29922
theme transcript.terminal.blue           #58a6ff
theme transcript.terminal.magenta        #bc8cff
theme transcript.terminal.cyan           #39c5cf
theme transcript.terminal.white          #d7dae0
theme transcript.terminal.bright.black   #a5adb8
theme transcript.terminal.bright.red     #ffa198
theme transcript.terminal.bright.green   #56d364
theme transcript.terminal.bright.yellow  #e3b341
theme transcript.terminal.bright.blue    #79c0ff
theme transcript.terminal.bright.magenta #d2a8ff
theme transcript.terminal.bright.cyan    #56d4dd
theme transcript.terminal.bright.white   #ffffff

# --- Source window -----------------------------------------------------------

theme source.background            #15181d
theme source.foreground            #d7dae0
theme source.gutter.background     #1b1e24
theme source.gutter.foreground     #6e7681
theme source.current.line          #2a3a4a
theme source.current.marker        #6cb6ff
theme source.breakpoint            #f85149
theme source.breakpoint.disabled   #6b3b38

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
