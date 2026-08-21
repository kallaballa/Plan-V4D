# Plan-DSL API Reference (ISA-style)

This document describes **Plan-DSL**, the compile-time task-graph language that makes
up the *Plan* part of Plan-V4D, in the style of an *Instruction Set Architecture* (ISA)
manual. It is intended as the authoritative contract for tools that lower other
intermediate representations — most notably LLVM IR — onto Plan-DSL.

The DSL is not a text language. It is a set of C++ *functions, operators and macros*
invoked from inside the `infer()` / `setup()` / `teardown()` methods of a class that
inherits from `cv::plan::Plan`. Every call records a **task node** and its **data
dependencies** at (compile-time-typed) runtime; the recorded accesses are then compiled
into a *directed acyclic graph* (DAG) that is executed every frame/iteration by a pool of
worker threads.

All API entities live in namespace `cv::plan` (aliased to `using namespace cv::plan;` in
samples). Headers: `<opencv2/plan/plan.hpp>` (DSL) and `<opencv2/v4d/v4d.hpp>` (graphics engine).

---

## 1. Execution Model

### 1.1 The graph is the program

A `Plan` subclass describes, in its three lifecycle methods, how data flows:

| Method    | Purpose                                                          | Runs |
|-----------|------------------------------------------------------------------|------|
| `setup()` | Build the initialization pipeline (alloca-like, one-shot)        | once per worker |
| `infer()` | Build the per-iteration pipeline (the "kernel"/main body)        | re-built every frame |
| `teardown()` | Build the shutdown pipeline (free, one-shot)                  | once per worker |
| `gui()`   | Immediate-mode GUI code (ImGui), runs on the display thread      | per frame |

`Plan::run<PlanT>(workers)` instantiates the plan, starts `workers` worker threads
(each of which builds and runs the full graph), and enters the display loop.

### 1.2 Edges are operands (SSA values)

An **edge** is the operand type of the DSL. It is a typed handle to either a
memory object (a C++ variable, plan member, or runtime property) or to a
computed value (the result of an operator). Every edge carries *access intent*:
read-only, read-write, copy, and/or shared (locked).

Thinking in LLVM terms:

- A **variable** wrapped by an edge ≈ a stack/global allocation (`alloca`/`global`).
- The **result edge** of an operator ≈ an SSA virtual register / temporary.
- An **operator application** ≈ an instruction (`add`, `icmp`, `select`, ...).
- Access intent ≈ the `readnone`/`readonly`/`writeonly` memory attributes.

Edges are created with *edge-calls* (Section 2) and consumed/produced by
*operators* (Section 3) and by *context calls* (Section 5.2).

### 1.3 Task nodes

Each recorded call creates a node keyed by a unique id (derived from a hash of the
callable pointer and the operand identities). Dependencies between nodes are the edges
they share:

- a **read** access (`read_deps_`) links a producer value to the consumer;
- a **write** access (`write_deps_`) enforces ordering between nodes that mutate the
  same object.

A node runs only when all its dependencies are satisfied and, if it is inside a
branch, when the branch condition is currently true. Multiple workers execute
independently built copies of the graph; the scheduler never migrates partial
iterations between workers.

### 1.4 Concurrency primitives

- **Shared variables** (registered with `_shared`) get an associated `std::mutex`.
  A *lockie* edge (`RS`/`RWS`/`CS`) acquires that mutex for the duration of the node's
  transaction.
- **Single-branch** regions are protected by a global per-branch lock so that at most
  one worker executes them at a time.
- **ONCE** regions run at most once (per worker for `PARALLEL_ONCE`).

---

## 2. Edge-Call Instructions (operand producers)

An *edge-call* wraps a value to make it usable as an operand. It is the closest analogue
to "load a register from a storage location" or "materialize a constant".

Template layout of an edge (`detail::Edge<T, Tcopy, Tread, Tshared, Tbase, TbyValue>`):

