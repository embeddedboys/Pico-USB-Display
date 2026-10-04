# Developer Knowledge Base Skill

## Purpose

This skill defines how an Agent should extract, compress, organize, maintain, and retrieve reusable knowledge from software and embedded development work.

The goal is **not to archive conversations**.

The goal is to build a compact, high-signal, long-lived engineering knowledge base that helps a developer:

* remember solutions
* avoid repeating mistakes
* understand important technical decisions
* reproduce useful debugging techniques
* transfer experience between projects
* quickly answer similar future questions

The knowledge base must optimize for:

> **Signal density, retrieval speed, first-glance comprehension, and long-term reuse.**

---

# 1. Core Principle

## Knowledge Base ≠ Conversation Archive

Never treat the knowledge base as a transcript of the development process.

Do not automatically preserve:

* every command executed
* every failed attempt
* every intermediate hypothesis
* long terminal output
* repetitive explanations
* obvious background knowledge
* temporary state
* irrelevant implementation details
* the entire reasoning process

Instead preserve:

```text
Problem
    ↓
Important observation
    ↓
Root cause / useful insight
    ↓
Solution
    ↓
Reusable lesson
```

The knowledge base stores the **distilled result**, not the complete journey.

---

# 2. First-Screen Rule

Every knowledge document must be understandable at first glance.

When opening a document, the reader should be able to determine within approximately 10 seconds:

1. What problem does this document address?
2. What is the conclusion?
3. What should I do?
4. When does this information apply?

Therefore every normal knowledge document should begin with a compact summary.

Preferred structure:

```markdown
# Title

> One-sentence conclusion.

## TL;DR

- Key point
- Key point
- Key point

## Solution

...

## Why

...

## Evidence / Debugging

...

## References

...
```

The first screen should contain the answer whenever practical.

Do not make the reader read 500 lines before discovering the conclusion.

---

# 3. Information Budget

Every document has an information budget.

Default target:

```text
Simple fact / solution:       20–80 lines
Normal knowledge article:     50–150 lines
Complex technical topic:    100–300 lines
Deep reference:             300+ lines only when justified
```

A document exceeding approximately 150 lines should trigger a compression review.

Before creating a long document, ask:

```text
Can this be reduced by 50% without losing the reusable knowledge?
```

If yes, reduce it.

If a document exceeds 300 lines, consider splitting it into multiple focused documents.

Do not create a 500–1000 line document merely because the Agent knows more information.

---

# 4. One Document, One Primary Question

Each document should answer one primary question.

Good:

```text
request_firmware() returns -ENOENT
```

Good:

```text
Why does filp_open() work while request_firmware() fails?
```

Good:

```text
How to debug a stuck USB URB?
```

Bad:

```text
Linux USB Development
```

containing:

* USB enumeration
* URB lifecycle
* libusb
* kernel USB driver
* firmware loader
* USB gadget
* DMA
* power management
* debugging
* udev

Large topics should be an index or a category, not a giant knowledge article.

---

# 5. Knowledge Value Classification

Before writing something into the long-term knowledge base, classify it.

## A — Durable Knowledge

Reusable across projects.

Examples:

* kernel subsystem behavior
* important API semantics
* reliable debugging techniques
* hardware/software interaction patterns
* root causes of recurring failures
* stable configuration knowledge
* architectural decisions

Action:

```text
SAVE
```

---

## B — Project Knowledge

Useful for a particular project but not necessarily universal.

Examples:

* board-specific GPIO assignments
* project-specific architecture
* firmware protocol
* custom device-tree configuration
* build system details
* project-specific hardware limitations

Action:

```text
SAVE IN PROJECT-SPECIFIC AREA
```

---

## C — Debugging Trace

Useful only for reconstructing one historical debugging session.

Examples:

* tried command A
* failed
* tried command B
* failed
* finally discovered typo
* temporary printk output
* repeated experiments without reusable insight

Action:

```text
DO NOT SAVE BY DEFAULT
```

Only preserve if the failed attempt itself teaches a reusable lesson.

---

## D — Ephemeral Information

