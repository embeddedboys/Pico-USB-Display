# Developer Code Quality Skill

## Purpose

This skill defines how an Agent should write, modify, review, and refactor source code for long-term human maintainability.

The goal is not merely:

> "The code works."

The goal is:

> **The code works, is understandable, is locally reasoned about, and can be safely modified by another developer later.**

This skill applies to:

* C
* C++
* Rust
* Python
* Go
* JavaScript / TypeScript
* Shell
* Linux kernel code
* embedded firmware
* device drivers
* test code
* build scripts
* configuration files
* generated code when it is intended for human maintenance

---

# 1. Core Principle

## Working Code Is Not Finished Code

An implementation that passes the current test is not automatically a good implementation.

The Agent must distinguish:

```text
Functional correctness
        +
Readability
        +
Maintainability
        +
Verifiability
        +
Appropriate complexity
```

The minimum acceptable result is not:

```text
"It runs."
```

It is:

```text
"It runs, and another developer can understand why it works
and modify it without reverse-engineering the entire implementation."
```

---

# 2. Two-Phase Implementation

Do not stop immediately after obtaining a working implementation.

Use this workflow:

```text
Understand
    ↓
Implement
    ↓
Verify
    ↓
Readability Review
    ↓
Simplification
    ↓
Maintainability Review
    ↓
Final Verification
```

The first implementation may be ugly.

That is acceptable.

The final submitted implementation should not be.

---

# 3. The 10-Second Code Test

A developer should be able to open the relevant function and understand its basic purpose within approximately 10 seconds.

They should be able to identify:

* what the function does
* its major inputs
* its major outputs
* important state changes
* important error paths
* unusual constraints

If this requires reading 300 lines of implementation, the code likely needs restructuring.

---

# 4. Local Comprehension

Prefer code that can be understood locally.

Bad:

```text
function_a()
    ↓
sets global state
    ↓
function_b()
    ↓
modifies hidden state
    ↓
macro changes behavior
    ↓
function_c()
    ↓
finally produces result
```

Better:

```text
prepare_input()
validate_input()
perform_operation()
handle_result()
```

A developer should not need to understand the entire repository to understand a small function.

---

# 5. Make Control Flow Obvious

Prefer straightforward control flow.

Good:

```c
if (!device_ready(dev))
    return -ENODEV;

if (!firmware_ready(dev))
    return -EAGAIN;

return start_transfer(dev);
```

Avoid unnecessary cleverness:

```c
return device_ready(dev) ?
       firmware_ready(dev) ?
       start_transfer(dev) :
       -EAGAIN :
       -ENODEV;
```

Both may be correct.

The first is easier to scan, debug, modify, and instrument.

Prefer clarity over cleverness.

---

# 6. Avoid Excessive Nesting

Deep nesting is a major maintainability problem.

Avoid:

```c
if (a) {
    if (b) {
        if (c) {
            if (d) {
                if (e) {
                    do_work();
                }
            }
        }
    }
}
```

Prefer guard clauses:

```c
if (!a)
    return;

if (!b)
    return;

if (!c)
    return;

if (!d)
    return;

if (!e)
    return;

do_work();
```

Use early returns when they make the main path easier to see.

---

# 7. Main Path Should Be Visually Obvious

For non-trivial functions, structure the code so the successful path is easy to follow.

Prefer:

```text
validate
    ↓
prepare
    ↓
perform
    ↓
cleanup
```

Avoid interleaving unrelated error handling throughout the main algorithm when a clearer structure is possible.

---

# 8. Function Size

There is no universal maximum function length.

However, a function should normally have one coherent responsibility.

A function becomes suspicious when:

* it has many unrelated phases
* it requires many comments to explain its sections
* it contains multiple independent algorithms
* it has many local variables with unrelated purposes
* its control flow is difficult to summarize in one sentence
* changing one behavior requires understanding unrelated code

When this happens, split the function.

Do not split merely to satisfy an arbitrary line count.

---

# 9. The Single-Responsibility Test

Ask:

> Can I describe what this function does in one short sentence?

Good:

```text
Submit the next USB frame.
```

Good:

```text
Initialize the display controller.
```

Suspicious:

```text
Initialize the display, configure USB,
allocate buffers, parse configuration,
start worker threads, and recover from errors.
```

Split it.

---

# 10. Function Extraction

Extract a function when a block has:

* a meaningful purpose
* a meaningful name
* independent error handling
* independent state transitions
* a reusable operation
* a distinct hardware/protocol step

Example:

Instead of:

