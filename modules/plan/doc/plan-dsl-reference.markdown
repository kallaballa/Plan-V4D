# Plan-DSL API Reference (ISA-style)

This document describes **Plan-DSL**, the compile-time task-graph language that makes
up the *Plan* part of Plan-V4D, in the style of an *Instruction Set Architecture* (ISA)
manual. It is intended as the authoritative contract for tools that lower other
intermediate representations — most notably LLVM IR — onto Plan-DSL.

The DSL is not a text language. It is a set of C++ *functions, operators and macros*
invoked from inside the `infer()` / `setup()` / `teardown()` methods of a class that
inherits from `cv::plan::Plan`. Every call records a **task node** and its **data
dependencies** at (compile-time-typed) runtime; the recorded accesses are then compiled
into a flat task-node list that is re-executed every frame/iteration by a set of worker
threads (one independent graph copy per worker). The graph is **built once** per worker
(via `makeGraph()`) and then the same graph is **re-executed every frame** (via
`runGraph()`), with branch predicates re-evaluated each iteration.

All API entities live in namespace `cv::plan` (aliased to `using namespace cv::plan;` in
samples). Header: `<opencv2/plan/plan.hpp>`.

This document covers only the Plan-DSL core. Concrete execution contexts and input
event sources are supplied by a *runtime* that implements the `PlanRuntime` interface
(such as V4D); runtime-specific extensions are explicitly marked as such below.

NOTE: Edge-expression chaining is merely syntactic sugar; plain semicolon-separated
statements work as well.

---

## 1. Execution Model

### 1.1 The graph is the program

A `Plan` subclass describes, in its three lifecycle methods, how data flows:

| Method    | Purpose                                                          | Runs |
|-----------|------------------------------------------------------------------|------|
| `setup()` | Build the initialization pipeline (alloca-like, one-shot)        | once per worker thread (see §1.5) |
| `infer()` | Define the per-iteration pipeline (the "kernel"/main body)       | once per worker thread (see §1.5) |
| `teardown()` | Build the shutdown pipeline (free, one-shot)                  | once per worker thread (see §1.5) |
| `gui()`   | optional UI hook (e.g. widget/callback setup)                    | once, on the main thread |

The graph is **built once** per worker (by calling `infer()` + `makeGraph()`) and then
**executed every frame** by calling `runGraph()`. The same `currentNodes_` list is
iterated each frame; branch predicates are re-evaluated every iteration, so control
flow can change dynamically (e.g. a `while` loop exits when its predicate becomes
false). The graph structure itself is **not** rebuilt between frames.

### 1.5 `Plan::run` lifecycle

The core provides `Plan::run<Tplan>(workers, args...)`, which orchestrates the complete
lifecycle: worker spawning, the setup/infer/teardown graph phases, the startup barrier
and the frame loop. Runtime specifics are delegated to four `PlanRuntime` hooks:
`initWorkerThread()`, `willGui()`, `runFrameLoop()` and `releaseIo()`. (V4D's
`V4DPlan::run` simply forwards to `Plan::run`; entering the display loop happens inside
its `runFrameLoop()` implementation.)

