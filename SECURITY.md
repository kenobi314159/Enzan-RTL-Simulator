# Security policy

## Reporting a vulnerability

Report a vulnerability privately, through
[GitHub's private reporting](https://github.com/kenobi314159/Enzan-RTL-Simulator/security/advisories/new),
rather than in a public issue. The report stays private until a release fixes it.

A fix goes into the next release. Earlier releases are not updated.

## What counts

A design is a program, and Enzan runs it. What a design can do because its language lets it is not
a vulnerability, and simulating a design you do not trust is running a program you do not trust:

- A design reads and writes files, through `$fopen`, `$readmemh`, `std.textio` and VHDL's files.
- `--vpi` and `--vhpi` load a shared library into the simulator, where it runs with the
  simulator's rights. That is how cocotb works.
- An analysis library holds analysed design units, and what it holds runs when it is simulated.
- A theme is a Tcl script, which the window runs when it loads the theme, so a theme someone sends
  you is a program too.

A vulnerability is Enzan doing more than an input asks for, where the input is meant to be data:

- A signal list file, which is parsed and never run, or the `config` file in the configuration
  directory, running code, writing a file or corrupting memory.
- A front end attached to a kernel, or a kernel a front end attaches to, corrupting the other's
  memory through the socket between them.

Anyone who can connect to a kernel's socket can drive its simulation, which is what the socket is
for, so it is guarded by its file's permissions. `enzan --gui` puts the socket in
`$XDG_RUNTIME_DIR` when that is set.

A crash on an input that is not data, such as an HDL source Enzan fails to analyse, is a bug rather
than a vulnerability: report it as an issue.