```c
probe()
{
    ...
    // 80 lines of USB initialization
    ...
    // 50 lines of display initialization
    ...
    // 40 lines of buffer allocation
    ...
}
```

prefer:

```c
probe()
{
    ret = init_usb(dev);
    if (ret)
        return ret;

    ret = init_display(dev);
    if (ret)
        return ret;

    return alloc_buffers(dev);
}
```

The top-level function becomes an architectural summary.

---

# 11. Do Not Over-Split

Avoid turning every two lines into a function.

Bad:

```c
check_a();
check_b();
set_x();
set_y();
```

where every helper is trivial and adds indirection without meaning.

Use functions when the name improves understanding.

The question is:

> Does the function name make the caller easier to understand?

If not, keep the code local.

---

# 12. Names Are Documentation

Prefer names that communicate purpose.

Bad:

```c
int x;
int y;
int tmp;
int val;
int ret2;
```

Better:

```c
int width;
int height;
int retry_count;
int pixel_count;
int ret;
```

Names should encode meaning, not implementation history.

---

# 13. Avoid Ambiguous Names

Avoid:

```text
data
buf
tmp
obj
ctx
state
info
value
result
thing
handle
```

when more precise names are practical.

Context-specific names are acceptable when the scope is very small.

For example:

```c
struct urb *urb;
```

is perfectly clear in a USB function.

Do not make names artificially long.

Prefer the shortest name that remains unambiguous.

---

# 14. Boolean Names

Boolean variables should read naturally.

Prefer:

```c
is_ready
has_firmware
device_connected
transfer_pending
```

Avoid:

```c
flag
status
x
state
```

when the value is actually boolean.

---

# 15. Avoid Generic `state`

If a structure has many states, use an explicit type:

```c
enum device_state {
    DEVICE_INIT,
    DEVICE_READY,
    DEVICE_ERROR,
};
```

instead of undocumented numeric values:

```c
int state;
```

The type itself should communicate the state machine.

---

# 16. Use Types to Encode Meaning

Prefer:

```c
enum device_state state;
```

over:

```c
int state;
```

Prefer:

```c
bool connected;
```

over:

```c
int connected;
```

Prefer strongly typed enums / wrappers / structures where the language supports them.

Good types reduce the amount of explanation required in comments.

---

# 17. Avoid Magic Numbers

Bad:

```c
if (ret == -104)
```

Prefer:

```c
if (ret == -ECONNRESET)
```

Bad:

```c
delay_ms(10);
```

if the meaning is important.

Prefer:

```c
delay_ms(RESET_SETTLE_MS);
```

when the constant represents a meaningful concept.

Do not blindly replace every number with a named constant.

Names should add meaning.

---

# 18. Constants Should Represent Concepts

Good:

```c
#define MAX_FRAME_SIZE 4096
#define RESET_SETTLE_MS 10
#define USB_TIMEOUT_MS 1000
```

Bad:

```c
#define VALUE1 4096
#define VALUE2 10
#define VALUE3 1000
```

A constant should answer:

> "What does this number mean?"

---

# 19. Avoid Excessive Abstraction

Do not introduce abstractions merely because they look architecturally sophisticated.

Bad:

```text
Interface
    ↓
Factory
    ↓
Provider
    ↓
Adapter
    ↓
Strategy
    ↓
Wrapper
    ↓
Actual operation
```

when the project only has one implementation.

Prefer direct code until abstraction solves a real problem.

---

# 20. Abstraction Must Earn Its Complexity

Introduce an abstraction when it provides at least one substantial benefit:

* removes duplication
* isolates a complex subsystem
* allows meaningful substitution
* enforces an invariant
* provides a stable interface
* simplifies callers
* separates hardware/platform differences

Do not create an abstraction solely to make the architecture "look clean."

---

# 21. Avoid Premature Generalization

Do not build a generic framework for one concrete use case unless there is evidence that the abstraction is needed.

Bad:

```text
GenericTransport
GenericBackend
GenericDevice
GenericProtocol
```

for a single device.

Prefer the simplest implementation that cleanly solves the current problem.

Generalize when actual requirements justify it.

---

# 22. Duplication vs Abstraction

Small duplication is sometimes better than premature abstraction.

Prefer:

```text
simple code × 2
```

over:

```text
complex generic framework × 1
```

when the duplicated code is small and the future common abstraction is unclear.

Refactor when duplication becomes meaningful and stable.

---

# 23. Avoid Cleverness

Do not optimize for code golf.

Bad:

```c
x += cond ? a : b;
```

when the actual behavior is clearer as:

```c
if (cond)
    x += a;
else
    x += b;
```

Do not compress multiple concepts into one expression merely to reduce line count.