| Param | Meaning |
|-------|---------|
| `T` | element type |
| `Tcopy` | produce a per-node copy (snapshot) |
| `Tread` | read the value (input) |
| `Tshared` | treat as shared: acquire the variable's mutex for the node |
| `Tbase` | base type when dereferencing a pointer/smart-pointer |
| `TbyValue` | pass by value instead of by reference |

### 2.1 `V(value)` — immediate / constant

```cpp
V(T value)
```
Materializes an immediate constant. Returns `Edge<Ptr<T>, false, true, false, T, true>`.

- LLVM equivalent: a **constant** (`ConstantInt`, `ConstantFP`, `ConstantArray`, ...),
  materialized by `Value`/`LLVMConst*`.

Example: `V(0.0)`, `V(cv::Scalar(102, 61, 51, 255))`, `V(false)`.

### 2.2 `R(variable)` — read operand

```cpp
template<typename T> Edge<T, false, true> R(const T& t)
```
Read-only access to an existing variable. Declares that the consuming node does **not**
mutate the value. Multiple concurrent readers are allowed.

- LLVM equivalent: a **load with read-only semantics** (or an operand that is
  `readonly`/`readnone`). Never becomes a definition.

### 2.3 `RW(variable)` — read-write operand

```cpp
template<typename T> Edge<T, false, false> RW(T& t)
```
Read-**and**-write access to an existing variable. The consuming node both depends on
the previous writer and defines a new value (a "def" for subsequent nodes).

- LLVM equivalent: a **load + store pair** (read-modify-write), i.e. a new definition in
  SSA that also writes back. Use for the destination of `ASSIGN`, `DEREF`, `NEG`, and
  any in-place computation.

### 2.4 `RS(variable)` — read shared

```cpp
template<typename T> Edge<T, false, true, true> RS(const T& t)
```
Read-only access to a **shared** variable. The variable must have been registered with
`_shared()`. The node locks the variable's mutex for its duration.

### 2.5 `RWS(variable)` — read-write shared

```cpp
template<typename T> Edge<T, false, false, true> RWS(T& t)
```
Read-write access to a **shared** variable (registered with `_shared()`), under the
variable's mutex.

### 2.6 `CS(variable)` — copy-shared (snapshot)

```cpp
template<typename T> Edge<T, true, true, true> CS(T& t)
```
A thread-safe **copy** of a shared variable. Reads the variable under its mutex and
produces a private copy. The result is a new value (like a load that snapshots).

- LLVM equivalent: a **load followed by a copy** — for data that is produced on one
  thread (e.g. GUI) and consumed on another without sharing a live definition.

### 2.7 `P<T>(key)` — runtime property

```cpp
template<typename Tval> Property<Tval> P(V4D::Keys::Enum key)        // runtime property
template<typename Tval> Property<Tval> P(LocalState::Keys::Enum key) // per-thread state
template<typename Tval> Property<Tval> P(GlobalState::Keys::Enum key)// global state
```
A property edge is a *shared, read-only* edge (`Edge<const T, false, true, true>`) bound
to a named runtime value. `V4D::Keys` contains `SIZE`, `VIEWPORT`, `CLEAR_COLOR`,
`NAMESPACE`, `DISABLE_INPUT_EVENTS`, etc.

- LLVM equivalent: reading a **global variable** (`@global`) or a fixed runtime feature
  register.

### 2.8 `E<T>(...)` — event stream

```cpp
template<typename Tclass> Event<Tclass> E()                     // all events
template<typename Tclass> Event<Tclass> E(Tclass::Type t)       // events of a type
template<typename Tclass> Event<Tclass> E(Tclass::Type t, Ttrigger tr) // type + trigger
```
An edge that produces a `std::vector<std::shared_ptr<Tclass>>` of input events fetched by
the runtime (`gwe::fetch`). `Mouse`, `Keyboard`, `Window`, `Joystick` are predefined.