Temporary or disposable information.

Examples:

* temporary paths
* temporary process IDs
* transient logs
* timestamps
* one-time generated output
* current machine state
* temporary test files

Action:

```text
DISCARD
```

---

# 6. Distill Before Writing

Never write raw research directly into the knowledge base.

Use this pipeline:

```text
Raw development
        ↓
Research / debugging
        ↓
Observations
        ↓
Conclusion
        ↓
Knowledge extraction
        ↓
Compression
        ↓
Knowledge-base document
```

The Agent should perform a mental synthesis step before writing.

Ask:

```text
What is the single most reusable insight here?

What would I want to know if I encountered this problem again?

Which details are historical rather than reusable?

Which details are implementation noise?

Which facts are actually verified?

Can the same knowledge be expressed in half the space?
```

---

# 7. Never Confuse Research With Knowledge

Research may be large.

The resulting knowledge should normally be much smaller.

For example:

```text
Research:
    30 files
    15 commands
    8 hypotheses
    4 failed approaches
    3 source-code investigations
    2 experiments
```

may produce:

```text
Knowledge:
    request_firmware() uses the firmware loader search mechanism,
    which is different from ordinary VFS path lookup.
```

The research process is not automatically worth recording.

---

# 8. Conclusions Come Before Background

Use an inverted-pyramid structure.

Preferred:

```text
Conclusion
    ↓
Solution
    ↓
Why
    ↓
Evidence
    ↓
Background
    ↓
References
```

Avoid:

```text
History
    ↓
Background
    ↓
Architecture
    ↓
Theory
    ↓
Experiments
    ↓
Conclusion
```

The reader should not have to earn the answer by reading the entire document.

---

# 9. Separate Facts, Observations, and Hypotheses

Knowledge documents must distinguish between:

### Verified fact

Something directly confirmed by:

* source code
* documentation
* reproducible experiment
* kernel behavior
* hardware measurement
* reliable specification

Use language such as:

```text
The driver does X.
```

---

### Observation

Something observed during an experiment.

Example:

```text
Polling below approximately 10 ms caused the controller to stop responding
during testing.
```

Prefer recording the test conditions.

---

### Hypothesis

Something not yet proven.

Use:

```text
Likely cause:
```

or:

```text
Hypothesis:
```

Do not turn a debugging guess into permanent knowledge.

---

# 10. Evidence Should Be Compact

Evidence is valuable, but raw evidence should not dominate the document.

Prefer:

```markdown
## Evidence

Observed:

- `filp_open()` succeeds
- `request_firmware()` returns `-ENOENT`
- firmware exists under `/lib/firmware`

This indicates that the VFS path itself is valid and the
firmware-loader search path is the relevant difference.
```

Avoid:

```text
300 lines of complete dmesg output
```

unless the complete output itself is important.

When logs are useful, preserve only the minimal diagnostic excerpt.

---

# 11. Failed Attempts

Do not automatically document every failed attempt.

A failed approach is worth preserving only when it teaches something reusable.

Bad:

```markdown
## Attempts

First I tried A.

Then I tried B.

Then I tried C.

Then I changed a variable.

Then I rebooted.

Then I tried D.

Finally E worked.
```

Good:

```markdown
## Failed Approach

Using `filp_open()` is not a valid replacement for
`request_firmware()` when the driver is expected to use
the firmware loader.

This distinction is useful when debugging firmware-loading failures.
```

The lesson is important.

The chronology is not.

---

# 12. Avoid Reasoning Transcripts

Never store chain-of-thought or internal reasoning.

Do not write:

```text
I initially thought X.

Then I considered Y.

Then I wondered whether Z.

After thinking about it, I realized...
```

Replace this with:

```text
## Root Cause

The failure is caused by X.

Y is unrelated because ...

```

Store the result of reasoning, not the private reasoning process.

---

# 13. Command Output Policy

Commands are useful only when they are part of a reusable procedure.

Prefer:

```bash
dmesg | grep -i firmware
```

over:

```text
$ dmesg
[1234.1234] ...
[1234.1235] ...
...
```