Readable code is often slightly longer.

---

# 24. One Concept Per Statement

Prefer:

```c
size = width * height;
buffer = alloc_buffer(size);

if (!buffer)
    return -ENOMEM;
```

over:

```c
if (!(buffer = alloc_buffer(width * height)))
    return -ENOMEM;
```

The second may be valid.

The first is easier to inspect and debug.

---

# 25. Avoid Clever Macros

Macros can hide control flow and types.

Avoid macros when an ordinary function, inline function, constant, enum, or language feature is clearer.

Use macros when they provide a real benefit, such as:

* compile-time configuration
* conditional compilation
* repetitive declarations
* kernel-style infrastructure
* hardware register definitions

Do not use macros merely to shorten code.

---

# 26. Hidden Side Effects

Avoid expressions where reading the code does not reveal side effects.

Bad:

```c
if (get_device() && update_state())
```

if either function modifies important state.

Prefer:

```c
dev = get_device();
if (!dev)
    return -ENODEV;

ret = update_state(dev);
if (ret)
    return ret;
```

Important state transitions should be visible.

---

# 27. Minimize Hidden Global State

Global state increases the amount of code a developer must understand.

Prefer explicit data flow:

```c
process_frame(dev, frame);
```

over hidden globals:

```c
current_device = dev;
process_frame();
```

Global state is sometimes required, especially in kernel/embedded systems, but it should be intentional.

---

# 28. Make Ownership Explicit

For resources such as:

* memory
* file descriptors
* URBs
* DMA mappings
* locks
* references
* device handles
* threads
* timers

the code should make ownership clear.

Prefer APIs and structures that make lifetime obvious.

If ownership cannot be made obvious through code, add a concise comment.

---

# 29. Error Handling Must Be Consistent

Use a consistent error-handling style within a project.

Avoid mixing:

```text
exceptions
return codes
global error state
NULL
magic values
```

without a clear reason.

For C/kernel code, make cleanup paths easy to follow.

Prefer:

```c
buf = alloc_buffer();
if (!buf)
    return -ENOMEM;

urb = alloc_urb();
if (!urb) {
    free_buffer(buf);
    return -ENOMEM;
}
```

or a consistent cleanup structure appropriate for the project.

---

# 30. Cleanup Must Be Obvious

Resource acquisition should have an obvious cleanup path.

A reviewer should be able to answer:

```text
What was allocated?
Who owns it?
Where is it released?
What happens if step 3 fails?
```

without reconstructing the entire function.

---

# 31. Avoid Deep Cleanup Mazes

If error handling creates a maze of labels and flags, consider restructuring.

However, in Linux kernel code, `goto`-based cleanup is often appropriate and should not be rejected merely because it uses `goto`.

Prefer:

```c
err_urb:
    usb_free_urb(urb);
err_buf:
    kfree(buf);
    return ret;
```

when it makes ownership and cleanup obvious.

Do not replace clear kernel cleanup paths with deeply nested conditionals merely to avoid `goto`.

---

# 32. State Machines Should Look Like State Machines

If the behavior is actually a state machine, model it explicitly.

Avoid:

```c
if (a && !b && c && retry < 3 && ...)
```

when the code represents distinct states.

Prefer:

```c
switch (dev->state) {
case DEVICE_INIT:
    ...
    break;

case DEVICE_READY:
    ...
    break;

case DEVICE_ERROR:
    ...
    break;
}
```

Use explicit states when they make transitions easier to understand.

---

# 33. Separate Policy From Mechanism

Prefer separating:

```text
What should happen?
```

from:

```text
How is it performed?
```

Example:

```c
if (should_retry(dev))
    retry_transfer(dev);
```

instead of embedding policy throughout low-level I/O code.

This is especially valuable in drivers and embedded systems.

---

# 34. Separate Hardware Access From Logic

Where practical:

```text
hardware access
        ↓
state / data representation
        ↓
policy
```

Avoid mixing:

```text
register writes
protocol parsing
retry policy
logging
memory allocation
UI logic
```

inside one giant function.

This separation greatly improves testing and maintenance.

---

# 35. Keep Hardware Quirks Local

When a hardware workaround is required, isolate it.

Bad:

```c
if (rev == B)
    ...
```

scattered across 20 functions.

Prefer:

```c
apply_rev_b_quirks(dev);
```

or a well-defined capability/quirk mechanism.

The goal is to make hardware-specific behavior easy to find.

---

# 36. Avoid Boolean Explosion

Bad:

```c
process(dev, true, false, true, false, true);
```

The caller is unreadable.

Prefer:

```c
struct process_options opts = {
    .flush = true,
    .blocking = false,
    .retry = true,
};

process(dev, &opts);
```

or explicit functions when appropriate.

---

# 37. Avoid Parameter Explosion

If a function has many parameters:

```c
foo(a, b, c, d, e, f, g, h);
```

consider whether they represent:

* a configuration structure
* a context object
* separate operations
* excessive responsibility

Do not blindly introduce a struct.

First determine whether the function itself should be split.

---

# 38. Keep Data Structures Focused

A structure containing:

```text
USB state
display state
audio state
test state
debug state
network state
```

is likely mixing responsibilities.

Prefer cohesive structures.

---

# 39. Avoid "God Objects"

A giant context structure passed everywhere is often a symptom of poor boundaries.

Bad:

```c
process_everything(struct application_context *ctx);
```

when the function only needs:

```c
struct display *display;
```

Pass the smallest meaningful context.

---

# 40. Minimize Function Parameter Context

Prefer:

```c
submit_frame(display, frame);
```

over:

```c
submit_frame(application, config, board, usb, display, state, frame);
```

unless all of that context is genuinely required.

A narrow interface communicates dependencies.

---

# 41. Reduce Dependency Surface

A module should depend only on what it needs.

Avoid importing or including entire subsystems for trivial operations.

Smaller dependency surfaces improve:

* compilation
* testing
* portability
* comprehension
* reuse

---

# 42. Keep APIs Small

A good API should expose the concepts users actually need.

Avoid exposing internal implementation details.

Bad:

```text
set_internal_state()
set_internal_buffer()
set_internal_flags()
```

when the real operation is:

```text
start_device()
```

Expose intent rather than mechanism when appropriate.

---

# 43. Avoid Leaking Implementation Details

If callers must understand:

```text
internal buffer layout
temporary state
internal retry counters
private synchronization
```

just to use an API, the abstraction boundary may be wrong.

---

# 44. Readability of Expressions

Break complex expressions when intermediate names improve understanding.

Bad:

```c
result = ((a + b) * scale + offset) >> shift;
```

Better when semantics matter:

```c
scaled = (a + b) * scale;
adjusted = scaled + offset;
result = adjusted >> shift;
```

Or use a meaningful helper:

```c
result = convert_sample(raw, calibration);
```

Do not split arithmetic merely to increase line count.

---

# 45. Operator Complexity

Avoid deeply nested expressions:

```c
if ((a && b) || (c && !d) || (e && f && !g))
```

Prefer named predicates when the conditions represent concepts:

```c
if (device_can_start(dev))
```

This makes the policy explicit.

---

# 46. Magic Control Flow

Avoid code whose behavior depends on subtle interactions between:

* macros
* callbacks
* globals
* implicit conversions
* side effects
* thread-local state
* signal handlers
* interrupt context

When unavoidable, isolate and document the invariant.

---

# 47. Concurrency Readability

Concurrency code should make synchronization visible.

Prefer:

```c
mutex_lock(&dev->lock);
state = dev->state;
mutex_unlock(&dev->lock);
```

over hidden synchronization inside unrelated helpers when the lock boundary matters.

Make it clear:

* who owns the lock
* what data it protects
* what may execute concurrently
* what context the callback runs in

---

# 48. Interrupt Context

In low-level code, make context constraints obvious.

Examples:

```c
/* Called from IRQ context; must not sleep. */
```

or use APIs whose names/types make the constraint obvious.

Do not hide context-sensitive operations behind generic helpers unless the boundary is clear.

---

# 49. Async Code

Asynchronous code should make lifecycle transitions explicit.

Prefer:

```text
submit
    ↓
pending
    ↓
callback
    ↓
complete
```

rather than a function where asynchronous state changes are hidden across unrelated globals.

This is especially important for:

* URBs
* workqueues
* timers
* DMA
* futures/promises
* callbacks
* threads

---

# 50. Avoid Callback Spaghetti

If callbacks call callbacks that mutate shared state in multiple places, consider introducing explicit state transitions.

Bad:

```text
callback A
  → callback B
      → callback C
          → global state
              → callback D
```

Prefer a clear state/event model.

---

# 51. Logging Quality

Logs should help diagnose real failures.

Avoid:

```c
printf("here");
printf("here2");
printf("here3");
```

Prefer:

```c
dev_dbg(dev, "URB completion status=%d\n", status);
```

Include useful context:

* operation
* identifier
* error
* state
* relevant parameter

Do not flood logs with obvious messages.

---

# 52. Logging Must Not Replace Structure

Do not make unreadable code understandable only through debug logs.

