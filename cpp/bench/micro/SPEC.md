# Micro-benchmark spec — LOOK vs Node.js (language core)

Two independent implementations (LOOK `.lk`, Node `.js`) of the SAME benchmarks.
The whole point is a fair, hygienic language-core comparison. Follow this spec exactly.

## Hard rules (measurement hygiene — non-negotiable)

1. **Identical work.** Each benchmark below defines exact logic. Both languages must do
   the same operations in the same order with the same N.
2. **Checksum = dead-code guard AND equivalence proof.** Every benchmark computes an
   INTEGER checksum and prints it. Because the logic is identical, LOOK and Node MUST
   print the SAME checksum. If they differ, the benchmark is not equivalent — fix it.
   Printing the checksum also stops a JIT from deleting the loop as dead code.
3. **Keep integer accumulators bounded** with `% 1000000007` so LOOK's int64 never
   overflows into float (that promotion is intentional LOOK design; we don't want it
   silently changing the arithmetic path mid-benchmark). Call this modulus M below.
4. **Warm then measure.** Run the timed body once as warm-up (discard), then run it again
   timed. This warms V8's JIT so we compare warm-vs-warm (a separate cold test covers
   startup). Use an internal monotonic timer (Node: `process.hrtime.bigint()`; LOOK: use
   whatever high-resolution clock the runtime exposes — find it and report which).
