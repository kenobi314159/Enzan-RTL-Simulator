# SPDX-License-Identifier: GPL-3.0-or-later
#
# The status `quit` asks for must reach the shell: a makefile that runs a regression through
# `enzan --do` has nothing else to go on.
puts "asking to fail"
quit -code 4