The code itself should make the control flow understandable.

---

# 53. Tests Are Part of Code Quality

A maintainable implementation should be testable where practical.

Prefer code that allows:

```text
input
    ↓
operation
    ↓
observable result
```

Avoid designs where behavior can only be verified by running an entire hardware system when a smaller unit could be tested independently.

---

# 54. Test Names Should Explain Intent

Bad:

```text
test_case_1
test_usb
test_timeout
```

Better:

```text
test_bulk_transfer_waits_for_firmware_ready
test_disconnect_cancels_pending_urb
test_reset_timeout_is_retried
```

A test name should communicate the behavior being protected.

---

# 55. Test Assertions Should Be Meaningful

Bad:

```python
assert x == 1234
```

when the reader cannot know why 1234 matters.

Better:

```python
assert samples_per_frame == EXPECTED_SAMPLES_PER_FRAME
```

The test should express the invariant.

---

# 56. Avoid Tests That Encode Accidental Behavior

Do not write tests that merely capture whatever the current implementation happens to do.

Tests should protect:

```text
requirements
invariants
protocol behavior
hardware guarantees
public API behavior
```

not accidental internal details.

---

# 57. Formatting Is Part of Readability

Follow the project's established formatting rules.

Do not introduce personal formatting preferences into an existing project unless necessary.

Use:

* formatter
* linter
* compiler warnings
* project style guide

where available.

Do not spend manual effort fighting an established formatter.

---

# 58. Avoid Unnecessary Formatting Changes

When fixing a specific issue, do not reformat the entire repository unless requested.

Large unrelated diffs make review harder.

Prefer:

```text
functional change
+
necessary readability improvement
```

over:

```text
functional change
+
entire-file reformat
+
unrelated refactoring
```

---

# 59. Keep Diffs Focused

A maintainable change should have a clear reason.

Avoid mixing:

```text
feature
+
renaming
+
architecture rewrite
+
formatting
+
unrelated cleanup
+
comment rewrite
```

unless the user explicitly requested broad refactoring.

Focused diffs are easier to review and safer to merge.

---

# 60. Refactoring Scope

When improving readability, distinguish:

### Local refactoring

Safe during feature work:

* rename local variables
* extract a small helper
* simplify control flow
* remove duplicate code
* clarify conditions

### Structural refactoring

Potentially disruptive:

* changing APIs
* moving modules
* changing ownership
* changing concurrency architecture
* replacing subsystems

Do not perform major structural refactoring merely because the Agent prefers another architecture.

---

# 61. Preserve Behavior During Refactoring

When refactoring existing working code:

```text
Before
    ↓
Understand behavior
    ↓
Refactor
    ↓
Run existing tests
    ↓
Compare behavior
```

Do not mix a large semantic change with a large readability refactor unless necessary.

---

# 62. Refactor Before Adding More Complexity

If a new feature requires adding logic to an already difficult function:

Do not automatically append another branch.

First ask:

```text
Can the existing structure be simplified enough
to make the new behavior natural?
```

Avoid:

```c
if (old_condition) {
    ...
} else if (new_condition) {
    ...
} else if (special_case) {
    ...
} else if (another_special_case) {
    ...
}
```

turning into a 300-line conditional.

---

# 63. Complexity Is a Design Signal

Watch for:

* deeply nested conditionals
* very long functions
* very long parameter lists
* many mutable variables
* repeated state checks
* duplicate cleanup logic
* duplicated conditions
* large switch statements
* large classes/structs
* many global variables

These do not automatically mean the code is wrong.

They are signals to review the design.

---

# 64. Prefer Explicit State Over Clever Inference

If state matters, represent it.

Bad:

```c
if (!buf && !urb && !ready)
```

where the combination implicitly represents "not initialized."

Better:

```c
enum device_state state;
```

when the lifecycle is meaningful.

---

# 65. Avoid Boolean State Explosion

Bad:

```c
bool initialized;
bool started;
bool connected;
bool configured;
bool running;
bool stopping;
bool error;
```

when these represent mutually exclusive states.

Consider:

```c
enum device_state {
    DEVICE_INIT,
    DEVICE_CONNECTED,
    DEVICE_RUNNING,
    DEVICE_STOPPING,
    DEVICE_ERROR,
};
```

when appropriate.

---

# 66. Data Flow Should Be Visible

Prefer:

```c
frame = capture_frame(dev);
processed = process_frame(frame);
submit_frame(display, processed);
```

over functions that silently mutate shared global buffers.

Explicit data flow is easier to test and reason about.

---

# 67. Avoid Unnecessary Mutation