5. **One process per benchmark**, or a single driver that runs each in isolation and
   prints one line per benchmark. Print exactly: `name,checksum,warm_ms` (warm_ms = the
   timed body's elapsed ms, 3 decimals). Do NOT compare to the other language — that is
   the parent's job.

## Your job (each agent)

- Implement every benchmark below in your language, in `cpp/bench/micro/<lang>/`.
- Provide a driver `run.<ext>` that runs all of them and prints the CSV lines.
- VALIDATE: run it yourself, confirm no crash, checksums are stable across two runs, and
  each warm_ms is > 0. Report the checksum table you got. Do NOT do cross-language timing.
- Report which high-resolution timer API you used.

## Benchmarks (M = 1000000007)

1. **int_arith** — s=0; for i in 0..N: s=(s + i*3 + 7) % M.  N=20_000_000. checksum=s.
2. **float_arith** — x=1.0; for i in 0..N: x = x*1.0000003 + 0.5; if x>1e6: x=x-1e6.
   N=20_000_000. checksum = floor(x*1000) mod M as integer. (Only +,-,*,/ are used
   anywhere — IEEE-754 makes these bit-identical across engines, so the checksum matches.)
3. **loop** — s=0; for i in 0..N: s=(s+1)%M.  N=100_000_000. checksum=s.
4. **fn_call** — define f(i)=i+1; s=0; for i in 0..N: s=(s+f(i))%M.  N=20_000_000.
5. **nested_fn** — h(x)=x+1; g(x)=h(x)+1; f(x)=g(x)+1; s=0; for i in 0..N: s=(s+f(i))%M.
   N=10_000_000. checksum=s.
6. **recursion** — fib(n): naive recursive (fib(n)=fib(n-1)+fib(n-2), fib<2 = n). Compute
   fib(32). checksum = fib(32) (=2178309). (Deterministic; measures call overhead.)
7. **array_create_iterate** — repeat R times: build array [0..K-1], then sum it. K=1000,
   R=20000. checksum = (R * sum(0..K-1)) % M.
8. **array_push_pop** — repeat R times: push 0..K-1 onto a fresh array, then pop all while
   summing. K=1000, R=20000. checksum=(R*sum(0..K-1))%M.
9. **assoc_access** — build a map/assoc with keys "k0".."k99" -> value i (100 keys). Then
   for i in 0..N: look up key ("k" + (i%100)) and add its value to s (mod M). N=5_000_000.
   checksum=s. (LOOK assoc is a ["__assoc__",k,v,...] sentinel array = O(n) lookup; JS
   object = O(1) hash. THIS is where the biggest gap is expected — measure it honestly.)
10. **object_create** — repeat N times: make a record {a:i, b:i+1, c:i+2, d:i+3, e:i+4}
    (LOOK assoc literal), read field c, s=(s+c)%M. N=5_000_000. checksum=s.
11. **string_concat** — repeat R times: build a string by concatenating "x" K times into a
    fresh string, record its length into s (mod M). K=1000, R=20000. checksum=s.
12. **string_search** — hay = a fixed 1000-char string with one "needle" near the end.
    for i in 0..N: find index of "needle", add (index) to s (mod M). N=1_000_000. checksum=s.
13. **string_slice** — hay = fixed 1000-char string. for i in 0..N: take substring
    [i%900 .. i%900+50], add its length (50) to s (mod M). N=2_000_000. checksum=s.
14. **json_parse** — src = a fixed JSON string: {"id":42,"name":"look","tags":[1,2,3],
    "active":true}. for i in 0..N: parse it, add the "id" field to s (mod M). N=1_000_000.
    checksum=s (=42*N mod M).
15. **json_serialize** — obj = the same structure. for i in 0..N: serialize to JSON string,
    add its length to s (mod M). N=1_000_000. checksum=s.
16. **regex** — pat matches an email-like token; text = fixed string with 3 matches.
    for i in 0..N: count matches in text, add count to s (mod M). N=500_000. checksum=s (=3*N mod M).
17. **exception** — for i in 0..N: try { if (i%2==0) throw error; s=(s+1)%M } catch { s=(s+2)%M }.
    N=2_000_000. checksum=s.

## Notes on fairness

- Use each language idiomatically (LOOK assoc literals / builtins; JS objects / Map where a
  hash map is meant — for assoc_access use a plain JS object to mirror LOOK's assoc, since
  that is the apples-to-apples data structure a developer reaches for).
- No unsafe tricks to delete work. The checksum print guarantees the loop ran.
- If a benchmark can't be expressed equivalently in your language, STOP and report it rather
  than approximating — equivalence matters more than coverage.

## Hygiene rule 7 — input sizes must represent real usage

Learned the hard way (2026-08-22): an assoc-lookup decomposition used keys `k0`..`k99` (≤3 chars),
which fit small-string optimization and never heap-allocate — so it measured a per-lookup `to_string()`
allocation as ~2% (noise) and wrongly concluded "no cost". With realistic keys (>15 chars, e.g. a DB
column name like `customer_email_address`) the same allocation showed a real 13-25% cost. Unrealistic
input sizes can make a real cost invisible. Pick inputs that mirror production: DB column names, typical
row widths, real string lengths — not the shortest thing that compiles.

## Hygiene rule 8 — absolute numbers from different toolchains are not comparable

Learned verifying a release (2026-08-22): a fix measured -13-25% on an MSVC build (757→570ms); the released
gcc-portable binary of the SAME code measured 786ms. That is not a regression — MSVC and gcc/libstdc++ produce
different absolute timings (SSO thresholds, allocator, codegen), so "786 vs expected 570" was an invalid
cross-toolchain comparison. To check whether an optimization is present in a binary you can't A/B against a
known baseline, use a SELF-CONTAINED discriminator inside that same binary: here, short-key (SSO, no alloc)
vs long-key (>SSO). If the fast-path is present the gap is small (comparison cost only); if absent it is large
(a heap allocation per lookup). Toolchain-independent, and it proved the fix was really in the shipped binary.

## Hygiene rule 9 — under a CPU quota, check `cpu.stat` before trusting tail latency

Learned solving the "40ms p99" mystery (2026-08-22): a multi-threaded server measured under a hard CPU
quota (`--cpus=2`) showed p99 40-60ms while p50 stayed sub-millisecond. Five network hypotheses and a
tcpdump were spent chasing it — it was CFS bandwidth throttling: with workers=CPUs×4=8 threads bursting
past a 2-CPU quota, the kernel throttled ALL threads ~92% of CFS periods, stalling in-flight requests.
The tell was in `cat /sys/fs/cgroup/cpu.stat`: `nr_throttled` and `throttled_usec` climbing under load.
If you measure tail latency under any CPU quota (container `--cpus`, k8s limit, systemd CPUQuota), read
cpu.stat first: `nr_throttled > 0` under load means the numbers include scheduler throttling, not the
server's own latency — set threads ≤ the quota (or remove the quota) and re-measure. The symptom is
invisible in strace (the stall is descheduling, not a syscall) — cpu.stat is the only direct evidence.
