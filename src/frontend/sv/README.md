# `src/frontend/sv` — the SystemVerilog frontend

Turns SystemVerilog source text into EIR, and nothing else.

## Responsibility

- Drive [slang](https://sv-lang.com/) over the input files and translate its diagnostics
  into Enzan's diagnostic engine.
- Walk slang's elaborated AST and emit one EIR module definition per distinct instance
  body.
- Translate procedural blocks, continuous assignments and expressions into EIR-B.
- Specialise the modules another language's units instantiate, as each `eir::UnitRequest` asks,
  through a module of its own instantiating the one asked for, which is dropped after; and list
  the modules the sources declare, and the names they instantiate and declare nowhere, from their
  syntax alone, for the other frontend.
- Check an instance of another language's unit against a module standing for it, built from its
  `eir::UnitInterface`, and make it ask for the unit with its values rather than translate it.

## What a frontend must not do

A frontend MUST NOT flatten hierarchy, make scheduling decisions or allocate runtime state,
and slang-specific code stays confined to this directory — **no slang type may appear in an
EIR header**, because that would make every downstream component depend on slang.

If this component ever needs something from the rest of Enzan beyond `common` and `eir`,
that is a design smell and should be raised rather than worked around.

## Why slang does the elaboration and Enzan does it again

slang resolves names and types, evaluates parameters and expands generate constructs, and
hands back an elaborated AST. Enzan then runs its *own* elaboration over the EIR. The two
are not the same job: slang's produces a typed tree per parameterisation, Enzan's produces
an instance tree and a state arena layout for one chosen top level.

The consequence for this component is that **a parameterised module becomes one EIR
definition per distinct parameterisation**, named for its parameter values and for the
interfaces its ports are connected to. That keeps parameters out of EIR entirely, which is
what makes an analysed unit reusable without re-evaluating anything, and it gives a
parameterisation the same name wherever it is produced.

## Deliberate translation decisions

**A delay counts the time unit of the scope stating it** and is translated into a count of the run's
unit: the module's `` `timescale `` or `timeunit`, or a task's package's, rounded to that scope's
precision and then, where the run's unit is fixed and coarser, to the run's with a warning
(ENZ1009). `$time` is divided back into the calling module's unit, and a system task call carries
that unit for `%t`.

**A block header's event control becomes the process's sensitivity list**, not a statement
inside its body. The scheduler re-arms a process, so the body has to begin after the wait.
`always @(*)` is the exception that needs the read set, so its list is filled in once the
body has been walked.

**A port connection that is an expression gets an intermediate object and a continuous
assignment.** The EIR contract says a connection names an object (see `eir/Module.h`), so
`.d({a, b})` becomes a real assignment in the EIR rather than something the elaborator has
to understand: into the port's object for an input, out of it for an output. A signal of
another width than its port is connected the same way, since elaboration joins a port only to
a signal as wide as it; the assignment extends or truncates as one written out would (IEEE
1800 section 23.3.3.7). An `inout` cannot be, since its assignment would run both ways.

**A construct that is not supported abandons the whole process.** A partially translated
body would produce a design that simulates and gives the wrong answer, which is the worst
possible outcome. Refusing to elaborate is the safe failure.

**A reference into another instance is a hierarchical reference**, an object with a relative
path and no storage, which elaboration binds. Creating storage for it instead would make
`dut.state` a copy nothing ever writes.

**A package variable is a reference too**, with the absolute path `/<package>/<variable>`. The
variables modules refer to make up a definition of the package's own, `<package>::`, built once
every module has been translated.

**An interface is an instance, and a member through an interface port is a reference through
the port** (`@in/data`); each instance records what its interface ports are connected to.

**A function or task call is inlined.** The body is translated into the process that calls it,
with its arguments, return variable and automatic locals bound to registers, so a task's delays
suspend the caller as its own statements would and the caller's read and write sets are exactly
what the call touches. A static variable of a subroutine, as each of one declared
without `automatic`, is one variable every call shares: storage of the module, or of the
package, that declares the subroutine, named behind it as `f.count`, and initialised once at
time zero. The module declares it whether or not its own code calls the subroutine, since a call
from another instance, `dev.f(x)`, reaches it by reference as it reaches the module's other
variables; one of a type Enzan cannot hold is reported only where it is used. It is marked `subroutineStatic` in the EIR, so that a call inlined into a continuous
assignment is not its continuous driver and no inferred sensitivity list watches it; callers
watching what one another write there would wake each other for ever.

**A select or member access is a window of bits.** Unpacked arrays and structs are stored flat, so
`mem[i].field[3:0]` composes into one offset into one object and is read with `ReadObjectPart` and
written with a part store. An index that could land past an inner dimension is checked, and outside
it the offset is X, which reads as X and writes nothing.

**Declarations are hoisted, keeping their scope in their name.** A generate block's
variables become module objects called `genblk1.q`, and block-local variables are created
on demand when something references them. Creating on demand rather than requiring every
variable to be found first matters for upgrades: block-local and loop variables live in
scopes slang has moved between releases, and a frontend that depends on where they are
attached breaks when it is rebased.

**Code a constant condition rules out is not translated.** The branch of a procedural `if` whose
condition the parameters fix, and the arm of a `?:` whose condition is known, can never run, so only
the live one is translated. That is also what lets a construct the parameters make degenerate there
be excused: `DeadCode` walks every instance, records the dead branches and arms, and slang's errors
of that family that lie in one in every instance become ENZ1008 warnings. slang wraps a statement or
expression around a bad one in an invalid node, so the translator unwraps those and reaches the live
branch through them.

## Not yet supported

Reported as `ENZ1004` with the construct named, rather than mistranslated:

- dynamic and associative arrays, queues, unpacked unions, `string` and class types (packed
  vectors, structs and arrays, enums, reals, and fixed-size unpacked arrays and structs, of reals
  among them, all work), and a port of a real or holding one. A computation with reals whose
  operands are all constants, such as `Period >= 2ps` with `Period` a parameter, is folded to its
  integral result;
- a recursive call, which slang does not fold to a constant, and a `ref` argument;
- `fork`, whichever way it joins, since its statements run at once as processes of their own;
- a `disable` of anything but a named block the statement is in, within the same process or
  subroutine body: another process's block, the block a subroutine is called from, a task.
  A `disable` of a block it is in goes on after the block, with the next iteration when the
  block is a loop's body;
- `foreach` over a dimension of no fixed size, which needs the dynamic arrays and queues that
  are not here; over fixed dimensions it works, and `break` leaves the whole statement;
- deferred assertions (`assert #0`, `assert final`) and concurrent ones (`assert property`),
  which are SVA; immediate assertions work;
- every system function but `$time`, `$signed`, `$unsigned`, `$bits`, `$clog2`, `$isunknown`,
  `$countones`, `$onehot`, `$onehot0`, `$random`, `$urandom`, `$urandom_range`, `$fopen` and
  `$fgetc`, and those slang folds to a constant. Files are written, with `$fdisplay`, `$fwrite`,
  `$fflush` and `$fclose`, and read a byte at a time by `$fgetc` and whole by `$readmemh` and
  `$readmemb`, into a memory of one unpacked dimension;
- a tolerance range in an `inside` set, such as `[value +/- tolerance]`;
- an event control on an expression other than a signal or a constant select of one, such
  as `@(a & b)` or `@(posedge clk[i])` with `i` a variable;
- an event control on a nonblocking assignment, such as `a <= @(posedge clk) b`, a delayed
  write to part of a signal, such as `a[3:0] <= #5 b`, separate rise, fall and turn-off
  delays, and a delay on a net, such as `wire #5 w`. Intra-assignment delays on whole
  signals and single delays on continuous assignments work, the latter inertially;
- `trireg`, `interconnect` and user-defined nettypes, which have no resolution table;
- a drive strength other than strong, such as `assign (weak0, weak1) w = a`, because
  resolution has no strength model;
