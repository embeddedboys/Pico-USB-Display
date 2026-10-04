# Developer Testing Skill

## Purpose

This skill defines how an AI coding agent should design, implement, maintain, and reuse project test tools.

The goal is not merely to make individual tests work.

The goal is to build a **consistent, reusable, inspectable, and trustworthy test system** over time.

The agent must behave as a maintainer of the project's testing infrastructure, not as a person writing isolated one-off scripts.

---

# 1. Core Principles

These principles are mandatory unless the user explicitly overrides them.

## 1.1 Reuse before creation

Before creating a new test script or helper tool:

1. Inspect the existing test infrastructure.
2. Search for existing tools with similar capabilities.
3. Search for existing tests that solve a related problem.
4. Prefer reusing an existing tool.
5. Prefer extending an existing tool over creating another similar tool.
6. Create a new tool only when existing tools cannot reasonably support the requirement.

Do not repeatedly create tools such as:

```text
test_usb.py
test_usb2.py
test_usb_new.py
usb_check.py
usb_verify.py
usb_probe_test.py
```

when one reusable `usbctl` or equivalent tool could provide the required functionality.

The test infrastructure should evolve toward a small number of composable tools rather than a large number of task-specific scripts.

---

## 1.2 Observation is not expectation

This is one of the most important rules in this skill.

A value observed during a test run is **not automatically an expected value**.

Never transform:

```text
measured value
```

into:

```text
expected value
```

without an explicit justification.

For example, this is suspicious:

```python
value = measure_sensor()

assert value == 1837
```

if `1837` was obtained only because the device happened to report `1837` during an earlier run.

The agent must be able to explain where `1837` came from.

Valid sources for an expected value include:

* hardware specification
* protocol specification
* software specification
* project requirement
* explicitly documented invariant
* approved golden/reference data
* statistically established baseline
* user-provided acceptance criterion
* mathematically derived expected value

An observed value from one run is not sufficient.

---

## 1.3 Never silently invent test criteria

The agent must not silently invent:

* expected values
* acceptable ranges
* tolerances
* timing limits
* retry counts
* golden values
* baseline values
* pass/fail thresholds

If the test requires a value that is not defined by the project, the agent should either:

1. derive it from an explicit documented rule,
2. ask the user,
3. use an explicitly defined configurable parameter,
4. or report the result as `INCONCLUSIVE` / observation-only.

Do not manufacture a specification from the first successful experiment.

---

# 2. Test Model

Every test should conceptually follow this model:

```text
Measurement
    ↓
Observation
    ↓
Oracle
    ↓
Evaluation
    ↓
PASS / FAIL / INCONCLUSIVE
```

Do not collapse all of these stages into a single operation.

For example:

```python
observation = measure_sensor()

expectation = load_specification()

result = evaluate(observation, expectation)
```

is preferable to:

```python
if measure_sensor():
    print("PASS")
```

---

# 3. Observation / Expectation / Decision

Every automated test should distinguish three concepts.

## 3.1 Observation

What the system actually measured or observed.

Examples:

```text
sample_rate = 98.7 Hz
voltage = 3.31 V
temperature = 42.7 °C
packet_count = 987
device_present = true
response_time = 31 ms
```

Observations are facts about a particular run.

---

## 3.2 Expectation

What the system is supposed to do.

Examples:

```text
sample_rate: 95..105 Hz
voltage: 3.2..3.4 V
packet_count >= 950
response_time < 100 ms
device_present == true
```

Expectations must have an identifiable source.

---

## 3.3 Decision

The result of comparing the observation with the expectation.

Allowed outcomes:

```text
PASS
FAIL
INCONCLUSIVE
ERROR
```

Do not force every experiment into PASS/FAIL.

---

# 4. Test Oracles

Every test that produces PASS/FAIL should have a clearly identifiable oracle.

## 4.1 Oracle types

The preferred oracle types are:

### SPEC

Value or range defined by a hardware/software/protocol specification.