Worker-count semantics: `workers == -1` selects a default of **2** worker threads; any
value `N >= 0` spawns **N + 1** worker threads (so `0` means "one worker plus main").
Each worker `i` runs the `initWorkerThread(i)` hook, records its `WORKER_INDEX`, and
then recurses into `Plan::run<Tplan>(0, ...)` on its own thread. The main thread never
builds a graph; it runs `gui()` once and then participates in the runtime's frame loop
(V4D's main thread only services the display/event loop).

The complete lifecycle is:

```
Plan::run<Tplan>(workers, args...)            // core entry point (plan.hpp)
  │
  ├── init GlobalState/LocalState keys
  ├── plan = Plan::make<Tplan>(args...); attach runtime (PlanRuntime::current())
  │
  ├── main thread spawns worker threads (see count semantics above)
  │
  ├── main thread                      ║  each worker thread
  │     willGui(plan); gui();          ║    setup()   -> makeGraph() -> runGraph()
  │                                    ║               -> clearGraph()
  │                                    ║    (worker setup phases are serialized
  │                                    ║     against each other by a semaphore)
  │                                    ║    infer()   -> makeGraph()
  │                                    ║    WORKERS_READY += 1
  │
  ├── barrier (workers + 1 participants arrive_and_wait)
  │
  ├── runtime->runFrameLoop(frameFn)   // frameFn == plan->runGraph()
  │      main thread: display/event loop (V4D)
  │      worker threads: frameFn() once per frame,
  │      until the runtime ends the loop
  │
  ├── worker threads (teardown):
  │      clearGraph()
  │      teardown() -> makeGraph() -> runGraph() -> clearGraph()
  │      releaseIo()
  │
  └── main thread: join worker threads; releaseIo()
```

Each worker builds and executes its own independent copy of the graph. Workers never
share partial iterations or migrate work between threads.

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

Each DSL call records one access tuple `(node-id, read?, operand-id)` in `accesses_`.
The node id is a string produced by `make_id()` from the plan's space/name, the callable
identity (function address or lambda-holder address) and the operand ids/types, made
unique within one build by appending `+` characters on collision. The operand id is the
edge's storage identity. `makeGraph()` walks `accesses_` in recording order and appends
a new node to `currentNodes_` whenever the id differs from the **previous** entry;
consecutive accesses sharing the same id merge into that node (recording additional
dependency entries). An id recurring later in the list therefore starts a second node.

Each node records:
- `read_deps_` — set of operand ids this node reads
- `write_deps_` — set of operand ids this node writes

> **Execution model.** `runGraph()` iterates `currentNodes_` **sequentially** in the
> order nodes were registered during `infer()`. It does **not** use the dependency
> edges for scheduling; a node runs unconditionally if it is enabled by its branch
> state (§4). The `read_deps_` / `write_deps_` fields are populated by `makeGraph()`
> for bookkeeping but are not consulted at runtime by the base `Plan::runGraph()`
> implementation.

Multiple workers execute independently built copies of the graph; the scheduler never
migrates partial iterations between workers.

### 1.4 Concurrency primitives

- **Shared variables** (registered with `_shared`) get an associated `std::mutex`
  (members living inside a registered variable's address range reuse that variable's
  mutex). A *lockie* edge (`RS`/`RWS`) acquires that mutex for the duration of the
  node's transaction; a **copy** edge (`CS`) snapshots the value under the same mutex.
- **Single-branch** regions are protected by a global per-branch lock so that at most
  one worker executes them at a time.
- **ONCE** regions run at most once: globally for `ONCE`, per worker for
  `PARALLEL_ONCE`.

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

`RS`/`RWS` throw `std::runtime_error` if the variable is a plan member that was not
registered with `_shared()`. Storage outside the plan object (globals, heap objects) is
implicitly registered as shared on first shared access.

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
Throws for non-shared variables (see §2.4).

- LLVM equivalent: a **load followed by a copy** — for data that is produced on one
  thread (e.g. GUI) and consumed on another without sharing a live definition.

### 2.7 `P<T>(key)` — runtime property

```cpp
template<typename Tval> Property<Tval> P(LocalState::Keys::Enum key) // per-thread state
template<typename Tval> Property<Tval> P(GlobalState::Keys::Enum key)// global state
```
A property edge is a *shared, read-only* edge (`Edge<const T, false, true, true>`) bound
to a named value in the global/per-thread state tables (`GlobalState`/`LocalState` in
`util.hpp`). Core keys are `FRAME_CNT`, `RUN_CNT`, `FPS`, ... (global) and
`WORKER_INDEX` (per-thread). A runtime may register additional key sets and provide a
matching `P` overload (e.g. V4D adds `V4D::Keys` with `SIZE`, `VIEWPORT`, ...).

- LLVM equivalent: reading a **global variable** (`@global`) or a fixed runtime feature
  register.

### 2.8 `E<T>(...)` — event stream

```cpp
template<typename Tclass> Event<Tclass> E()                     // all events
template<typename Tclass> Event<Tclass> E(Tclass::Type t)       // events of a type
template<typename Tclass> Event<Tclass> E(Tclass::Type t, Ttrigger tr) // type + trigger
```
An edge that produces a `std::vector<std::shared_ptr<Tclass>>` of input events for the
iteration. The event class must provide a nested `Type` enum and a `List` container;
the DSL core itself always produces an empty list — polling actual input devices is the
job of the runtime, which overrides the fetch callable (V4D's `V4DPlan::Event` fetches
from the `gwe` event queues each iteration unless `V4D::Keys::DISABLE_INPUT_EVENTS` is
set; event classes are `Mouse`, `Keyboard`, `Window`, `Joystick`).

- LLVM equivalent: an **external/volatile input channel**; reading it is a call with
  side effects (the events are polled each iteration).

### 2.9 `F(fn, args...)` — call / external function

```cpp
template<typename Tfn, typename ... Args> auto F(Tfn src, Args&& ... args)
```
Invokes a C++ function/member-function/lambda with the given operand edges. If `fn`
returns a non-`void` value, `F` returns a new **result edge** (an SSA temporary);
otherwise it returns `cv::Ptr<Plan>` (a statement). This is the escape hatch for any
operation without a dedicated operator opcode.

- LLVM equivalent: **`call`** instruction (with/without a return value). For a
  returning call, the result is a new SSA register.
- Callables are passed directly — function pointers, member function pointers and
  lambdas/function objects are all accepted and normalized internally
  (`wrap_callable`).

```cpp
F(&cv::split, R(src), RW(dst));          // free function, no result edge
auto t = F(&cv::getTickCount);           // call, produces result edge t
auto w = F(&cv::Size::width, R(sz));     // member function call
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

### 3.1 Arithmetic

| Opcode | Symbol | Named | Arity | Semantics (C++) | LLVM IR |
|--------|--------|-------|-------|-----------------|---------|
| `ADD_` | `+` | `ADD` | n-ary | `(a + b) + ...` (binary: `a+b`) | `add` |
| `SUB_` | `-` | `SUB` | n-ary | `(a - b) - ...` (binary: `a-b`) | `sub` |
| `MUL_` | `*` | `MUL` | n-ary | `(a * b) * ...` (binary: `a*b`) | `mul` |
| `DIV_` | `/` | `DIV` | n-ary | `(a / b) / ...` (binary: `a/b`) | `sdiv`/`udiv`/`fdiv` (via C++ `/`) |
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
| `AND_` | `&&` | `AND` | n-ary | `(a && b) && ...` | `and i1` (bool logic) |
| `OR_` | `\|\|` | `OR` | n-ary | `(a \|\| b) \|\| ...` | `or i1` (bool logic) |
| `NOT_` | `!` | `NOT` | unary | `!a` | `xor i1 a, true` |
| `XOR_` | `^` | `XOR` | n-ary | `(a ^ b) ^ ...` | `xor` |
| `BAND_` | `&` | `BAND` | n-ary | `(a & b) & ...` | `and` |
| `BOR_` | `\|` | `BOR` | n-ary | `(a \| b) \| ...` | `or` |
| `SHL_` | `<<` | `SHL` | n-ary | `(a << b) << ...` (binary: `a<<b`) | `shl` |
| `SHR_` | `>>` | `SHR` | n-ary | `(a >> b) >> ...` (binary: `a>>b`) | `lshr`/`ashr` (C++ `>>` on signed = arithmetic) |

### 3.3 Comparison

| Opcode | Symbol | Named | Arity | Semantics | LLVM IR |
|--------|--------|-------|-------|-----------|---------|
| `EQ_` | `==` | `EQ` | n-ary | `(a == b) == ...` (binary exact) | `icmp eq` / `fcmp oeq` |
| `NEQ_` | `!=` | `NEQ` | n-ary | `(a != b) != ...` (binary exact) | `icmp ne` / `fcmp une` |
| `LT_` | `<` | `LT` | n-ary | `(a < b) < ...` (binary exact) | `icmp slt/ult` / `fcmp olt` |
| `GT_` | `>` | `GT` | n-ary | `(a > b) > ...` (binary exact) | `icmp sgt/ugt` / `fcmp ogt` |
| `LE_` | `<=` | `LE` | n-ary | `(a <= b) <= ...` (binary exact) | `icmp sle/ule` / `fcmp ole` |
| `GE_` | `>=` | `GE` | n-ary | `(a >= b) >= ...` (binary exact) | `icmp sge/uge` / `fcmp oge` |

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
branch(predEdge)                                       // bool edge predicate (default: PARALLEL)
branch(fn)                                             // bool-returning callable
branch(fn, args...)                                    // callable + operand edges
branch(workerIdx, fn, args...)                         // + restrict to one worker (pinned)
branch(workerIdx, BranchType::Enum type, fn, args...)  // pinned + explicit branch type
branch(BranchType::Enum type, predEdge)                // + explicit branch type
branch(BranchType::Enum type, fn, args...)
branch(BranchType::Enum type, workerIdx, fn, args...)
```
Pushes a region onto the branch stack. Without an explicit type, `BranchType::PARALLEL`
is used. Returns `cv::Ptr<Plan>` so calls can be chained:
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
  the region body — see the `CountdownPlan` example in §12 of
  `plan-dsl-programming-guide.markdown`).

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
`subSetup` / `subTeardown`, which record the sub-plan's graph and splice its accesses
and transactions into the parent:

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

