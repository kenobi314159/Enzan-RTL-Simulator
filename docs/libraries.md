# Libraries, file lists and building with make

By default Enzan analyses a design's sources, simulates it and keeps nothing. This file covers what
a larger build needs: analysis libraries, file lists and dependency files, and a Makefile that ties
them together.

## Analysis libraries

A library is a directory of analysed design units. Libraries are opt-in. Without `--out-lib`
nothing is written, and Enzan never reads a library you did not name. VHDL's standard libraries,
`std` and `ieee`, are the exception: they are part of the installation and are found without being
named.

```
enzan lib create [--name <n>] <dir>   create a library and fix its logical name
enzan lib list <dir>                  what a library holds, and what wrote it
enzan lib clean <dir>                 empty it, keeping the library

--out-lib <dir>                       the library analysis is written into
--in-lib <dir>                        a library to read; repeatable, searched left to right
--in-lib <name>=<dir>                 the same, under another name than the one it carries
--context <file>                      analysed so references resolve, but not stored
```

**A library's logical name is stored in the library.** It is fixed when the library is created and
read back from it, so the build that writes a library and the run that reads it cannot disagree
about the name. `--out-lib` on a directory that does not exist creates a library named after the
directory, and refuses when that name is not an identifier. `<name>=<dir>` covers the cases a
stored name cannot, such as two vendors each shipping a library called `work`. A directory that was
never created as a library is an error rather than an empty library.

**A library holds its time unit.** A delay is stored as a count of the unit it was analysed in, so
a run from a library takes its unit from there and `--timescale` may be left out. A `--timescale`
that disagrees with a library's unit is an error, and so are two libraries in different units.
After `lib clean`, a library can be refilled in another unit.

**An entry is current or it is analysed again.** Each entry records what it was analysed from: the
source text, the defines, the include path, the time unit and Enzan's version. A source analysed
with anything different is analysed again. Entries are named after their source files, so a build
can name them, and editing a file rewrites its entry rather than adding another.

**A name two libraries both define is an error.** The message names both libraries, and the fix
is to qualify the name, as in `uart.fifo`. Two copies of the same unit, analysed from the same
source with the same options, are one unit and not a conflict.

### SystemVerilog across a library boundary

`--in-lib` resolves a module that the design instantiates but does not define. For that, a library
records the paths of the sources it was analysed from, and an analysis against it reads those
sources again: a module or package from another library has to be parsed in order to be
type-checked against. A file named more than once, as a source, as context or as a library's
source, is read once.

A parameterised module becomes one unit for each set of parameter values the design uses, named
after the values. A design that needs `Width=8` therefore creates that unit from the sources it
read and stores it in its own library, unless an input library already holds it. A parameterised
top is still named on the command line by its source name, as in `enzan run counter`, and its
hierarchy starts at `/counter`.

**Split a design into libraries for its names, not for speed.** Crossing a boundary re-reads the
other library's sources, so a split saves no analysis and adds some. An edit to a library's sources
rebuilds that library and every library analysed against it. Build a design as one library unless
names have to be kept apart: two vendors' modules that share a name, the `library` clauses of a
VHDL design, or IP whose owner and build are not yours.

`--context` names a source that is analysed so references resolve, without its units being stored.
Use it for a package shared by libraries that do not depend on each other, or a vendor header
compiled against but not redistributed. A context file is still a dependency of every library that
names it, and appears in its dependency file.

### VHDL in libraries

A VHDL library holds the units as VHDL analyses them, so one VHDL source can use a unit from
another library without re-reading its source: `use uart.uart_pkg.all` resolves against the
library on `--in-lib`. Sources analysed with `--out-lib` go into that library under its name, which
`library uart;` and `work.` both refer to. A `--work` naming another library is an error.
Analysing a source again replaces its units, and units that no source declares any more stay until
`lib clean`. A run given only libraries finds a VHDL top in whichever library declares it, or in
the one `lib.top` names.

## File lists

Enzan reads `.f` files with `-f` and `-F`, nested to any depth. Inside them it accepts `-v`, `-y`,
`+incdir+`, `+define+`, `+libext+` and `-sv`, `//` and `/* */` comments, double-quoted arguments,
and `$VAR` and `${VAR}` from the environment.

- **A path inside a list read with `-f` resolves against the working directory; one read with
  `-F` resolves against the list's own directory.** That is the convention existing file lists are
  written for, on the command line and nested.