```text
sample_rate = 100 ± 5 Hz
```

### REQUIREMENT

Explicit project requirement.

```text
boot time < 3 seconds
```

### INVARIANT

A property that must always hold.

```text
packet_length >= 0
```

```text
voltage must never be negative
```

### RELATIONSHIP

A relationship between measurements.

```text
output_voltage ≈ input_voltage
```

```text
counter_after > counter_before
```

### GOLDEN

Comparison against an explicitly approved reference artifact.

The reference must be identifiable and versioned.

### BASELINE

Comparison against a statistically established baseline.

The baseline must have a documented source and methodology.

### USER_DEFINED

An acceptance criterion explicitly provided by the user.

### NONE

No reliable oracle exists.

The test may collect and report observations, but must not automatically declare PASS/FAIL.

---

# 5. Oracle Declaration

Whenever practical, the test should make the oracle visible.

For example:

```python
# ORACLE: SPEC
# SOURCE: docs/device-spec.md
# EXPECTED: 95..105 Hz
```

or:

```yaml
oracle:
  type: SPEC
  source: docs/device-spec.md
  field: sample_rate_hz
  min: 95
  max: 105
```

For a relationship:

```python
# ORACLE: RELATIONSHIP
# REQUIREMENT: output must track input within 2%
```

For observation-only tests:

```python
# ORACLE: NONE
# This test collects measurements only.
```

---

# 6. The Single-Measurement Trap

The following pattern is forbidden unless explicitly justified:

```python
observed = measure()

EXPECTED = observed
```

or:

```python
observed = measure()

assert observed == 1234
```

when `1234` was obtained from a previous measurement.

Also forbidden:

```python
first_run = measure()

# Later:
assert measure() == first_run
```

unless the test is explicitly designed as a repeatability test and the relationship is the actual requirement.

A baseline is not automatically a specification.

A golden value is not automatically a specification.

A previous measurement is not automatically a golden value.

---

# 7. Baselines

Baselines are useful, but must be treated differently from specifications.

A baseline should record:

```yaml
baseline:
  value: 99.8
  tolerance: 2.0
  unit: Hz

  source: approved-device-sample
  sample_count: 100

  created: 2026-10-02
  methodology: median-of-100-runs
```

The agent must not create a baseline merely because a value was observed once.

A baseline should answer:

```text
What was measured?
How many samples were used?
On what device/environment?
Using which firmware/software version?
How was the baseline calculated?
Who/what approved it?
What tolerance was selected and why?
```

If these questions cannot be answered, treat the value as an observation rather than a baseline.

---

# 8. Golden Data

Golden/reference data must be explicitly approved.

Never silently create:

```text
golden.bin
golden.json
expected_output.txt
```

from the first successful test run.

A golden artifact should have provenance.

For example:

```yaml
golden:
  artifact: tests/golden/device_v1.bin
  source: reference-device
  firmware: abc123
  generated_at: 2026-10-02
  approved: true
```

If approval has not occurred, it is not a golden reference.

---

# 9. INCONCLUSIVE Is a Valid Result

Not every test has enough information to produce PASS/FAIL.

Use:

```text
INCONCLUSIVE
```

when:

* the measurement succeeded but no reliable oracle exists
* required specification data is missing
* the environment is unsuitable for a reliable judgment
* a baseline has not yet been established
* the result requires human review
* the test intentionally collects data for later analysis

Example:

```text
[TEST] USB sensor sample rate
[MEASURE] 98.7 Hz
[ORACLE] NONE
[RESULT] INCONCLUSIVE
```

This is better than inventing:

```text
PASS
```

---

# 10. Tool vs Test

Keep reusable measurement tools separate from tests.

## Tools

Tools acquire or manipulate facts.

Examples:

```text
serialctl
usbctl
i2cctl
spictl
sysfsctl
devicectl
flashctl
```

A tool should answer questions such as:

```text
What device is present?
What bytes were received?
What is the measured voltage?
What does the device report?
```

## Tests

Tests interpret observations.