> **Constructor-only `_sub`.** `_sub<>` must be called **only in the plan constructor**,
> as the first statement(s) after the initializer list. It must **not** be called from
> `infer()`, `setup()`, or `teardown()`. This guarantees the sub-plan instance is created
> exactly once (when the parent is constructed) and persists across frames — calling it
> from `infer()` would recreate it every frame and break nested sub-plan return-value
> propagation. `subInfer()` (the per-frame call) remains in `infer()`.

### 5.2 Context calls (side-effecting instructions)

Context calls attach a node to a **context** (a specialized execution environment). They
are the "peripheral instructions" of the ISA. The function is invoked inside that
context on the appropriate thread/device.

The DSL core defines exactly one context — the **plain/CPU context** — and one generic
attachment mechanism:

| Call | Context | Purpose |
|------|---------|---------|
| `plain(fn, args...)` | CPU | general-purpose code (heavy-weight) |
| `F(fn, args...)` | CPU | function call (see §2.9) |

Both are thin wrappers over the protected helper `add_transaction(ctx, id, fn, args...)`.

`plain`, `branch`, `elseBranch` and `endBranch` return `cv::Ptr<Plan>`, so they can be
chained: `branch(...)->plain(...)->endBranch()`. `F` returns a result edge when the
callee returns a value.