Prefer immutable or read-only data where practical.

Avoid modifying variables simply because they are available.

Every mutable variable increases the number of states a reader must consider.

---

# 68. Reduce Variable Lifetime

Declare variables close to where they are used.

Bad:

```c
int ret;
int size;
int status;
void *buf;
...
```

followed by 100 lines of unrelated logic.

Prefer:

```c
size = calculate_size(...);
```

near the point where `size` is needed.

Shorter variable lifetimes reduce cognitive load.

---

# 69. Avoid Reusing Variables for Different Meanings

Bad:

```c
int value;

value = width;
...
value = retry_count;
...
value = status;
```

Use separate variables.

A variable should have one conceptual meaning.

---

# 70. Prefer Narrow Scope

Keep:

* variables
* helper functions
* structures
* macros
* constants

as local as possible.

Do not expose internal details unnecessarily.

---

# 71. Error Messages Should Explain Action

Bad:

```text
error
failed
invalid
```

Better:

```text
failed to submit USB bulk transfer: -ETIMEDOUT
```

For user-facing errors, include enough context to diagnose the problem.

---

# 72. Avoid Defensive Programming Without Evidence

Do not add dozens of checks for impossible states merely because they are theoretically possible.

Defensive checks are valuable when they protect:

* external input
* hardware behavior
* concurrency boundaries
* public APIs
* unsafe operations

Avoid turning simple code into a forest of meaningless assertions.

---

# 73. Do Not Hide Bugs With Fallbacks

Avoid:

```c
if (operation_fails())
    use_random_fallback();
```

unless fallback behavior is actually part of the design.

A fallback should have a clear semantic purpose.

Do not add fallback paths simply to make tests pass.

---

# 74. Do Not Encode Measured Values as Truth

This is especially important for embedded and hardware testing.

Never convert:

```text
Measured once:
1234
```

into:

```c
#define EXPECTED_VALUE 1234
```

unless the value is:

* specified by protocol
* defined by hardware
* calibrated
* statistically justified
* intentionally a reference value

A measurement is not automatically a specification.

---

# 75. Hardware Code Must Preserve Evidence Quality

When writing hardware-related code:

Distinguish:

```text
Datasheet requirement
Observed behavior
Measured value
Derived value
Temporary workaround
```

Do not silently convert one into another.

---

# 76. Configuration Readability

Configuration should be immediately understandable.

Prefer:

```yaml
timeout_ms: 1000
retry_count: 3
enable_crc: true
```

over:

```yaml
t: 1000
r: 3
c: true
```

Names are especially important because configuration often lacks surrounding code context.

---

# 77. Avoid Comment Compensation

If a configuration file requires 100 lines of comments to explain 5 values, the configuration model may be poorly designed.

Consider:

* better names
* structured sections
* explicit enums
* meaningful defaults
* schema validation

Use comments only for information that cannot be encoded clearly in the structure.

---

# 78. Generated Code

When generated code is intended to be inspected by humans:

* keep formatting stable
* use meaningful names
* avoid giant generated functions when possible
* preserve source mapping
* avoid unnecessary comments
* document the generator rather than every generated line

If the file is purely machine-generated and not intended for manual editing, say so clearly.

---

# 79. Dependency Hygiene

Do not introduce a large dependency for a trivial operation.

Before adding a library, consider:

```text
Can the standard library solve it?
Can the existing project infrastructure solve it?
Can a small local implementation solve it safely?
Does the dependency introduce more complexity than it removes?
```

For embedded systems, consider:

* binary size
* RAM
* startup cost
* build complexity
* licensing
* maintenance burden

---

# 80. Performance vs Readability

Do not sacrifice readability for hypothetical performance.

Optimize when there is evidence.

Prefer:

```text
clear implementation
    ↓
measure
    ↓
identify bottleneck
    ↓
optimize targeted section
```

not:

```text
guess
    ↓
micro-optimize everything
```

When an optimization makes code less obvious, document the actual reason.

---

# 81. Low-Level Optimization Comments

Good:

```c
/*
 * Keep this copy aligned with the cache line because the DMA engine
 * reads the buffer directly.
 */
```

Bad:

```c
/*
 * This is faster because CPUs work better when...
 */
```

unless the performance claim has been measured or is guaranteed by the architecture.

---

# 82. Architecture Should Be Visible From the Top

A good file should reveal its architecture from its top-level functions.

For example:

```c
probe()
{
    init_hardware();
    init_protocol();
    init_buffers();
    register_device();
}
```

This is better than a 400-line function where all architectural phases are mixed together.

---

# 83. File Structure