If exact output matters, include only the relevant lines.

Commands should normally answer one of these questions:

* How do I reproduce the problem?
* How do I diagnose the problem?
* How do I verify the fix?
* How do I inspect the relevant state?

If a command does none of these, consider removing it.

---

# 14. Code Examples

Code should be minimal and directly relevant.

Good:

```c
ret = request_firmware(&fw, "dummy-firmware.bin", dev);
```

Bad:

Including an entire 500-line driver when only one API call matters.

When a complete implementation is required, store it separately from the conceptual knowledge article.

---

# 15. Project Knowledge vs General Knowledge

Do not mix project-specific information with general technical knowledge.

For example:

```text
Linux DRM atomic modesetting
```

is general knowledge.

```text
Our RK3588 board uses 480x800 MIPI DSI panel X
```

is project knowledge.

Keep them separate.

Recommended structure:

```text
developer-knowledge/
├── general/
│   ├── linux/
│   ├── kernel/
│   ├── usb/
│   ├── drm/
│   ├── embedded/
│   └── build-systems/
│
├── projects/
│   ├── project-a/
│   ├── project-b/
│   └── project-c/
│
└── troubleshooting/
```

---

# 16. Prefer Atomic Knowledge

Knowledge should be reusable as independent units.

Prefer:

```text
usb/request-firmware-path.md
usb/urb-cancel.md
usb/interface-binding.md
```

over:

```text
usb/complete-usb-driver-development-guide.md
```

Atomic documents improve:

* retrieval
* maintenance
* reuse
* linking
* future Agent context selection

---

# 17. Avoid Duplicate Knowledge

Before creating a new document:

1. Search the knowledge base.
2. Determine whether an existing document already contains the concept.
3. Update the existing document when appropriate.
4. Create a new document only when the topic is meaningfully distinct.

Do not create:

```text
usb-urb-debug.md
usb-urb-debugging.md
usb-debug-urb.md
usb-urb-problem.md
```

if they all describe the same knowledge.

Prefer one canonical document.

---

# 18. Canonical Knowledge

When several documents discuss the same concept, establish one canonical source.

Other documents should link to it rather than duplicating its contents.

Example:

```markdown
See [[USB URB Lifecycle]] for the general mechanism.

This document only covers the project-specific cancellation issue.
```

Avoid copying the entire explanation.

---

# 19. Knowledge Compression Pass

Before saving a document, perform a dedicated compression pass.

Check every paragraph:

```text
Does this sentence contain reusable information?

Does it support the conclusion?

Does it explain why the solution works?

Does it help reproduce or verify the solution?

Does it distinguish an important limitation?

If none apply:
    DELETE IT.
```

Then perform a second pass:

```text
Can two paragraphs become one?

Can a paragraph become a bullet list?

Can a verbose explanation become one sentence?

Can background information become a link?
```

---

# 20. 50% Compression Test

After drafting:

> Try to reduce the document by approximately 50%.

Do not mechanically cut half the words.

Instead identify:

* duplicated explanations
* historical narrative
* obvious facts
* unnecessary background
* repeated commands
* redundant examples
* excessive source-code excerpts
* low-value context

If compression removes essential meaning, restore only the necessary material.

---

# 21. First 20 Lines Rule

The first approximately 20 lines should contain the highest-value information.

A preferred structure:

```markdown
# Topic

> One-sentence conclusion.

## TL;DR

- ...
- ...
- ...

## Solution

1. ...
2. ...
3. ...

## Why

...
```

Detailed material comes later.

---

# 22. TL;DR Requirements

The TL;DR should not be a vague summary.

Bad:

```text
This document discusses firmware loading and related issues.
```

Good:

```text
`request_firmware()` does not use the same path lookup mechanism
as `filp_open()`.

If `filp_open()` succeeds but `request_firmware()` returns `-ENOENT`,
check the firmware-loader search path, especially `/lib/firmware`.
```

The TL;DR should be actionable.

---

# 23. Recommended Document Template

Use this template for most technical knowledge.