Runtimes expose additional contexts via the `PlanRuntime` accessors (`glCtx`, `fbCtx`,
`nvgCtx`, `bgfxCtx`, `extCtx`, `sourceCtx`, `sinkCtx`, `imguiCtx`). **V4D-specific**
attachment calls (all return `cv::Ptr<V4DPlan>`):

| Call | Context | Purpose |
|------|---------|---------|
| `gl(fn, args...)` / `gl(idxEdge, fn, args...)` | GL | raw OpenGL commands (optionally on GL context `idx`) |
| `fb<pos>(fn, args...)` | FrameBuffer | framebuffer access (fb edge auto-inserted at position `pos`) |
| `nvg(fn, args...)` | NanoVG | vector graphics |
| `bgfx(fn, args...)` | Bgfx | bgfx rendering |
| `ext(fn, args...)` / `ext(idxEdge, fn, args...)` | Ext | external renderer contexts |
| `capture(fn, args...)` / `capture(edge)` / `capture()` | Source | pull the next input frame |
| `write(fn, args...)` / `write(edge)` / `write()` | Sink | push the finished frame |
| `set(key, edge)` | CPU | property write node |
| `imgui(fn, args...)` | ImGui | installs the transaction into the ImGui context instead of the graph |

### 5.3 Entry points

```cpp
static cv::Ptr<Tplan> Plan::make<Tplan>(args...)         // instantiate
static void          Plan::run<Tplan>(workers, args...)  // full lifecycle (see §1.5)
```
Instantiation and the complete run lifecycle live in the core; runtime specifics enter
through the `PlanRuntime` hooks (`initWorkerThread`, `willGui`, `runFrameLoop`,
`releaseIo`). `V4DPlan::run` forwards to `Plan::run`; `V4DPlan::make` additionally
publishes the plan namespace (`V4D::Keys::NAMESPACE`).

---

## 6. State Model

| Concept | DSL entity | LLVM IR analogue |
|---------|------------|------------------|
| Plan member variable | plain C++ member; passed as `R`/`RW` | `alloca` slot in the function |
| Shared (locked) variable | `_shared(x)` then `RS`/`RWS`/`CS` | global with atomic/ordered access |
| Safe (never-shared) variable | `_safe(x)` | private, thread-local |
| Named state value | `GlobalState::get/set/create<V>(key)` + `P<T>(key)` | global variable with optional change callback |
| Per-worker state | `LocalState::Keys::WORKER_INDEX`, `LocalState::get/set` | thread-id-dependent value |