- LLVM equivalent: an **external/volatile input channel**; reading it is a call with
  side effects (the events are polled each iteration).

### 2.9 `F(fn, args...)` — call / external function

```cpp
template<typename Tfn, typename ... Args> auto F(Tfn fn, Args&& ... args)
```
Invokes a C++ function/member-function/lambda with the given operand edges. If `fn`
returns a non-`void` value, `F` returns a new **result edge** (an SSA temporary);
otherwise it returns `cv::Ptr<Plan>` (a statement). This is the escape hatch for any
operation without a dedicated operator opcode.

- LLVM equivalent: **`call`** instruction (with/without a return value). For a
  returning call, the result is a new SSA register.
- Pointer-to-member casts are produced with the `_OL_`/`_OLM_`/`_OLC_`/`_OLMC_` macros
  (`static_cast<r (*)(args...)>(fn)` / `static_cast<r (C::*)(args...)>(fn)`).

```cpp
constexpr static auto SPLIT_ = _OL_(void, cv::split, cv::InputArray, cv::OutputArrayOfArrays);
// ...
plain(F(SPLIT_, R(src), RW(dst)));   // call, no result edge
auto r = F(ROUND_, R(x));            // call, produces result edge r
```

### 2.10 `_(...)` — operand group / tuple

```cpp
template<typename ... Args> auto _(Args&& ... args)
```
Builds a `std::tuple` of operands. Used to (a) feed **n-ary** operators and (b) pass
multiple edges where a variadic call is expected. With operators it groups the tail
operands:

```cpp
auto t = R(a) + _(R(b), R(c));   // one ADD node with 3 operands
```

### 2.11 `_shared(var)` / `_safe(var)` — register storage

```cpp
template<typename Tvar> void _shared(Tvar& val)  // give var a mutex, make it shareable
template<typename Tvar> void _safe(Tvar& val)    // mark var as never-shared (opt-out)
```
Call once from the plan constructor. `_shared` registers the variable (and any member
that lives inside its address range) in the global shared-variable table, attaching a
`std::mutex` used by `RS`/`RWS`/`CS` and `Property`.

- LLVM equivalent: declaring a global with appropriate linkage/atomicity.

---

## 3. Operator Instructions (ALU / memory ops)

Operators are dispatched via the `Operators` enum
(`cv::plan::detail::Operators`, in `transaction.hpp`) and are applied in four ways:

1. **Symbol form** — C++ operator overloads: `a + b`, `a % b`, `!b`, `b[i]`, `x = y`, ...
2. **Named form** — member functions on `Plan`: `ADD(a, b)`, `MOD(a, b)`, `NEG(dst, x)`, ...
3. **Generic form** — `OP<Operators::ADD_>(a, b)`.
4. **Statement form** — `op<Operators::...>(...)`, `assign(...)`, `construct(...)`
   (lowercase, return `cv::Ptr<Plan>`, no result edge).

Result rules:

- **Expression forms** (symbol / named / `OP<>`) return a **new result edge**. The
  value is recomputed by the node each iteration. Multiple consumers share the node.
- **Statement forms** (`op<>`, `assign`, `construct`) do not create a result edge.

> **Associativity caveat.** The n-ary implementations fold the *tail* right-associatively:
> `SUB(a,b,c)` computes `a - (b - c)`, `DIV(a,b,c)` computes `a / (b / c)`, and the
> comparisons compute e.g. `a == (b == c)`. For the common **binary** case (`SUB(a,b)`,
> `EQ(a,b)`, ...) the result is exact. When lowering to LLVM IR, emit one operator per
> binary instruction.

### 3.1 Arithmetic