For example:

```text
tests/
    usb_sensor/
    display/
    serial/
    boot/
```

A test may use:

```bash
serialctl capture ...
usbctl probe ...
```

rather than implementing serial/USB handling again.

---

# 11. Reusable Tool Design

A reusable tool should be:

* deterministic
* scriptable
* composable
* machine-readable
* reasonably self-documenting
* independent of one particular test when possible

Avoid embedding project-specific test conclusions inside generic measurement tools.

Bad:

```bash
sensorctl --check-normal
```

when "normal" depends on a particular test.

Prefer:

```bash
sensorctl read --json
```

and let the test decide what constitutes acceptable behavior.

---

# 12. CLI Conventions

Use the project's existing conventions when they exist.

If no convention exists, prefer:

```text
tool <command> [options]
```

Common options:

```text
--help
--version
--json
--quiet
--verbose
--timeout
```

Examples:

```bash
serialctl capture /dev/ttyUSB0 --timeout 5
```

```bash
usbctl probe --json
```

```bash
sensorctl read --json
```

Do not create incompatible command-line conventions for every new tool.

---

# 13. Machine-Readable Output

Reusable tools should support machine-readable output where practical.

Prefer JSON for structured data.

Example:

```json
{
  "tool": "sensorctl",
  "version": "1.2.0",
  "status": "ok",
  "device": "/dev/sensor0",
  "observation": {
    "sample_rate_hz": 98.7,
    "samples": 987
  }
}
```

Human-readable output may still be supported:

```text
sensor: /dev/sensor0
sample rate: 98.7 Hz
samples: 987
```

But tests should preferably consume structured output rather than parsing human-oriented text.

---

# 14. Exit Codes

Use consistent exit codes across tools.

If the project already defines an exit-code convention, follow it.

Otherwise use a documented convention such as:

```text
0 = PASS / successful operation
1 = FAIL
2 = INVALID_USAGE
3 = ENVIRONMENT_ERROR
4 = TIMEOUT
5 = INCONCLUSIVE
```

Do not silently change an existing project's exit-code semantics.

---

# 15. Error vs FAIL

Distinguish test failure from infrastructure failure.

For example:

```text
Device reports wrong value
    → FAIL

Serial device does not exist
    → ENVIRONMENT_ERROR

Serial read timed out
    → TIMEOUT

Specification unavailable
    → INCONCLUSIVE

Invalid command line
    → INVALID_USAGE
```

Do not report:

```text
serial device missing → test FAIL
```

unless the test requirement explicitly states that device absence is the condition being tested.

---

# 16. Measurement and Parsing

Separate measurement from interpretation where practical.

Prefer:

```text
device
  ↓
measurement
  ↓
parser
  ↓
structured observation
  ↓
test oracle
```

rather than:

```text
device
  ↓
huge test script containing
measurement + parsing + assumptions + assertions
```

Reusable parsing logic should live in reusable tools.

---

# 17. Test Script Style

Tests should be easy to read.

Prefer:

```python
observation = measure()

expectation = load_expectation()

result = evaluate(observation, expectation)

report(result)
```

over deeply nested procedural code.

The main test should make the test logic obvious.

A reviewer should be able to answer within seconds:

```text
What is being measured?
What is expected?
Why is it expected?
How is PASS/FAIL determined?
```

---

# 18. Explicit Units

Every physical measurement must have an explicit unit.

Avoid:

```python
temperature = 42
```

Prefer:

```python
temperature_c = 42.0
```

or:

```json
{
  "value": 42.0,
  "unit": "degC"
}
```

Do not compare values with different units.

For example:

```text
mV vs V
us vs ms
Hz vs kHz
bytes vs KiB
```

must be handled explicitly.

---

# 19. Tolerances

Tolerances must have a reason.

Bad:

```python
assert abs(actual - expected) < 10
```

if `10` was chosen arbitrarily.

Prefer:

```python
# REQUIREMENT: ±2%
assert abs(actual - expected) <= expected * 0.02
```