`GlobalState::create<V>(key, value, cb)` registers a named value with an optional change
callback; `GlobalState::set<V>(key, v)` writes it and `GlobalState::apply<V>(key, f)`
updates it atomically. Edge-bound reads go through `P<T>(key)`. (A runtime may layer a
node-form property write on top — V4D's `set(key, edge)` does exactly that.)

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
   the n-ary form is only useful when you deliberately fuse instructions (e.g. `ADD`
   of a long constant chain).
4. **Destination-first ops.** `NEG` and `DEREF` take the destination as their first
   argument — allocate a storage slot (`RW`) for the result.
5. **Control flow must be structured.** Loops and conditionals must nest as balanced
   `branch`/`endBranch` regions. LLVM's CFG must be region-ified (dominator-tree based
   structuring) before lowering. See §7.2 for the two lowering strategies.
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
10. **Edge lifetime.** Result edges produced by `OP<>()`/`F()` are temporary values
    automatically kept alive by the transaction graph. No explicit `keep()` is needed;
    the Plan runtime preserves them in the execution graph.

### 7.2 CFG lowering strategies

When lowering LLVM IR to Plan-DSL, the control-flow graph (CFG) must be translated
into nested `branch`/`endBranch` regions. The `llvm2plan` compiler uses two
strategies, selected automatically: **structured lowering** (primary) and the
**PC state machine** (fallback). Before structuring, the translator inlines defined
internal calls into the entry function, canonicalizes the IR, and rewrites `switch`
terminators into equality-comparison diamonds.

#### The fundamental constraint

Plan-DSL's execution model is **frame-based**: the graph is built once, then
`runGraph()` re-executes the same `currentNodes_` list every frame with predicates
re-evaluated. LLVM IR's CFG is **continuous**: a function call traverses the CFG from
entry to return within a single execution. Every CFG lowering must force continuous
semantics into a frame-sequential model. Both strategies achieve this by executing
**one loop iteration per frame** — a loop with 10 iterations takes 10 frames.

#### A. Structured lowering (preferred)

The structuring pass region-ifies the CFG into a tree of structured regions using
LLVM's `LoopInfo` and dominator tree. The tree nodes are:

