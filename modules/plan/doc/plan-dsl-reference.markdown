# Plan-DSL Reference

*The formal reference for Plan-DSL, the task-graph language used by Plan-V4D.*

This document is the companion to `plan-dsl-programming-guide.markdown`.

The programming guide explains how to think about Plan-DSL. This reference describes the language model more directly:

* edge-calls,
* operators,
* control-flow instructions,
* contexts,
* state model,
* LLVM lowering correspondence.

---

## Table of Contents

1. [Execution model](#1-execution-model)
2. [Edge-calls](#2-edge-calls)
3. [Operator instructions](#3-operator-instructions)
4. [Control-flow instructions](#4-control-flow-instructions)
5. [Program structure and contexts](#5-program-structure-and-contexts)
6. [State model](#6-state-model)
7. [LLVM IR to Plan-DSL translation](#7-llvm-ir-to-plan-dsl-translation)
8. [Opcode index](#8-opcode-index)

---

## 1. Execution model

Plan-DSL is an embedded C++ language that records a task graph.

User code is written inside lifecycle methods of a class derived from `Plan` or `V4DPlan`.

The main lifecycle methods are:

| Method | Role |
|---|---|
| `setup()` | One-shot initialization graph |
| `infer()` | Per-frame graph |
| `teardown()` | One-shot cleanup graph |
| `gui()` | Main-thread GUI setup |

The graph is built once and then executed repeatedly.

```text
infer() records nodes
runGraph() replays nodes every frame
```

Nodes are executed in record order.

Plan-DSL does not perform dynamic dependency scheduling, work stealing, or automatic vectorization.

The runtime evaluates branch predicates each frame and executes enabled nodes.

---

## 2. Edge-calls

Edges are the only value type in Plan-DSL.

An edge represents either:

* storage,
* a computed value,
* a runtime property,
* an event stream.

Each edge has an access intent.

### 2.1 `V(value)` — constant

```cpp
template<typename T> Edge<T, true, true, false> V(T&& value)
```

Creates an immediate constant edge.

Equivalent conceptually to an LLVM constant.

Example:

```cpp
V(42)
V(3.14f)
V(cv::Size(640, 480))
```

### 2.2 `R(variable)` — read

```cpp
template<typename T> Edge<T, false, true, false> R(const T& t)
```

Creates a read-only edge to the current value of a variable.

Use `R` when a node only needs to read a value.

Example:

```cpp
R(frame_)
```

LLVM analogue: `load`.

### 2.3 `RW(variable)` — read-write

```cpp
template<typename T> Edge<T, false, false, false> RW(T& t)
```

Creates a read-write edge to a variable’s storage.

Use `RW` for destinations and in-place mutation.

Example:

```cpp
RW(counter_)
```

LLVM analogue: address operand for `store` or in-place memory operation.

### 2.4 `RS(variable)` — read shared

```cpp
template<typename T> Edge<T, false, true, true> RS(const T& t)
```

Read-only access to a shared variable.

The variable must be registered with `_shared()` if it is a plan member.

The node locks the variable’s mutex for the duration of the access.

Example:

```cpp
RS(params_)
```

LLVM analogue: atomic or synchronized load.

### 2.5 `RWS(variable)` — read-write shared

```cpp
template<typename T> Edge<T, false, false, true> RWS(T& t)
```

Read-write access to a shared variable under its mutex.

Example:

```cpp
RWS(params_)
```

LLVM analogue: synchronized read-modify-write or locked store/load pair.

### 2.6 `CS(variable)` — copy shared snapshot

```cpp
template<typename T> Edge<T, true, true, true> CS(T& t)
```

Creates a thread-safe copy of a shared variable.

The variable is read under its mutex, and a private copy is produced.

This is useful when one thread produces data and another thread should consume a stable snapshot.

Example:

```cpp
CS(params_)
```

LLVM analogue: load followed by copy.

### 2.7 `P<T>(key)` — runtime property

```cpp
template<typename Tval> Property<Tval> P(LocalState::Keys::Enum key);
template<typename Tval> Property<Tval> P(GlobalState::Keys::Enum key);
```

Creates a property edge bound to a named value in `LocalState` or `GlobalState`.

Properties are shared read-only edges.

Examples:

```cpp
P<uint64_t>(GlobalState::Keys::FRAME_CNT)
P<cv::Size>(V4D::Keys::SIZE)
P<size_t>(LocalState::Keys::WORKER_INDEX)
```

Core global keys include:

```cpp
FRAME_CNT
CAPTURE_CNT
FPS_CNT
RUN_CNT
START_TIME
FPS
WORKERS_READY
WORKERS_STARTED
LOCKING
DISPLAY_READY
LOCK_CONTENTION_CNT
LOCK_CONTENTION_RATE
LCR_CNT
SHOW_GUI
SHOW_FRAME_TIME
TIME_TRACKER
```

Core local keys include:

```cpp
WORKER_INDEX
```

Runtimes may add additional key families, such as `V4D::Keys`.

LLVM analogue: global variable or fixed runtime register.

### 2.8 `E<T>(...)` — event stream

```cpp
template<typename Tclass> Event<Tclass> E();
template<typename Tclass> Event<Tclass> E(Tclass::Type t);
template<typename Tclass, typename Ttrigger> Event<Tclass> E(Tclass::Type t, Ttrigger tr);
```

Creates an edge that produces a list of input events for the current iteration.

Event classes must provide:

* a nested `Type` enum,
* a `List` container.

The core DSL produces empty lists by default. Runtimes such as V4D provide real event polling.

V4D event classes include:

```cpp
Mouse
Keyboard
Window
Joystick
```

Examples:

```cpp
E<Mouse>()
E<Mouse>(Mouse::Type::PRESS)
E<Mouse>(Mouse::Type::PRESS, trigger)
```

LLVM analogue: external volatile input channel.

### 2.9 `F(fn, args...)` — call

```cpp
template<typename Tfn, typename... Args>
auto F(Tfn src, Args&&... args);
```

Records a call to a C++ callable.

If the callable returns a non-`void` type, `F` returns a result edge.

If the callable returns `void`, `F` behaves as a statement.

Examples:

```cpp
auto t = F(&cv::getTickCount);
auto w = F(&cv::Size::width, R(sz));
F(&cv::split, R(src), RW(dst));
```

Accepted callables include:

* free functions,
* member functions,
* lambdas,
* function objects.

LLVM analogue: `call`.

### 2.10 `_()` — operand group

```cpp
template<typename... Args>
auto _(Args&&... args);
```

Builds a tuple of operands.

Used for n-ary operators and variadic calls.

Example:

```cpp
auto t = R(a) + _(R(b), R(c));
```

### 2.11 `_shared(var)` and `_safe(var)` — storage registration

```cpp
template<typename Tvar> void _shared(Tvar& val);
template<typename Tvar> void _safe(Tvar& val);
```

`_shared` registers a variable as shared and gives it a mutex.

`_safe` marks a variable as never shared.

These should usually be called from the plan constructor.

---

## 3. Operator instructions

Operators are the computational instructions of Plan-DSL.

They are dispatched through the `Operators` enum.

Operators can be written in four forms:

### 3.1 Symbol form

```cpp
a + b
a && b
a[i]
x = y
```

### 3.2 Named form

```cpp
ADD(a, b)
MOD(a, b)
NEG(dst, x)
```

### 3.3 Generic form

```cpp
OP<Operators::ADD_>(a, b)
```

### 3.4 Statement form

```cpp
op<Operators::ADD_>(a, b)
assign(dst, src)
construct(dst, args...)
```

Expression forms return result edges.

Statement forms do not return result edges.

---

## 3.1 Arithmetic operators

| Opcode | Symbol | Named | Arity | Meaning | LLVM analogue |
|---|---:|---|---:|---|---|
| `ADD_` | `+` | `ADD` | n-ary | Addition | `add` |
| `SUB_` | `-` | `SUB` | n-ary | Subtraction | `sub` |
| `MUL_` | `*` | `MUL` | n-ary | Multiplication | `mul` |
| `DIV_` | `/` | `DIV` | n-ary | Division | `sdiv` / `udiv` / `fdiv` via C++ |
| `MOD_` | `%` | `MOD` | binary | Remainder | `srem` / `urem` / `frem` via C++ |
| `NEG_` | — | `NEG` | binary | `dst = -src` | `fneg` or `sub 0, x` |
| `INCL_` | `++x` | `INCL` | unary | Pre-increment | `add x, 1` + store |
| `INCR_` | `x++` | `INCR` | unary | Post-increment | `add x, 1` + store, old value |
| `DECL_` | `--x` | `DECL` | unary | Pre-decrement | `sub x, 1` + store |
| `DECR_` | `x--` | `DECR` | unary | Post-decrement | `sub x, 1` + store, old value |

`NEG` takes the destination first:

```cpp
NEG(RW(dst), R(src));
```

Unary minus written as `-x` is lowered as multiplication by `-1` using `MUL`.

---

## 3.2 Logical and bitwise operators

| Opcode | Symbol | Named | Arity | Meaning | LLVM analogue |
|---|---:|---|---:|---|---|
| `AND_` | `&&` | `AND` | n-ary | Logical and | boolean `and` |
| `OR_` | `||` | `OR` | n-ary | Logical or | boolean `or` |
| `NOT_` | `!` | `NOT` | unary | Logical not | `xor i1 x, true` |
| `XOR_` | `^` | `XOR` | n-ary | Bitwise xor | `xor` |
| `BAND_` | `&` | `BAND` | n-ary | Bitwise and | `and` |
| `BOR_` | `|` | `BOR` | n-ary | Bitwise or | `or` |
| `SHL_` | `<<` | `SHL` | n-ary | Left shift | `shl` |
| `SHR_` | `>>` | `SHR` | n-ary | Right shift | `lshr` or `ashr` depending on C++ type |

---

## 3.3 Comparison operators

| Opcode | Symbol | Named | Arity | Meaning | LLVM analogue |
|---|---:|---|---:|---|---|
| `EQ_` | `==` | `EQ` | n-ary | Equality | `icmp eq` / `fcmp oeq` |
| `NEQ_` | `!=` | `NEQ` | n-ary | Inequality | `icmp ne` / `fcmp une` |
| `LT_` | `<` | `LT` | n-ary | Less than | `icmp slt/ult` / `fcmp olt` |
| `GT_` | `>` | `GT` | n-ary | Greater than | `icmp sgt/ugt` / `fcmp ogt` |
| `LE_` | `<=` | `LE` | n-ary | Less or equal | `icmp sle/ule` / `fcmp ole` |
| `GE_` | `>=` | `GE` | n-ary | Greater or equal | `icmp sge/uge` / `fcmp oge` |

Comparison semantics follow native C++ operators.

---

## 3.4 Selection, memory, and construction

| Opcode | Symbol | Named | Arity | Meaning | LLVM analogue |
|---|---:|---|---:|---|---|
| `IF_` | — | `IF` | ternary | `cond ? a : b` | `select` |
| `IDX_` | `[]` | `IDX` | binary | Container indexing | `getelementptr` + `load` |
| `DEREF_` | — | `DEREF` | binary | `dst = *ptr` | `load` into destination |
| `ASSIGN_` | `=` | `ASSIGN` | binary | Assignment | `store` |
| `CONSTRUCT_` | `()` | via `operator()` | variadic | Construct value | constructor call / allocation |

Examples:

```cpp
IF(cond, a, b)

IDX(container, index)

DEREF(RW(dst), R(ptr))

assign(RW(x), R(y))

RW(x) = R(y)

construct(dst, a, b)
```

`DEREF` and `NEG` take the destination as the first operand.

---

## 3.5 Lowercase statement helpers

```cpp
template<Operators Top, typename... Edges>
cv::Ptr<Plan> op(Edges...);

template<typename... Edges>
cv::Ptr<Plan> assign(Edges...);

template<typename... Edges>
cv::Ptr<Plan> construct(Edges...);
```

These create nodes that perform operations but do not return result edges.

Use them when the only meaningful effect is a write-back.

---

## 4. Control-flow instructions

Control flow is structured.

There are no arbitrary jumps.

Control flow is expressed with branch regions.

---

## 4.1 `branch(...)`

```cpp
branch(predEdge)
branch(fn)
branch(fn, args...)
branch(workerIdx, fn, args...)
branch(workerIdx, BranchType::Enum type, fn, args...)
branch(BranchType::Enum type, predEdge)
branch(BranchType::Enum type, fn, args...)
branch(BranchType::Enum type, workerIdx, fn, args...)
```

Opens a predicated region.

Default branch type is `BranchType::PARALLEL`.

Returns `cv::Ptr<Plan>` and can be chained.

Example:

```cpp
branch(R(x) == V(0));
    plain(doWork);
endBranch();
```

---

## 4.2 `elseBranch()`

```cpp
elseBranch();
```

Negates the current branch condition.

Equivalent to an `else` block.

Example:

```cpp
branch(cond);
    plain(a);
elseBranch();
    plain(b);
endBranch();
```

---

## 4.3 `endBranch()`

```cpp
endBranch();
```

Closes the current branch region.

Every `branch` must be matched by exactly one `endBranch`.

Branching is determined by source nesting.

---

## 4.4 Branch types

| Value | Name | Semantics |
|---|---|---|
| `0` | `NONE` | Plain node behavior |
| `1` | `SINGLE` | At most one worker executes |
| `2` | `PARALLEL` | Every worker executes if predicate is true |
| `4` | `ONCE` | Executes exactly once globally |
| `8` | `PARALLEL_ONCE` | Executes exactly once per worker |

---

## 4.5 Predefined predicates

```cpp
always_
isTrue_(bool)
isFalse_(bool)
and_(bool, bool)
or_(bool, bool)
```

These are convenience predicates exposed by the DSL.

---

## 5. Program structure and contexts

## 5.1 Plans and sub-plans

A `Plan` is a module or program.

A plan can contain sub-plans.

Sub-plans are created with `_sub`:

```cpp
template<typename TsubPlan, typename Tparent, typename... Args>
auto _sub(Tparent* parent, Args&&... args);

template<typename TsubPlan, typename TparentPtr, typename... Args>
auto _sub(TparentPtr parent, Args&&... args);
```

Sub-plans are spliced into the parent graph with:

```cpp
subInfer(subPlan);
subSetup(subPlan);
subTeardown(subPlan);
```

`_sub` must be called only in the parent constructor.

`subInfer` is normally called from the parent’s `infer()`.

LLVM analogue:

* `_sub` ≈ function declaration/instantiation,
* sub-plan ≈ function body,
* `subInfer` ≈ call site.

---

## 5.2 Context calls

Context calls attach nodes to specialized execution contexts.

The core DSL defines the plain CPU context.

### Core context calls

| Call | Context | Purpose |
|---|---|---|
| `plain(fn, args...)` | CPU | General-purpose node |
| `F(fn, args...)` | CPU | Function-call node |

Both are wrappers over the underlying transaction-adding mechanism.

### V4D context calls

| Call | Context | Purpose |
|---|---|---|
| `gl(fn, args...)` | OpenGL | Execute GL commands |
| `gl(idxEdge, fn, args...)` | OpenGL | Execute GL commands on context index |
| `fb<pos>(fn, args...)` | Framebuffer | Framebuffer access |
| `nvg(fn, args...)` | NanoVG | Vector graphics |
| `bgfx(fn, args...)` | bgfx | bgfx rendering |
| `ext(fn, args...)` | External | External renderer context |
| `capture(fn, args...)` | Source | Pull input frame |
| `capture(edge)` | Source | Pull input frame into edge |
| `capture()` | Source | Pull input frame |
| `write(fn, args...)` | Sink | Push output frame |
| `write(edge)` | Sink | Push output frame |
| `write()` | Sink | Push output frame |
| `set(key, edge)` | CPU | Property write node |
| `imgui(fn, args...)` | ImGui | Install ImGui transaction |

Most context calls return `cv::Ptr<V4DPlan>` and can be chained.

---

## 5.3 Entry points

```cpp
static cv::Ptr<Tplan> Plan::make<Tplan>(args...);
static void Plan::run<Tplan>(workers, args...);
```

`make` instantiates a plan.

`run` starts the full lifecycle.

For V4D:

```cpp
V4DPlan::make<Tplan>(args...);
V4DPlan::run<Tplan>(workers, args...);
```

`V4DPlan::run` forwards to `Plan::run`.

`V4DPlan::make` additionally publishes the plan namespace.

---

## 6. State model

| Concept | Plan-DSL entity | LLVM analogue |
|---|---|---|
| Plan member variable | Plain C++ member accessed via `R`/`RW` | `alloca` slot |
| Shared variable | `_shared(x)` + `RS`/`RWS`/`CS` | global with synchronized access |
| Safe variable | `_safe(x)` | private thread-local storage |
| Named global state | `GlobalState` + `P<T>(key)` | global variable |
| Named local state | `LocalState` + `P<T>(key)` | thread-local variable |

`GlobalState` supports:

```cpp
GlobalState::create<V>(key, value, cb)
GlobalState::set<V>(key, v)
GlobalState::apply<V>(key, f)
```

`P<T>(key)` reads a state value as an edge.

Runtimes may add property write nodes. For example, V4D provides:

```cpp
set(key, edge)
```

---

## 7. LLVM IR to Plan-DSL translation

Plan-DSL can be treated as a graph-level ISA.

The following table summarizes the intended lowering from LLVM IR to Plan-DSL.

| LLVM IR | Plan-DSL |
|---|---|
| `%r = add i32 %a, %b` | `auto r = ADD(R(a), R(b));` |
| `sub` | `SUB` |
| `mul` | `MUL` |
| `sdiv` | `DIV` |
| `srem` | `MOD` |
| `udiv` | `DIV` |
| `urem` | `MOD` |
| `fdiv` | `DIV` |
| `frem` | `F(std::fmod, ...)` |
| `and` | `BAND` |
| `or` | `BOR` |
| `xor` | `XOR` |
| `shl` | `SHL` |
| `lshr` | `SHR` |
| `ashr` | `SHR` |
| `icmp eq` | `EQ` |
| `icmp ne` | `NEQ` |
| `icmp slt` | `LT` |
| `icmp sle` | `LE` |
| `icmp sgt` | `GT` |
| `icmp sge` | `GE` |
| `fcmp oeq` | `EQ` |
| `fcmp one` | `NEQ` |
| `fcmp olt` | `LT` |
| `fcmp ole` | `LE` |
| `fcmp ogt` | `GT` |
| `fcmp oge` | `GE` |
| `select i1 %c, %t, %f` | `IF(R(c), R(t), R(f))` |
| `alloca` | plan member variable |
| `load` | `R(v)` |
| `store` | `assign(RW(dst), R(src))` |
| load plus copy | `CS(v)` |
| `getelementptr` + `load` | `IDX(container, index)` |
| pointer load | `DEREF(RW(dst), R(ptr))` |
| void call | `F(fn, args...)` or context call |
| non-void call | result edge from `F(fn, args...)` |
| `ret` | end of graph; return values via shared state or result members |
| conditional `br` | `branch(...) ... endBranch()` |
| `switch` | nested equality branches |
| `phi` | assignments to a member from predecessor branches |
| constant | `V(value)` |
| global variable | `_shared` variable or property |
| `unreachable` | `CV_Assert(false)` inside `plain` |
| `fneg` | `NEG(RW(dst), R(x))` |

---

## 7.1 Practical lowering notes

### SSA temporaries

Each LLVM virtual register can be represented by a Plan-DSL result edge.

Example:

```cpp
auto r = ADD(R(a), R(b));
auto s = MUL(r, R(c));
```

### Memory operations

LLVM `alloca`, `load`, and `store` sequences lower to plan member variables and `R`/`RW` edges.

Use the most restrictive intent possible:

* `R` for reads,
* `RW` for writes,
* `RS`/`RWS`/`CS` for shared state.

### One operation per instruction

For faithful lowering, emit one Plan-DSL operator per LLVM instruction.

N-ary forms are useful for deliberate instruction fusion.

### Destination-first operators

`NEG` and `DEREF` take the destination first:

```cpp
NEG(RW(dst), R(src));
DEREF(RW(dst), R(ptr));
```

### Structured control flow

LLVM control flow must be transformed into nested branch regions.

Loops are represented as branch regions whose predicates are updated by the loop body.

Because Plan-DSL executes frame by frame, one loop iteration may correspond to one frame.

### `select` versus branches

`IF` is an operator node. Its operands are computed eagerly as graph nodes.

For lazy execution of whole regions, use `branch`.

### Integer widths

Map LLVM integer types to native C++ types:

| LLVM type | C++ type |
|---|---|
| `i1` | `bool` |
| `i8` | `int8_t` / `uint8_t` |
| `i16` | `int16_t` / `uint16_t` |
| `i32` | `int32_t` / `uint32_t` |
| `i64` | `int64_t` / `uint64_t` |

### Pointers

LLVM pointer values can be represented by:

* raw pointers,
* `cv::Ptr<T>`,
* container indices,
* `DEREF` for loading through pointers.

### Floating point

`DIV` and `MOD` use C++ semantics.

For floating-point remainder, use:

```cpp
F(std::fmod, R(a), R(b))
```

---

## 7.2 CFG lowering strategies

Plan-DSL’s execution model is frame-based.

LLVM IR’s control-flow model is continuous.

Therefore, lowering must translate continuous control flow into frame-sequential execution.

Two strategies are used:

1. structured lowering,
2. program-counter state-machine lowering.

### A. Structured lowering

Structured lowering is preferred for reducible control-flow graphs.

The LLVM CFG is converted into structured regions using loop and dominator information.

Region mapping:

| Region kind | Plan-DSL output |
|---|---|
| Sequence | Sequential child emission |
| Block | Basic block instructions |
| If | Two predicated regions or branch/else structure |
| While | Branch region with loop-state members |
| DoWhile | Branch region with first-iteration bypass |
| Return | Assignment to return value plus finish request |
| Break | Assign break flag |
| Unreachable | `CV_Assert(false)` |
| Empty | PHI carrier only |

Loop-state flags may include:

| Flag | Purpose |
|---|---|
| `runN_` | Arms loop execution |
| `brkN_` | Break latch |
| `failN_` | Loop-exit latch |
| `startN_` | First-iteration bypass |
| `condN_` | Loop condition |
| `doneN_` | One-shot continuation latch |

Structured lowering generates cleaner and more readable Plan-DSL.

It cannot handle irreducible control flow.

### B. PC state-machine lowering

The PC state-machine lowering is the fallback.

It replaces control flow with a single program counter:

```cpp
int32_t pc_ = 0;
```

Each basic block becomes a branch guarded by:

```cpp
pc_ == N
```

Terminators assign the next block index:

```cpp
assign(RW(pc_), V(nextBlock));
```

Example:

```cpp
branch(this { return pc_ == 0; }, R(pc_));
    // block 0
    assign(RW(pc_), V(1));
endBranch();

branch(this { return pc_ == 1; }, R(pc_));
    // block 1
    branch(cond);
        assign(RW(pc_), V(2));
    endBranch();
    branch(!cond);
        assign(RW(pc_), V(3));
    endBranch();
endBranch();
```

Back edges naturally take effect on the next frame.

This strategy handles arbitrary control-flow graphs, including irreducible ones.

It uses `O(1)` control state and avoids boot nodes.

### C. Comparison

| Aspect | Structured lowering | PC state machine |
|---|---|---|
| Branch nesting | Natural | One branch per block |
| State variables | Per-loop flags | One `pc_` |
| Boot node | No | No |
| Loop handling | Native structured regions | PC assignment |
| Readability | Best | Good |
| Applicability | Reducible CFGs | Any CFG |

---

## 8. Opcode index

| Mnemonic | Opcode | Arity | Symbol | Named | Statement form |
|---|---|---:|---|---|---|
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
| OR | `OR_` | n | `||` | `OR` | `op<OR_>` |
| EQ | `EQ_` | n | `==` | `EQ` | `op<EQ_>` |
| NEQ | `NEQ_` | n | `!=` | `NEQ` | `op<NEQ_>` |
| LT | `LT_` | n | `<` | `LT` | `op<LT_>` |
| GT | `GT_` | n | `>` | `GT` | `op<GT_>` |
| LE | `LE_` | n | `<=` | `LE` | `op<LE_>` |
| GE | `GE_` | n | `>=` | `GE` | `op<GE_>` |
| NOT | `NOT_` | 1 | `!` | `NOT` | `op<NOT_>` |
| XOR | `XOR_` | n | `^` | `XOR` | `op<XOR_>` |
| BAND | `BAND_` | n | `&` | `BAND` | `op<BAND_>` |
| BOR | `BOR_` | n | `|` | `BOR` | `op<BOR_>` |
| SHL | `SHL_` | n | `<<` | `SHL` | `op<SHL_>` |
| SHR | `SHR_` | n | `>>` | `SHR` | `op<SHR_>` |
| IF | `IF_` | 3 | — | `IF` | `op<IF_>` |
| IDX | `IDX_` | 2 | `[]` | `IDX` | `op<IDX_>` |
| DEREF | `DEREF_` | 2 | — | `DEREF` | `op<DEREF_>` |
| NEG | `NEG_` | 2 | — | `NEG` | `op<NEG_>` |

