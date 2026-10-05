# `src/frontend/sv` — the SystemVerilog frontend

Turns SystemVerilog source text into EIR, and nothing else (DESIGN_GUIDE.md section 5).

## Responsibility

- Drive [slang](https://sv-lang.com/) over the input files and translate its diagnostics
  into Enzan's diagnostic engine.
- Walk slang's elaborated AST and emit one EIR module definition per distinct instance
  body.
- Translate procedural blocks, continuous assignments and expressions into EIR-B.

## What a frontend must not do

Section 5 is explicit: a frontend MUST NOT flatten hierarchy, make scheduling decisions or
allocate runtime state. Section 17.4 adds that slang-specific code stays confined to this
directory — **no slang type may appear in an EIR header**, because that would make every
downstream component depend on slang.

If this component ever needs something from the rest of Enzan beyond `common` and `eir`,
that is a design smell and should be raised rather than worked around.

## Why slang does the elaboration and Enzan does it again

slang resolves names and types, evaluates parameters and expands generate constructs, and
hands back an elaborated AST. Enzan then runs its *own* elaboration over the EIR. The two
are not the same job: slang's produces a typed tree per parameterisation, Enzan's produces
an instance tree and a state arena layout for one chosen top level (section 2.1).

The consequence for this component is that **a parameterised module becomes one EIR
definition per distinct parameterisation**, with a numeric suffix distinguishing them. That
keeps parameters out of EIR entirely, which is what makes an analysed unit reusable without
re-evaluating anything. The suffixes are assigned in hierarchy-walk order, so they are the
same on every run (section 3.8).

## Deliberate translation decisions

**A block header's event control becomes the process's sensitivity list**, not a statement
inside its body. The scheduler re-arms a process, so the body has to begin after the wait.
`always @(*)` is the exception that needs the read set, so its list is filled in once the
body has been walked.

**A port connection that is an expression gets an intermediate object and a continuous
assignment.** The EIR contract says a connection names an object (see `eir/Module.h`), so
`.d({a, b})` becomes a real assignment in the EIR rather than something the elaborator has
to understand. Only inputs are handled this way: driving an expression from an output would
need the assignment to run the other way.

**A construct that is not supported abandons the whole process.** A partially translated
body would produce a design that simulates and gives the wrong answer, which section 1.2
ranks as the worst possible outcome. Refusing to elaborate is the safe failure.

**A reference into another instance is a hierarchical reference**, an object with a relative
path and no storage, which elaboration binds. Creating storage for it instead would make
`dut.state` a copy nothing ever writes.

**An interface is an instance, and a member through an interface port is a reference through
the port** (`@in/data`); each instance records what its interface ports are connected to.

**A function or task call is inlined.** The body is translated into the process that calls
it, with its arguments, return variable and locals bound to registers, so a task's delays
suspend the caller as its own statements would and the caller's read and write sets are
exactly what the call touches. A static subroutine's variables are treated as automatic,
which differs only for a design that reads a local before writing it and relies on the value
a previous call left there.

**A select or member access is a window of bits.** Unpacked arrays and structs are stored
flat (DESIGN_GUIDE.md section 4.3), so `mem[i].field[3:0]` composes into one offset into one
object and is read with `ReadObjectPart` and written with a part store. An index that could
land past an inner dimension is checked, and outside it the offset is X, which reads as X and
writes nothing.

**Declarations are hoisted, keeping their scope in their name.** A generate block's
variables become module objects called `genblk1.q`, and block-local variables are created
on demand when something references them. Creating on demand rather than requiring every
variable to be found first matters for upgrades: block-local and loop variables live in
scopes slang has moved between releases, and a frontend that depends on where they are
attached breaks when it is rebased.

## Not yet supported

Reported as `ENZ1004` with the construct named, rather than mistranslated:

- dynamic and associative arrays, queues, unpacked unions, `real`, `string` and class types
  (packed vectors, structs and arrays, enums, and fixed-size unpacked arrays and structs all
  work). A computation with reals whose operands are all constants, such as `Period >= 2ps`
  with `Period` a parameter, is folded to its integral result;
- a recursive call, which slang does not fold to a constant, and a `ref` argument;
- deferred assertions (`assert #0`, `assert final`) and concurrent ones (`assert property`),
  which are SVA; immediate assertions work;
- every system function but `$time`, `$signed`, `$unsigned`, `$bits`, `$clog2`, `$isunknown`,
  `$countones`, `$onehot` and `$onehot0`, and those slang folds to a constant;
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