```markdown
# <Problem / Concept>

> <One-sentence conclusion>

## TL;DR

- <Key fact>
- <Key fact>
- <Key action>

## Problem

<Short description of the problem.>

## Solution

<Minimal reproducible solution.>

## Why

<Explanation of the important mechanism.>

## Debugging

<Only reusable diagnostic commands or observations.>

## Caveats

<Important limitations, if any.>

## Related

- [[Related Topic]]
- [[Related Topic]]

## References

- <Official documentation / source>
```

Not every section is mandatory.

Do not create empty sections just to follow the template.

---

# 24. Simple Problem Template

For very small discoveries, use an even smaller format.

```markdown
# <Problem>

> <Conclusion>

## Solution

<solution>

## Why

<short explanation>

## Verify

<verification command or observation>
```

A one-minute discovery should not become a 300-line article.

---

# 25. Troubleshooting Template

Use this for reusable debugging knowledge.

```markdown
# <Failure>

> <Root cause / key conclusion>

## Symptom

<What happens>

## Root Cause

<Verified cause>

## Fix

<Minimal fix>

## Diagnosis

<How to distinguish this failure from similar failures>

## Verification

<How to confirm the fix>

## Lesson

<Reusable engineering lesson>
```

---

# 26. Architecture / Design Template

For architectural decisions:

```markdown
# <Decision>

> <Decision summary>

## Context

<Why the decision was needed>

## Decision

<What was chosen>

## Reasons

- ...
- ...
- ...

## Alternatives

- <Alternative A> — why it was not selected
- <Alternative B> — why it was not selected

## Trade-offs

<Important costs and benefits>

## Consequences

<What this decision means for future development>
```

Do not turn architectural records into generic tutorials.

---

# 27. Experimental Knowledge

For hardware and embedded experiments, preserve:

* hardware configuration
* relevant software version
* test conditions
* measured result
* conclusion

Do not preserve every experiment step unless necessary for reproduction.

Preferred:

```markdown
## Experiment

Board: RP2350
Firmware: ...
SPI: 12 MHz
Display: ST7735R 128x160

Result:

12 MHz is stable with this display configuration.

Above this frequency, corruption was observed.

## Conclusion

The tested configuration should remain at or below 12 MHz.
```

---

# 28. Hardware Measurements

For oscilloscope, logic analyzer, current measurement, or timing experiments, record:

```text
What was measured?
Under what conditions?
What was observed?
What conclusion follows?
```

Avoid dumping screenshots or waveforms unless they contain information that cannot reasonably be expressed as text.

---

# 29. Version-Sensitive Knowledge

Clearly identify information that depends on versions.

Example:

```markdown
## Environment

- Linux kernel: 6.6
- Ubuntu: 24.04
- clang: 18
```

Do not present version-specific behavior as universal behavior.

Use:

```text
In Linux 6.6 ...
```

rather than:

```text
Linux always does ...
```

when the behavior is version-dependent.

---

# 30. Time-Sensitive Information

Avoid putting temporary information into durable knowledge unless it has historical value.

Examples:

```text
Current package version
Current website UI
Temporary branch
Temporary IP address
Temporary device node
Temporary build directory
```

If it matters to a project, store it in project documentation instead.

---

# 31. Secrets and Sensitive Data

Never store:

* passwords
* API keys
* tokens
* private keys
* access credentials
* authentication cookies
* personal secrets
* unnecessary personal information

Redact sensitive values:

```text
API_KEY=<REDACTED>
```

Do not copy credentials from terminal output into the knowledge base.

---

# 32. Desensitization

When converting personal development experience into reusable knowledge:

Remove:

* usernames
* home directories
* private IP addresses
* serial numbers
* customer information
* proprietary names
* private repository URLs
* credentials
* internal hostnames
* unrelated personal information

Replace them with neutral identifiers:

```text
/home/<user>/
<PROJECT_ROOT>
<BOARD>
<DEVICE>
<HOST>
```

Preserve the technical meaning.

---

# 33. Naming

Use descriptive, stable filenames.

Prefer:

```text
request-firmware-search-path.md
usb-urb-cancellation.md
drm-plane-atomic-update.md
rp2350-usb-device-enumeration.md
```