or:

```python
# SPEC: 3.3V ± 0.1V
assert 3.2 <= voltage <= 3.4
```

Never silently introduce a "reasonable" tolerance.

---

# 20. Timing Tests

Timing measurements are especially vulnerable to accidental hard-coded assumptions.

Do not do:

```python
assert boot_time < 1.234
```

because a previous run happened to take `1.234s`.

Instead derive timing criteria from:

* explicit requirement
* documented performance target
* protocol requirement
* statistically established baseline
* user-defined acceptance criterion

Record environmental information when timing matters:

```text
CPU
kernel
firmware
CPU governor
temperature
load
storage
memory pressure
```

if relevant to interpretation.

---

# 21. Hardware Tests

Hardware tests should distinguish:

```text
device detection
device communication
measurement
functional behavior
performance
stress behavior
```

Do not treat successful communication as proof of correct functionality.

For example:

```text
I2C ACK
```

proves that something acknowledged the address.

It does not automatically prove:

```text
sensor is functioning correctly
```

Similarly:

```text
USB enumeration succeeded
```

does not automatically prove:

```text
USB data path is correct
```

---

# 22. Serial Tests

Serial-related tests should use reusable serial infrastructure when available.

Avoid repeatedly implementing:

```python
open()
select()
read()
timeout()
decode()
reset()
```

inside every test.

Prefer:

```bash
serialctl capture ...
```

or a shared library.

Serial parsing should distinguish:

```text
transport failure
timeout
invalid data
expected application message
application failure
```

Do not interpret any arbitrary output as PASS.

---

# 23. USB Tests

USB tests should distinguish:

```text
physical presence
enumeration
descriptor correctness
endpoint availability
control transfer
bulk/interrupt/isochronous transfer
protocol correctness
functional behavior
performance
```

For example:

```text
USB device exists
```

is not equivalent to:

```text
USB device works correctly
```

Tests should specify which layer is being tested.

---

# 24. Embedded-System Tests

For embedded projects, consider the complete chain:

```text
build
 ↓
flash
 ↓
reset
 ↓
boot
 ↓
enumeration
 ↓
communication
 ↓
measurement
 ↓
functional test
 ↓
result collection
```

Reusable automation should separate these phases where possible.

For example:

```text
flashctl
serialctl
usbctl
testctl
```

rather than one enormous script containing every operation.

---

# 25. Reset and Reproducibility

Tests involving hardware should clearly define reset behavior.

A test should not silently depend on:

```text
device happened to already be running
```

When appropriate, explicitly perform:

```text
reset
wait for boot
wait for enumeration
run test
collect result
```

Record relevant state.

---

# 26. Environment Validation

Before running a hardware test, validate required resources.

Examples:

```text
device exists
USB permission available
serial port accessible
required executable installed
firmware image exists
expected kernel interface exists
```

Do not confuse missing infrastructure with device failure.

---

# 27. Test Isolation

Tests should minimize hidden dependencies on previous tests.

Avoid:

```text
test B only works because test A left the device configured
```

Prefer explicit setup:

```text
setup
test
cleanup
```

When a test intentionally depends on previous state, document that dependency.

---

# 28. Cleanup

Tests should clean up resources they own:

```text
temporary files
processes
USB handles
serial connections
mounts
loop devices
background processes
temporary kernel modules
```

Do not leave the system in an unexpected state unless explicitly required.

---

# 29. Temporary Experiments

Not every experiment deserves to become a permanent test.

For exploratory work, it is acceptable to create:

```text
scratch/
experiments/
tmp/
```

or another project-defined location.

But exploratory scripts should not automatically become part of the permanent test suite.

When an experiment proves useful, extract the reusable part into a proper tool or test.

---

# 30. Promotion From Experiment to Test

A temporary experiment may become a permanent regression test only when:

1. The behavior being tested is clearly defined.
2. The oracle is known.
3. Expected values have a documented source.
4. The test is reproducible.
5. Required environment is understood.
6. Output and exit-code behavior follow project conventions.
7. The test has a stable name and location.