| Opcode | Symbol | Named | Arity | Semantics (C++) | LLVM IR |
|--------|--------|-------|-------|------------------|---------|
| `ADD_` | `+` | `ADD` | n-ary | `a + (b + ...)` | `add` |
| `SUB_` | `-` | `SUB` | n-ary | `a - (b - ...)` (binary: `a-b`) | `sub` |
| `MUL_` | `*` | `MUL` | n-ary | `a * (b * ...)` | `mul` |
| `DIV_` | `/` | `DIV` | n-ary | `a / (b / ...)` (binary: `a/b`) | `sdiv`/`udiv`/`fdiv` (via C++ `/`) |
| `MOD_` | `%` | `MOD` | binary | `a % b` (integer remainder) | `srem`/`urem`/`frem` (via C++ `%`) |
| `NEG_` | — | `NEG` | binary | `dst = -value` (dst first!) | `sub 0, %x` / `fneg` |
| `INCL_` | `++x` | `INCL` | unary | `++x` (pre-increment) | `add x, 1` + store |
| `INCR_` | `x++` | `INCR` | unary | `x++` (post-increment) | `add x, 1` + store, old value |
| `DECL_` | `--x` | `DECL` | unary | `--x` | `sub x, 1` + store |
| `DECR_` | `x--` | `DECR` | unary | `x--` | `sub x, 1` + store, old value |

**NEG is binary**: `NEG(RW(dst), R(src))` computes `dst = -src`. It has no symbol form.
The unary `-x` symbol instead expands to `x * (-1)` (`MUL`).

### 3.2 Logical & bitwise

| Opcode | Symbol | Named | Arity | Semantics | LLVM IR |
|--------|--------|-------|-------|-----------|---------|
| `AND_` | `&&` | `AND` | n-ary | `a && (b && ...)` | `and i1` (bool logic) |
| `OR_` | `\|\|` | `OR` | n-ary | `a \|\| (b \|\| ...)` | `or i1` (bool logic) |
| `NOT_` | `!` | `NOT` | unary | `!a` | `xor i1 a, true` |
| `XOR_` | `^` | `XOR` | n-ary | `a ^ (b ^ ...)` | `xor` |
| `BAND_` | `&` | `BAND` | n-ary | `a & (b & ...)` | `and` |
| `BOR_` | `\|` | `BOR` | n-ary | `a \| (b \| ...)` | `or` |
| `SHL_` | `<<` | `SHL` | n-ary | `a << (b << ...)` (binary: `a<<b`) | `shl` |
| `SHR_` | `>>` | `SHR` | n-ary | `a >> (b >> ...)` (binary: `a>>b`) | `lshr`/`ashr` (C++ `>>` on signed = arithmetic) |

> Note: `SHL`/`SHR`'s n-ary form also folds the tail right-associatively
> (`a << (b << c)`); emit one op per binary shift when lowering.

### 3.3 Comparison

| Opcode | Symbol | Named | Arity | Semantics | LLVM IR |
|--------|--------|-------|-------|-----------|---------|
| `EQ_` | `==` | `EQ` | n-ary | `a == (b == ...)` (binary exact) | `icmp eq` / `fcmp oeq` |
| `NEQ_` | `!=` | `NEQ` | n-ary | `a != (b != ...)` (binary exact) | `icmp ne` / `fcmp une` |
| `LT_` | `<` | `LT` | n-ary | `a < (b < ...)` (binary exact) | `icmp slt/ult` / `fcmp olt` |
| `GT_` | `>` | `GT` | n-ary | `a > (b > ...)` (binary exact) | `icmp sgt/ugt` / `fcmp ogt` |
| `LE_` | `<=` | `LE` | n-ary | `a <= (b <= ...)` (binary exact) | `icmp sle/ule` / `fcmp ole` |
| `GE_` | `>=` | `GE` | n-ary | `a >= (b >= ...)` (binary exact) | `icmp sge/uge` / `fcmp oge` |

All comparisons are implemented with the native C++ operators, so signed/unsigned and
integer/float behavior follow C++ semantics. Emit one op per LLVM `icmp`/`fcmp`.

### 3.4 Select / memory / construction

