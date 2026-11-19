# LOOK Tests — Standing Rules

## ⚠️ RULE: No assertion is added without a positive control

**Green is not trusted until red has been seen.** A test/assertion is not added until it has been
shown to **FAIL** in a state where the bug it catches _exists_. An assertion that passes green but
has never been seen red may not be testing anything at all.

This pattern has bitten this project in the same way **at least four times**:

| When | False green |
|------|-------------|
| `t1` (TSan race) | Every thread wrote to a SEPARATE index, so they never raced; CI was green but tested nothing. Fixed with the deterministic `t1b` (all on the same element + a gate). |
| `t4=24` / ASLR | The local libtsan produced a bogus count; "0" was bogus too. CI (ext4) is authoritative. |
| `release_gate.lk` non-finite/float | The assertions also passed on the UNPATCHED binary (they threw for the wrong reason / the old path was also correct), so it was a fake gate. Converted to a distinguishing message check / removed. |
| `crypto_vectors.lk` (2026-08-03) | Two LAYERS: (1) there was no `exit(1)`, so a broken vector still exited 0 and CI was green (the external-contributor case). (2) Even AFTER `exit(1)` was added, when an array was emptied `$run` fell through and printed "PASS: 13/13" -> **empty-green**. Closed with the `EXPECTED_VECTORS` constant. |

## ⚠️ RULE 2: When adding a guard, write a positive control for the GUARD ITSELF

It is not enough for a guard to be "loud on failure"; it must also be "loud when it never ran".
`exit(1)` gives fail-loud but does **not** catch **empty-green** (the array was emptied / the `foreach`
broke / no vector ran -> "PASS: 2/2"). Every layer of the chain checks the _output_ of the one before it;
nobody checks the guard's **unexecuted paths**. The rule: when adding a new guard, show red in **both
directions**: (a) corrupt a value -> red; (b) empty the test array -> red. A counter constant
(`EXPECTED_VECTORS`) closes the second: when you add a vector you are forced to raise the counter deliberately.

**LIMIT (no infinite regress): guards are against ACCIDENTS, not against INTENT.** "A positive control in
two directions" yes; "a guard for the guard of the guard" NO, because the return drops to zero. Classes that
will not be caught and must NOT be chased: (a) the expected value itself may be wrong -> that already produces a
FAIL (the safe direction; HMAC case-6 had 131/134 once and it was caught); (b) someone could fit the expected
value to buggy output or delete the counter and the vectors together -> that is INTENT, a matter of PROCESS
(code review/commit), not of a guard. `EXPECTED_VECTORS` is the right stopping point: it catches accidental
emptying and does not catch deliberate sabotage, and it should not. **End the hunt for a fourth trap here.**

**In practice:** When adding a new assertion, run it against an unpatched (buggy) state:
- Temporarily revert the fix -> the test must be RED -> restore the fix -> GREEN (positive control).
- For `release_gate.lk`: **keep an old binary** (e.g. from before this round's DB work). No new assertion
  goes into the gate until it has been shown RED on that old binary; that is the gate's regression test.
- Stronger still: have the assertion check a signature that only the patched code produces (e.g. a
  substring of a new error message) -> _by construction_ no old binary can pass.

## ⚠️ RULE 3: Before trusting a result, verify that the MEASUREMENT RAN

Rule 1 was against **false-green** (broken but green), Rule 2 against **empty-green** (never ran but green).
Rule 3 applies in both directions and adds **false-red**: trusting the output of a measuring tool
without verifying the tool itself. If a measurement comes back empty/negative, that does not mean "absent";
first check **whether the tool actually ran**.

**State the expected value IN ADVANCE and check the shape too.** If the output does not match the expected form
(line number, format, exit code, empty output), the TOOL is suspect, not the result. Example: "if the edit
was applied to line 32, `grep -n --target` should say **32**". If you see `29`, the tree you are reading is
suspect, not the edit.

**This caught us twice (2026-08-04):**
- `git fetch` was run but the working tree was not reset -> `grep` read the file from BEFORE the commit ->
  "look-cgi is MISSING" (false red). The line number (29 vs 32) gave it away but was not noticed.
- git-bash turns the `:` in `git show origin/main:path` into `;` (MSYS path conversion) -> `git show`
  silently fails -> grep runs on empty output -> a false "NO". The right way: `MSYS_NO_PATHCONV=1`.

**A false red is sometimes more expensive**: it makes you do work that does not exist (rebuild, re-measure).
The antidote is the same as Rules 1/2: **run with an expected value** (a positive control applied to the
measurement ITSELF). "Measure before claiming" -> "also measure that the tool you measure with works".

## ⚠️ RULE 4: Before writing the FIX for a bug, REPRODUCE THE BUG in isolation with a positive control

Rules 1-3 were about assertions/guards/measuring tools. Rule 4 is one step earlier: **the bug that the fix
presupposes.** If a "bug" cannot be reproduced cleanly, in isolation, with a positive control, do NOT write
the fix. Otherwise you add code for a problem that does not exist (worst of all: into the most bug-prone
region of the published core, for zero benefit).

**The most expensive phantom (2026-08-21):** the premise "concurrent outbound http:: calls serialize to ~10 rps"
had never been measured under control. FOUR separate readings were built on it (a measurement -> "it
serializes"; a source reading -> "a global lock"; a fix session -> "worker starvation"; a second reading ->
"epfd_ is shared") and an `AsyncHttpPool` fix was designed and written. **When the fix was measured it gave
zero benefit** (pool ON/OFF = 209 vs 207 rps). The real cause was two confounds: (1) the single-threaded
Python downstream ITSELF serialized to 5 rps; (2) the SSRF guard blocked loopback and returned a fast-fail
`status=0`, so every early measurement was invalid. **On a clean setup http:: does 209 rps** (healthy). A
positive+negative control (threaded downstream -> 209, single-threaded -> 11 = the original "10") settled it
in 30 seconds. The fix was reverted entirely.

**Sharp side-lesson: repetition does not make a mistake right.** Four separate readings built on the same
rotten premise; none of them checked the premise ITSELF. Source reading and a second session both raised
confidence, not correctness. No matter how many times a number is repeated, if it has not been
reproduced cleanly under control even once, it does not exist.

## release_gate.lk — the release gate

PROVES that EVERY packaged binary really contains the fixes. It is not shipped in the package (it stays in
the repo): unzip -> run the **repo's** `release_gate.lk` with the **extracted** `lk`. For fixes that CANNOT be
tested in the gate (those needing a live DB / FCGI env) the only assurance is the **build stamp** (`lk --version`
= the expected git sha; check it by eye on all four artifacts. The stamp can lie in an incremental build, so the
release build must be FRESH).

## Other

- `regression_all.lk` — general regression (after every engine change).
- `differential_test.sh` / `parallel_db_test.sh` — CLI-VM × web-VM × category parity (+ golden) + parallel/DB leak.
- `tsan/enforced/` — race tests are MANDATORY (CI goes red if one breaks). `known_fail/` was REMOVED
  (it was the opposite of fail-loud: a "failure acceptance zone").
- `fuzz/` — libFuzzer harnesses (clang; 30s on every push in CI). The corpus is gitignored.
- `repro/` — pins for individual bugs.