Avoid:

```text
issue1.md
problem.md
test.md
notes.md
misc.md
new.md
final.md
```

Use lowercase kebab-case unless the existing knowledge base has a different established convention.

---

# 34. Titles

Titles should describe the knowledge, not the conversation.

Bad:

```text
What we discussed yesterday about USB
```

Good:

```text
USB URB Cancellation During Device Removal
```

Bad:

```text
Interesting thing I discovered
```

Good:

```text
Why request_firmware() Can Fail While filp_open() Succeeds
```

---

# 35. Tags

Use tags sparingly.

Prefer a small controlled vocabulary:

```text
#linux
#kernel
#usb
#drm
#embedded
#rp2350
#debugging
```

Do not create dozens of nearly identical tags.

Tags should improve retrieval, not become another taxonomy project.

---

# 36. Links

Link related knowledge when the relationship is useful.

Good:

```markdown
See [[USB URB Lifecycle]] for the general URB model.
```

Avoid excessive linking.

A document containing 50 links is not necessarily more useful than one containing 3 relevant links.

---

# 37. References

Prefer authoritative sources:

1. official documentation
2. source code
3. kernel documentation
4. hardware datasheets
5. standards
6. reliable technical references

Do not copy large amounts of external documentation into the knowledge base.

Store the conclusion and link to the source.

---

# 38. Source Code as Evidence

When a conclusion depends on source code, record:

```text
file
function
important behavior
```

Example:

```markdown
The behavior is implemented in:

`drivers/base/firmware_loader/main.c`

Relevant path:
`request_firmware()` → firmware loader → userspace/filesystem lookup.
```

Do not automatically copy the entire source file.

---

# 39. Retrieval-Oriented Writing

The knowledge base is primarily read through search.

Therefore documents should contain the terms a future developer is likely to search for.

Include:

* API names
* error codes
* subsystem names
* important kernel symbols
* hardware model
* common symptom
* relevant command
* likely terminology

Example:

```markdown
# request_firmware() ENOENT

Keywords:

- request_firmware
- -ENOENT
- firmware loader
- /lib/firmware
- firmware_class
```

Do not add keywords solely for SEO-like keyword stuffing.

---

# 40. Error Codes Are Valuable Anchors

When documenting failures, include exact error codes where relevant.

Examples:

```text
-ENOENT
-ENODEV
-EPROBE_DEFER
-EBUSY
-ETIMEDOUT
```

These often provide much better future retrieval than vague descriptions such as:

```text
device doesn't work
```

---

# 41. Do Not Over-Explain Obvious Concepts

Assume the reader is an experienced developer unless the document is explicitly educational.

Do not explain basic concepts unnecessarily.

For example, if the document is about a Linux DRM driver:

Do not spend 100 lines explaining what a Linux kernel module is.

Focus on the non-obvious part.

---

# 42. Deep Explanations

Deep explanations are allowed when the concept is genuinely difficult.

Examples:

* Linux DRM atomic state
* USB URB lifetime
* DMA coherency
* kernel probe ordering
* device-tree dependency resolution
* Wayland compositor architecture

But even deep documents must start with a concise summary.

Use:

```text
TL;DR
    ↓
Mental model
    ↓
Important details
    ↓
Implementation
    ↓
References
```

Never use complexity as an excuse for poor information architecture.

---

# 43. Split Large Documents

Split a document when:

* it answers multiple independent questions
* different sections are retrieved independently
* sections have different audiences
* one section can stand alone
* the document exceeds the information budget
* the TL;DR becomes too long

Example:

Instead of:

```text
RK3588 Graphics Everything.md
```

use:

```text
rk3588-vop.md
rk3588-rga.md
rk3588-drm.md
rk3588-mali.md
rk3588-wayland.md
```

---

# 44. Avoid Recursive Knowledge Growth

A critical rule:

> Existing knowledge must not automatically become new knowledge.

When an Agent retrieves a large document while solving a problem, it must not summarize the entire retrieved document into the new document.

Only preserve the new information.