| Opcode | Symbol | Named | Arity | Semantics | LLVM IR |
|--------|--------|-------|-------|-----------|---------|
| `IF_` | — | `IF` | ternary | `cond ? ifTrue : ifFalse` | `select` |
| `IDX_` | `[]` | `IDX` | binary | `container[index]` | `getelementptr` + `load` |
| `DEREF_` | — | `DEREF` | binary | `dst = *ptr` (dst first!) | `load` into `dst` |
| `ASSIGN_` | `=` | `ASSIGN` | binary | `dst = src` | `store` |
| `CONSTRUCT_` | `()` | (via `operator()`) | variadic | `dst = T(args...)` / `new T(args...)` | `call` to ctor + `alloca` |

Notes:

- `IF(cond, ifTrue, ifFalse)` maps directly to LLVM `select` (all operands are values).
- `Edge::operator[]` is the symbol form of `IDX_`; the named `IDX(a, i)` form exists too.
- `DEREF` and `NEG` take the **destination first**: `DEREF(RW(dst), R(ptr))`,
  `NEG(RW(dst), R(src))`.
- `ASSIGN` also exists as the statement form `assign(dst, src)` and the symbol `=` on an
  edge: `RW(x) = R(y)`.
- `CONSTRUCT_` is reached by calling a plan like a function: `plan(V(a), V(b))` (the
  `operator()` on `Plan`), or `construct(dst, a, b)`. It creates a value by value
  construction (or `new`/`makePtr` for smart/raw pointers).

### 3.5 Lowercase statement helpers

```cpp
template<Operators Top, typename ... Edges> cv::Ptr<Plan> op(Edges...);    // op<> as statement
template<typename ... Edges>                 cv::Ptr<Plan> assign(Edges...);   // ASSIGN_
template<typename ... Edges>                 cv::Ptr<Plan> construct(Edges...);// CONSTRUCT_
```
These create a node that performs the operation but returns **no result edge** — use
them when the operation's only effect is its write-back (e.g. `assign(RW(x), R(y))`).

---

## 4. Control-Flow Instructions

Control flow is expressed with **branch regions**, not jumps. A branch region is a
predicated sub-graph; the predicate is evaluated at runtime each iteration and the
nodes inside are executed only when it is true.

### 4.1 `branch(...)` — enter a predicated region

```cpp
branch(predEdge)                              // predicate is a bool edge (e.g. R(cond))
branch(fn, args...)                           // predicate is a bool-returning callable
branch(workerIdx, fn, args...)                // + restrict to one worker (pinned)
branch(BranchType::Enum type, predEdge)       // + explicit branch type
branch(BranchType::Enum type, fn, args...)
branch(BranchType::Enum type, workerIdx, fn, args...)
```
Pushes a region onto the branch stack. Returns `cv::Ptr<Plan>` so calls can be chained:
`branch(...)->plain(...)->endBranch()`.

### 4.2 `elseBranch()` — negate the current condition

Toggles the enclosing branch's condition to its complement (like an `else` block).

### 4.3 `endBranch()` — close a region

Pops the current region off the branch stack. Every `branch` must be matched by exactly
one `endBranch`. Both are matched by *source nesting*, giving a structural control-flow
form:

```cpp
branch(R(x) == V(0));
{ ... }
elseBranch();
{ ... }
endBranch();
```

- LLVM equivalent: `br` (conditional) with structured `if/else`. Because the DSL has no
  `goto`-style edges between arbitrary blocks, irreducible control flow must be
  restructured into nested regions; loops are expressed as a `branch` on a predicate
  that is updated by the loop body (the graph is re-evaluated each iteration, so a
  "while" is naturally a predicated region whose predicate reads a value written by
  the region body — see tutorial `11-video`).

### 4.4 Branch types (`BranchType::Enum`)