- **An option Enzan does not recognise is an error**, reported with its file and line. File lists
  carry tool-specific switches that change what is compiled, and skipping one would quietly
  simulate a different design. `-sv` is the only switch accepted and ignored.
- **An unset variable is an error**, rather than an empty string that turns `$RTL_ROOT/cpu.sv` into
  `/cpu.sv`.
- **`-v <file>` and `-y <directory>` supply modules nothing else defines.** When the sources
  instantiate a module that no source and no library you resolve against defines, Enzan looks for
  it in the `-v` files and then as `<module>.v` in the `-y` directories, in the order the lists give
  them; `+libext+.v+.sv` sets the extensions tried, in order. A file found is analysed like a
  source, every module in it whether the design uses it or not, stored in `--out-lib` and named in
  the dependency file, and what it instantiates is looked for in turn. Only files holding a module
  the design needs are read. A VHDL component with no entity of its name binds to a module found
  this way.

`--dry-run` prints the list as expanded, in order, and exits.

`--depfile <file>` writes a make-style dependency file naming every file the analysis read: the
sources the lists expanded to, the files `-v` and `-y` supplied, the headers they included and the
context files. A Makefile can
then name the `.f` file as its only hand-written prerequisite and learn the rest after the first
build, as a C build learns its headers from `-MMD`. `--depfile-target <name>` names the target
those prerequisites belong to. Without it the `--out-lib` directory is used; give it when the
Makefile uses a stamp file instead. Any command that analyses sources can write a dependency file:
`analyse`, `run` and the one-shot form.

## A design built with make

This Makefile builds a design split into three libraries: two IP blocks, a UART and an SPI
controller that share a package and do not depend on each other, and a testbench that instantiates
both. A design without names to keep apart is the same Makefile with one library.

```make
ENZAN ?= enzan
PKG   := rtl/pkg/types_pkg.sv

# A library is analysed in one time unit, and a run takes it from there.
UNIT  := --timescale ns

# Creating a library is a step of its own: analysis never creates one by itself here.
libs/%/.created:
	$(ENZAN) lib create libs/$*
	@touch $@

# One stamp per library: a library is the smallest thing make rebuilds.
#
# Neither IP library stores the package. Each reads it only to resolve against, so there is
# exactly one copy of it at elaboration.
libs/uart/.stamp: rtl/uart.f | libs/uart/.created
	$(ENZAN) analyse $(UNIT) --out-lib libs/uart -f rtl/uart.f --context $(PKG) \
	         --depfile libs/uart/deps.d --depfile-target $@
	@touch $@

libs/spi/.stamp: rtl/spi.f | libs/spi/.created
	$(ENZAN) analyse $(UNIT) --out-lib libs/spi -f rtl/spi.f --context $(PKG) \
	         --depfile libs/spi/deps.d --depfile-target $@
	@touch $@

# The testbench's list names the package as a source, so the package is stored here, with the
# design that ties the two IPs together. Analysing against both IP libraries reads their sources,
# and the dependency file names them.
libs/tb/.stamp: rtl/tb.f libs/uart/.stamp libs/spi/.stamp | libs/tb/.created
	$(ENZAN) analyse $(UNIT) --out-lib libs/tb -f rtl/tb.f \
	         --in-lib libs/uart --in-lib libs/spi \
	         --depfile libs/tb/deps.d --depfile-target $@
	@touch $@

# Every file each analysis read. Before the first build only the .f files are known, which is
# enough, because nothing has been built yet.
-include $(wildcard libs/*/deps.d)

IN     := --in-lib libs/uart --in-lib libs/spi --in-lib libs/tb
STAMPS := libs/uart/.stamp libs/spi/.stamp libs/tb/.stamp

sim: $(STAMPS)
	$(ENZAN) run $(IN) --fst wave.fst tb

gui: $(STAMPS)
	$(ENZAN) run --gui $(IN) tb

check: $(STAMPS)
	$(ENZAN) elaborate $(IN) tb

clean:
	rm -rf libs
```

What each edit rebuilds:

- A file in `rtl/tb` rebuilds `libs/tb` alone, though that analysis reads both IPs' sources again.
- A file in `rtl/uart` rebuilds `libs/uart` and `libs/tb`, which reads it, and leaves `libs/spi`
  alone.
- `types_pkg.sv` rebuilds all three, because each names it.

None of these is cheaper than rebuilding a single library holding the whole design. `make check`
elaborates without simulating and stores nothing, which is what a CI job wants when it only needs
to know that the design still builds.