---

# 31. Naming

Names should describe what is tested.

Prefer:

```text
test_usb_enumeration
test_sensor_sample_rate
test_display_framebuffer
test_boot_time
test_serial_protocol
```

Avoid:

```text
test_new
test_final
test_final2
test_temp
test_fix
test_working
```

Tools should use stable functional names:

```text
serialctl
usbctl
flashctl
sensorctl
testctl
```

---

# 32. Directory Structure

Follow the project's existing structure.

If none exists, a reasonable structure is:

```text
tests/
    README.md
    common/
    unit/
    integration/
    hardware/
    regression/
    golden/

tools/
    serialctl/
    usbctl/
    flashctl/
    sensorctl/
```

Do not create a new hierarchy when the project already has a consistent one.

---

# 33. Shared Libraries

If multiple tests duplicate logic, consider extracting a library.

For example:

```text
tests/common/serial.py
tests/common/usb.py
tests/common/assertions.py
tests/common/report.py
```

Do not immediately abstract every tiny function.

The goal is to remove meaningful duplication, not to create an over-engineered framework.

---

# 34. Avoid Premature Frameworks

Do not build a large testing framework for a simple project.

Prefer:

```text
small reusable CLI tools
+
small test scripts
+
simple conventions
```

over introducing a large dependency stack without a demonstrated need.

For embedded/Linux projects, favor low-dependency solutions when practical.

---

# 35. Dependency Policy

Before adding a dependency:

1. Check whether the project already uses it.
2. Check whether the standard library can solve the problem.
3. Check whether an existing project tool already provides the functionality.
4. Consider whether the dependency is practical on the target development environment.

Do not introduce a large framework merely to implement a small test.

---

# 36. Versioning

Reusable tools should expose a version when practical:

```bash
serialctl --version
```

Test reports should record relevant versions when reproducibility matters:

```text
tool version
firmware version
software commit
kernel version
hardware revision
```

---

# 37. Test Metadata

For important tests, record metadata such as:

```yaml
test:
  name: usb_sensor_sample_rate
  version: 1

oracle:
  type: SPEC
  source: docs/sensor-spec.md

environment:
  firmware: abc123
  hardware: revB
```

Do not collect metadata that has no practical value.

---

# 38. Test Reports

A useful test report should make these questions easy to answer:

```text
What test ran?
When did it run?
What environment was used?
What was observed?
What was expected?
Where did the expectation come from?
What was the result?
If it failed, why?
```

Example:

```text
[TEST] usb_sensor_sample_rate
[DEVICE] sensor-01
[FIRMWARE] abc123

[MEASURE] samples=987
[MEASURE] duration=10.0s
[CALCULATED] rate=98.7Hz

[ORACLE] SPEC
[SOURCE] docs/sensor-spec.md
[EXPECTED] 95..105Hz

[RESULT] PASS
```

---

# 39. JSON Test Result Schema

For machine-consumed results, prefer a consistent structure.

Example:

```json
{
  "test": "usb_sensor_sample_rate",
  "status": "PASS",

  "observation": {
    "samples": 987,
    "duration_s": 10.0,
    "sample_rate_hz": 98.7
  },

  "oracle": {
    "type": "SPEC",
    "source": "docs/sensor-spec.md",
    "min": 95,
    "max": 105,
    "unit": "Hz"
  }
}
```

For observation-only tests:

```json
{
  "test": "sensor_characterization",
  "status": "INCONCLUSIVE",

  "observation": {
    "sample_rate_hz": 98.7
  },

  "oracle": {
    "type": "NONE"
  }
}
```

---

# 40. Agent Investigation Order

When asked to add or modify a test, follow this order.

## Step 1 — Understand the requirement

Determine:

```text
What behavior is being tested?
What is being measured?
What constitutes success?
```

Do not start coding immediately.

---

## Step 2 — Inspect existing infrastructure

Search:

```text
tests/
test/
tools/
scripts/
scripts/test/
Makefile
CMakeLists.txt
pyproject.toml
package.json
README.md
CONTRIBUTING.md
```

Also search for related commands, libraries, fixtures, and CI jobs.

---

## Step 3 — Find reusable tools

Search for:

```text
serial
usb
i2c
spi
gpio
flash
reset
device
probe
capture
measure
assert
test
fixture
golden
baseline
```

Prefer existing infrastructure.

---

## Step 4 — Identify the oracle

Explicitly determine:

```text
Oracle type:
Oracle source:
Expected value/range:
Tolerance:
```

If unavailable:

```text
ORACLE = NONE
```

and do not invent one.

---

## Step 5 — Design the smallest change

Prefer:

```text
reuse
→ extend
→ refactor
→ create new tool
```

in that order.

---

## Step 6 — Implement

Follow existing project conventions.

---

## Step 7 — Run

Run the smallest relevant test first.

Then run broader regression tests if appropriate.

---

## Step 8 — Inspect results

Do not blindly trust the test script.

Check:

```text
Does the measurement make sense?
Is the oracle correct?
Could the script accidentally pass for the wrong reason?
```

---

## Step 9 — Report

Report:

```text
what was measured
what was expected
why it was expected
result
```

---

# 41. Mandatory Pre-Test Checklist

Before implementing a new test, the agent should mentally or explicitly verify:

```text
[ ] Does an existing tool already provide this measurement?
[ ] Does an existing test already cover part of this behavior?
[ ] Can an existing tool be extended?
[ ] What exactly is being measured?
[ ] What is the oracle?
[ ] Where does the expected value come from?
[ ] Is the expected value specified or merely observed?
[ ] Is tolerance explicitly defined?
[ ] Could this test accidentally encode a measurement as a constant?
[ ] Is this a reusable tool or a one-off experiment?
[ ] Is INCONCLUSIVE more appropriate than PASS/FAIL?
[ ] Does the tool follow existing CLI conventions?
[ ] Does the tool follow existing output conventions?
[ ] Does the tool follow existing exit-code conventions?
[ ] Does the test leave the environment clean?
```

---

# 42. Anti-Patterns

The agent MUST avoid the following.

## 42.1 Measurement-as-specification

```python
measured = measure()
EXPECTED = measured
```

unless explicitly intended as a repeatability comparison.

---

## 42.2 First-run golden value

```text
Run once
→ get value
→ commit as golden
```

without explicit approval.

---

## 42.3 Arbitrary tolerance

```python
assert abs(actual - expected) < 10
```

with no documented reason.

---

## 42.4 Duplicate tools

Creating another helper that substantially duplicates an existing tool.

---

## 42.5 Test-specific infrastructure

Putting reusable device communication logic inside a single test script when multiple tests could use it.

---

## 42.6 Hidden assumptions

Examples:

```python
assert len(devices) == 1
```

when the project does not guarantee exactly one device.

Or:

```python
time.sleep(1)
```

when the actual requirement is "wait until the device is ready."

Prefer explicit readiness detection when practical.

---

## 42.7 Success-of-command-as-success-of-device

This:

```bash
command succeeded
```

does not necessarily mean:

```text
device behavior is correct
```

---

## 42.8 Human-readable output as API

Do not make other tests depend on fragile text such as:

```text
Everything looks good!
```

Prefer structured output.

---

## 42.9 Silent behavior changes

Do not change the semantics of an existing reusable tool merely to satisfy one test.

If a behavior change is necessary:

1. understand existing consumers,
2. preserve compatibility where practical,
3. document the change,
4. update tests.

---

# 43. Regression Test Promotion

When a bug is found, prefer this workflow:

```text
bug
 ↓
minimal reproduction
 ↓
identify invariant / requirement
 ↓
write regression test
 ↓
fix
 ↓
verify regression test
```

Do not merely write a test that reproduces the current implementation.

The test should capture the intended behavior.

---

# 44. Test the Invariant, Not the Implementation