| Value | Name | Semantics |
|-------|------|-----------|
| `0` | `NONE` | no branch (plain node) |
| `1` | `SINGLE` | executed by at most **one worker** (globally locked, serialized) |
| `2` | `PARALLEL` | executed by every worker when predicate holds (default) |
| `4` | `ONCE` | executed **once globally** (predicate + global once-semantics) |
| `8` | `PARALLEL_ONCE` | executed once **per worker** |

### 4.5 Predefined predicates

```cpp
always_                    // []{ return true; }
isTrue_   (bool)           // [](const bool& b){ return b; }
isFalse_  (bool)           // [](const bool& b){ return !b; }
and_      (bool, bool)     // a && b
or_       (bool, bool)     // a || b
```

---

## 5. Program Structure & Contexts

### 5.1 Plans and sub-plans

A `Plan` is a program (module). A plan can contain **sub-plans** (like a call to another
function/module). Sub-plans are created with `_sub` and executed with `subInfer` /
`subSetup` / `subTeardown`, which splice the sub-plan's graph into the parent:

```cpp
template<typename TsubPlan, typename Tparent, typename ... Args>
auto _sub(Tparent* parent, Args&& ... args)      // create sub-plan child
template<typename TsubPlan, typename TparentPtr, typename ... Args>
auto _sub(TparentPtr parent, Args&& ... args)    // create sub-plan child (Ptr variant)

subInfer(cv::Ptr<TsubPlan>)      // inline child's infer() graph into the parent
subSetup(cv::Ptr<TsubPlan>)      // inline child's setup() graph
subTeardown(cv::Ptr<TsubPlan>)   // inline child's teardown() graph
```

- LLVM equivalent: sub-plans ≈ **functions** (`define`), `_sub` ≈ function declaration,
  `subInfer`/`subSetup`/`subTeardown` ≈ `call` sites. State is passed by closing over
  the child plan's member variables, which the shared-variable table associates with the
  parent's address range.

### 5.2 Context calls (side-effecting instructions)

Context calls attach a node to a **context** (a specialized execution environment). They
are the "peripheral instructions" of the ISA. The function is invoked inside that
context on the appropriate thread/device.

| Call | Context | Purpose |
|------|---------|---------|
| `plain(fn, args...)` | CPU | general-purpose code (default for operators) |
| `F(fn, args...)` | CPU | external function call (see §2.9) |
| `gl(fn, args...)` | OpenGL | raw GL commands; `gl(idx, ...)` selects a context |
| `gl<Tedge>(idxEdge, fn, args...)` | OpenGL | context selected by an edge's value |
| `fb(fn, args...)` | framebuffer | operate on the display framebuffer (`cv::UMat`) |
| `nvg(fn, args...)` | NanoVG | 2D vector graphics / text |
| `bgfx(fn, args...)` | bgfx | bgfx rendering API |
| `imgui(fn, args...)` | Dear ImGui | immediate-mode GUI |
| `capture(fn, args...)` | source | read from the video source |
| `write(fn, args...)` | sink | write to the video sink |
| `ext(fn, args...)` | external | custom context, `ext(idx, ...)` for specific index |
| `clear()` | OpenGL | clear the framebuffer to `V4D::Keys::CLEAR_COLOR` |
| `set(key, valueEdge)` | runtime | write a runtime property (`V4D::set`) |

All of these return `cv::Ptr<Plan>`, so they can be chained: `branch(...)->plain(...)->endBranch()`.

### 5.3 Entry points

```cpp
static cv::Ptr<Tplan> Plan::make<Tplan>(args...)         // instantiate + register size
static void          Plan::run<Tplan>(int32_t workers, args...)
```
`Plan::run` with `workers` from the command line: `-1`/`0`/`>0` (auto/off/main-only+workers).

---

## 6. State Model

