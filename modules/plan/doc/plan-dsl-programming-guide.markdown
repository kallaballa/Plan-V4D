# The Plan-DSL Programming Guide

*A friendly introduction to the language that powers Plan-V4D.*

> **Who this is for.** A C++ developer who has never touched Plan-V4D and wants
> to understand how to *write* a program in Plan-DSL. After reading this
> document you will be able to read every sample in `modules/v4d/samples/`
> without reaching for the reference manual. For the canonical, no-nonsense
> description of every opcode and every overload, see the companion
> `plan-dsl-reference.markdown` (the "ISA manual").
>
> **Companion reference:** [`plan-dsl-reference.markdown`](./plan-dsl-reference.markdown)

---

## Table of contents

1. [Hello, graph](#1-hello-graph)
2. [What kind of language is this?](#2-what-kind-of-language-is-this)
3. [Your first plan](#3-your-first-plan)
4. [The execution model: built once, run forever](#4-the-execution-model-built-once-run-forever)
5. [Edges: the only values in the language](#5-edges-the-only-values-in-the-language)
6. [Operators: the ALU](#6-operators-the-alu)
7. [Variables, locals and shared state](#7-variables-locals-and-shared-state)
8. [Control flow: branch / elseBranch / endBranch](#8-control-flow-branch--elsebranch--endbranch)
9. [Functions, calls and `F(...)`](#9-functions-calls-and-f)
10. [Properties: reading runtime state](#10-properties-reading-runtime-state)
11. [Events: reading input](#11-events-reading-input)
12. [Loops, as a special case of branches](#12-loops-as-a-special-case-of-branches)
13. [Sub-plans: modules and functions](#13-sub-plans-modules-and-functions)
14. [Side-effect contexts: gl, nvg, fb, capture, write, imgui](#14-side-effect-contexts-gl-nvg-fb-capture-write-imgui)
15. [The lifecycle: setup / infer / teardown / gui / run](#15-the-lifecycle-setup--infer--teardown--gui--run)
16. [Workers: how parallelism actually happens](#16-workers-how-parallelism-actually-happens)
17. [A walk-through of `video_editing.cpp`](#17-a-walk-through-of-video_editingcpp)
18. [A walk-through of `beauty-demo.cpp`](#18-a-walk-through-of-beauty-democpp)
19. [Cheat-sheet](#19-cheat-sheet)

---

## 1. Hello, graph

Open `modules/v4d/samples/video_editing.cpp`. The entire program is fifty
lines long. Here it is, with the DSL bits highlighted:

```cpp
class VideoEditingPlan : public V4DPlan {
    cv::UMat frame_;
    const string hv_ = "Hello Video!";
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        capture();                                       // (1) pull a frame in
        nvg([](const Size& sz, const string& str) {      // (2) draw "Hello Video!" on it
            using namespace cv::v4d::nvg;
            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0, str.c_str(), str.c_str() + str.size());
        }, sz_, R(hv_));
        write();                                         // (3) push the frame out
    }
};

int main(int argc, char** argv) {
    // ... open source and sink ...
    V4DPlan::run<VideoEditingPlan>(0);                   // (4) start the runtime
}
```

The class `VideoEditingPlan` *describes* what happens every frame: pull a
frame, draw text, push a frame. `V4DPlan::run<VideoEditingPlan>(0)` then
*executes* that description forever in a window. There is no `while (true)`,
no `main loop`, no `update()`. The DSL takes care of all of it.

Everything you write goes inside one of four lifecycle methods — `setup`,
`infer`, `teardown`, `gui` — of a class that inherits from `Plan` (or, when
targeting the V4D windowing system, `V4DPlan`). Everything else is library
glue.

---

## 2. What kind of language is this?

Plan-DSL is best understood by what it is *not*:

| What it looks like  | What it actually is                                  |
|---------------------|------------------------------------------------------|
| A C++ library       | A *language embedded in C++* that *records* a graph. |
| A dataflow language | A graph of *task nodes* with explicit memory deps.   |
| An interpreter      | A *compiler*: every call records, then `runGraph()` replays. |
| An actor framework  | A flat list of nodes executed in record order every frame. |

A few consequences fall out of that, and they are worth keeping in your head
throughout the rest of this guide:

* **Side effects are deferred.** Calling `add(R(x), R(y))` does *not* add
  `x + y`. It records an `ADD` node that, on every frame, will add `x` and
  `y` and produce a new value. This is the single most important
  mental-model shift, because it means errors (wrong type, dangling pointer,
  undeclared shared variable) only surface when the graph is built — usually
  *before* `runGraph()` ever runs, but conceptually a different phase from
  ordinary C++ execution.

* **The "function" you are writing is `infer()`.** Every DSL call inside
  `infer()` emits one or more *nodes* into a list. The list is built *once*
  and then iterated every frame. `setup()` is for one-shot initialization
  (analogous to a global constructor); `teardown()` is for one-shot
  destruction; `runGraph()` is the inner per-frame loop that you never write
  yourself.

* **The semantics are those of a frame loop, not a thread of execution.**
  You are not *running* code; you are *describing* one iteration of a loop.
  Every frame, the same recorded code is re-executed on potentially new
  data.

If you have ever written OpenGL shaders, think of Plan-DSL as a *CPU-side
shader* for a per-frame computation graph: it reads inputs (`R`, `P`, `E`),
computes things (`ADD`, `MUL`, `IF`, `F`), and writes outputs (`RW`,
`assign`, `set`, `write`).

---

## 3. Your first plan

Stripped to its minimum, a plan looks like this:

```cpp
#include <opencv2/plan/plan.hpp>
using namespace cv;
using namespace cv::plan;

struct CountToTen : Plan {
    void infer() override {
        // Reads frame counter, compares to ten, exits when reached.
        branch(P<uint64_t>(GlobalState::Keys::FRAME_CNT) < V(uint64_t(10)));
            plain([]() { std::cout << "tick" << std::endl; });
        endBranch();
    }
};

int main() {
    Plan::run<CountToTen>(0);   // spawn one worker + main
}
```

* `Plan` is the core base class. Everything lives in `cv::plan`.
* `infer()` is the per-frame body. We do *not* override it with a `while`
  loop; the runtime drives frames for us.
* `branch(pred)` opens a predicated region. If `pred` is true this frame,
  its body runs. Otherwise it is skipped. `endBranch()` closes it.
* `plain([](){ ... })` is the simplest node: a C++ lambda run every time
  its enclosing region is enabled.
* `V(uint64_t(10))` materializes the constant `10` as an edge (think
  "operand"). `P<...>(key)` reads a runtime property (the frame counter).
* `Plan::run<CountToTen>(0)` starts the lifecycle. `0` means one worker
  plus the main thread. See §15.

If you build and execute this you will see one `tick` per frame for ten
frames and then the program exits.

---

## 4. The execution model: built once, run forever

This is the single most important section in the document. Re-read it before
writing any non-trivial plan.

A `Plan` defines four lifecycle hooks:

| Method      | When it runs                                         |
|-------------|------------------------------------------------------|
| `setup()`   | Once per worker thread, before the frame loop.       |
| `infer()`   | Once per worker thread, *builds* the per-frame graph. |
| `teardown()`| Once per worker thread, after the frame loop.         |
| `gui()`     | Once, on the main thread, before the frame loop.      |

The key insight is the split between *building* and *running*:

```
setup()      ─┐
              │  called once per worker
infer()      ─┤  (infer is where the per-frame graph is *recorded*)
              │
teardown()   ─┘

After all workers' infer() return, every worker enters:

    loop forever:
        runGraph()       // re-executes the recorded graph
```

`runGraph()` is provided by the runtime; you never write it. It iterates the
recorded list of *task nodes* in record order. For each node:

1. If the node is a `branch`, evaluate its predicate. If false, disable the
   branch until the matching `endBranch`.
2. If the node is inside a disabled branch, skip it.
3. Otherwise, call the node's callable (a lambda, a free function, an
   operator) with its operand values.

That is all. There is no data-dependency scheduling, no work-stealing, no
automatic vectorization. The nodes run in the order you wrote them, on the
worker thread that built them.

### 4.1 Why this matters

Because the graph is built **once**, you can — and should — write things
that would be wrong in a normal C++ program:

* You can hand the same `cv::UMat` to thirty different `R()` operands. They
  all just *record* that the upcoming node reads that buffer. The runtime
  snapshots the value when the node runs.
* You can compute a sub-expression once with `auto tmp = F(...)` and then
  use `R(tmp)` twenty times. The compiler dedupes nodes by their `id()`
  (see §5), so twenty uses of `R(tmp)` still emit one compute node.
* You can write `branch(R(x) == V(0))` even though `x` is uninitialized at
  graph-build time. The comparison node will be re-evaluated every frame
  and its result will change as `x` changes.

What you *cannot* do:

* You cannot mutate C++ state from inside `infer()` and expect the change
  to persist between frames, **except** through a node's `RW` output or a
  shared variable. Plain C++ side effects (writing to a global) happen
  once, at build time, on the worker thread that built the graph — and
  they happen *every time the worker calls `infer()`, which is once* — but
  this is almost never what you want.
* You cannot have the structure of the graph itself depend on runtime
  values. `if (someEdge) emitAdd() else emitMul()` always emits *both*
  sides; only one runs. Use `branch(...)` instead.

---

## 5. Edges: the only values in the language

Plan-DSL has exactly one value type: the **edge**. An edge is a typed
handle to either a *storage location* (a member variable, a shared global)
or to a *computed value* (the result of an operator or a function call).
Edges are produced by *edge-calls* and consumed by *operators*.

Think of an edge as a typed SSA value with an explicit memory access
intent. The intent tells the runtime how the node should treat the storage
on the other end of the edge.

### 5.1 The five kinds of edge-calls

| Edge-call | What it points at                | Access intent              |
|-----------|----------------------------------|----------------------------|
| `V(x)`    | An immediate constant `x`        | none (constant)            |
| `R(x)`    | The current value of `x`         | read-only                  |
| `RW(x)`   | The storage of `x`               | read-write (def)           |
| `RS(x)`   | A shared variable `x`            | read under lock            |
| `RWS(x)`  | A shared variable `x`            | read-write under lock      |
| `CS(x)`   | A *snapshot* of a shared variable| read + copy under lock     |
| `P(key)`   | A named runtime property         | shared, read-only          |
| `E<T>()`  | A stream of input events         | shared, polled each frame   |

Where `x` is any C++ lvalue the plan owns or has been told about (see §7).

The intent is what enables optimization. If a node reads five different
edges of `R(x)` and never writes `x`, the runtime knows the node is
read-only on `x`. If a node uses `RW(x)`, the runtime knows `x` is now
*defined* by that node and any subsequent `R(x)` reads its new value.

### 5.2 Edges are typed

The DSL inherits C++ types: `R(int_var)` is an edge of type `int`, `R(cv::UMat)`
is an edge of type `cv::UMat`, etc. Operator overloading does the rest:
`R(int_var) + V(1)` is an `ADD` node returning a new int edge.

The "type" the runtime sees is the C++ type, but each edge has an internal
*id* that is just the storage address it ultimately points at. Two edges
that ultimately wrap the same `int` member share the same id and merge
into the same node when accessed consecutively.

### 5.3 The `_()` tuple helper

Operators take edges, but variadic operators (`ADD`, `MUL`, ...) need to
take more than two. C++ doesn't have variadic operator overloading, so we
expose a single-argument tuple builder:

```cpp
auto sum3 = R(a) + _(R(b), R(c));   // R(a) + (R(b) + R(c))
```

`_` is just `std::make_tuple` for edges. Use it whenever you want n-ary
operators.

### 5.4 One slot, three views

A single C++ variable can be wrapped three ways:

```cpp
int x = 0;
auto eR  = R(x);    // read-only edge — "what is x now?"
auto eRW = RW(x);   // read-write edge — "set x to something"
auto eV  = V(7);    // an immediate 7 — does not touch x
```

`RW(x)` is the destination of an assignment. `R(x)` is the source of a read.
They share the same id (the address of `x`), but their *roles* in any
operator are different.

### 5.5 A worked example

```cpp
int threshold_ = 128;

void infer() override {
    auto src = R(frame_);                                  // read frame
    auto bright = F(&cv::mean, src) > V(threshold_);       // compute a bool
    branch(bright);
        plain([](const cv::UMat& f){ /* ... process ... */ }, R(frame_));
    endBranch();
}
```

* `R(frame_)` is an edge wrapping the `frame_` member.
* `F(&cv::mean, src)` calls `cv::mean(frame_)` inside a node; the result is
  a fresh edge holding the computed mean.
* `> V(threshold_)` builds a `GT` node returning a `bool` edge.
* `branch(bright)` opens a predicated region around the `plain` node that
  only runs on bright frames.

Every one of those calls *records*. Nothing executes. When `runGraph()` runs
the first frame it pulls in the actual `frame_`, computes the mean,
compares to 128, and either runs the body or skips it.

---

## 6. Operators: the ALU

Operators are the named instructions of the language. There are four ways
to spell any of them:

| Spelling        | Example                | Returns a result edge? |
|-----------------|------------------------|------------------------|
| Symbol (C++)    | `a + b`                | yes                    |
| Named (member)  | `ADD(a, b)`            | yes                    |
| Generic         | `OP<Operators::ADD_>(a, b)` | yes              |
| Statement       | `op<Operators::ADD_>(a, b)` / `assign(a, b)` | no |

Symbol form is the most ergonomic. The generic and statement forms exist
for tools (such as `llvm2plan`) that lower other representations.

### 6.1 Arithmetic and logic

Plan-DSL implements the full C++ operator set:

* Arithmetic: `+`, `-`, `*`, `/`, `%`, unary `-`, `++x`, `x++`, `--x`, `x--`
* Logical: `&&`, `||`, `!`
* Bitwise: `&`, `|`, `^`, `<<`, `>>`
* Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
* Ternary: `IF(cond, a, b)` (LLVM-`select`-shaped)
* Memory: `container[i]` → `IDX`, `*ptr` → `DEREF`, `dst = src` → `ASSIGN`
* Construction: `T(args...)` → `CONSTRUCT`

The named and `OP<>` forms map exactly onto these. `DEREF` and `NEG` are
special: they take the *destination first*, because they perform a write.

### 6.2 Statement form

When you only care about the write-back (e.g. `RW(x) = R(y)`), you can use
the *statement* form. It returns `cv::Ptr<Plan>` so it can be chained.

```cpp
assign(RW(x), R(y));   // x = y  (no result edge needed)
op<Operators::ADD_>(RW(x), R(y));  // x += y (statement form)
```

Statement forms are how you get "void" instructions without forcing the
caller to discard a result.

### 6.3 Associativity caveat

The n-ary implementations fold the *tail* right-associatively. For binary
operators — the common case — this is irrelevant. For ternary and beyond,
remember: `BAND(a, b, c)` means `a & (b & c)`, not `(a & b) & c`. If in
doubt, spell it out with explicit parentheses using `_(...)`.

---

## 7. Variables, locals and shared state

A plan's C++ member variables are its *local storage*. They correspond to
the "allocas" of the language:

```cpp
struct DemoPlan : Plan {
    int counter_ = 0;
    cv::UMat frame_;

    void infer() override {
        // counter_++ emits an INCR node; frame_ is just storage.
        auto c = RW(counter_);
        c = c + V(1);                 // explicit read-modify-write
    }
};
```

### 7.1 Local vs shared

By default, plan members are *thread-local*: each worker has its own copy
and never sees another worker's writes. That is exactly what you want for
most things (per-worker scratch buffers, per-worker counters).

When two workers need to see the same variable, declare it `_shared`. A
shared variable gets a `std::mutex` and must be accessed through `RS`,
`RWS`, or `CS`. A `CS` edge takes a thread-safe snapshot of the variable
under the lock — useful for cross-thread handoffs (e.g. GUI setting a flag
that an inference thread reads).

```cpp
struct DemoPlan : Plan {
    static Params params_;                  // shared between GUI and workers
    _safe(cv::UMat());                      // declare-as-safe (no shared mutex)

    DemoPlan() {
        _shared(params_);                   // give params_ a mutex
    }
};

// access:
auto p_copy = CS(params_);                  // snapshot under lock
auto p_rw   = RWS(params_);                 // read/write under lock
auto p_ro   = RS(params_);                  // read under lock
```

`_safe` is the escape hatch: it tells the runtime "this variable will
*never* be accessed shared, so don't bother giving it a mutex even if its
address range overlaps a shared variable".

### 7.2 Globals and per-thread state

Two type-keyed maps are available globally:

* `GlobalState` — one value per key, shared across all workers and the GUI.
* `LocalState` — one value per thread per key.

Read them through `P<T>(key)`. Write them through `GlobalState::set<T>(key,
v)`. The Plan-DSL core ships with a small set of keys; runtimes like V4D
add their own (e.g. `V4D::Keys::SIZE`, `V4D::Keys::NAMESPACE`).

---

## 8. Control flow: branch / elseBranch / endBranch

Plan-DSL has no `goto`, no `break`, no `continue`. Control flow is *structured*
and expressed by **branch regions**:

```cpp
branch(predicate);                  // open a predicated region
    ... statements ...
elseBranch();                       // (optional) flip the predicate
    ... statements ...
endBranch();                        // close
```

A branch region is itself a node. When the runtime reaches the node, it
evaluates the predicate. If true, the body runs and `endBranch` is a no-op.
If false, the body is skipped; if an `elseBranch` follows, its body runs
instead.

### 8.1 Predicates

The predicate can be:

* A bool edge — `branch(R(x) == V(0))`.
* A callable returning `bool` — `branch([](){ return true; })` or
  `branch(always_)` (the predefined `always_` literal in `cv::plan`).
* A callable taking operand edges — `branch([](int a){ return a > 0; }, R(n))`.

Predefined predicates are exposed as static constexpr members of `Plan`:

```cpp
always_                   // []{ return true; }
isTrue_(bool)              // [](const bool& b){ return b; }
isFalse_(bool)             // [](const bool& b){ return !b; }
and_(bool, bool)           // a && b
or_(bool, bool)            // a || b
```

### 8.2 Branch types

Every `branch` is also tagged with a `BranchType::Enum`. The default is
`PARALLEL`. The full table is:

| Value | Name             | Semantics                                      |
|-------|------------------|------------------------------------------------|
| `0`   | `NONE`           | no branch (plain node)                         |
| `1`   | `SINGLE`         | at most one worker executes it                 |
| `2`   | `PARALLEL`       | every worker executes when predicate holds     |
| `4`   | `ONCE`           | exactly once, globally                          |
| `8`   | `PARALLEL_ONCE`  | exactly once, per worker                       |

Spell them with `branch(BranchType::SINGLE, pred)`. `SINGLE` and `ONCE`
acquire a global mutex; `ONCE` semantics are also sticky (the branch
*never* runs again, even on subsequent frames).

### 8.3 Chaining

`branch`, `elseBranch`, `endBranch`, `F`, `plain`, and all the context
calls return `cv::Ptr<Plan>`. You can chain them with `->` instead of
writing nested blocks:

```cpp
branch(cond)
    ->plain(work_a)
    ->branch(sub_cond)
        ->plain(work_b)
    ->endBranch()
->elseBranch()
    ->plain(work_c)
->endBranch();
```

This compiles to the same thing as the nested-block form. Choose whichever
you find more readable.

---

## 9. Functions, calls and `F(...)`

The escape hatch for "I need an instruction that doesn't exist as an
operator" is `F(fn, args...)`. `F` accepts any callable — a free function,
a member-function pointer, a lambda, a function object — and wraps it as a
node. If the callable returns non-`void`, `F` returns a fresh result edge
holding that value.

```cpp
auto t = F(&cv::getTickCount);                // returns a uint64 edge
auto w = F(&cv::Size::width, R(sz));          // member function call
F(&cv::split, R(src), RW(dst));               // free function, void result
```

`F` is also the way to bring external libraries into the graph — anything
callable from C++ can be a node.

There is no separate "function definition" statement. To reuse code, write
a regular C++ function or lambda and call it through `F`. The graph
records each call site as a node; identical call sites with identical
operand edges dedupe to one node.

---

## 10. Properties: reading runtime state

A *property* is an edge bound to a value in the `GlobalState` or
`LocalState` table:

```cpp
Property<cv::Size>  size_  = P<cv::Size>(V4D::Keys::SIZE);
Property<uint64_t>  frame_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
Property<size_t>    widx_  = P<size_t>(LocalState::Keys::WORKER_INDEX);
```

The DSL core ships these keys:

* Global: `FRAME_CNT`, `CAPTURE_CNT`, `FPS_CNT`, `RUN_CNT`, `START_TIME`,
  `FPS`, `WORKERS_READY`, `WORKERS_STARTED`, `LOCKING`, `DISPLAY_READY`,
  `LOCK_CONTENTION_CNT`, `LOCK_CONTENTION_RATE`, `LCR_CNT`, `SHOW_GUI`,
  `TIME_TRACKER`.
* Local (per thread): `WORKER_INDEX`.

Runtimes extend these. V4D adds `V4D::Keys::SIZE`, `VIEWPORT`, `NAMESPACE`,
`FULLSCREEN`, `DISABLE_INPUT_EVENTS`, and others.

A property is a *kind* of edge, so you can pass it directly to operators
without wrapping it in `R()`:

```cpp
branch(seqCnt_ % V(uint64_t(8)) == V(uint64_t(0)));   // every 8th frame
```

Writing to a global property is done with `set(key, edge)` (which creates
a write *node*) or directly with `GlobalState::set<T>(key, v)` from a
`plain` node.

---

## 11. Events: reading input

An *event edge* produces a vector of input events for the current frame:

```cpp
Event<Mouse> pressEvents_ = E<Mouse>(Mouse::Type::PRESS);
auto anyPress = !F(&Mouse::List::empty, pressEvents_);     // bool edge
```

The DSL core always produces an empty list. The runtime (e.g. V4D) plugs
in a real fetcher that drains its windowing/event subsystem. Available
event classes are `Mouse`, `Keyboard`, `Window`, `Joystick`. Each carries
a nested `Type` enum and a `List` container.

Use `E<T>()` for "all events of class T", `E<T>(t)` for "events of type t",
and `E<T>(t, trigger)` for the more advanced case where you also supply a
trigger predicate.

---

## 12. Loops, as a special case of branches

There is no `for` or `while` keyword. A loop is a branch region whose
predicate is a value the body itself updates. The classic example is a
counter:

```cpp
struct CountdownPlan : Plan {
    int counter_ = 10;

    void infer() override {
        // First frame: counter_==10, predicate true, body runs and
        // decrements counter_. Next frame counter_==9, predicate true,
        // ... until counter_==0, predicate false, region is skipped.
        branch(R(counter_) > V(0));
            plain([](int c){ std::cout << "tick " << c << std::endl; }, R(counter_));
            assign(RW(counter_), R(counter_) - V(1));
        endBranch();
    }
};
```

`runGraph()` re-evaluates every predicate every frame, so the body of the
branch runs as long as the predicate is true — exactly the semantics of
`while (cond) { body; }`. The graph is not rebuilt; only the predicate
changes.

For examples of more elaborate loop constructs (with break / continue,
state-machine lowering, etc.) see §17 and the video-editing sample's
`infer()` method.

---

## 13. Sub-plans: modules and functions

A sub-plan is a `Plan` instance owned by another `Plan`. You create one
in the parent constructor:

```cpp
struct ParentPlan : Plan {
    cv::Ptr<SubPlan> sub_;
    ParentPlan() : sub_(_sub<SubPlan>(this, /*ctor args...*/)) {}
    void infer() override {
        subInfer(sub_);                    // splice sub-plan's graph in
    }
};
```

`_sub<T>(this, args...)` is a *constructor-only* operation. The first
statement of the parent plan's constructor (right after the initializer
list) is the canonical place. After construction, you splice the sub-plan
into the parent's flow with:

* `subInfer(sub)` — splice `infer()` graph
* `subSetup(sub)` — splice `setup()` graph
* `subTeardown(sub)` — splice `teardown()` graph

`subInfer` is what you call every frame from the parent's `infer()`.

Sub-plans are the right tool when:

* You want a piece of logic you can name and reuse across plans.
* You want to organize a large `infer()` into composable sections
  (`BeautyFilterPlan`, `FaceFeatureMasksPlan` in `beauty-demo.cpp`).
* You want to call into a sub-plan from inside a branch region — a
  sub-plan graph is spliced in *at the call site*, so it inherits the
  enclosing branch's predicate.

---

## 14. Side-effect contexts: gl, nvg, fb, capture, write, imgui

A *context call* attaches a node to a specialized execution environment.
The DSL core defines one — the plain CPU context, exposed as `plain(fn,
args...)` and `F(fn, args...)`. Runtimes add more. V4D provides:

| Call                | Context         | Purpose                                  |
|---------------------|-----------------|------------------------------------------|
| `gl(fn, args...)`   | OpenGL          | Raw GL commands on context `idx`         |
| `fb<pos>(fn, args...)` | Framebuffer  | Framebuffer access (`fb` edge inserted)   |
| `nvg(fn, args...)`  | NanoVG          | Vector graphics                          |
| `bgfx(fn, args...)` | bgfx            | bgfx rendering                           |
| `ext(fn, args...)`  | External        | External renderer contexts               |
| `capture(...)`      | Source          | Pull the next input frame                |
| `write(...)`        | Sink            | Push the finished frame                  |
| `imgui(...)`        | ImGui           | Install UI node                          |
| `set(key, edge)`    | CPU             | Property write node                      |

A typical frame-loop body looks like:

```cpp
void infer() override {
    capture(RW(frames_.orig_));                          // (1) pull input
    plain(prepare_frames, R(downSize_), RW(frames_));    // (2) pre-process

    branch(RWS(params_.enabled_) = IF(...));             // (3) toggle
        branch(!F(&FaceFeatureExtractor::extract,
                  RW(extractor_), R(frames_.down_), RWS(features_)));
            assign(RWS(params_.state_), V(Params::NOT_DETECTED));
            plain(compose_result, RW(frames_), CS(params_));
        ->endBranch()
    ->elseBranch()
        plain(compose_result, RW(frames_), CS(params_));
        assign(RWS(params_.state_), V(Params::OFF));
    ->endBranch();

    fb<1>(cv::cvtColor, R(frames_.result_), V(cv::COLOR_BGR2RGBA),
          V(0), V(cv::ALGO_HINT_DEFAULT));               // (4) write fb
    write(R(frames_.result_));                           // (5) push to sink
}
```

Notice how every C++ function (`prepare_frames`, `compose_result`,
`cv::cvtColor`) is wrapped in a node that the runtime will dispatch to the
correct context.

---

## 15. The lifecycle: setup / infer / teardown / gui / run

A program starts with `Plan::run<Tplan>(workers, args...)` or, when
running under V4D, `V4DPlan::run<Tplan>(workers, args...)`. The argument
list is forwarded to `Tplan`'s constructor.

The runtime then:

1. Spawns `N + 1` worker threads (`workers == -1` defaults to `2` ⇒ `3`
   workers; `workers == 0` ⇒ `1` worker + main; `workers >= 1` ⇒ `N + 1`
   workers + main). Each worker builds its own independent copy of the
   graph.
2. On the main thread: calls `gui()` once.
3. On each worker: calls `setup()` → `makeGraph()` → `runGraph()` →
   `clearGraph()`. (setup is one-shot; we run it once and discard.)
4. On each worker: calls `infer()` → `makeGraph()`. (this is the
   per-frame graph.)
5. Barrier. All workers + main arrive_and_wait.
6. The runtime's frame loop begins. Each frame, every worker calls
   `runGraph()` — re-executing the recorded graph with fresh data.
7. When the runtime ends the loop, each worker calls `teardown()` →
   `makeGraph()` → `runGraph()` → `clearGraph()`, then joins.

`makeGraph()` is the operation that turns the recorded accesses into a
flat list of nodes; `runGraph()` is the per-frame replay; `clearGraph()`
discards the recorded state in preparation for the next phase.

### 15.1 `gui()` is special

`gui()` runs once, on the main thread, *outside* the graph. It is the
right place for UI node installation (V4D exposes `imgui(...)` for that)
or any one-shot main-thread setup. The current rule, as documented in
`beauty-demo.cpp`, is:

> "at the moment gui is an exception from the rule that a Plan only
> implements the graph, because it runs on the display thread. in the
> future it should implement its own graph which would run in concurrent
> to the main algorithm - locking shared state where neccessary"

So: keep `gui()` simple, do not store state in plan members that the
graph also writes, and use `_shared` for anything cross-thread.

---

## 16. Workers: how parallelism actually happens

Each worker thread builds and runs its **own** copy of the graph. Workers
do not share nodes, do not migrate work, and do not coordinate at the node
level. There is no work-stealing.

What workers *do* share is:

* The `GlobalState` table (so `P<...>(GlobalState::...)` reads the same
  value everywhere).
* The shared-variable table (so `_shared(x)` declares one mutex for `x`,
  no matter which worker touches it first).
* The runtime (so `runtime_->plainCtx()` returns the same CPU context).

When a branch is marked `SINGLE`, the runtime acquires a global mutex
inside the branch region so at most one worker executes its body. This is
the standard way to serialize cross-worker side effects (printing,
logging, file IO).

When a branch is marked `PARALLEL` (the default), every worker evaluates
the predicate and (if true) executes the body *concurrently* with the
others. There is no implicit serialization. If you write to an `RW`
member inside a `PARALLEL` branch, that member becomes a race unless it
is `_shared` and accessed via `RWS`.

The implication for designing a plan is: think of `infer()` as the
per-thread body of an OpenMP `#pragma omp parallel`. Whatever you would
do in a `parallel for`, do here.

---

## 17. A walk-through of `video_editing.cpp`

```cpp
class VideoEditingPlan : public V4DPlan {
    cv::UMat frame_;                                  // local per-worker
    const string hv_ = "Hello Video!";                // local per-worker
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);  // runtime prop
public:
    void infer() override {
        capture();                                    // (1) pull a frame
        nvg([](const Size& sz, const string& str) {   // (2) draw text
            using namespace cv::v4d::nvg;
            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0,
                 str.c_str(), str.c_str() + str.size());
        }, sz_, R(hv_));
        write();                                      // (3) push a frame
    }
};

int main(int argc, char** argv) {
    cv::Rect viewport(0, 0, 960, 960);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Video Editing",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    auto src = Source::make(runtime, argv[1]);        // wire a video source
    auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());
    runtime->setSource(src);
    runtime->setSink(sink);
    V4DPlan::run<VideoEditingPlan>(0);                // 0 == one worker + main
}
```

Walking through it:

* The plan inherits from `V4DPlan`, the V4D-flavored base class.
* `frame_` is declared but unused — a hint that V4D binds a default
  capture buffer to the plan.
* `Property<cv::Size> sz_` is an edge that reads the runtime's framebuffer
  size. It is updated every time the window resizes.
* `infer()` does three things every frame:
  1. `capture()` — pull the next video frame into the source buffer.
  2. `nvg(lambda, sz_, R(hv_))` — schedule a NanoVG draw call that uses
     `sz_` (the current viewport size) and `R(hv_)` (the local string).
     The lambda runs inside the NanoVG context.
  3. `write()` — push the composited frame to the sink (the output video).
* `main()` initializes the V4D runtime with a viewport, attaches a
  source/sink, and starts the plan. `0` means one worker thread plus the
  main thread; since V4D uses the main thread for the display/event loop,
  we effectively get one rendering worker.

The plan has *no* `setup()` or `teardown()` — there is nothing to
initialize or release.

---

## 18. A walk-through of `beauty-demo.cpp`

`beauty-demo.cpp` is the most representative example in the project. It
shows how the building blocks combine into a realistic plan: shared
state, sub-plans, branching with `IF`, mouse events, frame counters,
NanoVG, OpenCV's framebuffer, and a full GUI in `gui()`. Let's walk
through it section by section.

### 18.1 State and properties

```cpp
struct BeautyDemoPlan : public V4DPlan {
public:
    struct Params {
        float eyesAndLipsSaturation_ = 1.25f;
        float skinSaturation_ = 1.35f;
        float skinContrast_ = 0.75f;
        bool  sideBySide_ = false;
        bool  stretch_ = true;
        bool  fullscreen_ = false;
        bool  enabled_ = true;
        enum State { ON, OFF, NOT_DETECTED } state_ = ON;
    };
    struct Frames {
        cv::UMat orig_, stitched_, down_, bgr_, faceOval_, eyesAndLips_,
                 skin_, faceSkinMaskGrey_, eyesAndLipsMaskGrey_,
                 backgroundMaskGrey_, result_;
    };

private:
    static Params params_;             // shared: GUI writes, infer reads
    static FaceFeatures features_;     // shared: extractor writes, mask reads
    cv::Ptr<FaceFeatureExtractor> extractor_;
    float scale_ = 1;
    const cv::Size downSize_ = {640, 360};
    Frames frames_;

    Property<cv::Size>  size_     = P<cv::Size>(V4D::Keys::SIZE);
    Property<uint64_t>  seqCnt_   = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
    Event<Mouse>        pressEvents_ = E<Mouse>(Mouse::Type::PRESS);
    ...
};
```

* `params_` and `features_` are *static*, so they live across all plan
  instances. They are also declared with `_shared` in the constructor
  (below) so they get a mutex and can be safely accessed from `gui()`
  and from worker threads alike.
* `frames_` is per-worker (a non-static member) — each worker has its
  own scratch buffers.
* `size_` and `seqCnt_` are *property edges* — typed views onto the
  runtime's global state. They behave like edges, so they can be passed
  directly to operators without `R(...)`.
* `pressEvents_` is an *event edge*. The DSL core produces an empty list,
  but V4D's implementation drains its mouse queue every frame.

### 18.2 Sub-plans

```cpp
BeautyDemoPlan() {
    prepareFeatureMasksPlan_ = _sub<FaceFeatureMasksPlan>(this, features_, frames_);
    beautyFilterPlan_        = _sub<BeautyFilterPlan>(this, params_, frames_);
}
```

Two sub-plans are constructed in the parent constructor. They capture
references to the parent's `features_` and `frames_`, which makes the
sub-plans effectively "live inside" the parent — calling `subInfer(sub)`
splices the sub-plan's graph into the parent's at the call site.

### 18.3 The GUI

```cpp
void gui() override {
    imgui([](Params& params) {
        // ... ImGui widgets writing to params.sideBySide_, params.stretch_, ...
    }, RWS(params_));
}
```

`gui()` runs *once* on the main thread and installs an ImGui node. The
`RWS(params_)` edge grants the GUI thread write access under the
parameter mutex; from inside the lambda you can mutate the params
directly. The graph, when it later does `CS(params_)`, will take a fresh
snapshot.

### 18.4 `setup()`

```cpp
void setup() override {
    assign(RW(scale_), F(aspect_preserving_scale, size_, R(downSize_)));
    plain(setLogLevel, V(LOG_LEVEL_WARNING))
    ->construct(RW(extractor_), R(downSize_), R(scale_))
    ->plain(setLogLevel, V(LOG_LEVEL_INFO));
}
```

`setup()` runs once per worker. Here it:

1. Computes `scale_` (a scaling factor for the aspect-preserving resize)
   by calling `aspect_preserving_scale` via `F(...)` and assigning the
   result to `RW(scale_)`.
2. Quiets the log, constructs the DNN-backed face extractor (a heavy
   one-time operation), then restores the log level. Each step is a
   node; the chain is one continuous graph.

### 18.5 `infer()`

This is the per-frame body. Walking through it:

```cpp
void infer() override {
    set(V_::FULLSCREEN, CS(params_.fullscreen_));     // (a) write runtime state
    capture(RW(frames_.orig_));                       // (b) grab a frame
    plain(prepare_frames, R(downSize_), RW(frames_)); // (c) pre-process

    branch(                                            // (d) toggle on click
        RWS(params_.enabled_) = IF(
            F(&Mouse::List::empty, pressEvents_),
            CS(params_.enabled_),
            !CS(params_.enabled_)
        )
    )
        ->branch(seqCnt_ % V(uint64_t(8)) == V(uint64_t(0)))   // (e) every 8 frames
            ->branch(!F(&FaceFeatureExtractor::extract,
                        RW(extractor_), R(frames_.down_), RWS(features_)))
                assign(RWS(params_.state_), V(Params::NOT_DETECTED));
                plain(compose_result, RW(frames_), CS(params_));
            ->endBranch()
        ->endBranch()
        ->branch(!(F(&FaceFeatures::empty, RS(features_))));   // (f) features ready
            assign(RWS(params_.state_), V(Params::ON));
            subInfer(prepareFeatureMasksPlan_);
            subInfer(beautyFilterPlan_);
            plain(compose_result, RW(frames_), CS(params_));
        ->endBranch()
    ->elseBranch()
        plain(compose_result, RW(frames_), CS(params_));
        assign(RWS(params_.state_), V(Params::OFF));
    ->endBranch();

    fb<1>(cv::cvtColor, R(frames_.result_),
          V(cv::COLOR_BGR2RGBA), V(0), V(cv::ALGO_HINT_DEFAULT));
    write(R(frames_.result_));
}
```

A few things worth pointing out:

* `(d)` uses `IF` to flip `params_.enabled_` whenever a mouse press
  happens this frame. The whole expression is an *edge*; the
  assignment `RWS(...) = IF(...)` is itself a node (well, an assignment
  in expression form).
* `(e)` and `(f)` are nested `branch` regions. They form a structured
  `if / else if` chain: every 8 frames, try to detect features; if
  features are ready, run the effect; otherwise show a "Not detected"
  state.
* `subInfer(...)` splices the sub-plan's graph in at the call site.
  Inside the "features ready" branch we splice the mask generator and
  the beauty filter; inside the "not enabled" branch we don't.
* `fb<1>(cv::cvtColor, ...)` writes the result into framebuffer context
  #1 (the visible window). `write(R(frames_.result_))` pushes it to
  the sink (here, an `imshow`-style display, since the demo doesn't
  wire a real sink).

The whole `infer()` is one linear description of "what happens every
frame", expressed as a chain of nodes.

---

## 19. Cheat-sheet

The minimum vocabulary, in one page.

### Edge producers

```cpp
V(x)               // immediate constant
R(x)               // read of local x
RW(x)              // read/write of local x
RS(x)              // read of shared x (locks)
RWS(x)             // read/write of shared x (locks)
CS(x)              // snapshot of shared x (locks + copies)
P<T>(key)          // read global/local property
E<T>(type)         // read event stream (e.g. mouse presses)
_(_e1, _e2, ...)   // tuple of edges (variadic operator operands)
F(fn, args...)     // call any callable; non-void returns an edge
```

### Variables and storage

```cpp
T member_;                            // local per-worker storage
_shared(member_)                      // give member_ a shared mutex
_safe(member_)                        // declare-as-safe (never shared)
_globalstate<T>::set(key, v)          // write a global property
_globalstate<T>::get<T>(key)          // read a global property directly
```

### Operators (all return a result edge; use lowercase form for no-result)

```cpp
a + b            ADD(a, b)            // add (n-ary via `_(...)`)
a - b            SUB(a, b)
a * b            MUL(a, b)
a / b            DIV(a, b)
a % b            MOD(a, b)            // binary only
!a               NOT(a)
a && b           AND(a, b)
a || b           OR(a, b)
a == b           EQ(a, b)
a != b           NEQ(a, b)
a < b            LT(a, b)
a > b            GT(a, b)
a <= b           LE(a, b)
a >= b           GE(a, b)
++a              INCL(a)
a++              INCR(a)
--a              DECL(a)
a--              DECR(a)
e[i]             IDX(e, i)            // e[i]
plan(args...)    CONSTRUCT(dst, args) // dst = T(args)
dst = src        ASSIGN(dst, src)
*ptr             DEREF(RW(dst), R(ptr))
- value          NEG(RW(dst), R(value))
cond ? a : b     IF(cond, a, b)
```

Statement forms (return `cv::Ptr<Plan>`, no result edge):

```cpp
op<Operators::ADD_>(RW(x), R(y));      // x += y
assign(RW(x), R(y));                  // x = y
construct(RW(ptr), ...);              // ptr = new T(...)
```

### Control flow

```cpp
branch(pred);                  // open predicated region
    ... statements ...
elseBranch();                  // optional else arm
    ... statements ...
endBranch();                   // close

// One-liner with chaining:
branch(pred)
    ->plain(work)
    ->branch(sub_pred)
        ->plain(sub_work)
    ->endBranch()
->elseBranch()
    ->plain(other)
->endBranch();

// Predefined predicates:
always_                        // true
isTrue_(b)                     // b
isFalse_(b)                    // !b
and_(a, b)                     // a && b
or_(a, b)                      // a || b

// Branch types:
branch(BranchType::PARALLEL, pred);          // every worker (default)
branch(BranchType::SINGLE, pred);            // at most one worker (global lock)
branch(BranchType::ONCE, pred);              // exactly once, globally
branch(BranchType::PARALLEL_ONCE, pred);     // once per worker
```

### Sub-plans

```cpp
// In parent's constructor:
cv::Ptr<Sub> sub_ = _sub<Sub>(this, args...);

// In parent's infer():
subInfer(sub_);          // splice sub's infer() graph here
subSetup(sub_);          // splice sub's setup() graph
subTeardown(sub_);       // splice sub's teardown() graph
```

### Context calls (V4D)

```cpp
plain(fn, args...)              // CPU context
gl(fn, args...)                 // OpenGL
nvg(fn, args...)                // NanoVG
fb<pos>(fn, args...)            // Framebuffer (fb edge auto-inserted at pos)
bgfx(fn, args...)               // bgfx
ext(fn, args...)                // External renderer
capture() / capture(edge)       // Pull input frame
write()    / write(edge)        // Push output frame
imgui(fn, args...)              // Install UI node
set(key, edge)                  // Property write
```

### Lifecycle

```cpp
struct MyPlan : V4DPlan {
    void setup()    override { /* one-shot init */ }
    void infer()    override { /* per-frame graph (REQUIRED) */ }
    void teardown() override { /* one-shot cleanup */ }
    void gui()      override { /* main-thread UI, called once */ }
};

int main() {
    cv::Ptr<V4D> rt = V4D::init(viewport, "title", AllocateFlags::NANOVG | IMGUI);
    // ... wire source/sink ...
    V4DPlan::run<MyPlan>(/*workers=*/0);
}
```

---

## Where to go next

* [`plan-dsl-reference.markdown`](./plan-dsl-reference.markdown) — the
  authoritative reference for every opcode, every overload, every branch
  type, and the LLVM-IR lowering table. Keep it open while you write.
* `modules/v4d/samples/` — `video_editing.cpp`, `beauty-demo.cpp`,
  `font_rendering.cpp`, `optflow-demo.cpp`, etc. Each is a working plan.
* `modules/plan/include/opencv2/plan/plan.hpp` — the entire DSL fits in a
  single header (with `detail/transaction.hpp` and `util.hpp`). When in
  doubt, read it.
* `modules/plan/src/` — `plan.cpp` for the lifecycle loop, plus
  `detail/transaction.hpp` for how an operator turns into a callable.