| Region kind | Plan-DSL output |
|-------------|-----------------|
| **Seq** | Sequential emission of children (no wrapper) |
| **Block** | One basic block's instructions + PHI contributions to successors |
| **If** | Two independent predicated regions: `branch(cond); then; endBranch(); branch(NOT(cond)); else; endBranch();` (an `elseBranch()` pair is deliberately avoided: the runtime's `[else]` handler flips the stacked branch state without consulting enclosing regions, so a disabled outer construct would still fire the else arm) |
| **While** | Arming gate (`PARALLEL_ONCE`) sets `runN_` and recomputes `condN_`; the body is guarded by `branch(runN_ && !brkN_ && !returned_ && (startN_ \|\| condN_))`, whose predicate clears `startN_` and latches `failN_` when it fails; `condN_` is recomputed after the body |
| **DoWhile** | Like `While`, but the first iteration bypasses the condition via `startN_` (set by the arming gate, cleared by the first predicate evaluation) |
| **Return** | `assign(RW(ret_), val)` (or a plain node for inline constants), then `plain(... RW(returned_) ... r_ = true)`, then `plain([this]() { request_finish(); })` |
| **Break** | PHI contributions along the break edge + `assign(RW(brkN_), V(true))` |
| **Unreachable** | `plain([this]() { CV_Assert(false); });` |
| **Empty** | Phi carrier only: control continues at a join/latch without code (emits the recorded PHI contributions) |

**Loop flags** (per loop N, declared automatically as plan members):

| Flag | Purpose |
|------|---------|
| `runN_` | Armed once by the `PARALLEL_ONCE` arming gate to start the loop |
| `brkN_` | Break flag: set `true` by `Break` region; rest of body skipped this frame |
| `failN_` | Exit flag: latched `true` when the loop predicate fails; opens the post-loop continuation era |
| `startN_` | First-iteration bypass (DoWhile): initially `false`, set by the arming gate, cleared by every predicate evaluation |
| `condN_` | Loop condition: recomputed from member-rooted operands before the first test and after each body run |
| `doneN_` | One-shot latch: gates straight-line statements inside continuation gates to run exactly once |

The structured model generates smaller, cleaner Plan-DSL because it leverages the
source program's natural nesting. Everything before the first top-level loop is emitted
inside a `PARALLEL_ONCE` guard (guarded additionally by `!returned_` when the program
returns). Code between loops chains through a continuation gate `branch(R(failN_))`;
straight-line statements inside are wrapped in a one-shot latch guarded by
`!doneN_` (and `!returned_`), which sets `doneN_` after running. Nested loops chain
through their own gates inside the outer continuation.

**Strengths:** No boot node, no token variables, correct by construction, generates
the most readable Plan-DSL. Handles all reducible control flow.

**Limitation:** Cannot handle irreducible CFGs (bails to the PC state machine, §7.2C,
printing the structuring bail reason as a diagnostic).

#### B. Legacy token lowering (removed)

An earlier design kept one `tokN_` bool per basic block (plus `pendN_` pending flags
for loop headers) and a complex boot node that transferred tokens each frame. It has
been **removed** from the translator; the PC state machine (§7.2C) replaced it because
it needs O(1) state, no boot node and no emission-order assumptions. This subsection is
kept only as a tombstone for older references.

#### C. PC state machine lowering (fallback)

The PC state machine replaces structured control flow with a single `int32_t pc_`
member. Each basic block becomes a `branch` guarded by `pc == N`; terminators write the
successor block's index. Back-edges work naturally: the PC value takes effect next
frame, producing one iteration per frame. Implemented in the `llvm2plan` translator
(`src/pc_state.cpp` / `src/cfg_emitter.cpp` in that project, not in this module).

```
// State: int32_t pc_ = 0;  (single plan member, declared automatically)

// Block 0 (entry):
branch([this](const int32_t& pc) { return pc == 0; }, R(pc_));
    // ... instructions ...
    assign(RW(pc_), V(1));               // fall through
endBranch();

// Block 1 (conditional):
branch([this](const int32_t& pc) { return pc == 1; }, R(pc_));
    // ... instructions ...
    branch(cond);    assign(RW(pc_), V(2)); endBranch();   // true edge
    branch(!cond);   assign(RW(pc_), V(3)); endBranch();   // false edge
endBranch();

// Block 2 (loop header):
branch([this](const int32_t& pc) { return pc == 2; }, R(pc_));
    // ... instructions ...
    assign(RW(pc_), V(3));               // enter loop body
endBranch();

// Block 3 (loop latch -> header):
branch([this](const int32_t& pc) { return pc == 3; }, R(pc_));
    // ... instructions ...
    assign(RW(pc_), V(2));               // back-edge: takes effect next frame
endBranch();
```

Details:

- **No boot node.** `pc_` starts at the entry block's index.
- **Conditional terminators** are emitted as two independent `branch` regions
  (condition, then negation) rather than an `if/else` expression — matching the
  structured emitter and avoiding `elseBranch()` ordering issues.
- **PHI nodes** are written at predecessor tails immediately before the terminator's
  `pc_` write.
- **Returns** emit `assign(RW(ret_), val)` followed by `plain([this]() { request_finish(); })`.
- **Switch** never reaches this stage (pre-lowered into equality diamonds);
  `unreachable` lowers to `plain([this]() { CV_Assert(false); });`.

Compared to the removed token model (§7.2B), this reduces state from O(N) booleans to
O(1), eliminates the boot node, removes the reverse-post-order emission-order
dependency (each block checks its own `pc == N`), and handles arbitrary (including
irreducible) CFGs.

#### D. CFG lowering comparison

| Aspect | Structured | PC state machine |
|--------|-----------|------------------|
| Branch regions | Nested, O(depth) | One per block, O(N) |
| State variables | Per-loop flags (`run_/brk_/fail_/start_/cond_/done_`) + `returned_` | 1 `int32_t pc_` |
| Boot node | None | None |
| Loop handling | Native `branch(pred)` nesting | `pc_ = headerIndex` (next frame) |
| Ordering | Structural nesting | None needed (each block checks `pc == N`) |
| Code readability | Best (natural nesting) | Good (explicit PC) |
| Selection | Primary (reducible CFGs) | Fallback (any CFG) |

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