| Concept | DSL entity | LLVM IR analogue |
|---------|------------|------------------|
| Plan member variable | plain C++ member; passed as `R`/`RW` | `alloca` slot in the function |
| Shared (locked) variable | `_shared(x)` then `RS`/`RWS`/`CS` | global with atomic/ordered access |
| Safe (never-shared) variable | `_safe(x)` | private, thread-local |
| Runtime property | `V4D::Keys` + `P<T>(key)` + `set(key, edge)` | global variable with runtime callbacks |
| Per-worker state | `LocalState::Keys::WORKER_INDEX`, `LocalState::get/set` | thread-id-dependent value |
| Global counters | `GlobalState::Keys::{FRAME_CNT, RUN_CNT, FPS, ...}` | global variables |

`set(key, value)` and `V4D::set(key, value)` support plain values, single edges, and
callable setters (`set(key, fn, args...)` computing the value from edges).

---

## 7. LLVM-IR → Plan-DSL Translation Table

| LLVM IR | Plan-DSL |
|---------|----------|
| `%r = add i32 %a, %b` | `auto r = ADD(R(a), R(b));` |
| `sub` / `mul` / `sdiv` / `srem` | `SUB` / `MUL` / `DIV` / `MOD` |
| `udiv` / `urem` / `fdiv` / `frem` | `DIV` / `MOD` (C++ semantics) |
| `and` / `or` / `xor` | `BAND` / `BOR` / `XOR` |
| `shl` | `SHL` |
| `lshr` / `ashr` | `SHR` (signed operand ⇒ arithmetic via C++ `>>`) |
| `icmp eq/ne/slt/...` | `EQ` / `NEQ` / `LT` / `GT` / `LE` / `GE` |
| `fcmp oeq/...` | same comparisons (C++ operators) |
| `select i1 %c, %t, %f` | `IF(R(c), R(t), R(f))` |
| `alloca` | plan member variable (or `_shared`) |
| `load` (read) | `R(v)` as an operand |
| `store` | `RW(v)` as destination + `assign` / `=` |
| `load`+copy (snapshot) | `CS(v)` |
| `getelementptr` + `load` | `IDX(RW(container), R(idx))` |
| `load` from pointer | `DEREF(RW(dst), R(ptr))` |
| `call` (void) | `F(fn, args...)` / `plain(fn, args...)` / context call |
| `call` (non-void) | result edge of `F(fn, args...)` |
| `ret` | end of `infer()`/`setup()`/`teardown()`; values via shared vars |
| `br` (cond) | `branch(pred) ... elseBranch() ... endBranch()` |
| `switch` | nested `branch` regions on `EQ` predicates |
| `phi` | a plain member variable written in each region with `assign` |
| constant (all types) | `V(value)` |
| global variable | `_shared` member + `RS`/`RWS`, or `P<T>(key)` |
| `unreachable` | `CV_Assert(false)` inside a `plain` node |
| `fneg` / `sub 0,x` | `NEG(RW(dst), R(x))` |

### 7.1 Practical lowering notes

1. **SSA temporaries.** Give each LLVM virtual register a named Plan-local `auto`
   variable holding the result edge of its producing instruction. A later instruction
   consumes it with `R(tmp)`. Because a result edge is a value (not storage), use
   `R(r)` — the DSL reads the computed value.
2. **Memory, not SSA, for `store`/`load`.** LLVM `alloca`+`store`+`load` chains lower to
   plan member variables and `R`/`RW`/`assign`. Use the most restrictive intent you can
   prove (`R` when a value is only read) to maximize parallelism.
3. **One op per instruction.** Emit binary operators exactly as the IR expresses them;
   the n-ary/right-fold behavior (§3) only matters when you deliberately fuse
   instructions (e.g. `ADD` of a long constant chain).
4. **Destination-first ops.** `NEG` and `DEREF` take the destination as their first
   argument — allocate a storage slot (`RW`) for the result.
5. **Control flow must be structured.** Loops and conditionals must nest as balanced
   `branch`/`endBranch` regions. LLVM's CFG must be region-ified (dominator-tree based
   structuring) before lowering.