Prefer:

```python
assert output == expected_protocol_result
```

when the protocol defines the result.

Avoid:

```python
assert internal_variable == 42
```

unless that internal value itself is the documented contract.

Tests should generally protect behavior rather than implementation details.

---

# 45. Negative Tests

Where useful, test invalid behavior explicitly.

Examples:

```text
invalid packet
missing device
timeout
wrong descriptor
out-of-range value
disconnect during transfer
corrupted data
```

A negative test must still have a clear expected outcome.

---

# 46. Flaky Tests

When a test is flaky, do not simply increase retries until it passes.

Investigate:

```text
race condition
timing dependency
hardware state
resource cleanup
USB enumeration
serial buffering
scheduler behavior
power state
external dependency
```

If retries are necessary, make them explicit and report them.

Do not hide repeated failures behind an unlimited retry loop.

---

# 47. Repeated Measurements

When measuring noisy hardware or timing-sensitive behavior, use an explicitly defined methodology.

Examples:

```text
N samples
mean
median
min/max
standard deviation
percentile
confidence interval
```

Do not use a single sample when the requirement concerns statistical behavior.

If a statistical criterion is used, document the methodology.

---

# 48. Environment-Specific Expectations

If behavior legitimately differs by environment, do not silently hard-code one environment's result.

Prefer explicit dimensions:

```yaml
environment:
  board: revA
  firmware: v1.2
  kernel: 6.6
```

Then define expectations accordingly.

---

# 49. User Overrides

The user may explicitly define a test criterion.

For example:

> Treat 100 ± 3 Hz as acceptable.

This becomes a valid `USER_DEFINED` oracle.

The agent should preserve that information rather than replacing it with an inferred value.

---

# 50. When Requirements Are Missing

If the user asks:

> "Check whether the sensor is working."

but no specification is available, the agent should not invent a definition of "working."

Instead, determine what objective observations can be made:

```text
device detected
communication succeeds
sensor produces data
data changes when input changes
values remain within physically valid range
```

Clearly distinguish:

```text
observed behavior
```

from:

```text
formal pass/fail criterion
```

If necessary, report:

```text
INCONCLUSIVE: no formal acceptance criterion was available.
```

---

# 51. Agent Output Requirements

When reporting a test result, prefer this structure:

```text
Test:
    <name>

Measurement:
    <what was measured>

Observation:
    <actual result>

Oracle:
    <SPEC / REQUIREMENT / INVARIANT / RELATIONSHIP / GOLDEN / BASELINE / USER_DEFINED / NONE>

Expected:
    <criterion>

Source:
    <where the criterion came from>

Result:
    PASS / FAIL / INCONCLUSIVE / ERROR
```

Do not report only:

```text
PASS
```

when the reasoning behind the result matters.

---

# 52. Minimality

The agent should implement the smallest solution that:

1. satisfies the requirement,
2. follows existing conventions,
3. reuses existing infrastructure,
4. provides sufficient diagnostics,
5. does not introduce unnecessary dependencies.

Do not build a generalized framework when a small reusable helper is sufficient.

---

# 53. Consistency Over Cleverness

When choosing between:

```text
clever new design
```

and:

```text
boring design consistent with the existing project
```

prefer consistency unless there is a concrete reason to change the architecture.

The purpose of this skill is to make future Agent-generated work predictable.

---

# 54. Long-Term Tool Evolution

The testing infrastructure should evolve toward:

```text
few reusable tools
+
many small tests
+
clear oracles
+
consistent output
+
stable conventions
```

not:

```text
many one-off scripts
+
duplicated device access code
+
hidden assumptions
+
hard-coded measurements
```

---

# 55. Recommended Architecture

For projects that need substantial automated hardware testing, a useful architecture is:

```text
                  ┌──────────────────────┐
                  │      Test Cases      │
                  │  PASS/FAIL logic     │
                  └──────────┬───────────┘
                             │
                             ▼
                  ┌──────────────────────┐
                  │   Test Assertions    │
                  │      / Oracles       │
                  └──────────┬───────────┘
                             │
                             ▼
                  ┌──────────────────────┐
                  │ Reusable Test Tools  │
                  │ serialctl / usbctl   │
                  │ flashctl / sensorctl │
                  └──────────┬───────────┘
                             │
                             ▼
                  ┌──────────────────────┐
                  │     Device/System    │
                  └──────────────────────┘
```

Keep the layers conceptually separate.

---

# 56. Example: Bad Test

Do not write:

```python
rate = read_sample_rate()

# Observed during development:
# rate was 98.7 Hz

assert abs(rate - 98.7) < 1
```

unless `98.7 ± 1 Hz` is actually an approved requirement or baseline.

---

# 57. Example: Good Test

If the specification says:

```text
100 Hz ± 5%
```

write:

```python
rate = read_sample_rate()

assert 95 <= rate <= 105
```

and document:

```text
Oracle: SPEC
Source: sensor specification
Expected: 95..105 Hz
```

---

# 58. Example: Observation-Only Experiment

If no specification exists:

```python
rate = read_sample_rate()

print({
    "sample_rate_hz": rate,
    "status": "INCONCLUSIVE",
    "oracle": "NONE",
})
```

This is preferable to inventing a threshold.

---

# 59. Example: Relationship Test

Suppose a loopback device should return the transmitted payload unchanged.

The test can use:

```python
sent = generate_payload()

received = send_and_receive(sent)

assert received == sent
```

Here the expected value is not an arbitrary constant.

The oracle is:

```text
RELATIONSHIP
```

The test is based on an invariant:

```text
received == transmitted
```

---

# 60. Example: Reusable Serial Tool

Instead of implementing serial communication repeatedly:

```bash
serialctl capture \
    --device /dev/ttyUSB0 \
    --timeout 5 \
    --json
```

Output:

```json
{
  "status": "ok",
  "device": "/dev/ttyUSB0",
  "duration_ms": 5000,
  "lines": [
    "booting",
    "ready",
    "sensor: 42"
  ]
}
```

Then multiple tests can reuse it.

---

# 61. Example: Agent Should Extend, Not Duplicate

Suppose:

```text
serialctl
```

already supports:

```bash
serialctl capture
```

but the new test needs:

```text
wait until a specific line appears
```

The preferred solution is to consider adding:

```bash
serialctl wait --pattern "READY" --timeout 5
```

rather than creating:

```text
wait_for_ready.py
```

for a single test.

---

# 62. Test Tool Review

Before considering a new test tool complete, review:

```text
Architecture
    [ ] Existing tools searched
    [ ] Duplication minimized

Measurement
    [ ] Observation clearly defined
    [ ] Units explicit

Oracle
    [ ] Oracle type defined
    [ ] Source documented
    [ ] No inferred constants

Reliability
    [ ] Timeouts explicit
    [ ] Cleanup implemented
    [ ] Environment errors distinguished

Interface
    [ ] CLI follows project conventions
    [ ] JSON output available when useful
    [ ] Exit codes documented

Maintainability
    [ ] Naming consistent
    [ ] No unnecessary dependencies
    [ ] Reusable functionality extracted

Result
    [ ] PASS/FAIL/INCONCLUSIVE semantics clear
```

---

# 63. Final Agent Rule

Before writing any new test code, ask:

> **"Am I measuring a fact, checking a requirement, or merely observing what happened?"**

Then ask:

> **"Where does my expected value come from?"**

Then ask:

> **"Does the project already have a tool that can do this?"**

If the answer to the second question is:

```text
"I just measured it once."
```

that value must not silently become an expected value.

If the answer to the third question is:

```text
"Yes."
```

do not create another tool unless there is a concrete reason.

The desired end state is:

```text
Agent-generated tests should look like they were written
by the same engineer, even when they were generated months apart.
```

Consistency, reuse, explicit oracles, reproducibility, and trustworthy conclusions are more important than clever individual scripts.