Before writing, ask:

```text
What did this investigation add that is not already known?
```

If the answer is:

```text
Nothing
```

do not create a new document.

If the answer is:

```text
One new fact
```

update the canonical document rather than duplicating it.

---

# 45. Prevent Knowledge Inflation

When updating an existing document:

Do not simply append new information.

Instead:

```text
Read existing document
        ↓
Identify new knowledge
        ↓
Merge with existing knowledge
        ↓
Remove obsolete/redundant material
        ↓
Compress
        ↓
Save
```

Never use:

```text
cat >> knowledge.md
```

as the default knowledge-management strategy.

---

# 46. Prefer Replacement Over Append

Bad:

```markdown
## Update 2026-10-02

Another discovery...

## Update 2026-10-03

Another discovery...

## Update 2026-10-04

Another discovery...
```

This creates a development diary.

Better:

```markdown
## Conclusion

<updated canonical knowledge>
```

If historical chronology matters, keep it in a separate development log.

---

# 47. Knowledge vs Journal

The knowledge base and development journal serve different purposes.

### Journal

Answers:

```text
What happened?
What did I try?
When did I try it?
```

### Knowledge base

Answers:

```text
What do I know now?
What should I remember?
How do I solve this next time?
```

Do not merge them.

---

# 48. When to Create a Knowledge Entry

Create a durable entry when at least one of these is true:

* the solution is likely to be reused
* the issue was difficult to diagnose
* the behavior was non-obvious
* the discovery contradicts an intuitive assumption
* the solution prevents future wasted time
* the knowledge applies to multiple projects
* the result documents an important architectural decision
* the result captures a valuable hardware limitation

Do not create an entry merely because something happened.

---

# 49. When NOT to Create a Knowledge Entry

Do not create one for:

* trivial syntax errors
* obvious typos
* one-time commands
* temporary debugging state
* routine build failures
* information already documented elsewhere
* information that is easy to rediscover
* conversation filler
* generic background knowledge
* speculative conclusions

---

# 50. Knowledge Quality Test

Before saving, evaluate the document using these questions:

```text
1. Is there a clear conclusion?

2. Is the conclusion visible immediately?

3. Is the problem clearly defined?

4. Is the solution actionable?

5. Is the root cause separated from speculation?

6. Are facts distinguishable from observations?

7. Did I remove debugging history that has no reusable value?

8. Did I remove unnecessary background?

9. Did I remove duplicated information?

10. Can this document be shortened?

11. Does it belong in the knowledge base at all?

12. Does an existing document already contain this knowledge?

13. Is the information sensitive?

14. Is the information version-specific?

15. Would another developer actually search for this later?
```

If several answers are negative, revise before saving.

---

# 51. Knowledge Quality Levels

Use the following mental model.

## Level 0 — Raw Notes

```text
Everything that happened.
```

Do not put directly into the knowledge base.

---

## Level 1 — Organized Notes

```text
Problem
Attempts
Result
```

Acceptable for temporary project notes.

---

## Level 2 — Distilled Knowledge

```text
Problem
Conclusion
Solution
Why
Evidence
```

Preferred for normal knowledge.

---

## Level 3 — High-Value Knowledge

```text
Reusable insight
Minimal solution
Root cause
Diagnostic method
Important caveats
Related knowledge
```

This is the target for the long-term knowledge base.

---

# 52. Agent Output Strategy

When the user asks the Agent to save knowledge, the Agent should not immediately write.

Use:

```text
1. Identify the primary insight.
2. Search for existing related knowledge.
3. Determine whether the knowledge is new.
4. Classify it as A/B/C/D.
5. Extract only reusable information.
6. Draft a concise document.
7. Run compression.
8. Check for duplication.
9. Check for sensitive information.
10. Save or update the canonical document.
```

---

# 53. Default Writing Behavior

Unless explicitly requested otherwise:

```text
Be concise.
Prefer facts over narrative.
Prefer conclusions over chronology.
Prefer actionable information.
Prefer small documents.
Prefer links over duplication.
Prefer tables over repetitive prose when appropriate.
Prefer bullets for compact factual lists.
Prefer code snippets over large source dumps.
Prefer verified information over speculation.
```

