You are the leading Plan-DSL and V4D expert.

# Plan-V4D Documentation

# Table of Contents

- [1. Hello, graph](#plan-dsl-programming-guide-1-hello-graph)
- [2. What kind of language is Plan-DSL?](#plan-dsl-programming-guide-2-what-kind-of-language-is-plan-dsl)
  - [Side effects are deferred](#plan-dsl-programming-guide-2-what-kind-of-language-is-plan-dsl-side-effects-are-deferred)
  - [The function you are really writing is `infer()`](#plan-dsl-programming-guide-2-what-kind-of-language-is-plan-dsl-the-function-you-are-really-writing-is-infer)
  - [Plan-DSL describes one iteration of a frame loop](#plan-dsl-programming-guide-2-what-kind-of-language-is-plan-dsl-plan-dsl-describes-one-iteration-of-a-frame-loop)
- [3. Your first plan](#plan-dsl-programming-guide-3-your-first-plan)
- [4. The execution model: build once, run forever](#plan-dsl-programming-guide-4-the-execution-model-build-once-run-forever)
  - [Why this matters](#plan-dsl-programming-guide-4-the-execution-model-build-once-run-forever-why-this-matters)
  - [What you cannot do](#plan-dsl-programming-guide-4-the-execution-model-build-once-run-forever-what-you-cannot-do)
- [5. Edges: the only values in Plan-DSL](#plan-dsl-programming-guide-5-edges-the-only-values-in-plan-dsl)
  - [Edge-calls](#plan-dsl-programming-guide-5-edges-the-only-values-in-plan-dsl-edge-calls)
  - [Access intent enables optimization and safety](#plan-dsl-programming-guide-5-edges-the-only-values-in-plan-dsl-access-intent-enables-optimization-and-safety)
  - [Edges are typed](#plan-dsl-programming-guide-5-edges-the-only-values-in-plan-dsl-edges-are-typed)
  - [The `_()` tuple helper](#plan-dsl-programming-guide-5-edges-the-only-values-in-plan-dsl-the-_-tuple-helper)
  - [One variable, multiple views](#plan-dsl-programming-guide-5-edges-the-only-values-in-plan-dsl-one-variable-multiple-views)
  - [Example](#plan-dsl-programming-guide-5-edges-the-only-values-in-plan-dsl-example)
- [6. Operators: the ALU](#plan-dsl-programming-guide-6-operators-the-alu)
  - [Arithmetic and logic](#plan-dsl-programming-guide-6-operators-the-alu-arithmetic-and-logic)
  - [Statement form](#plan-dsl-programming-guide-6-operators-the-alu-statement-form)
- [7. Variables, locals, and shared state](#plan-dsl-programming-guide-7-variables-locals-and-shared-state)
  - [Local state is thread-local by default](#plan-dsl-programming-guide-7-variables-locals-and-shared-state-local-state-is-thread-local-by-default)
  - [Shared state must be explicit](#plan-dsl-programming-guide-7-variables-locals-and-shared-state-shared-state-must-be-explicit)
  - [`_safe`](#plan-dsl-programming-guide-7-variables-locals-and-shared-state-_safe)
  - [Global and local state tables](#plan-dsl-programming-guide-7-variables-locals-and-shared-state-global-and-local-state-tables)
- [8. Control flow: `branch`, `elseBranch`, `endBranch`](#plan-dsl-programming-guide-8-control-flow-branch-elsebranch-endbranch)
  - [Predicate forms](#plan-dsl-programming-guide-8-control-flow-branch-elsebranch-endbranch-predicate-forms)
  - [Predefined predicates](#plan-dsl-programming-guide-8-control-flow-branch-elsebranch-endbranch-predefined-predicates)
  - [Branch types](#plan-dsl-programming-guide-8-control-flow-branch-elsebranch-endbranch-branch-types)
  - [Chaining](#plan-dsl-programming-guide-8-control-flow-branch-elsebranch-endbranch-chaining)
- [9. Functions and calls with `F(...)`](#plan-dsl-programming-guide-9-functions-and-calls-with-f)
- [10. Properties: reading runtime state](#plan-dsl-programming-guide-10-properties-reading-runtime-state)
  - [Core global keys](#plan-dsl-programming-guide-10-properties-reading-runtime-state-core-global-keys)
  - [Core local keys](#plan-dsl-programming-guide-10-properties-reading-runtime-state-core-local-keys)
- [11. Events: reading input](#plan-dsl-programming-guide-11-events-reading-input)
- [12. Loops as a special case of branches](#plan-dsl-programming-guide-12-loops-as-a-special-case-of-branches)
- [13. Sub-plans: modules and reusable logic](#plan-dsl-programming-guide-13-sub-plans-modules-and-reusable-logic)
- [14. Side-effect contexts: `gl`, `nvg`, `fb`, `capture`, `write`, `imgui`](#plan-dsl-programming-guide-14-side-effect-contexts-gl-nvg-fb-capture-write-imgui)
  - [V4D context calls](#plan-dsl-programming-guide-14-side-effect-contexts-gl-nvg-fb-capture-write-imgui-v4d-context-calls)
- [15. Lifecycle: `setup`, `infer`, `teardown`, `gui`, `run`](#plan-dsl-programming-guide-15-lifecycle-setup-infer-teardown-gui-run)
  - [Worker count semantics](#plan-dsl-programming-guide-15-lifecycle-setup-infer-teardown-gui-run-worker-count-semantics)
  - [`gui()` is special](#plan-dsl-programming-guide-15-lifecycle-setup-infer-teardown-gui-run-gui-is-special)
- [16. Workers: how parallelism works](#plan-dsl-programming-guide-16-workers-how-parallelism-works)
- [17. Walkthrough: `video_editing.cpp`](#plan-dsl-programming-guide-17-walkthrough-video_editing-cpp)
- [18. Walkthrough: `beauty-demo.cpp`](#plan-dsl-programming-guide-18-walkthrough-beauty-demo-cpp)
  - [State and properties](#plan-dsl-programming-guide-18-walkthrough-beauty-demo-cpp-state-and-properties)
  - [High-level flow](#plan-dsl-programming-guide-18-walkthrough-beauty-demo-cpp-high-level-flow)
- [19. Cheat sheet](#plan-dsl-programming-guide-19-cheat-sheet)
  - [Minimal plan](#plan-dsl-programming-guide-19-cheat-sheet-minimal-plan)
  - [Constants and reads](#plan-dsl-programming-guide-19-cheat-sheet-constants-and-reads)
  - [Shared state](#plan-dsl-programming-guide-19-cheat-sheet-shared-state)
  - [Properties](#plan-dsl-programming-guide-19-cheat-sheet-properties)
  - [Events](#plan-dsl-programming-guide-19-cheat-sheet-events)
  - [Branching](#plan-dsl-programming-guide-19-cheat-sheet-branching)
  - [Function call](#plan-dsl-programming-guide-19-cheat-sheet-function-call)
  - [Assignment](#plan-dsl-programming-guide-19-cheat-sheet-assignment)
  - [Sub-plan](#plan-dsl-programming-guide-19-cheat-sheet-sub-plan)
  - [Context calls](#plan-dsl-programming-guide-19-cheat-sheet-context-calls)
- [1. Execution model](#plan-dsl-reference-isa-1-execution-model)
- [2. Edge-calls](#plan-dsl-reference-isa-2-edge-calls)
  - [2.1 `V(value)` — constant](#plan-dsl-reference-isa-2-edge-calls-2-1-v-value-constant)
  - [2.2 `R(variable)` — read](#plan-dsl-reference-isa-2-edge-calls-2-2-r-variable-read)
  - [2.3 `RW(variable)` — read-write](#plan-dsl-reference-isa-2-edge-calls-2-3-rw-variable-read-write)
  - [2.4 `RS(variable)` — read shared](#plan-dsl-reference-isa-2-edge-calls-2-4-rs-variable-read-shared)
  - [2.5 `RWS(variable)` — read-write shared](#plan-dsl-reference-isa-2-edge-calls-2-5-rws-variable-read-write-shared)
  - [2.6 `CS(variable)` — copy shared snapshot](#plan-dsl-reference-isa-2-edge-calls-2-6-cs-variable-copy-shared-snapshot)
  - [2.7 `P<T>(key)` — runtime property](#plan-dsl-reference-isa-2-edge-calls-2-7-p-t-key-runtime-property)
  - [2.8 `E<T>(...)` — event stream](#plan-dsl-reference-isa-2-edge-calls-2-8-e-t-event-stream)
  - [2.9 `F(fn, args...)` — call](#plan-dsl-reference-isa-2-edge-calls-2-9-f-fn-args-call)
  - [2.10 `_()` — operand group](#plan-dsl-reference-isa-2-edge-calls-2-10-_-operand-group)
  - [2.11 `_shared(var)` and `_safe(var)` — storage registration](#plan-dsl-reference-isa-2-edge-calls-2-11-_shared-var-and-_safe-var-storage-registration)
- [3. Operator instructions](#plan-dsl-reference-isa-3-operator-instructions)
  - [3.1 Symbol form](#plan-dsl-reference-isa-3-operator-instructions-3-1-symbol-form)
  - [3.2 Named form](#plan-dsl-reference-isa-3-operator-instructions-3-2-named-form)
  - [3.3 Generic form](#plan-dsl-reference-isa-3-operator-instructions-3-3-generic-form)
  - [3.4 Statement form](#plan-dsl-reference-isa-3-operator-instructions-3-4-statement-form)
- [3.1 Arithmetic operators](#plan-dsl-reference-isa-3-1-arithmetic-operators)
- [3.2 Logical and bitwise operators](#plan-dsl-reference-isa-3-2-logical-and-bitwise-operators)
- [3.3 Comparison operators](#plan-dsl-reference-isa-3-3-comparison-operators)
- [3.4 Selection, memory, and construction](#plan-dsl-reference-isa-3-4-selection-memory-and-construction)
- [3.5 Lowercase statement helpers](#plan-dsl-reference-isa-3-5-lowercase-statement-helpers)
- [4. Control-flow instructions](#plan-dsl-reference-isa-4-control-flow-instructions)
- [4.1 `branch(...)`](#plan-dsl-reference-isa-4-1-branch)
- [4.2 `elseBranch()`](#plan-dsl-reference-isa-4-2-elsebranch)
- [4.3 `endBranch()`](#plan-dsl-reference-isa-4-3-endbranch)
- [4.4 Branch types](#plan-dsl-reference-isa-4-4-branch-types)
- [4.5 Predefined predicates](#plan-dsl-reference-isa-4-5-predefined-predicates)
- [5. Program structure and contexts](#plan-dsl-reference-isa-5-program-structure-and-contexts)
- [5.1 Plans and sub-plans](#plan-dsl-reference-isa-5-1-plans-and-sub-plans)
- [5.2 Context calls](#plan-dsl-reference-isa-5-2-context-calls)
  - [Core context calls](#plan-dsl-reference-isa-5-2-context-calls-core-context-calls)
  - [V4D context calls](#plan-dsl-reference-isa-5-2-context-calls-v4d-context-calls)
- [5.3 Entry points](#plan-dsl-reference-isa-5-3-entry-points)
- [6. State model](#plan-dsl-reference-isa-6-state-model)
- [7. LLVM IR to Plan-DSL translation](#plan-dsl-reference-isa-7-llvm-ir-to-plan-dsl-translation)
- [7.1 Practical lowering notes](#plan-dsl-reference-isa-7-1-practical-lowering-notes)
  - [SSA temporaries](#plan-dsl-reference-isa-7-1-practical-lowering-notes-ssa-temporaries)
  - [Memory operations](#plan-dsl-reference-isa-7-1-practical-lowering-notes-memory-operations)
  - [One operation per instruction](#plan-dsl-reference-isa-7-1-practical-lowering-notes-one-operation-per-instruction)
  - [Destination-first operators](#plan-dsl-reference-isa-7-1-practical-lowering-notes-destination-first-operators)
  - [Structured control flow](#plan-dsl-reference-isa-7-1-practical-lowering-notes-structured-control-flow)
  - [`select` versus branches](#plan-dsl-reference-isa-7-1-practical-lowering-notes-select-versus-branches)
  - [Integer widths](#plan-dsl-reference-isa-7-1-practical-lowering-notes-integer-widths)
  - [Pointers](#plan-dsl-reference-isa-7-1-practical-lowering-notes-pointers)
  - [Floating point](#plan-dsl-reference-isa-7-1-practical-lowering-notes-floating-point)
- [7.2 CFG lowering strategies](#plan-dsl-reference-isa-7-2-cfg-lowering-strategies)
  - [A. Structured lowering](#plan-dsl-reference-isa-7-2-cfg-lowering-strategies-a-structured-lowering)
  - [B. PC state-machine lowering](#plan-dsl-reference-isa-7-2-cfg-lowering-strategies-b-pc-state-machine-lowering)
  - [C. Comparison](#plan-dsl-reference-isa-7-2-cfg-lowering-strategies-c-comparison)
- [8. Opcode index](#plan-dsl-reference-isa-8-opcode-index)


## Plan-DSL Programming Guide


### 1. Hello, graph


Open `modules/v4d/samples/video_editing.cpp`. The entire plan is small enough to read in one sitting:

```cpp
class VideoEditingPlan : public V4DPlan {
    cv::UMat frame_;
    const std::string hv_ = "Hello Video!";
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);

public:
    void infer() override {
        capture();

        nvg({
            using namespace cv::v4d::nvg;

            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz_.width / 2.0, sz_.height / 2.0,
                 hv_.c_str(), hv_.c_str() + hv_.size());
        }, sz_, R(hv_));

        write();
    }
};

int main(int argc, char** argv) {
    // Open source and sink...
    V4DPlan::run<VideoEditingPlan>(0);
}
```

The class `VideoEditingPlan` does not directly run a frame loop. Instead, it *describes* what should happen every frame:

1. Pull in a frame.
2. Draw text over it.
3. Push the frame out.

Then this call:

```cpp
V4DPlan::run<VideoEditingPlan>(0);
```

starts the runtime and executes that description repeatedly.

There is no explicit `while (true)`, no `update()` method, and no hand-written main loop. Plan-DSL and the Plan runtime handle that.

Every piece of user code belongs to one of the lifecycle methods of a class derived from `Plan`, or from `V4DPlan` when using the V4D windowing and rendering runtime:

```cpp
setup()
infer()
teardown()
gui()
```

Everything else is mostly library glue.

---



### 2. What kind of language is Plan-DSL?


Plan-DSL is easiest to understand by comparing what it looks like with what it actually is.

| What it looks like | What it actually is |
|---|---|
| A C++ library | An embedded C++ language that records a graph |
| A dataflow language | A task graph with explicit memory dependencies |
| An interpreter | A compiler-like recording phase followed by replay |
| An actor framework | A flat list of nodes executed in record order every frame |

This distinction matters throughout the rest of the guide.

### Side effects are deferred

This call:

```cpp
add(R(x), R(y));
```

does **not** immediately compute `x + y`.

Instead, it records an `ADD` node. Every frame, that node will read the current values of `x` and `y` and produce a result.

This is the most important mental shift in Plan-DSL.

Because execution is deferred, many errors surface during graph construction rather than during normal C++ execution. Examples include:

* wrong types,
* dangling references,
* undeclared shared variables,
* invalid operand combinations.

### The function you are really writing is `infer()`

Every DSL call inside `infer()` emits one or more nodes into a graph.

That graph is built once per worker and then replayed every frame.

The lifecycle methods have different roles:

* `setup()` — one-shot initialization.
* `infer()` — records the per-frame graph.
* `teardown()` — one-shot cleanup.
* `runGraph()` — the runtime-provided frame loop.

### Plan-DSL describes one iteration of a frame loop

You are not writing a normal sequential program. You are describing what should happen during one frame.

Every frame, the same recorded graph is executed again, potentially on new data.

If you have written GPU shaders before, think of Plan-DSL as a CPU-side shader for a per-frame computation graph:

* inputs come from `R`, `P`, and `E`,
* computation happens through operators such as `ADD`, `MUL`, `IF`, and `F`,
* outputs happen through `RW`, `assign`, `set`, and `write`.

---



### 3. Your first plan


A minimal Plan-DSL program looks like this:

```cpp
#include <opencv2/plan/plan.hpp>

using namespace cv;
using namespace cv::plan;

struct CountToTen : Plan {
    void infer() override {
        branch(P<uint64_t>(GlobalState::Keys::FRAME_CNT) < V(uint64_t(10)));
            plain({ std::cout << "tick" << std::endl; });
        endBranch();
    }
};

int main() {
    Plan::run<CountToTen>(0);
}
```

Key points:

* `Plan` is the core base class.
* The main plan logic lives inside `infer()`.
* You do not write the frame loop yourself.
* `branch(predicate)` opens a conditional region.
* `plain({ ... })` creates a node containing ordinary C++ code.
* `V(uint64_t(10))` creates a constant edge.
* `P<uint64_t>(GlobalState::Keys::FRAME_CNT)` reads the runtime frame counter.
* `Plan::run<CountToTen>(0)` starts the runtime.

If built and run, this prints one `tick` per frame for ten frames, then exits.

---



### 4. The execution model: build once, run forever


This is the most important section in the guide. If you are writing a nontrivial plan, reread this before continuing.

A plan defines four lifecycle hooks:

| Method | When it runs |
|---|---|
| `setup()` | Once per worker thread, before the frame loop |
| `infer()` | Once per worker thread, to build the per-frame graph |
| `teardown()` | Once per worker thread, after the frame loop |
| `gui()` | Once, on the main thread, before the frame loop |

The essential idea is the split between **building** the graph and **running** the graph.

```text
setup()
infer()
teardown()
```

are called once per worker during initialization.

`infer()` is where the per-frame graph is recorded.

After all workers have built their graphs, each worker enters the runtime frame loop:

```text
loop forever:
    runGraph()
```

`runGraph()` is provided by the runtime. You do not write it.

It walks the recorded node list in record order. For each node:

1. If the node is a `branch`, evaluate its predicate.
2. If the predicate is false, skip the branch body until the matching `endBranch()`.
3. If the node is enabled, invoke its callable with the current operand values.

That is the entire execution model.

There is no dependency-based scheduler, no work stealing, and no automatic vectorization. Nodes run in the order they were recorded, on the worker thread that recorded them.

### Why this matters

Because the graph is built once, you can write code that would be incorrect in ordinary C++ if interpreted as immediate execution.

For example:

```cpp
auto tmp = F(compute_something, R(input));
use(R(tmp));
use(R(tmp));
use(R(tmp));
```

The call to `F(...)` records one compute node. The later uses of `R(tmp)` refer to that node’s result.

Similarly:

```cpp
branch(R(x) == V(0));
    // ...
endBranch();
```

is valid even if `x` is uninitialized when `infer()` runs. The comparison node is re-evaluated every frame.

### What you cannot do

You cannot rely on ordinary C++ side effects inside `infer()` to persist across frames.

This is usually wrong:

```cpp
void infer() override {
    counter++; // happens during graph construction, not every frame
}
```

If you want per-frame mutation, use a node that writes through a `RW` edge or through shared state.

You also cannot make the structure of the graph depend on runtime values.

This does not do what a normal C++ `if` does:

```cpp
if (someEdge) {
    emitAdd();
} else {
    emitMul();
}
```

Both sides would be recorded at build time. For runtime selection, use `branch(...)`.

---



### 5. Edges: the only values in Plan-DSL


Plan-DSL has one fundamental value type: the **edge**.

An edge is a typed handle to either:

* a storage location, such as a plan member or shared variable, or
* a computed value produced by an operator or function call.

Edges are created by **edge-calls** and consumed by operators.

A useful mental model is:

> An edge is like a typed SSA value with an explicit memory-access intent.

The intent tells the runtime how the node should treat the storage behind the edge.

### Edge-calls

| Edge-call | Meaning | Access intent |
|---|---|---|
| `V(x)` | Immediate constant | None |
| `R(x)` | Current value of `x` | Read-only |
| `RW(x)` | Storage of `x` | Read-write |
| `RS(x)` | Shared variable `x` | Read under lock |
| `RWS(x)` | Shared variable `x` | Read-write under lock |
| `CS(x)` | Snapshot of shared variable `x` | Read and copy under lock |
| `P(key)` | Runtime property | Shared read-only |
| `E<T>()` | Input event stream | Shared, polled each frame |

Here, `x` is any C++ lvalue that the plan owns or has registered.

### Access intent enables optimization and safety

The intent is not just notation. It lets the runtime reason about dependencies.

If a node uses only `R(x)`, the runtime knows it only reads `x`.

If a node uses `RW(x)`, the runtime knows it defines or modifies `x`.

If a node uses `RS(x)` or `RWS(x)`, the runtime knows the access must be synchronized.

### Edges are typed

Plan-DSL inherits C++ types.

For example:

```cpp
R(intVar)
```

is an edge of type `int`.

```cpp
R(frame_)
```

may be an edge of type `cv::UMat`.

Operator overloading then works naturally:

```cpp
R(intVar) + V(1)
```

records an `ADD` node that returns a new integer edge.

### The `_()` tuple helper

C++ does not support variadic operator overloading, so Plan-DSL provides `_()` as a tuple helper for n-ary operations:

```cpp
auto sum = R(a) + _(R(b), R(c));
```

This means roughly:

```cpp
a + (b + c)
```

or, depending on lowering, one n-ary `ADD` node with three operands.

Use `_()` whenever you need to pass multiple edges to a variadic operator.

### One variable, multiple views

A single C++ variable can be accessed through multiple edge types:

```cpp
int x = 0;

auto readEdge  = R(x);
auto writeEdge = RW(x);
auto constEdge = V(7);
```

* `R(x)` reads the current value.
* `RW(x)` designates `x` as a destination.
* `V(7)` is just the constant `7` and does not refer to `x`.

`R(x)` and `RW(x)` may refer to the same storage, but they play different roles in operators.

### Example

```cpp
int threshold_ = 128;

void infer() override {
    auto src = R(frame_);
    auto bright = F(&cv::mean, src) > V(threshold_);

    branch(bright);
        plain({
            // process bright frame
        }, R(frame_));
    endBranch();
}
```

What happens here?

1. `R(frame_)` creates a read-only edge to `frame_`.
2. `F(&cv::mean, src)` records a call node that computes the mean.
3. `> V(threshold_)` records a comparison node.
4. `branch(bright)` opens a conditional region that runs only when the comparison is true.

Nothing here executes immediately as a normal C++ computation. It is all recorded.

---



### 6. Operators: the ALU


Operators are the named instructions of Plan-DSL.

They are the arithmetic, logical, comparison, memory, and construction operations of the graph.

There are four ways to express an operator:

| Form | Example | Returns result edge? |
|---|---|---|
| Symbol form | `a + b` | Yes |
| Named form | `ADD(a, b)` | Yes |
| Generic form | `OP<Operators::ADD_>(a, b)` | Yes |
| Statement form | `assign(a, b)` | No |

Symbol form is usually the most readable.

Named and generic forms are useful for tools and code generation.

Statement forms are useful when the operation only writes to storage and does not need to return a result edge.

### Arithmetic and logic

Plan-DSL supports the normal C++ operator set:

#### Arithmetic

```cpp
+
-
*
/
%
-x
++x
x++
--x
x--
```

#### Logical

```cpp
&&
||
!
```

#### Bitwise

```cpp
&
|
^
<<
>>
```

#### Comparison

```cpp
==
!=
<
>
<=
>=
```

#### Ternary selection

```cpp
IF(cond, a, b)
```

#### Memory operations

```cpp
container[i]   // IDX
*ptr           // DEREF
dst = src      // ASSIGN
```

#### Construction

```cpp
T(args...)     // CONSTRUCT
```

### Statement form

If you only care about writing a value back to storage, use a statement form:

```cpp
assign(RW(x), R(y));
```

Equivalent in intent to:

```cpp
x = y;
```

You can also write:

```cpp
RW(x) = R(y);
```

Statement forms return `cv::Ptr<Plan>`, so they can be chained.

---



### 7. Variables, locals, and shared state


A plan’s C++ member variables act as its local storage.

They are similar to `alloca` slots in a traditional compiler.

Example:

```cpp
struct DemoPlan : Plan {
    int counter_ = 0;
    cv::UMat frame_;

    void infer() override {
        auto c = RW(counter_);
        c = c + V(1);
    }
};
```

### Local state is thread-local by default

By default, plan members are local to each worker thread.

Each worker builds its own graph and has its own copy of the plan’s non-shared members.

This is useful for:

* scratch buffers,
* per-worker counters,
* temporary images,
* thread-local state.

### Shared state must be explicit

If multiple workers, or the GUI thread and a worker, must share a variable, declare it shared:

```cpp
_shared(params_);
```

Shared variables are protected by a mutex.

Access them using shared edge-calls:

```cpp
RS(params_)
RWS(params_)
CS(params_)
```

where:

* `RS(x)` reads shared state under lock.
* `RWS(x)` reads and writes shared state under lock.
* `CS(x)` takes a thread-safe snapshot of shared state.

`CS` is especially useful for handoffs between threads, such as GUI-written parameters consumed by worker threads.

### `_safe`

`_safe(x)` is the opposite escape hatch:

```cpp
_safe(someVariable);
```

It tells the runtime that this variable will never be accessed as shared state.

### Global and local state tables

Plan provides two type-keyed state tables:

* `GlobalState`
* `LocalState`

`GlobalState` is shared across all workers and the GUI thread.

`LocalState` stores one value per key per thread.

Read values using `P<T>(key)`.

Write values using the appropriate state API, such as:

```cpp
GlobalState::set<T>(key, value);
```

The Plan core defines several keys, and runtimes such as V4D add more.

---



### 8. Control flow: `branch`, `elseBranch`, `endBranch`


Plan-DSL has no `goto`, `break`, or `continue`.

Control flow is structured and expressed using branch regions.

Basic form:

```cpp
branch(predicate);
    // statements
elseBranch();
    // statements
endBranch();
```

A branch region is itself part of the graph.

Every frame, when the runtime reaches a `branch`, it evaluates the predicate.

* If true, the branch body runs.
* If false, the branch body is skipped.
* If `elseBranch()` is present, the else body runs when the original predicate is false.

### Predicate forms

The predicate may be:

#### A boolean edge

```cpp
branch(R(x) == V(0));
```

#### A callable returning bool

```cpp
branch([] { return true; });
```

#### A callable taking operand edges

```cpp
branch([](auto a) { return a > 0; }, R(n));
```

### Predefined predicates

Plan provides several helper predicates:

```cpp
always_
isTrue_(bool)
isFalse_(bool)
and_(bool, bool)
or_(bool, bool)
```

### Branch types

Branches may carry a `BranchType::Enum`:

| Value | Name | Meaning |
|---|---|---|
| `0` | `NONE` | No branch; plain node behavior |
| `1` | `SINGLE` | At most one worker executes the region |
| `2` | `PARALLEL` | Every worker executes the region if predicate is true |
| `4` | `ONCE` | Executes exactly once globally |
| `8` | `PARALLEL_ONCE` | Executes exactly once per worker |

Example:

```cpp
branch(BranchType::SINGLE, predicate);
    // serialized work
endBranch();
```

`SINGLE` and `ONCE` acquire global locking.

`ONCE` is sticky: once it has run, it will not run again.

### Chaining

Many Plan-DSL calls return `cv::Ptr<Plan>`, so you can chain them:

```cpp
branch(cond)
    ->plain(workA)
    ->branch(subCond)
        ->plain(workB)
    ->endBranch()
->elseBranch()
    ->plain(workC)
->endBranch();
```

This is equivalent to the nested block form. Use whichever is clearer.

---



### 9. Functions and calls with `F(...)`


When you need an operation that does not have a dedicated operator, use `F(...)`.

`F` accepts any callable:

* free function,
* member function pointer,
* lambda,
* function object.

Examples:

```cpp
auto t = F(&cv::getTickCount);
auto w = F(&cv::Size::width, R(sz));
F(&cv::split, R(src), RW(dst));
```

If the callable returns a non-`void` value, `F` returns a fresh result edge.

If the callable returns `void`, `F` acts as a statement.

`F` is also the main way to integrate external C++ libraries into the graph.

There is no separate Plan-DSL function-definition syntax. To reuse logic, write a normal C++ function or lambda and call it through `F`.

The graph records each call site as a node. Identical call sites with identical operands may be deduplicated.

---



### 10. Properties: reading runtime state


A **property** is an edge bound to a value in `GlobalState` or `LocalState`.

Examples:

```cpp
Property<cv::Size>  size_  = P<cv::Size>(V4D::Keys::SIZE);
Property<uint64_t>  frame_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
Property<size_t>    widx_  = P<size_t>(LocalState::Keys::WORKER_INDEX);
```

Properties behave like edges, so you can pass them directly to operators without wrapping them in `R()`.

Example:

```cpp
branch(seqCnt_ % V(uint64_t(8)) == V(uint64_t(0)));
```

This runs the branch body every eighth frame.

### Core global keys

Examples include:

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

### Core local keys

```cpp
WORKER_INDEX
```

Runtimes can add their own keys.

V4D adds keys such as:

```cpp
V4D::Keys::SIZE
V4D::Keys::VIEWPORT
V4D::Keys::NAMESPACE
V4D::Keys::FULLSCREEN
V4D::Keys::DISABLE_INPUT_EVENTS
```

---



### 11. Events: reading input


An **event edge** produces a vector of input events for the current frame.

Example:

```cpp
Event<Mouse> pressEvents_ = E<Mouse>(Mouse::Type::PRESS);
auto anyPress = !F(&Mouse::List::empty, pressEvents_);
```

The DSL core itself produces empty event lists. A runtime such as V4D connects the event edge to a real input source.

Available event classes include:

```cpp
Mouse
Keyboard
Window
Joystick
```

Each event class has:

* a nested `Type` enum,
* a `List` container.

Usage forms:

```cpp
E<T>()
E<T>(type)
E<T>(type, trigger)
```

where:

* `E<T>()` means all events of class `T`.
* `E<T>(type)` means events of a specific type.
* `E<T>(type, trigger)` adds a trigger predicate.

---



### 12. Loops as a special case of branches


Plan-DSL has no `for` or `while` keywords.

A loop is expressed as a branch whose predicate is updated by the loop body.

Example:

```cpp
struct CountdownPlan : Plan {
    int counter_ = 10;

    void infer() override {
        branch(R(counter_) > V(0));
            plain({ std::cout << "tick" << std::endl; }, R(counter_));
            assign(RW(counter_), R(counter_) - V(1));
        endBranch();
    }
};
```

Execution proceeds as follows:

1. First frame: `counter_ == 10`, predicate true, body runs, `counter_` becomes `9`.
2. Next frame: `counter_ == 9`, predicate true, body runs, `counter_` becomes `8`.
3. This continues until `counter_ == 0`.
4. Predicate becomes false, and the branch body is skipped.

This gives `while`-like behavior in a frame-based execution model.

The graph is not rebuilt. Only the predicate value changes from frame to frame.

---



### 13. Sub-plans: modules and reusable logic


A sub-plan is a `Plan` object owned by another `Plan`.

Create it in the parent constructor:

```cpp
struct ParentPlan : Plan {
    cv::Ptr<SubPlan> sub_;

    ParentPlan() {
        sub_ = _sub<SubPlan>(this);
    }

    void infer() override {
        subInfer(sub_);
    }
};
```

Important rule:

> `_sub` must be called only during parent construction.

Do not call `_sub` from `infer()`, `setup()`, or `teardown()`.

After construction, splice the sub-plan into the parent using:

```cpp
subInfer(sub);
subSetup(sub);
subTeardown(sub);
```

Most often, you call `subInfer(sub)` from the parent’s `infer()`.

Sub-plans are useful for:

* reusable logic,
* modularizing large plans,
* encapsulating filters or processing stages,
* calling reusable graphs from inside branch regions.

Because a sub-plan graph is spliced at the call site, it inherits the enclosing branch predicate.

---



### 14. Side-effect contexts: `gl`, `nvg`, `fb`, `capture`, `write`, `imgui`


A **context call** attaches a node to a specialized execution environment.

The core DSL defines the plain CPU context:

```cpp
plain(fn, args...)
F(fn, args...)
```

Runtimes such as V4D add additional contexts.

### V4D context calls

| Call | Context | Purpose |
|---|---|---|
| `gl(fn, args...)` | OpenGL | Execute GL commands |
| `fb<pos>(fn, args...)` | Framebuffer | Access framebuffer |
| `nvg(fn, args...)` | NanoVG | Vector graphics |
| `bgfx(fn, args...)` | bgfx | bgfx rendering |
| `ext(fn, args...)` | External | External renderer contexts |
| `capture(...)` | Source | Pull next input frame |
| `write(...)` | Sink | Push output frame |
| `imgui(...)` | ImGui | Install UI node |
| `set(key, edge)` | CPU | Property write node |

A typical frame body looks like this:

```cpp
void infer() override {
    capture(RW(frames_.orig_));

    plain(prepareFrames, R(downSize_), RW(frames_));

    branch(enabled());
        plain(processFrames, RW(frames_));
    ->elseBranch()
        plain(bypassProcessing, RW(frames_));
    ->endBranch();

    fb<1>(cv::cvtColor,
          R(frames_.result_),
          V(cv::COLOR_BGR2RGBA),
          V(0),
          V(cv::ALGO_HINT_DEFAULT));

    write(R(frames_.result_));
}
```

Each C++ function is wrapped into a node that the runtime dispatches to the correct context.

---



### 15. Lifecycle: `setup`, `infer`, `teardown`, `gui`, `run`


A program starts with:

```cpp
Plan::run<Tplan>(workers, args...);
```

or, for V4D:

```cpp
V4DPlan::run<Tplan>(workers, args...);
```

The arguments are forwarded to the plan constructor.

The runtime then performs roughly these steps:

1. Spawn worker threads.
2. Call `gui()` once on the main thread.
3. For each worker:
   * call `setup()`,
   * build and run the setup graph,
   * clear it.
4. For each worker:
   * call `infer()`,
   * build the per-frame graph.
5. Synchronize all workers and the main thread.
6. Enter the frame loop.
7. Every frame, each worker calls `runGraph()`.
8. When the runtime shuts down, each worker calls `teardown()`.

### Worker count semantics

The meaning of the `workers` argument is:

| `workers` value | Meaning |
|---|---|
| `-1` | Default worker count |
| `0` | One worker plus main thread |
| `>= 1` | N workers plus main thread |

For V4D, the main thread usually handles the display and event loop.

### `gui()` is special

`gui()` runs once on the main thread, outside the normal graph.

Use it for:

* installing ImGui nodes,
* one-shot main-thread UI setup,
* creating UI state.

Keep `gui()` simple.

Do not store state in plan members that are also written by worker graphs unless that state is properly shared.

---



### 16. Workers: how parallelism works


Each worker thread builds and runs its own copy of the graph.

Workers do not share nodes.

They do not migrate work.

They do not perform node-level scheduling.

There is no work stealing.

Workers do share:

* `GlobalState`,
* registered shared variables,
* the runtime object.

When a branch is marked `SINGLE`, the runtime serializes execution of that branch body across workers.

When a branch is marked `PARALLEL`, every worker evaluates and potentially executes the branch body concurrently.

If multiple workers write to the same state in a `PARALLEL` branch, that state must be shared and accessed through the appropriate shared edge-calls.

A good mental model is:

> Think of `infer()` as the body of an OpenMP parallel region.

Whatever you would safely do in a `#pragma omp parallel` region, do here.

---



### 17. Walkthrough: `video_editing.cpp`


```cpp
class VideoEditingPlan : public V4DPlan {
    cv::UMat frame_;
    const std::string hv_ = "Hello Video!";
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);

public:
    void infer() override {
        capture();

        nvg({
            using namespace cv::v4d::nvg;

            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz_.width / 2.0, sz_.height / 2.0,
                 hv_.c_str(), hv_.c_str() + hv_.size());
        }, sz_, R(hv_));

        write();
    }
};
```

The plan does three things every frame:

1. `capture()` pulls a frame from the source.
2. `nvg(...)` draws text over the frame.
3. `write()` pushes the frame to the sink.

The `main()` function initializes the V4D runtime, attaches a source and sink, and runs the plan:

```cpp
int main(int argc, char** argv) {
    cv::Rect viewport(0, 0, 960, 960);

    cv::Ptr<V4D> runtime = V4D::init(
        viewport,
        "Video Editing",
        AllocateFlags::NANOVG | AllocateFlags::IMGUI
    );

    auto src = Source::make(runtime, argv[1]);
    auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());

    runtime->setSource(src);
    runtime->setSink(sink);

    V4DPlan::run<VideoEditingPlan>(0);
}
```

Because `0` is passed to `run`, the runtime uses one worker plus the main thread.

For V4D, the main thread handles display and events, while the worker executes the plan graph.

---



### 18. Walkthrough: `beauty-demo.cpp`


`beauty-demo.cpp` is one of the most complete examples in the project.

It demonstrates:

* shared state,
* sub-plans,
* branching,
* mouse events,
* frame counters,
* NanoVG drawing,
* framebuffer access,
* ImGui-based GUI.

### State and properties

A simplified view of the plan state:

```cpp
struct BeautyDemoPlan : public V4DPlan {
    struct Params {
        float eyesAndLipsSaturation_ = 1.25f;
        float skinSaturation_ = 1.35f;
        float skinContrast_ = 0.75f;
        bool sideBySide_ = false;
        bool stretch_ = true;
        bool fullscreen_ = false;
        bool enabled_ = true;

        enum State {
            ON,
            OFF,
            NOT_DETECTED
        } state_ = ON;
    };

    struct Frames {
        cv::UMat orig_;
        cv::UMat stitched_;
        cv::UMat down_;
        cv::UMat bgr_;
        cv::UMat faceOval_;
        cv::UMat eyesAndLips_;
        cv::UMat skin_;
        cv::UMat faceSkinMaskGrey_;
        cv::UMat eyesAndLipsMaskGrey_;
        cv::UMat backgroundMaskGrey_;
        cv::UMat result_;
    };

private:
    static Params params_;
    static FaceFeatures features_;

    cv::Ptr<FaceFeatureExtractor> extractor_;

    float scale_ = 1;
    const cv::Size downSize_ = {640, 360};

    Frames frames_;

    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
    Property<uint64_t> seqCnt_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
    Event<Mouse> pressEvents_ = E<Mouse>(Mouse::Type::PRESS);
};
```

Important points:

* `params_` and `features_` are static, so they are shared across threads.
* `frames_` is a non-static member, so it is per-worker.
* `size_` and `seqCnt_` are property edges.
* `pressEvents_` is an event edge.

Because `params_` and `features_` are shared, worker threads and the GUI thread can safely interact with them through shared edge-calls.

### High-level flow

The demo generally does the following each frame:

1. Capture input.
2. Downscale or prepare frames.
3. Detect facial features.
4. Build masks.
5. Apply cosmetic filters.
6. Composite the result.
7. Write to framebuffer and/or sink.
8. Process GUI interaction.

The important lesson is that all of this is expressed as a graph, not as a normal immediate-mode C++ frame loop.

---



### 19. Cheat sheet



### Minimal plan

```cpp
struct MyPlan : Plan {
    void infer() override {
        plain({ std::cout << "frame" << std::endl; });
    }
};

int main() {
    Plan::run<MyPlan>(0);
}
```

### Constants and reads

```cpp
V(42)
R(x)
RW(x)
```

### Shared state

```cpp
_shared(params_);

RS(params_)
RWS(params_)
CS(params_)
```

### Properties

```cpp
Property<uint64_t> frameCnt_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
```

### Events

```cpp
Event<Mouse> mousePress_ = E<Mouse>(Mouse::Type::PRESS);
```

### Branching

```cpp
branch(R(x) > V(0));
    plain(doWork);
elseBranch();
    plain(doOtherWork);
endBranch();
```

### Function call

```cpp
auto result = F(&someFunction, R(input));
```

### Assignment

```cpp
assign(RW(x), R(y));
RW(x) = R(y);
```

### Sub-plan

```cpp
sub_ = _sub<SubPlan>(this);

void infer() override {
    subInfer(sub_);
}
```

### Context calls

```cpp
capture();
write();

nvg({ /* draw */ }, size_, R(text_));

gl({ /* OpenGL commands */ });

fb<1>(cv::cvtColor, R(src_), V(cv::COLOR_BGR2RGBA));
```



---


## Plan-DSL Reference (ISA)


### 1. Execution model


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



### 2. Edge-calls


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



### 3. Operator instructions


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



### 3.1 Arithmetic operators


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



### 3.2 Logical and bitwise operators


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



### 3.3 Comparison operators


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



### 3.4 Selection, memory, and construction


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



### 3.5 Lowercase statement helpers


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



### 4. Control-flow instructions


Control flow is structured.

There are no arbitrary jumps.

Control flow is expressed with branch regions.

---



### 4.1 `branch(...)`


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



### 4.2 `elseBranch()`


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



### 4.3 `endBranch()`


```cpp
endBranch();
```

Closes the current branch region.

Every `branch` must be matched by exactly one `endBranch`.

Branching is determined by source nesting.

---



### 4.4 Branch types


| Value | Name | Semantics |
|---|---|---|
| `0` | `NONE` | Plain node behavior |
| `1` | `SINGLE` | At most one worker executes |
| `2` | `PARALLEL` | Every worker executes if predicate is true |
| `4` | `ONCE` | Executes exactly once globally |
| `8` | `PARALLEL_ONCE` | Executes exactly once per worker |

---



### 4.5 Predefined predicates


```cpp
always_
isTrue_(bool)
isFalse_(bool)
and_(bool, bool)
or_(bool, bool)
```

These are convenience predicates exposed by the DSL.

---



### 5. Program structure and contexts





### 5.1 Plans and sub-plans


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



### 5.2 Context calls


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



### 5.3 Entry points


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



### 6. State model


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



### 7. LLVM IR to Plan-DSL translation


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



### 7.1 Practical lowering notes



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



### 7.2 CFG lowering strategies


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



### 8. Opcode index


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

# Plan-V4D Documentation

# Table of Contents

- [Three things to know](#readme-three-things-to-know)
- [How it compares](#readme-how-it-compares)
- [Hello, graph](#readme-hello-graph)
- [The two modules](#readme-the-two-modules)
- [Plan-DSL — the language](#readme-plan-dsl-the-language)
- [V4D — the runtime](#readme-v4d-the-runtime)
- [Samples](#readme-samples)
- [Requirements](#readme-requirements)
- [Building](#readme-building)
  - [macOS](#readme-building-macos)
  - [Third-party code](#readme-building-third-party-code)
- [Documentation](#readme-documentation)
- [Packaging](#readme-packaging)
- [Installing the packages](#readme-installing-the-packages)
  - [From the OBS repository](#readme-installing-the-packages-from-the-obs-repository)
- [License](#readme-license)
- [Attribution](#readme-attribution)
- [1. What is V4D?](#v4d-application-programming-guide-1-what-is-v4d)
- [2. The One Mental Model: Record Once, Replay Forever](#v4d-application-programming-guide-2-the-one-mental-model-record-once-replay-forever)
- [3. Milestone 1 — Hello, NanoVG](#v4d-application-programming-guide-3-milestone-1-hello-nanovg)
- [4. Milestone 2 — The Shape of a V4D Application](#v4d-application-programming-guide-4-milestone-2-the-shape-of-a-v4d-application)
- [5. Milestone 3 — Plan-DSL Crash Course](#v4d-application-programming-guide-5-milestone-3-plan-dsl-crash-course)
  - [5.1 Edges — the only values in the language](#v4d-application-programming-guide-5-milestone-3-plan-dsl-crash-course-5-1-edges-the-only-values-in-the-language)
  - [5.2 Operators — the ALU](#v4d-application-programming-guide-5-milestone-3-plan-dsl-crash-course-5-2-operators-the-alu)
  - [5.3 Control flow — branch regions, not jumps](#v4d-application-programming-guide-5-milestone-3-plan-dsl-crash-course-5-3-control-flow-branch-regions-not-jumps)
  - [5.4 Loops — branches whose predicate the body updates](#v4d-application-programming-guide-5-milestone-3-plan-dsl-crash-course-5-4-loops-branches-whose-predicate-the-body-updates)
  - [5.5 Variables and shared state](#v4d-application-programming-guide-5-milestone-3-plan-dsl-crash-course-5-5-variables-and-shared-state)
- [6. Milestone 4 — Video In / Video Out](#v4d-application-programming-guide-6-milestone-4-video-in-video-out)
  - [6.1 Sources and Sinks](#v4d-application-programming-guide-6-milestone-4-video-in-video-out-6-1-sources-and-sinks)
  - [6.2 `capture(...)` and `write(...)`](#v4d-application-programming-guide-6-milestone-4-video-in-video-out-6-2-capture-and-write)
  - [6.3 Exercise program: video → grayscale → video](#v4d-application-programming-guide-6-milestone-4-video-in-video-out-6-3-exercise-program-video-grayscale-video)
- [7. Milestone 5 — Drawing on Top of Video](#v4d-application-programming-guide-7-milestone-5-drawing-on-top-of-video)
- [8. Milestone 6 — Pixel Access with `fb(...)`](#v4d-application-programming-guide-8-milestone-6-pixel-access-with-fb)
- [9. Milestone 7 — GUIs with ImGui](#v4d-application-programming-guide-9-milestone-7-guis-with-imgui)
- [10. Milestone 8 — Events and Properties](#v4d-application-programming-guide-10-milestone-8-events-and-properties)
  - [10.1 Events](#v4d-application-programming-guide-10-milestone-8-events-and-properties-10-1-events)
  - [10.2 The toggle idiom from `beauty-demo.cpp`](#v4d-application-programming-guide-10-milestone-8-events-and-properties-10-2-the-toggle-idiom-from-beauty-demo-cpp)
  - [10.3 Properties: reading and writing runtime state](#v4d-application-programming-guide-10-milestone-8-events-and-properties-10-3-properties-reading-and-writing-runtime-state)
- [11. Milestone 9 — Raw OpenGL (Optional)](#v4d-application-programming-guide-11-milestone-9-raw-opengl-optional)
- [12. Milestone 10 — Going Big: Sub-plans, Threads, Shared State](#v4d-application-programming-guide-12-milestone-10-going-big-sub-plans-threads-shared-state)
  - [12.1 Sub-plans — composing large programs](#v4d-application-programming-guide-12-milestone-10-going-big-sub-plans-threads-shared-state-12-1-sub-plans-composing-large-programs)
  - [12.2 Threads and workers](#v4d-application-programming-guide-12-milestone-10-going-big-sub-plans-threads-shared-state-12-2-threads-and-workers)
- [13. Capstone Project — Chroma, a Complete Video Filter App](#v4d-application-programming-guide-13-capstone-project-chroma-a-complete-video-filter-app)
- [14. Debugging and Common Pitfalls](#v4d-application-programming-guide-14-debugging-and-common-pitfalls)
- [15. Where to Go Next](#v4d-application-programming-guide-15-where-to-go-next)
- [16. Appendix — Cheat Sheet](#v4d-application-programming-guide-16-appendix-cheat-sheet)


## Readme


### Three things to know


* **No frame loop.** Describe one iteration of the loop; the runtime records it
  once and replays it every frame. "Record once, replay forever."
* **Window, GPU and GUI included.** GLFW + OpenGL, NanoVG 2D vector graphics,
  ImGui immediate-mode UI, and bgfx — no boilerplate, no `while (true)`.
* **A normal OpenCV extra module.** Drop it into `OPENCV_EXTRA_MODULES_PATH`,
  build with C++20, and use it like any other contrib module.



### How it compares


Plan-DSL sits alongside a well-known family of graph-based media/vision
frameworks. The difference is *when* the graph is decided.

| Framework | Graph topology | Checking | Notes |
|---|---|---|---|
| GStreamer | built at runtime, plugins linked by name | runtime caps negotiation | playback/streaming graphs, `gst-launch` pipelines |
| NVIDIA DeepStream | built on GStreamer | runtime | inference-oriented plugin pipelines |
| OpenCV G-API | DAG built as C++ macros | runtime for untyped, typed wrapper partially checked | same project family; pipelines as expressions, backends |
| Halide | DSL-specific pipeline | compile-time | stencil/image-processing algorithm+schedule eDSL |
| DSPatch | runtime-wired objects | runtime, untyped | generic C++ dataflow/patching framework |
| TBB flow graph | runtime-wired nodes | runtime | dataflow graphs from function nodes |
| **Plan-DSL** | **recorded once, in C++** | **compile-time, type-safe** | task graphs with control flow, threading, GPU contexts |

Plan-DSL goes beyond a pure DAG:

* **Control flow is first-class.** `branch(pred)` / `elseBranch()` /
  `endBranch()` regions with parallel, single-time, and once-only semantics —
  not just a static DAG.
* **Shared memory is explicit and safe.** Edges declare access intent
  (`R`/`RW`/`RS`/`RWS`/`CS`), and `_shared(member)` layers a mutex on top.
* **The compiler is the checker.** Wrong operand types, dangling references,
  and invalid side-effect contexts fail to compile or fail at graph-build time —
  not ten minutes into a long encode.
* **Workers are per-thread graph copies.** Each worker records and replays its
  own independent copy of the graph, with deterministic in-order execution.



### Hello, graph


```cpp
#include <opencv2/v4d/v4d.hpp>
using namespace cv;
using namespace cv::v4d;

class FontRenderingPlan : public V4DPlan {
    string text_ = "Hello World";
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        nvg([](const Size& sz, const string& str) {
            using namespace cv::v4d::nvg;
            clearScreen();
            fontSize(40.0f);
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0, str.c_str(), str.c_str() + str.size());
        }, size_, R(text_));
    }
};

int main() {
    cv::Ptr<V4D> runtime = V4D::init(cv::Rect(0, 0, 960, 960), "Font Rendering",
                                     AllocateFlags::NANOVG);
    V4DPlan::run<FontRenderingPlan>(0);
}
```

No event loop, no GL calls, no cleanup code. `infer()` *records* the per-frame
graph; `V4DPlan::run<...>` boots the workers, drives the frame loop, and joins
when the window closes.



### The two modules


| Module | Description | Docs |
|---|---|---|
| `plan` | The type-safe dataflow eDSL: edges, operators, control flow, sub-plans, shared state. | [plan README](modules/plan/README.md) |
| `v4d` | The graphics runtime: window + GPU contexts, NanoVG/ImGui layers, Sources & Sinks. | [v4d README](modules/v4d/README.md) |

---



### Plan-DSL — the language


Four lifecycle methods on a class derived from `Plan`:

| Method       | When it runs                                        |
|--------------|-----------------------------------------------------|
| `setup()`    | once per worker thread, before the frame loop       |
| `infer()`    | once per worker thread — records the per-frame graph |
| `teardown()` | once per worker thread, after the frame loop        |
| `gui()`      | once, on the main thread, before the frame loop     |

Building blocks:

* **Edges** — the only value type. `V(x)`, `R(x)`, `RW(x)`, `RS(x)`, `RWS(x)`,
  `CS(x)`, `P<T>(key)`, `E<T>()` describe how a node accesses storage or runtime
  state.
* **Operators** — C++ operators that record nodes: `ADD`, `MUL`, `IF`, `IDX`,
  `DEREF`, … in symbol, named, or generic form.
* **Functions** — `F(callable, args...)` wraps any C++ callable as a node.
* **Control flow** — `branch(pred)` / `elseBranch()` / `endBranch()` regions
  with parallel, single, and once-only semantics.
* **Sub-plans** — compose large programs with `_sub<T>(...)` and `subInfer()`.
* **Shared state, properties, events** — mutex-protected members, typed runtime
  property edges, and input event streams.

Because execution is deferred, type errors, dangling references, and invalid
operand combinations surface at graph-build time — not hours into a recording
process.



### V4D — the runtime


A `V4DPlan` subclass gets a window, an event loop, and five side-effect contexts
on top of the DSL's `plain(...)`:

| Call               | Context        | Purpose                               |
|--------------------|----------------|---------------------------------------|
| `gl(fn, args...)`  | OpenGL         | Raw GL commands                       |
| `fb<pos>(fn, args...)` | Framebuffer | Direct framebuffer access             |
| `nvg(fn, args...)` | NanoVG         | Vector graphics on top of GL          |
| `bgfx(fn, args...)`| bgfx           | bgfx rendering (alternative to GL)    |
| `ext(fn, args...)` | External       | External renderer contexts            |
| `capture()` / `write()` | Source / Sink | Pull the next input frame / push the finished frame |
| `imgui(fn, args...)` | ImGui         | UI nodes from `gui()`                 |

Sources and sinks read from video files, webcams, or arbitrary functors, and
write to files or anything else:

```cpp
auto src  = Source::make(rt, "in.mp4");
auto sink = Sink::make(rt, "out.mkv", src->fps(), viewport.size());
rt->setSource(src);
rt->setSink(sink);
```



### Samples


More than two dozen small programs in [modules/v4d/samples/](modules/v4d/samples/):

| Start here | What it shows |
|---|---|
| `video_editing.cpp` | capture → nvg → write, the canonical pipeline |
| `beauty-demo.cpp` | the kitchen sink: shared state, sub-plans, `IF`, events, NanoVG, ImGui |
| `font_rendering.cpp` | the smallest visible program (32 lines) |
| `imshow_reimplementation.cpp` | a full GUI image viewer |

Plus: raw OpenGL (`render_opengl`, `cube-demo`, `shader-demo`), vector graphics
(`nanovg-demo`, `font-demo`), video processing (`optflow-demo`,
`pedestrian-demo`), multi-window (`montage-demo`, `many_cubes-demo`), custom
I/O (`custom_source_and_sink`), and more.



### Requirements


* C++20 (`<barrier>` and `<semaphore>`)
* OpenCV 4.x (core + imgproc; V4D samples additionally use videoio, video,
  imgcodecs, dnn, face, objdetect, tracking, optflow, plot, features2d, flann)
* GLFW 3 (V4D only)
* An OpenGL-capable driver (or OpenGL ES 3.0)



### Building


Both modules build as standard OpenCV extra modules:

```bash
mkdir build && cd build
cmake -DOPENCV_EXTRA_MODULES_PATH=../modules \
      -DBUILD_opencv_plan=ON \
      -DBUILD_opencv_v4d=ON \
      -DBUILD_EXAMPLES=ON \
      ../..
cmake --build . --target example_v4d_video_editing
./bin/example_v4d_video_editing in.mp4 out.mkv
```

| CMake option                    | Effect                                       |
|---------------------------------|----------------------------------------------|
| `OPENCV_V4D_ENABLE_ES3`         | Build against OpenGL ES 3.0 instead of desktop GL. |
| `OPENCV_V4D_ENABLE_BGFX`        | Build the bgfx context and link bgfx.        |
| `OPENCV_V4D_ENABLE_MALI`        | Mali GPU support (requires libmali).         |
| `BUILD_EXAMPLES`                | Build the programs in `modules/v4d/samples/`. |

Run the Plan-DSL test suite with:

```bash
cmake -DOPENCV_BUILD_TEST_MODULES_LIST=plan ...
cmake --build . --target opencv_test_plan
./bin/opencv_test_plan
```

### macOS

* Requires macOS 13+, Xcode 14+ (Apple Clang 14+ / libc++ 14+), and GLFW via
  Homebrew: `brew install glfw`.
* Leave `OPENCV_V4D_ENABLE_ES3=OFF` — the ES3 path uses EGL, which is not
  available on macOS. V4D automatically uses a desktop GL 3.2 core profile with
  forward compatibility and loads system GL function pointers.
* Verified continuously in CI by `macOS-ARM64-v4d` and `macOS-X64-v4d`
  GitHub Actions jobs.

### Third-party code

V4D vendors NanoVG, ImGui, GLAD and friends under
[modules/v4d/third/](modules/v4d/third/); may require
`git submodule update --init --recursive`. Assets such as the YuNet face
detector and the LBF landmark model ship in
[modules/v4d/assets/](modules/v4d/assets/).



### Documentation


* [Plan-DSL Programming Guide](modules/plan/doc/plan-dsl-programming-guide.markdown) —
  a friendly tour through the language.
* [Plan-DSL Reference](modules/plan/doc/plan-dsl-reference.markdown) —
  the canonical edge-by-edge, operator-by-operator reference.
* [V4D Application Programming Tutorial](modules/v4d/doc/v4d-application-programming-guide.markdown) —
  the V4D tutorial, milestone by milestone.
* [Sample walkthroughs](modules/v4d/doc/samples/) — annotated `00-intro` through `18-many-cubes`.



### Packaging


The project ships Debian packaging (`plan-v4d.dsc` + `debian/`) and an OBS recipe
(`obs/plan-v4d.spec`).



### Installing the packages


The OBS recipes under [`obs/`](obs/) build binary packages for four targets. The
same runtime is shipped everywhere; only the package names differ:

| Target | Format | Packages |
|---|---|---|
| openSUSE Tumbleweed | RPM (x86_64) | `plan-v4d-libs`, `plan-v4d-devel`, `plan-v4d-data`, `plan-v4d-docs`, `plan-v4d-samples` |
| Fedora | RPM (x86_64) | `plan-v4d-libs`, `plan-v4d-devel`, `plan-v4d-data`, `plan-v4d-docs`, `plan-v4d-samples` |
| Ubuntu 24.04 | DEB (amd64 and aarch64) | `plan-v4d-libs`, `plan-v4d-dev`, `plan-v4d-data`, `plan-v4d-samples` |
| Raspberry Pi OS (Debian 12) | DEB (armv7l and aarch64) | `plan-v4d-libs`, `plan-v4d-dev`, `plan-v4d-data`, `plan-v4d-samples` |

What each package provides:

| Package | Contents |
|---|---|
| `plan-v4d-libs` | Shared libraries (`libopencv_*.so`, `libnanovg.so`). |
| `plan-v4d-devel` / `plan-v4d-dev` | Headers, pkgconfig and CMake config for building against the modules. |
| `plan-v4d-data` | Pre-trained models, cascade classifiers, fonts (`/usr/share/opencv4`). |
| `plan-v4d-docs` (RPM only) | Programming guides and module documentation. |
| `plan-v4d-samples` | `example_v4d_*` binaries plus sample sources. |

### From the OBS repository

Once the binaries are published, add the Open Build Service repo to your distro
and install by name, so updates arrive through the normal package manager. The
`download.opensuse.org` paths below contain the exact format that exists on the
server: `home:/<user>:/Plan-V4D:/<subproject>/<repo>`. Note that the DEB
repositories are signed and split per architecture.

**openSUSE Tumbleweed** (x86_64)

```bash
sudo zypper ar \
  https://download.opensuse.org/repositories/home:/elchaschab:/Plan-V4D:/openSUSE_Tumbleweed/openSUSE_Tumbleweed/ \
  plan-v4d
sudo zypper refresh
sudo zypper install plan-v4d-libs plan-v4d-devel
```

**Fedora** (x86_64)

```bash
sudo dnf config-manager --add-repo \
  https://download.opensuse.org/repositories/home:/elchaschab:/Plan-V4D:/Fedora/Fedora/home:elchaschab:Plan-V4D:Fedora.repo
sudo dnf install plan-v4d-libs plan-v4d-dev
```

**Ubuntu 24.04** (DEB) — pick the repo matching your architecture (`Ubuntu_24.04`
for amd64, `Ubuntu_24.04_arm64` for aarch64). The apt source must reference the
repo's signing key, fetched from its published `Release.key`; the same command
rewrites an existing unsigned `plan-v4d.list`.

*amd64:*

```bash
sudo mkdir -p /etc/apt/keyrings
curl -fsSL https://download.opensuse.org/repositories/home:/elchaschab:/Plan-V4D:/Ubuntu_24.04/Ubuntu_24.04/Release.key \
  | sudo gpg --dearmor -o /etc/apt/keyrings/plan-v4d-archive-keyring.gpg
echo "deb [signed-by=/etc/apt/keyrings/plan-v4d-archive-keyring.gpg] https://download.opensuse.org/repositories/home:/elchaschab:/Plan-V4D:/Ubuntu_24.04/Ubuntu_24.04/ /" \
  | sudo tee /etc/apt/sources.list.d/plan-v4d.list
sudo apt update
sudo apt install plan-v4d-libs plan-v4d-dev
```

*aarch64:*

```bash
sudo mkdir -p /etc/apt/keyrings
curl -fsSL https://download.opensuse.org/repositories/home:/elchaschab:/Plan-V4D:/Ubuntu_24.04_arm64/Ubuntu_24.04/Release.key \
  | sudo gpg --dearmor -o /etc/apt/keyrings/plan-v4d-archive-keyring.gpg
echo "deb [signed-by=/etc/apt/keyrings/plan-v4d-archive-keyring.gpg] https://download.opensuse.org/repositories/home:/elchaschab:/Plan-V4D:/Ubuntu_24.04_arm64/Ubuntu_24.04/ /" \
  | sudo tee /etc/apt/sources.list.d/plan-v4d.list
sudo apt update
sudo apt install plan-v4d-libs plan-v4d-dev
```

**Raspberry Pi OS (Debian 12)** (DEB) — same pattern as Ubuntu, with the
`Raspbian_12` repo path in place of the `Ubuntu_24.04` one (arch-suffixed,
e.g. `Raspbian_12_arm64`, once its binaries are published).

Swap `plan-v4d-devel`/`plan-v4d-dev` for `plan-v4d-samples` to get the
demonstration programs instead of the development headers, or install
`plan-v4d-data` to pull in the pre-trained models and fonts.



### License


Apache 2.0, like the rest of OpenCV — see [LICENSE](LICENSE). Vendored
third-party code under `modules/v4d/third/` is licensed under its own terms.



### Attribution

By far the biggest thank you goes to: [Marius Kintel](https://github.com/kintel/)


* The author of the bunny video is the Blender Foundation ([Original video](https://upload.wikimedia.org/wikipedia/commons/transcoded/f/f3/Big_Buck_Bunny_first_23_seconds_1080p.ogv/Big_Buck_Bunny_first_23_seconds_1080p.ogv.1080p.vp9.webm)).
* The author of the dance video is GNI Dance Company ([Original video](https://www.youtube.com/watch?v=yg6LZtNeO_8)).
* The author of the video used in the beauty-demo video is Kristen Leanne ([Original video](https://www.youtube.com/watch?v=hUAT8Jm_dvw)).
* The author of cxxpool is Copyright (c) 2022 Christian Blume: ([LICENSE](https://github.com/bloomen/cxxpool/blob/master/LICENSE))
* The author of the roboto font family is Google Inc. ([LICENSE](https://github.com/googlefonts/roboto/blob/main/LICENSE))



---


## V4D Application Programming Guide


### 1. What is V4D?


V4D (Visualization for Video and Data) is a runtime built on top of Plan-DSL, a small graph-recording language embedded in C++. Plan-DSL is the core; V4D adds four things on top of it:

1. A **window and event loop** built on GLFW + OpenGL, with optional NanoVG (2D vector graphics) and ImGui (immediate-mode GUI) layers.
2. A **Source / Sink abstraction** — a `Plan` can read frames from a video file, a webcam, or any functor (`Source`), and write them to a file, a stream, or anything else (`Sink`).
3. **Side-effect contexts** — a way to schedule a C++ lambda or function onto a specific pipeline: the framebuffer (`fb`), NanoVG (`nvg`), OpenGL (`gl`), bgfx / external renderers (`bgfx`, `ext`), ImGui (`imgui`), or plain CPU (`plain`).
4. **`V4D::Keys` properties** — typed views onto runtime state (framebuffer size, viewport, fullscreen flag, etc.).

That is the whole system. Everything else in this tutorial is elaboration.

**What you will build**

| Milestone | You will build |
|---|---|
| 1 | A window that renders “Hello World” with NanoVG |
| 4 | A video-file → grayscale → video-file converter |
| 5 | A text overlay on live video |
| 6 | A per-pixel threshold effect using framebuffer access |
| 7 | A GUI with sliders controlling the effect |
| 8 | Mouse-driven toggling and runtime property writes |
| 13 | **Chroma** — a complete app combining everything |

---



### 2. The One Mental Model: Record Once, Replay Forever


Read this section twice. It is the single most important idea in V4D.

A `Plan` is a C++ class whose methods — `setup()`, `infer()`, `teardown()`, `gui()` — do not *execute* your code. They *record* it as a list of task nodes. The runtime then replays that recorded list every frame on worker threads.

```
┌───────────────────────── BUILD PHASE (once per worker) ─────────────────────────┐
│  setup()    → records one-shot init graph   → makeGraph → runGraph → clearGraph │
│  infer()    → records the per-frame graph   → makeGraph                         │
├───────────────────────── FRAME LOOP (forever) ──────────────────────────────────┤
│  every frame: runGraph()  ← re-executes the SAME recorded node list             │
├───────────────────────── SHUTDOWN (once per worker) ────────────────────────────┤
│  teardown() → records one-shot cleanup graph → makeGraph → runGraph → clearGraph│
└──────────────────────────────────────────────────────────────────────────────────┘
```

Key consequences:

- **There is no `while (true)`, no `update()`, no `glfwPollEvents()`.** The runtime drives the loop. You describe one iteration of the loop in `infer()`.
- **Nodes execute sequentially, in recording order.** `runGraph()` iterates the recorded node list in the order you wrote the calls. There is no data-dependency scheduler, no work stealing, and no automatic vectorization; dependency metadata is bookkeeping, not scheduling.
- **The graph structure is fixed at build time.** You cannot emit different nodes depending on runtime data. Runtime decisions are expressed with `branch(...)` regions whose predicates are re-evaluated every frame.
- **Side effects written directly in `infer()` (outside a node) happen once, at build time.** If you want something to happen every frame, it must be inside a node: `plain(...)`, `nvg(...)`, `fb(...)`, an operator, etc.
- **Each worker thread builds and runs its own independent copy of the graph.** Workers never share nodes, share partial iterations, or migrate work. Think of `infer()` as the per-thread body of an OpenMP `#pragma omp parallel` region.

A good analogy: Plan-DSL is a *CPU-side shader* for a per-frame computation graph. It reads inputs (`R`, `P`, `E`), computes (`+`, `IF`, `F`), and writes outputs (`RW`, `assign`, `set`, `write`) — once per frame, on fresh data.

---



### 3. Milestone 1 — Hello, NanoVG


Create `hello_nanovg.cpp`. This is modeled on the sample `modules/v4d/samples/font_rendering.cpp`:

```cpp
#include <opencv2/v4d/v4d.hpp>
using namespace cv;
using namespace cv::v4d;

class FontRenderingPlan : public V4DPlan {
    string text_ = "Hello World";                                // ordinary C++ member
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);     // runtime property edge
public:
    void infer() override {
        nvg([](const Size& sz, const string& str) {              // recorded, not executed!
            using namespace cv::v4d::nvg;
            clearScreen();
            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));                   // BGRA
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0,
                 str.c_str(), str.c_str() + str.size());
        }, size_, R(text_));
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Font Rendering",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    V4DPlan::run<FontRenderingPlan>(0);
    return 0;
}
```

Build and run it. A 960×960 window opens with red “Hello World” text centered in it.

**Dissecting the program**

- `class FontRenderingPlan : public V4DPlan` — every V4D program subclasses `V4DPlan` (which itself extends the core `Plan`).
- `infer()` is the per-frame body. Here it records a single node: `nvg(...)` says “every frame, run this lambda inside the NanoVG drawing context.” V4D handles context activation, font setup, and double-buffering for you.
- **Lambda parameters are bound to edges.** The second and third arguments of `nvg(...)` — `size_` and `R(text_)` — are edges. Every frame, the runtime fetches their current values and passes them to the lambda as `sz` and `str`.
- `size_` is a `Property<cv::Size>` — a typed, auto-updating view onto the runtime's framebuffer size (`V4D::Keys::SIZE`). Resize handling comes for free. A `Property` *is* an edge; you never wrap it in `R(...)`.
- `R(text_)` is a read edge wrapping the plain C++ member `text_`.
- `V4D::init(...)` creates the runtime: window, OpenGL context, and whichever subsystems you request via `AllocateFlags`. Because this plan uses `nvg(...)`, we must pass `AllocateFlags::NANOVG`.
- `V4DPlan::run<FontRenderingPlan>(0)` starts the lifecycle. The argument `0` means **one worker thread plus the main thread** (the main thread runs the display/event loop). See §12.2 for the full table.

⚠️ **Common mistake:** copying a sample that uses `nvg(...)` but forgetting `AllocateFlags::NANOVG`. If you don't ask for a subsystem, calls into it are no-ops (or assert).

**Exercise 1.1.** Change the text, color, and font size. Then make the font size depend on the window width: compute it inside the lambda from `sz`.

---



### 4. Milestone 2 — The Shape of a V4D Application


Every V4D program has the same nine-point skeleton:

```cpp
#include <opencv2/v4d/v4d.hpp>
using namespace cv;
using namespace cv::v4d;

// 1. Subclass V4DPlan.
class MyPlan : public V4DPlan {
    // 2. Declare state as ordinary C++ members (per-worker copies).
    cv::UMat scratch_;
    string   label_ = "hello";
    //    Properties are typed views onto runtime state.
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    // 3. (Optional) One-shot initialization — recorded graph, runs once per worker.
    void setup() override {
        plain([](cv::UMat& s) { s.create(cv::Size(640, 480), CV_8UC3); },
              RW(scratch_));
    }
    // 4. The per-frame body. Recorded once, replayed every frame. REQUIRED.
    void infer() override {
        capture(RW(scratch_));        // pull a frame into scratch_
        nvg(/* ... draw ... */);      // draw on top
        write(R(scratch_));           // push the result to the sink
    }
    // 5. (Optional) Main-thread UI installation — runs once on the main thread.
    void gui() override {
        imgui(/* ... widgets ... */, RWS(label_));
    }
    // 6. (Optional) One-shot teardown — recorded graph, runs once per worker.
    void teardown() override { /* ... */ }
};

int main(int argc, char** argv) {
    cv::Rect viewport(0, 0, 1280, 720);
    // 7. Initialize the runtime.
    cv::Ptr<V4D> runtime = V4D::init(viewport, "My V4D App",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    // 8. (Optional) Wire a Source and a Sink.
    auto src  = Source::make(runtime, argv[1]);
    auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());
    runtime->setSource(src);
    runtime->setSink(sink);
    // 9. Start the plan. The workers argument selects the worker count
    //    (0 → one worker plus the main thread; see §12.2).
    V4DPlan::run<MyPlan>(/*workers=*/0);
}
```

**Where each phase runs**

| Phase | Where it runs | When |
|---|---|---|
| `setup()` | each worker thread | once, before the frame loop |
| `infer()` | each worker thread | recorded once; the graph is replayed every frame |
| `gui()` | main thread | once, before the frame loop |
| `teardown()` | each worker thread | once, after the frame loop |

The split mirrors OpenGL habits: `setup()` builds resources, `infer()` does per-frame work, `teardown()` releases them. The novelty is that everything inside these methods *records* rather than *executes*.

**`V4D::init` in full**

```cpp
cv::Ptr<V4D> runtime = V4D::init(
    /* viewport     */ cv::Rect(0, 0, 1280, 720),
    /* window title */ "My V4D App",
    /* subsystems   */ AllocateFlags::NANOVG | AllocateFlags::IMGUI,
    /* config       */ ConfigFlags::DEFAULT,
    /* debug        */ DebugFlags::DEFAULT,
    /* MSAA samples */ 0);
```

There is also an overload taking a separate `framebufferSize` (for high-DPI or when window size ≠ pixel size).

`AllocateFlags` — which subsystems to bring up:

| Flag | Initializes |
|---|---|
| `NONE` | Just the OpenGL framebuffer |
| `NANOVG` | NanoVG vector-graphics context on top of GL |
| `IMGUI` | ImGui immediate-mode GUI context on top of GL |
| `BGFX` | bgfx rendering context (alternative to GL) |
| `DEFAULT` | Same as `NONE`. You almost always want `NANOVG \| IMGUI` |

`ConfigFlags` — how the window behaves:

| Flag | Effect |
|---|---|
| `DEFAULT` | No display-related bits set |
| `OFFSCREEN` | Render off-screen (no visible window) |
| `DISPLAY_MODE` | Sync display thread and workers via semaphores — needed for `imshow`-style programs |
| `RESIZEABLE` | Allow user resizing (not resizable by default!) |

`DebugFlags` — what to log: `PRINT_CONTROL_FLOW` (branch decisions), `PRINT_LOCK_CONTENTION`, `MONITOR_RUNTIME_PROPERTIES`, `LOWER_WORKER_PRIORITY` (Linux), `DEBUG_GL_CONTEXT`, `DONT_PAUSE_LOG`. Keep `DEFAULT` for day-to-day work; reach for `PRINT_CONTROL_FLOW` when a branch misbehaves.

The last argument is the MSAA sample count (`0`, `2`, `4`, `8`…). It only matters for direct `gl(...)` rendering — `nvg(...)` does its own anti-aliasing.

---



### 5. Milestone 3 — Plan-DSL Crash Course


You can't write V4D without a working knowledge of Plan-DSL. This section is the minimum viable subset; the Plan-DSL Programming Guide and Reference cover every detail, and their wording is authoritative.

### 5.1 Edges — the only values in the language

An edge is a typed handle to either a storage location (member variable, shared variable, runtime property) or a computed value. Every edge carries an **access intent** (read-only, read-write, copy, locked/shared). Edge-calls produce edges:

| Edge-call | Meaning | Intent |
|---|---|---|
| `V(x)` | Immediate constant `x` | constant |
| `R(x)` | Read of variable `x` | read-only |
| `RW(x)` | Read-write access to `x` (a *definition*) | read-write |
| `RS(x)` | Read of *shared* variable `x` | read under lock |
| `RWS(x)` | Read-write of *shared* `x` | read-write under lock |
| `CS(x)` | Snapshot copy of shared `x` | read + copy under lock |
| `P<T>(key)` | Runtime property (global or per-thread state) | shared, read-only |
| `E<T>(...)` | Stream of input events | polled each frame |
| `F(fn, args...)` | Call any C++ callable; non-void returns a result edge | — |
| `_(a, b, ...)` | Tuple of edges (for n-ary operators) | — |

```cpp
auto a  = R(counter_);                 // read-only edge
auto b  = RW(buffer_);                 // read-write edge (a destination)
auto c  = V(42);                       // constant edge
auto s  = CS(params_);                 // thread-safe snapshot of shared params
auto fc = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
```

Rule of thumb: use the most restrictive intent you can prove. `R` when a value is only read; `RW` only for true destinations. The intent is what makes the runtime's locking and bookkeeping correct.

### 5.2 Operators — the ALU

Plan-DSL implements the full C++ operator set via operator overloading. In their **expression forms**, operators record a node and return a new result edge:

- Arithmetic: `+`, `-`, `*`, `/`, `%`, `++x`, `x++`, `--x`, `x--`
- Logical: `&&`, `||`, `!`
- Bitwise: `&`, `|`, `^`, `<<`, `>>`
- Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
- Ternary select: `IF(cond, ifTrue, ifFalse)`
- Memory: `container[i]` (`IDX`), `*ptr` (`DEREF`), `dst = src` (`ASSIGN`)
- Construction: `construct(dst, args...)` (`CONSTRUCT`)

```cpp
auto every8th = (seqCnt_ % V(uint64_t(8))) == V(uint64_t(0));   // bool edge
auto bright   = F(&cv::mean, R(frame_)) > V(128.0);              // bool edge
assign(RW(x_), R(x_) + V(1));                                    // statement form: x_ += 1
```

Every operator has four spellings — symbol (`a + b`), named (`ADD(a, b)`), generic (`OP<Operators::ADD_>(a, b)`), and statement (`op<Operators::ADD_>(a, b)` / `assign(...)`). The symbol form is the most ergonomic.

**Expression vs. statement forms.** Symbol/named/generic forms return result edges. The lowercase statement helpers (`assign(...)`, `op<...>(...)`, `construct(...)`) create the same nodes but do **not** return a result edge; they return `cv::Ptr<Plan>` for chaining. Note that the symbol spelling `dst = src` is an *expression* form of `ASSIGN`: it records the store **and yields a result edge**. The toggle idiom in Milestone 8 relies on exactly that.

⚠️ **`IF` is eager.** All three operands of `IF(cond, a, b)` are computed every frame as graph nodes, then one is selected (like LLVM `select`). If the arms have side effects or are expensive, use `branch(...)` regions instead — only the taken arm executes.

### 5.3 Control flow — branch regions, not jumps

There is no `goto`, `break`, or `continue`. Control flow is structured and expressed with predicated regions:

```cpp
branch(R(x_) == V(0))
    ->plain(doA)
->elseBranch()
    ->plain(doC)
->endBranch();
```

The predicate is re-evaluated every frame. If true, the region's nodes run; if false, they are skipped.

**Chaining.** `branch`, `elseBranch`, `endBranch`, `plain`, statement-form operators, void-`F` calls, and the V4D context calls return a plan pointer (`cv::Ptr<Plan>`; most V4D context calls return `cv::Ptr<V4DPlan>`), so you can chain them with `->`. Semicolon-separated statements work equally well — chaining is sugar. A non-void `F(...)` is the exception: it returns a *result edge*, not a plan pointer.

Predicates can be bool edges (`R(x) == V(0)`), bool-returning callables (`branch(always_)`), or callables with operand edges. Predefined predicates: `always_`, `isTrue_(b)`, `isFalse_(b)`, `and_(a, b)`, `or_(a, b)`.

**Branch types** refine concurrency semantics:

| Type | Semantics |
|---|---|
| `NONE` | No branch; plain node behavior |
| `PARALLEL` *(default)* | Every worker executes the region when the predicate holds |
| `SINGLE` | At most *one* worker executes it (globally locked) — use for serialized side effects (logging, file I/O) |
| `ONCE` | Executes exactly once, globally, then permanently disabled (sticky) |
| `PARALLEL_ONCE` | Executes exactly once *per worker* |

```cpp
branch(BranchType::ONCE, always_)
    ->plain([] { /* global one-shot init */ })
->endBranch();
```

### 5.4 Loops — branches whose predicate the body updates

There is no `while` keyword. A loop is a `branch` region whose body updates the predicate. Because the graph is re-executed every frame, each loop iteration takes one frame:

```cpp
struct CountdownPlan : Plan {
    int counter_ = 10;
    void infer() override {
        branch(R(counter_) > V(0));
            plain([](int c) { std::cout << "tick " << c << std::endl; }, R(counter_));
            assign(RW(counter_), R(counter_) - V(1));
        endBranch();
    }
};
```

This prints one `tick` per frame for ten frames, then the region's predicate is false forever. The graph is not rebuilt; only the predicate value changes from frame to frame. Internalize this: V4D's semantics are those of a frame loop, not a thread of execution.

### 5.5 Variables and shared state

**Plain C++ members are per-worker storage.** Each worker gets its own copy; no synchronization, no sharing. Use for scratch buffers, per-worker counters, DNN objects.

**Shared variables get a mutex.** For plan members: declare the member, register it with `_shared(member_)` (usually in the constructor), then access it only via `RS` / `RWS` / `CS`. Using `RS`/`RWS` on an unregistered plan member throws `std::runtime_error`. (Storage *outside* the plan object — e.g. `static` globals — is implicitly registered as shared on first shared access.)

- `RS(x)` — read under the mutex.
- `RWS(x)` — read-write under the mutex.
- `CS(x)` — the variable is read under its mutex and a **private copy** is produced; downstream nodes use the copy, so the lock is held only for the copy itself. Prefer `CS` whenever consumers only need a snapshot (the canonical GUI→worker handoff).

`_safe(var)` is the opt-out: “this variable is never accessed shared; don't give it a mutex.”

`GlobalState` (program-wide) and `LocalState` (per-thread) are key-value tables read via `P<T>(key)` and written via `set(key, edge)` nodes or `GlobalState::set<T>(key, v)`.

**Check your understanding:**

1. Why does `capture()` inside `infer()` not grab a frame right now?
2. What happens if you put `std::cout << "hi"` directly in `infer()` (not inside a node)?
3. `IF` vs `branch` — which one is lazy?

Answers: (1) it records a node; the frame is pulled during replay. (2) It prints once, at graph-build time, on each worker. (3) `branch` is lazy; `IF` computes all operands eagerly.

---



### 6. Milestone 4 — Video In / Video Out



### 6.1 Sources and Sinks

A Source produces frames; a Sink consumes them. The factories understand filenames and use `cv::VideoCapture` / `cv::VideoWriter` under the hood:

```cpp
auto src  = Source::make(runtime, "input.mp4");       // anything VideoCapture handles
auto sink = Sink::make(runtime, "out.mkv", src->fps(), viewport.size());
runtime->setSource(src);
runtime->setSink(sink);
```

### 6.2 `capture(...)` and `write(...)`

Both come in three forms:

```cpp
// 1. Default buffer — V4D binds an internal capture buffer to the plan.
capture();                    // pull a frame into the default buffer
write();                      // push the default buffer out

// 2. Explicit member buffer:
capture(RW(frame_));
write(R(result_));

// 3. With an inline transform:
capture({ cv::cvtColor(in, out, cv::COLOR_BGR2GRAY); }, RW(gray_));
write({ f.copyTo(out); }, R(result_));
```

Notes:

- `capture()` records a node. At replay time the runtime pulls the next frame from the source into the buffer (or through your lambda).
- When you call `capture()` with no argument, the first frame determines the buffer's size.
- If no sink is configured, `write()` is a no-op. That's the normal setup for windowed demos — the visible window is the output.
- `write()` is also silently a no-op inside sub-plans; only the top-level plan pushes to the sink.

### 6.3 Exercise program: video → grayscale → video

```cpp
class GrayscalePlan : public V4DPlan {
    cv::UMat in_, gray_, out_;
public:
    void infer() override {
        capture(RW(in_));
        plain([](const cv::UMat& in, cv::UMat& gray, cv::UMat& out) {
            cv::cvtColor(in, gray, cv::COLOR_BGR2GRAY);
            cv::cvtColor(gray, out, cv::COLOR_GRAY2BGR);   // sink expects 3 channels
        }, R(in_), RW(gray_), RW(out_));
        write(R(out_));
    }
};

int main(int argc, char** argv) {
    cv::Rect viewport(0, 0, 1280, 720);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Grayscale",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    auto src  = Source::make(runtime, argv[1]);
    auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());
    runtime->setSource(src);
    runtime->setSink(sink);
    V4DPlan::run<GrayscalePlan>(0);
}
```

Run it as `./grayscale in.mp4 out.mkv`. Notice how `plain(lambda, edges...)` binds edges to lambda parameters positionally — the same mechanism as `nvg(...)`.

**Exercise 4.1.** Rewrite the pipeline using `capture({ ... }, RW(gray_))` with an inline color conversion, eliminating the `in_` buffer.

**Exercise 4.2.** Make the output half resolution. Hint: `cv::resize` in the `plain` node; pass the sink the new size in `Sink::make`.

---



### 7. Milestone 5 — Drawing on Top of Video


The canonical V4D recipe (see `samples/video_editing.cpp`) is:

```
capture()  →  nvg(...)  →  write()
```

The captured frame lands in the default buffer; the NanoVG context draws into the same framebuffer that the visible window and the sink are bound to; `write()` pushes the composited result.

```cpp
class VideoEditingPlan : public V4DPlan {
    const string hv_ = "Hello Video!";
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        capture();                                        // 1. pull a frame
        nvg([](const Size& sz, const string& str) {      // 2. draw on top
            using namespace cv::v4d::nvg;
            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0,
                 str.c_str(), str.c_str() + str.size());
        }, sz_, R(hv_));
        write();                                          // 3. push the result
    }
};
```

**NanoVG idioms worth knowing.** Inside a `nvg(...)` lambda, `using namespace cv::v4d::nvg;` exposes a near line-for-line mirror of the NanoVG C API:

```cpp
clearScreen();                                   // wipe before drawing

// Text
fontSize(40.0f); fontFace("sans-bold");
fillColor(Scalar(255, 0, 0, 255));               // BGRA
textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
text(x, y, begin, end);

// Shapes
beginPath();
rect(10, 10, 200, 100);
fillColor(Scalar(0, 255, 0, 128));
fill();

// Gradients
Paint gloss = linearGradient(x, y, x + w, y + h,
                             Scalar(0, 0, 0, 32), Scalar(0, 0, 0, 16));
fillPaint(gloss);
fill();

// Images
int handle = createImage("foo.png", NVG_IMAGE_NEAREST);
Paint img = imagePattern(0, 0, w, h, 0.0f, handle, 1.0f);
fillPaint(img);
fill();
```

Two extra tips:

- You can pass member functions instead of lambdas: `nvg(&FaceFeatures::drawFaceOvalMask, RS(features_))`.
- To render into an off-screen `cv::UMat` (e.g. to use the drawing as a texture later), draw with `nvg(...)` and then snapshot the framebuffer with `fb(...)` — covered next.

**Exercise 5.1.** Add a frame counter in the top-left corner. You'll need `Property<uint64_t> seq_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);` passed as an edge and formatted with `snprintf` inside the lambda.

---



### 8. Milestone 6 — Pixel Access with `fb(...)`


`nvg(...)` is for vector graphics; when you need to read or write raw pixels as a `cv::UMat`, use `fb(...)`. V4D creates an OpenCL-OpenGL-shared `UMat` for the framebuffer and implicitly inserts it into your function's argument list at the position given by the template parameter:

```cpp
fb<1>(cv::cvtColor,
      R(result_),                            // arg 0: src
      V(cv::COLOR_BGR2RGBA),                 // arg 2: code   (fb is arg 1: dst)
      V(0),                                  // arg 3: dstCn
      V(cv::ALGO_HINT_DEFAULT));             // arg 4: hint
```

At replay time this calls `cv::cvtColor(result_, framebuffer, ...)` — i.e. it writes `result_` into the visible framebuffer. With the default `fb<0>` (or just `fb(...)`), the framebuffer is the first argument — typically the source.

Because the framebuffer is a genuine `cv::UMat` backed by shared memory, you can run OpenCV (and thus OpenCL) directly on framebuffer data without copies.

**Copying the framebuffer out.** Snapshot the framebuffer into a `UMat` with `copyTo` (samples define the helper pointer `UMAT_COPY_` via the `_OLMC_` macro in `util.hpp`):

```cpp
nvg(&StarsRenderer::draw, RWS(stars_), size_);
fb(UMAT_COPY_, RWS(stars_.rendering_));      // framebuffer → UMat

constexpr static auto UMAT_COPY_ =
    _OLMC_(void, cv::UMat, &cv::UMat::copyTo, cv::OutputArray);
```

Read the macro as “static-cast a member-function pointer to a concrete signature so the DSL can deduce types.” It is zero-cost.

**When to use what**

| Use `nvg(...)` when… | Use `fb(...)` when… |
|---|---|
| You want text, shapes, gradients, images | You want raw pixel read/write |
| The result lives in the visible framebuffer | You want a `cv::UMat` to manipulate later |
| Per-pixel performance isn't critical | You want OpenCL kernels on framebuffer data |

**Exercise 6.1.** Build a threshold effect: `capture(RW(frame_))`, then in a `plain` node compute `cv::threshold` into `mask_`, and finally `fb<1>(cv::cvtColor, R(mask_), ...)` to display it.

---



### 9. Milestone 7 — GUIs with ImGui


V4D ships with ImGui. Request it with `AllocateFlags::IMGUI`, then override `gui()`:

```cpp
void gui() override {
    imgui([](Params& params) {
        using namespace ImGui;
        Begin("Effect");
        Checkbox("Enable",        &params.enabled_);
        SliderFloat("Saturation", &params.skinSaturation_, 0.0f, 10.0f);
        if (Button("Fullscreen")) params.fullscreen_ = !params.fullscreen_;
        End();
    }, RWS(params_));
}
```

**The contract (important!)**

- `gui()` runs once, on the main thread, before the frame loop, outside the normal worker graph. Inside it, `imgui(...)` records a node into a special UI transaction that ImGui re-invokes every display refresh, on the main thread.
- Your ImGui lambda therefore does not participate in the worker graph. It can freely mutate shared state — but only through proper edges. Passing `RWS(params_)` tells the runtime to take the shared mutex for the duration of the UI update.

The current design is a documented exception: *“at the moment gui is an exception from the rule that a Plan only implements the graph, because it runs on the display thread.”* Practical rules:

- Don't share mutable state between `gui()` and `infer()` without shared-variable discipline (`_shared` + `RS`/`RWS`/`CS`).
- Workers should read GUI-controlled parameters with `CS(params_)` — a snapshot copy under the lock — so the UI never holds the lock while the pipeline runs.

**The standard pattern**

```cpp
class MyPlan : public V4DPlan {
    struct Params { float strength_ = 1.0f; bool enabled_ = true; };
    static Params params_;                    // one instance for the whole program
public:
    MyPlan() { _shared(params_); }            // give it a mutex
    void gui() override {
        imgui([](Params& p) { /* widgets mutate p */ }, RWS(params_));
    }
    void infer() override {
        capture();
        branch(F([](const Params& p) { return p.enabled_; }, CS(params_)))
            ->plain(apply_effect, CS(params_))
        ->endBranch();
        write();
    }
};
MyPlan::Params MyPlan::params_;               // definition
```

The predicate snapshots the whole struct under the lock (`CS(params_)`) and extracts the field inside an `F` node — purely core-DSL constructs.

> 📌 **Member-level shared access in the samples.** The Plan-DSL Reference documents `RS`/`RWS`/`CS` on *registered variables*. The samples additionally apply them to *members of a registered shared struct* — e.g. `branch(CS(params_.enabled_))` in `beauty-demo.cpp` — relying on the struct's mutex to protect the whole object. That usage is sample-defined, not part of the formal contract. If in doubt, snapshot the whole struct with `CS(params_)` and select the field inside a node, as above.

(A `static` member living outside the plan object is implicitly registered as shared on first shared access, so the explicit `_shared` call is strictly required only for plan members — but writing it is good habit.)

**Exercise 7.1.** Add a slider that controls text size in your Milestone 5 overlay. The GUI writes `Params` on the main thread; the `nvg` lambda reads a `CS(...)` snapshot.

---



### 10. Milestone 8 — Events and Properties



### 10.1 Events

Events are edges that produce a list of input events for the current frame:

```cpp
Event<Mouse> pressEvents_ = E<Mouse>(Mouse::Type::PRESS);                  // presses only
Event<Mouse> allMouse_    = E<Mouse>();                                    // all mouse events
Event<Mouse> dragEvents_  = E<Mouse>(Mouse::Type::DRAG, Mouse::LEFT);      // type + trigger
Event<Mouse> scroll_      = E<Mouse>(Mouse::Type::SCROLL);
```

Event classes: `Mouse`, `Keyboard`, `Window`, `Joystick`. Mouse types include `PRESS`, `RELEASE`, `CLICK`, `DRAG`, `MOVE`, `SCROLL`, `HOVER_ENTER`, `HOVER_EXIT`. The DSL core produces empty lists; V4D's runtime fills them from GLFW each frame.

The classic usage pattern — test whether the list is empty:

```cpp
auto anyPress = !F(&Mouse::List::empty, pressEvents_);     // bool edge
branch(anyPress)
    ->assign(RWS(params_.enabled_), !CS(params_.enabled_)) // toggle
->endBranch();
```

### 10.2 The toggle idiom from `beauty-demo.cpp`

The demo folds “toggle on click” and “is it enabled?” into a single node — the assignment's result edge is the branch predicate:

```cpp
branch(
    RWS(params_.enabled_) = IF(
        F(&Mouse::List::empty, pressEvents_),   // cond: the event list is empty
        CS(params_.enabled_),                   // cond TRUE  (no press) → keep
        !CS(params_.enabled_)                   // cond FALSE (a press)  → flip
    )
)
    -> /* effect runs only while enabled_ is true */
->elseBranch()
    -> /* compose without the effect */
->endBranch();
```

Read `RWS(...) = IF(...)` as one `ASSIGN` node (symbol form, so it yields a result edge): compute the new value, write it under the shared mutex, and use that value as the predicate.

**Polarity, carefully.** `IF(cond, a, b)` is an eager `select`: it picks `a` when `cond` is true. Here the condition is *“the event list is empty”* — so the **true arm keeps the old value** (no click this frame) and the **false arm flips it** (there was a press). It is easy to misread the first operand as “was there a press?”; it is the negation of that. Trace it once by hand — understanding this expression means you understand edges. (See `beauty-demo.cpp` for the idiom in context.)

### 10.3 Properties: reading and writing runtime state

**Reading** — declare a `Property` and pass it around like any edge:

```cpp
Property<cv::Size>  size_   = P<cv::Size>(V4D::Keys::SIZE);
Property<cv::Rect>  vp_     = P<cv::Rect>(V4D::Keys::VIEWPORT);
Property<uint64_t>  seqCnt_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
Property<size_t>    widx_   = P<size_t>(LocalState::Keys::WORKER_INDEX);
```

V4D keys: `SIZE`, `WINDOW_SIZE`, `VIEWPORT`, `FRAMEBUFFER_SIZE`, `CLEAR_COLOR`, `NAMESPACE`, `FULLSCREEN`, `DISABLE_INPUT_EVENTS`, `VISIBLE`. Core DSL keys include `FRAME_CNT`, `CAPTURE_CNT`, `FPS`, `RUN_CNT`, `TIME_TRACKER`, `WORKERS_READY`, and more (see the Plan-DSL Reference for the full lists).

**Writing** — record a `set(key, edge)` node:

```cpp
set(V4D::Keys::FULLSCREEN,  CS(params_.fullscreen_));
set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(30, 30, 30, 255)));
```

`set` fires every frame at that point in the graph; wrap it in `branch(BranchType::ONCE, always_)` for one-shot writes. There is also a tuple form for setting several keys in one node. (As in §9, the samples pass struct members like `CS(params_.fullscreen_)`; the formal contract describes whole registered variables.)

**Exercise 8.1.** Set `V4D::Keys::DISABLE_INPUT_EVENTS` to `true` and verify your click-toggle stops responding (useful for automated tests and recordings).

---



### 11. Milestone 9 — Raw OpenGL (Optional)


For full OpenGL control — your own shaders, FBOs, vertex buffers — use `gl(...)`. The smallest useful OpenGL program:

```cpp
class RenderOpenGLPlan : public V4DPlan {
public:
    void setup() override {
        gl(glClearColor, V(0.0f), V(0.0f), V(1.0f), V(1.0f));   // blue
    }
    void infer() override {
        gl(glClear, V(GL_COLOR_BUFFER_BIT));                     // each frame
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "GL Blue Screen", AllocateFlags::IMGUI);
    V4DPlan::run<RenderOpenGLPlan>(0);
}
```

Notes:

- `gl(fn, args...)` records a node that calls `fn` inside the worker's OpenGL context. Free functions, member functions and lambdas all work: `gl(&MyScene::render, R(scene_), V(false))`.
- `nvg(...)` is implemented on top of `gl(...)` — it just manages NanoVG state for you. You can freely mix both.
- For debugging, wrap calls in `GL_CHECK(...)`: in debug builds it checks for GL errors after the call; in release builds it compiles away.
- **Multiple contexts.** The reference also defines `gl(idxEdge, fn, args...)`, which executes the GL commands on a selected context index so several contexts render in parallel (in the samples you will see this spelled `gl<-1>(V(ctxIdx), fn, args...)`; see `many_cubes-demo.cpp`, where ten contexts each render a cube).

---



### 12. Milestone 10 — Going Big: Sub-plans, Threads, Shared State



### 12.1 Sub-plans — composing large programs

A sub-plan is a `Plan`/`V4DPlan` instance owned by a parent plan.

```cpp
class BeautyDemoPlan : public V4DPlan {
    cv::Ptr<FaceFeatureMasksPlan> prepareFeatureMasksPlan_;
    cv::Ptr<BeautyFilterPlan>      beautyFilterPlan_;
public:
    BeautyDemoPlan() {
        // constructor-only! first statements after the initializer list
        prepareFeatureMasksPlan_ = _sub<FaceFeatureMasksPlan>(this, features_, frames_);
        beautyFilterPlan_        = _sub<BeautyFilterPlan>(this, params_, frames_);
    }
    void infer() override {
        // ...
        subInfer(prepareFeatureMasksPlan_);   // splice child's infer() graph here
        subInfer(beautyFilterPlan_);
        // ...
    }
};
```

Rules and reasons:

- `_sub<>` is **constructor-only**. Calling it in `infer()` would recreate the child every frame and break state. `subInfer(...)` (the per-frame splice) belongs in `infer()`; `subSetup(...)`/`subTeardown(...)` exist too.
- The child receives references to the parent's state, so its writes land in the same buffers the parent reads — the canonical way to share scratch buffers.
- A spliced graph **inherits the enclosing branch's predicate** — this is the primary tool for conditional sub-pipelines.

Why bother? Organization (a 500-line `infer()` becomes three focused classes), reuse, and conditional composition.

### 12.2 Threads and workers

`V4DPlan::run<Tplan>(N)` forwards `N` to `Plan::run`; the worker-count semantics are defined by Plan-DSL:

| N | Threads |
|---|---|
| -1 | runtime default worker count + main |
| 0 | 1 worker + main ← the common case |
| 1 | 1 worker + main |
| ≥ 1 | N workers + main (e.g. `beauty-demo` runs with 6) |

`0` is the special case meaning “one worker”; any `N ≥ 1` means exactly N workers. For V4D, the main thread usually handles the display and event loop.

**What's shared vs. not:**

| Shared across the program | Not shared |
|---|---|
| `GlobalState` table | Plan member variables (per-worker copies) |
| `_shared(...)` variables (one mutex each) | `LocalState` (per-thread) |
| The runtime itself | `gui()`'s view of the plan (main thread only) |

**Access intents for shared variables:** `RS(x)` (read under lock), `RWS(x)` (read-write under lock), `CS(x)` (read under lock and produce a private copy; downstream nodes use the copy, so prefer it when consumers only need a snapshot). Use `BranchType::SINGLE` to serialize a side-effecting region across workers, and `BranchType::ONCE` / `PARALLEL_ONCE` for one-shot work.

**Design rule:** think of `infer()` as the body of an OpenMP parallel region. Whatever you'd do in a `#pragma omp parallel` region, do here; whatever needs cross-thread visibility goes through shared variables.

---



### 13. Capstone Project — Chroma, a Complete Video Filter App


Let's combine everything: video input, an adjustable color effect on the framebuffer, a HUD overlay, an ImGui control panel, mouse toggling, fullscreen support, and optional file output.

```cpp
#include <opencv2/v4d/v4d.hpp>
using namespace cv;
using namespace cv::v4d;

class ChromaPlan : public V4DPlan {
public:
    struct Params {
        float contrast_   = 1.0f;
        float brightness_ = 0.0f;
        bool  enabled_    = true;
        bool  hud_        = true;
        bool  fullscreen_ = false;
    };
private:
    static Params params_;                       // shared: GUI writes, workers read
    Property<cv::Size>  size_    = P<cv::Size>(V4D::Keys::SIZE);
    Property<uint64_t>  frameNo_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
    Event<Mouse>        clicks_  = E<Mouse>(Mouse::Type::CLICK);

    // CPU-side effect: runs inside an fb(...) node, directly on the framebuffer.
    static void adjust_colors(cv::UMat& img, const Params& p) {
        img.convertTo(img, -1, p.contrast_, p.brightness_);
    }
public:
    ChromaPlan() { _shared(params_); }

    // ---- Main-thread UI, installed once ------------------------------------
    void gui() override {
        imgui([](Params& p) {
            using namespace ImGui;
            Begin("Chroma");
            Checkbox("Enable effect", &p.enabled_);
            SliderFloat("Contrast",   &p.contrast_,   0.0f, 3.0f);
            SliderFloat("Brightness", &p.brightness_, -100.0f, 100.0f);
            Checkbox("HUD", &p.hud_);
            if (Button("Fullscreen")) p.fullscreen_ = !p.fullscreen_;
            End();
        }, RWS(params_));
    }

    // ---- Per-frame graph ----------------------------------------------------
    void infer() override {
        set(V4D::Keys::FULLSCREEN, CS(params_.fullscreen_));     // (0) GUI → runtime

        // (1) toggle enabled_ on mouse click; the assignment doubles as predicate.
        //     IF's condition is "the click list is empty":
        //     true arm (no click) keeps the value, false arm (click) flips it.
        branch(
            RWS(params_.enabled_) = IF(
                F(&Mouse::List::empty, clicks_),
                CS(params_.enabled_),          // no click → unchanged
                !CS(params_.enabled_)          // click    → flip
            )
        )
        ->endBranch();

        capture();                                             // (2) input

        branch(CS(params_.enabled_))                           // (3) effect, only while enabled
            ->fb(adjust_colors, CS(params_))
        ->endBranch();

        branch(CS(params_.hud_))                               // (4) HUD overlay
            ->nvg([](const Size& sz, uint64_t frameNo) {
                using namespace cv::v4d::nvg;
                char buf[64];
                snprintf(buf, sizeof(buf), "frame %llu", (unsigned long long)frameNo);
                fontSize(28.0f); fontFace("sans-bold");
                fillColor(Scalar(255, 255, 255, 220));
                textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                text(16.0f, 12.0f, buf, buf + strlen(buf));
            }, size_, frameNo_)
        ->endBranch();

        write();                                               // (5) output
    }
};
ChromaPlan::Params ChromaPlan::params_;
```

(As in §9, the sample-style `CS(params_.field)` spellings take member-level snapshots of the registered `params_` struct; the formal contract documents whole-variable snapshots. See the note in §9.)

And the `main`:

```cpp
int main(int argc, char** argv) {
    cv::Rect viewport(0, 0, 1280, 720);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Chroma",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                                     ConfigFlags::DISPLAY_MODE);
    // Input: argv[1] = video file (or wire a webcam Source here).
    auto src = Source::make(runtime, argv[1]);
    runtime->setSource(src);
    // Optional output: pass a third argument to record the composited frames.
    if (argc > 2) {
        auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());
        runtime->setSink(sink);
    }
    V4DPlan::run<ChromaPlan>(0);
    return 0;
}
```

**Walk through what happens at runtime:**

1. The main thread runs `gui()` once, installing the ImGui panel. Workers build their graphs.
2. Every frame: click-toggle node → `capture()` → (maybe) `fb(adjust_colors)` mutating the framebuffer in place → (maybe) HUD → `write()`.
3. The GUI thread mutates `params_` under the shared mutex; workers always see consistent snapshots via `CS(...)`.
4. No sink configured? `write()` is a no-op and the window is the output. Sink configured? The composited frames (with HUD) are encoded.

**Capstone exercises.**

1. Add a `Keyboard` event that also toggles the effect.
2. Replace the HUD frame counter with a rolling FPS readout (property `GlobalState::Keys::FPS`).
3. Add a “side-by-side” checkbox that composes original vs. filtered frames (study `beauty-demo.cpp` — it does exactly this).
4. Make the window resizable (`ConfigFlags::RESIZEABLE`) and verify `size_` keeps the HUD positioned.

---



### 14. Debugging and Common Pitfalls


**Debug tools**

| Tool | Use when… |
|---|---|
| `DebugFlags::PRINT_CONTROL_FLOW` | A branch isn't behaving as expected — logs per-node enable/disable decisions |
| `DebugFlags::PRINT_LOCK_CONTENTION` | Suspected shared-mutex contention |
| `DebugFlags::MONITOR_RUNTIME_PROPERTIES` | You want every property read/write logged |
| `DebugFlags::DEBUG_GL_CONTEXT` | Deep OpenGL debugging (huge log) |
| `GL_CHECK(expr)` | Checking GL errors around raw GL calls (debug builds only) |

**Pitfall gallery**

| # | Pitfall | Why / Fix |
|---|---|---|
| 1 | “My `cout` prints once and never again” | Code outside nodes runs at graph-build time. Wrap per-frame work in `plain(...)`, `nvg(...)`, etc. |
| 2 | “My `if (edge)` at build time does nothing useful” | Graph structure is fixed at build time. Use `branch(...)` for runtime decisions. |
| 3 | `nvg(...)` silently does nothing | You forgot `AllocateFlags::NANOVG` in `V4D::init`. Same for `IMGUI` / `BGFX`. |
| 4 | `std::runtime_error` from `RS`/`RWS` | The plan member wasn't registered with `_shared(...)`. (Globals/statics outside the plan are implicitly shared.) |
| 5 | Both arms of `IF` run | `IF` is eager — it's a `select`, not a branch. Use `branch` regions for lazy/side-effecting arms. |
| 6 | “My loop is slow” | Loops advance *one iteration per frame* — that's the frame-sequential model, not a bug. |
| 7 | Window doesn't resize | Pass `ConfigFlags::RESIZEABLE`. |
| 8 | No output file appears | You never called `runtime->setSink(...)` — `write()` is a no-op without a sink (and inside sub-plans). |
| 9 | Data race between GUI and workers | Mutate shared state from `gui()` only through `RWS(...)`; read it in `infer()` with `CS(...)`. |
| 10 | “Tearing”/display desync in `imshow`-style apps | Use `ConfigFlags::DISPLAY_MODE`. |
| 11 | Toggle fires on the wrong arm | Re-read §10.2: `IF`'s first operand is the *condition*; the true arm is selected when it holds. |

---



### 15. Where to Go Next


Read, in order:

1. `plan-dsl-programming-guide.markdown` — the DSL tutorial (the foundation of everything here).
2. `plan-dsl-reference.markdown` — the ISA-style reference: every opcode, every overload, every branch type, plus the LLVM-IR lowering table. Keep it open while you write. If it disagrees with this tutorial, the reference wins.
3. `modules/v4d/include/opencv2/v4d/v4d.hpp` — the entry header; everything in these guides is an ergonomic summary of what's there.

Study the samples (`modules/v4d/samples/`), roughly in this order:

| Sample | Teaches |
|---|---|
| `font_rendering.cpp` | Minimum NanoVG program |
| `render_opengl.cpp` | Minimum OpenGL program |
| `display_image_fb.cpp` / `display_image_nvg.cpp` | Image display via `fb` vs `nvg` |
| `video_editing.cpp` | `capture → nvg → write` |
| `font_with_gui.cpp` | GUI feeding NanoVG |
| `custom_source_and_sink.cpp` | Rolling your own I/O + conditional `write()` in a branch |
| `cube-demo.cpp` / `many_cubes-demo.cpp` | Pure GL; multiple parallel GL contexts |
| `pedestrian-demo.cpp` / `optflow-demo.cpp` | Non-trivial detection + tracking pipelines |
| `imshow_reimplementation.cpp` | A full GUI image viewer |
| `beauty-demo.cpp` | The kitchen sink: shared state, sub-plans, `IF` toggling, events, GUI |

---



### 16. Appendix — Cheat Sheet


```cpp
// ── Includes ────────────────────────────────────────────────────────────────
#include <opencv2/v4d/v4d.hpp>
using namespace cv;
using namespace cv::v4d;

// ── Init ────────────────────────────────────────────────────────────────────
cv::Ptr<V4D> rt = V4D::init(viewport, "Title",
                            AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                            ConfigFlags::DEFAULT, DebugFlags::DEFAULT, /*msaa*/0);

// ── Plan skeleton ───────────────────────────────────────────────────────────
class MyPlan : public V4DPlan {
    cv::UMat scratch_;                                    // per-worker
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void setup()    override { /* one-shot init graph   */ }
    void infer()    override { /* per-frame graph (REQ) */ }
    void gui()      override { /* main-thread UI, once  */ }
    void teardown() override { /* one-shot cleanup      */ }
};

// ── Sources / sinks ─────────────────────────────────────────────────────────
auto src  = Source::make(rt, "in.mp4");
auto sink = Sink::make(rt, "out.mkv", src->fps(), viewport.size());
rt->setSource(src);  rt->setSink(sink);

// ── Capture / write ─────────────────────────────────────────────────────────
capture();  capture(RW(buf));  capture({ /*transform*/ }, RW(buf));
write();    write(R(buf));     write({ /*transform*/ }, R(buf));

// ── Edges ───────────────────────────────────────────────────────────────────
V(x)  R(x)  RW(x)  RS(x)  RWS(x)  CS(x)  P<T>(key)  E<T>(type)  F(fn, ...)  _(...)

// ── Contexts ────────────────────────────────────────────────────────────────
plain(fn, args...)          // CPU
nvg(fn, args...)            // NanoVG
fb<pos>(fn, args...)        // framebuffer UMat auto-inserted at pos
gl(fn, args...)             // OpenGL   (gl(idxEdge, fn, ...) selects a context index)
imgui(fn, args...)          // inside gui() only
set(key, edge)              // property write node

// ── Control flow ────────────────────────────────────────────────────────────
branch(pred)->plain(a)->elseBranch()->plain(b)->endBranch();
branch(BranchType::SINGLE, pred)   // at most one worker
branch(BranchType::ONCE, always_)  // once, globally

// ── Shared state ────────────────────────────────────────────────────────────
static Params params_;  /* ctor: */ _shared(params_);
RS(params_)  RWS(params_)  CS(params_)

// ── Sub-plans ───────────────────────────────────────────────────────────────
sub_ = _sub<Sub>(this, args...);      // constructor only
subInfer(sub_);                       // in infer()

// ── Run ─────────────────────────────────────────────────────────────────────
V4DPlan::run<MyPlan>(/*workers=*/0);  // 0 → one worker + main thread
```

Happy hacking — record once, replay forever.