Organize files so related concepts are near each other.

A typical implementation file may follow:

```text
includes
constants
types
forward declarations
small helpers
core operations
public entry points
callbacks
cleanup
```

Follow the project's existing conventions where they differ.

Do not impose a new ordering style without reason.

---

# 84. Helper Function Placement

A helper should be located near related helpers unless project conventions dictate otherwise.

Avoid scattering a feature across a file so that understanding one operation requires jumping through unrelated sections.

---

# 85. Avoid Giant Utility Files

A file called:

```text
utils.c
helpers.py
common.ts
misc.rs
```

often becomes a dumping ground.

Prefer domain-specific helpers when a utility has meaningful ownership.

---

# 86. Avoid "Miscellaneous" Abstractions

Do not create:

```text
common_helper()
misc_utils()
do_everything()
handle_data()
process()
```

without a precise domain meaning.

Names should communicate responsibility.

---

# 87. API Naming Consistency

Use consistent verbs.

For example:

```text
create / destroy
init / deinit
start / stop
enable / disable
attach / detach
register / unregister
submit / cancel
```

Do not mix:

```text
start_device()
halt_device()
device_disable()
shutdown_device()
```

unless the semantics are actually different.

---

# 88. Naming Should Encode Lifecycle

For resources, use names that communicate lifecycle.

Examples:

```text
alloc_buffer()
free_buffer()

submit_urb()
cancel_urb()

enable_irq()
disable_irq()
```

This reduces the need for comments.

---

# 89. Avoid Abbreviations Unless Established

Prefer:

```text
configuration
```

over:

```text
cfg
```

unless the project convention consistently uses `cfg`.

Established technical abbreviations are fine:

```text
usb
urb
dma
irq
drm
fb
```

---

# 90. Review Like a Maintainer

Before finalizing, mentally switch roles.

Do not ask only:

> "Does this work?"

Ask:

> "If I inherited this code six months from now, what would confuse me?"

Look for:

* hidden assumptions
* strange names
* unexplained state
* unnecessary indirection
* excessive comments
* duplicated logic
* long functions
* difficult cleanup
* implicit ownership
* magic values
* surprising side effects

Fix the highest-value problems.

---

# 91. The "Could I Modify It?" Test

Imagine a future requirement:

```text
Change timeout behavior.
Add a second hardware revision.
Add another transport.
Change retry policy.
Add another state.
Replace the backend.
```

Ask:

> Can I identify the relevant code quickly?

If not, the structure needs improvement.

---

# 92. The "Could Someone Else Debug It?" Test

Imagine the original author disappears.

Can another developer determine:

```text
What failed?
Where should they start?
What state is involved?
What owns the resource?
What assumptions exist?
Where is the hardware quirk?
```

If not, improve the code structure or add concise high-value documentation.

---

# 93. Readability Before Clever Optimization

When choosing between two implementations with equivalent behavior:

Prefer the one with:

* fewer hidden states
* fewer implicit side effects
* simpler control flow
* clearer names
* smaller interfaces
* easier testing
* easier debugging

unless there is a documented reason to choose the more complex implementation.

---

# 94. Refactoring Priority

When improving ugly working code, prioritize in this order:

```text
1. Correctness
2. Dangerous hidden behavior
3. Control-flow clarity
4. Naming
5. Ownership / lifetime
6. Function boundaries
7. Duplication
8. Abstraction quality
9. Comments
10. Formatting
```

Do not spend time polishing comments while the control flow is fundamentally unreadable.

---

# 95. Readability Debt

Treat unreadable working code as technical debt.

Symptoms include:

```text
"Don't touch this."
"Nobody knows why this works."
"Just copy this pattern."
"Changing this breaks something."
"We need three files open to understand this."
```

When such symptoms appear, improve the structure instead of adding more comments.

---

# 96. Do Not Add Comments to Rescue Bad Structure

This is an explicit rule.

Bad:

```text
Complex code
    +
200-line comment
```

Prefer:

```text
Complex code
    ↓
better structure
    ↓
meaningful names
    ↓
small helpers
    ↓
short rationale
```

Comments should supplement good structure, not compensate for bad structure.

---

# 97. Implementation vs Explanation

When something is difficult to understand, try these solutions in order:

```text
1. Better naming
2. Better control flow
3. Better function boundaries
4. Better data structures
5. Explicit state
6. Small high-value comment
7. External documentation
```

Do not immediately add a long comment.

---

# 98. Code Smell Triggers

The Agent should reconsider the implementation when it sees:

```text
- function > ~100 lines
- nesting > ~3–4 levels
- many boolean parameters
- > ~6–8 function parameters
- repeated conditionals
- repeated cleanup logic
- many global variables
- many mutable shared fields
- large switch statements
- giant context structures
- duplicated blocks
- long macros
- deeply nested ternaries
- unclear ownership
- unclear state transitions
- comments longer than the implementation they explain
```

These are heuristics, not absolute rules.

---

# 99. Complexity Budget

Every implementation has a complexity budget.

Complexity may come from:

```text
control flow
state
abstraction
concurrency
dependencies
hardware constraints
error handling
configuration
```

Do not spend complexity in one area unnecessarily.

For example:

If hardware already makes the code complex, avoid adding a complicated abstraction framework on top.

---

# 100. Preserve Simplicity

When two implementations satisfy the same requirements:

> Prefer the simpler implementation that remains maintainable.

Do not optimize for:

* architectural novelty
* abstraction count
* line-count minimization
* cleverness
* maximum genericity

Optimize for:

```text
correctness
clarity
stability
maintainability
```

---

# 101. Final Code Review Pipeline

Before delivering code, perform these passes.

## Pass 1 — Correctness

Check:

```text
[ ] Does it compile?
[ ] Does it run?
[ ] Are error paths handled?
[ ] Are resource lifetimes correct?
[ ] Are concurrency assumptions correct?
[ ] Are hardware constraints respected?
```

---

## Pass 2 — Readability

Check:

```text
[ ] Can I understand the main path quickly?
[ ] Are names meaningful?
[ ] Is nesting reasonable?
[ ] Are functions cohesive?
[ ] Is state explicit?
[ ] Are important operations visible?
```

---

## Pass 3 — Maintainability

Check:

```text
[ ] Is ownership obvious?
[ ] Is cleanup obvious?
[ ] Are APIs narrow?
[ ] Is duplication controlled?
[ ] Is abstraction justified?
[ ] Are hardware quirks localized?
[ ] Can a future change be made locally?
```

---

## Pass 4 — Comment Quality

Check:

```text
[ ] Comments explain WHY.
[ ] No conversation history.
[ ] No reasoning transcript.
[ ] No obvious comments.
[ ] No large context dumps.
[ ] No unverified claims.
[ ] Unusual constants have meaningful rationale.
```

---

## Pass 5 — Simplification

Ask:

```text
Can this be simpler?

Can this function be smaller?

Can this condition be clearer?

Can this variable be renamed?

Can this abstraction be removed?

Can this comment be deleted?

Can this state be made explicit?
```

Simplify before delivering.

---

## Pass 6 — Diff Review

Inspect the final diff.

Look for:

```text
unrelated changes
formatting noise
temporary debug code
unused variables
unused functions
dead code
temporary comments
TODOs that should not exist
accidental API changes
```

Remove them.

---

# 102. Agent Finalization Rule

The Agent must not consider the task complete immediately after obtaining a working result.

Before presenting the final code, it must perform:

```text
Working implementation
        ↓
Self-review
        ↓
Simplification
        ↓
Readability review
        ↓
Comment review
        ↓
Diff review
        ↓
Final verification
```

This review may modify the implementation even when the original implementation already passes tests.

---

# 103. What the Agent Should Optimize For

The Agent should optimize for this order:

```text
1. Correctness
2. Safety
3. Clear behavior
4. Maintainability
5. Testability
6. Simplicity
7. Performance where justified
8. Conciseness
```

Do not optimize for minimum line count.

Do not optimize for maximum abstraction.

Do not optimize for maximum comment coverage.

---

# 104. What "Good Code" Means

Good code is not necessarily:

```text
short
clever
highly abstract
heavily commented
generic
```

Good code is:

```text
easy to understand
easy to verify
easy to debug
easy to modify
hard to misuse
explicit about important constraints
appropriately simple
```

---

# 105. Final Principle

The Agent's job is not merely to produce code that the machine accepts.

The Agent is producing code that another human will inherit.

Therefore:

> **Write code for the next developer, not only for the compiler.**

When in doubt:

```text
Prefer explicit over implicit.
Prefer simple over clever.
Prefer meaningful names over comments.
Prefer local reasoning over global reasoning.
Prefer narrow interfaces over giant contexts.
Prefer clear state over hidden state.
Prefer small cohesive functions over giant functions.
Prefer justified abstractions over speculative frameworks.
Prefer verified facts over assumptions.
Prefer focused diffs over broad rewrites.
Prefer refactoring over comment dumps.
Prefer maintainability over "it works".
```

The final source code should make the important behavior obvious without requiring the reader to reconstruct the Agent's entire context window.