---

# 54. User-Requested Deep Documentation

If the user explicitly asks for a detailed tutorial, reference, architecture document, or complete investigation record, the information budget may be relaxed.

However:

Even long documents must have:

```text
TL;DR
Conclusion
Navigation
```

at the beginning.

Do not interpret:

```text
"be comprehensive"
```

as:

```text
"include every piece of information you know."
```

Comprehensive means:

> Cover all important aspects while removing irrelevant detail.

---

# 55. Explicit "Detailed" Mode

When detailed documentation is requested, use:

```markdown
# Topic

> Short conclusion

## TL;DR

...

## Table of Contents

...

## Problem

...

## Architecture / Mental Model

...

## Solution

...

## Detailed Explanation

...

## Debugging

...

## Experiments

...

## Limitations

...

## References
```

The summary must still come first.

---

# 56. Document Length Escalation

If the Agent believes a document needs to be unusually long:

First ask:

```text
Can the content be split into independent documents?
```

If yes:

```text
Split.
```

If no:

```text
Keep the document together,
but maintain a strong summary and navigation structure.
```

If still larger than approximately 500 lines:

```text
Explicitly justify the size.
```

---

# 57. Example: Bad Knowledge Entry

Avoid this:

```markdown
# USB Firmware Loading Investigation

Yesterday I was working on the dummy driver.
First I compiled the module.
Then I loaded it.
Then I noticed the firmware was not found.
I checked dmesg.
Then I tried filp_open().
That worked.
I thought maybe request_firmware was broken.
I looked at the source code.
Then I checked /lib/firmware.
Then I read about udev.
Then I checked...
```

This is a journal.

---

# 58. Example: Good Knowledge Entry

Prefer:

````markdown
# Why request_firmware() Fails While filp_open() Succeeds

> `request_firmware()` uses the kernel firmware-loader mechanism,
> while `filp_open()` performs ordinary VFS path lookup.

## TL;DR

- `filp_open()` succeeding does not prove `request_firmware()` can find the file.
- Check the firmware-loader search path.
- For a normal system firmware file, `/lib/firmware` is the expected location.

## Solution

Place:

    dummy-firmware.bin

under the configured firmware search path, then reload the driver.

## Why

The two APIs use different mechanisms.

`filp_open()` directly resolves a filesystem path.

`request_firmware()` goes through the firmware loader.

Therefore the same filename can behave differently through the two APIs.

## Debugging

```bash
dmesg | grep -i firmware
````

Check the configured firmware path and whether the file is visible there.

## Lesson

When debugging firmware-loading failures, distinguish:

```
VFS file access
```

from:

```
firmware-loader lookup
```

````

This is the target quality.

---

# 59. The "Would I Search This?" Test

Before saving a document, imagine six months from now.

Ask:

> If I encounter the same problem again, would I search for this?

If the answer is no:

```text
Do not save it.
````

If yes:

```text
Make the title and content match the search terms
the future developer would actually use.
```

---

# 60. The "Future Me" Test

The document should help a future developer act.

A good document allows:

```text
Future me:
    "I remember this problem."

Search:
    finds document

First screen:
    shows conclusion

Solution:
    tells me what to do

Why:
    prevents me from making the wrong assumption again

Debugging:
    lets me verify it

Done.
```

If instead the workflow is:

```text
Search
 ↓
500 lines
 ↓
20 minutes reading
 ↓
still unclear
```

the document has failed.

---

# 61. Final Rule

The most important rule of this skill is:

> **Capture knowledge, not volume.**

A 20-line document containing one hard-won reusable insight is more valuable than a 1000-line document containing the entire history of discovering it.

Optimize the knowledge base for:

```text
High signal
Low noise
Fast retrieval
Immediate comprehension
Long-term reuse
Minimal duplication
```

When in doubt:

```text
Delete.
Compress.
Split.
Link.
Keep the conclusion.
```

Never make the developer read 1000 lines to discover the five lines that matter.