6. **`select` operands are eager.** LLVM `select` is lazy (only chosen side is
   evaluated), but `IF` is an operator node whose operands are all computed. For truly
   lazy branch-on-value semantics use `branch` regions instead.
7. **Integer widths.** Plan-DSL operates on C++ types; map `iN` to the smallest
   native type that fits (`i8`→`int8_t`, `i32`→`int32_t`, `i64`→`int64_t`, `i1`→`bool`).
8. **Pointers.** LLVM pointers map to raw pointers, `cv::Ptr<T>`, or `std::vector`/array
   indexing (`IDX`). `DEREF` handles the load; pointer values themselves are stored in
   plan members.
9. **Floating point.** `DIV`/`MOD` use C++ `/`/`%`; for `frem` use `F(std::fmod, ...)`.
   `fneg` → `NEG`.

---

## 8. Opcode Index

| Mnemonic | Opcode | Arity | Symbol | Named | Statement form |
|----------|--------|-------|--------|-------|----------------|
| CONSTRUCT | `CONSTRUCT_` | variadic | `plan(...)` | `operator()` | `construct(...)` |
| ASSIGN | `ASSIGN_` | 2 | `=` | `ASSIGN` | `assign(...)` |
| ADD | `ADD_` | n | `+` | `ADD` | `op<ADD_>` |
| SUB | `SUB_` | n | `-` | `SUB` | `op<SUB_>` |
| MUL | `MUL_` | n | `*` | `MUL` | `op<MUL_>` |
| DIV | `DIV_` | n | `/` | `DIV` | `op<DIV_>` |
| MOD | `MOD_` | 2 | `%` | `MOD` | `op<MOD_>` |
| INCL | `INCL_` | 1 | `++x` | `INCL` | `op<INCL_>` |
| INCR | `INCR_` | 1 | `x++` | `INCR` | `op<INCR_>` |
| DECL | `DECL_` | 1 | `--x` | `DECL` | `op<DECL_>` |
| DECR | `DECR_` | 1 | `x--` | `DECR` | `op<DECR_>` |
| AND | `AND_` | n | `&&` | `AND` | `op<AND_>` |
| OR | `OR_` | n | `\|\|` | `OR` | `op<OR_>` |
| EQ | `EQ_` | n | `==` | `EQ` | `op<EQ_>` |
| NEQ | `NEQ_` | n | `!=` | `NEQ` | `op<NEQ_>` |
| LT | `LT_` | n | `<` | `LT` | `op<LT_>` |
| GT | `GT_` | n | `>` | `GT` | `op<GT_>` |
| LE | `LE_` | n | `<=` | `LE` | `op<LE_>` |
| GE | `GE_` | n | `>=` | `GE` | `op<GE_>` |
| NOT | `NOT_` | 1 | `!` | `NOT` | `op<NOT_>` |
| XOR | `XOR_` | n | `^` | `XOR` | `op<XOR_>` |
| BAND | `BAND_` | n | `&` | `BAND` | `op<BAND_>` |
| BOR | `BOR_` | n | `\|` | `BOR` | `op<BOR_>` |
| SHL | `SHL_` | n | `<<` | `SHL` | `op<SHL_>` |
| SHR | `SHR_` | n | `>>` | `SHR` | `op<SHR_>` |
| IF | `IF_` | 3 | — | `IF` | `op<IF_>` |
| IDX | `IDX_` | 2 | `[]` | `IDX` | `op<IDX_>` |
| DEREF | `DEREF_` | 2 | — | `DEREF` | `op<DEREF_>` |
| NEG | `NEG_` | 2 | — | `NEG` | `op<NEG_>` |

Source locations: opcode enum `detail::Operators` in
`modules/plan/include/opencv2/plan/detail/transaction.hpp`; operator implementations in
`make_operator_func` (same file); symbol/named forms in
`modules/v4d/include/opencv2/v4d/v4d.hpp` and `modules/plan/include/opencv2/plan/detail/transaction.hpp`.