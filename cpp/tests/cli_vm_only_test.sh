#!/usr/bin/env bash
# LOOK 2: the command line runs only on the VM, like the web server.
#
# `lk` used to fall back to the tree-walk engine — with a warning — when the VM could not compile
# a script, when the script used timer:: or jobs::worker, or when it called a module function the
# VM does not have; LOOK_CLI_VM=0 chose that engine. Now:
#   * a script the VM cannot compile does not run: the reason is printed, exit 1, no output
#     (an included file is named)
#   * a call to a module function that does not exist is refused before anything runs
#   * LOOK_CLI_VM=0 and LOOK_BYTECODE=0 are ignored with a warning; LOOK_VM_STRICT changes nothing
#   * a job handler (jobs::worker + jobs::run) and a timer callback run on the VM; their output
#     reaches stdout (the tree-walk engine wrote it to a buffer it threw away)
#   * ws:: and sse:: say they belong to a web application
#   * a script the VM runs prints nothing on stderr; lines starting with ++ / -- are statements;
#     nested file includes run
# Usage: cli_vm_only_test.sh <lk>
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
run() { out="$(cd "$TMP" && env "$@" 2>"$TMP/err.txt")"; rc=$?; err="$(cat "$TMP/err.txt")"; }

# ── a program that does not compile does not run ────────────────────────────────────────────
{ echo 'print("started")'; echo 'function cnt(...$a) { return count($a) }'; echo "print(\"got=\" . cnt($(seq -s, 1 65535)))"; } > "$TMP/big.lk"
run "$LK" big.lk
if [ "$rc" = 1 ] && [ -z "$out" ] && echo "$err" | grep -q 'more than 65534 arguments' && ! echo "$err" | grep -qi 'tree-walk'; then
    ok "a script the VM cannot compile: exit 1, the reason, no output"
else bad "big.lk: rc=$rc out=[$out] err=[$(echo "$err" | head -c 200)]"; fi

mkdir -p "$TMP/lib"
{ echo 'function cnt(...$a) { return count($a) }'; echo "function many() { return cnt($(seq -s, 1 65535)) }"; } > "$TMP/lib/big.lk"
printf 'print("started")\nuse "lib/big.lk"\nprint("got=" . many())\n' > "$TMP/incbig.lk"
run "$LK" incbig.lk
if [ "$rc" = 1 ] && [ -z "$out" ] && echo "$err" | grep -q 'use "lib/big.lk": .*more than 65534 arguments'; then
    ok "an included file the VM cannot compile: exit 1, the file and the reason"
else bad "incbig.lk: rc=$rc out=[$out] err=[$(echo "$err" | head -c 200)]"; fi

printf 'use string\nprint("started")\n$a = string::uper("x")\n' > "$TMP/typo.lk"
run "$LK" typo.lk
if [ "$rc" = 1 ] && [ -z "$out" ] && echo "$err" | grep -qF 'string::uper() does not exist'; then
    ok "a module function that does not exist: refused before anything runs"
else bad "typo.lk: rc=$rc out=[$out] err=[$err]"; fi

# ── the old switches ────────────────────────────────────────────────────────────────────────
echo 'print("ok")' > "$TMP/small.lk"
for sw in LOOK_CLI_VM=0 LOOK_BYTECODE=0; do
    run "$sw" "$LK" small.lk
    if [ "$rc" = 0 ] && [ "$out" = ok ] && echo "$err" | grep -qF "${sw} is ignored: LOOK 2 runs only on the VM"; then
        ok "$sw is ignored with a warning; the script runs on the VM"
    else bad "$sw: rc=$rc out=[$out] err=[$err]"; fi
done
run LOOK_VM_STRICT=1 "$LK" small.lk
[ "$rc" = 0 ] && [ "$out" = ok ] && [ -z "$err" ] && ok "LOOK_VM_STRICT=1 changes nothing" || bad "strict: rc=$rc out=[$out] err=[$err]"
run "$LK" small.lk
[ "$out" = ok ] && [ -z "$err" ] && ok "a script the VM runs prints nothing on stderr" || bad "small.lk: out=[$out] err=[$err]"

# ── callbacks on the command line ───────────────────────────────────────────────────────────
cat > "$TMP/jobs.lk" <<'LK'
use jobs
$prefix = "handled"
jobs::worker("mail", function($job) {
    $p = $job["payload"]
    print("{$prefix} {$p}")
    return true
})
jobs::push("mail", "a@x")
jobs::push("mail", "b@x")
jobs::run(0)
$s = jobs::stats("mail")
$d = $s["done"]
print("done={$d}")
LK
run "$LK" jobs.lk
if [ "$rc" = 0 ] && [ "$(echo "$out" | grep -c '^handled [ab]@x$')" = 2 ] && echo "$out" | grep -qx 'done=2' && [ -z "$err" ]; then
    ok "a job handler runs on the VM, sees the script's globals, and its output reaches stdout"
else bad "jobs.lk: rc=$rc out=[$out] err=[$err]"; fi

cat > "$TMP/timer.lk" <<'LK'
use cache
$n = 5
timer::after(30, function() { print("timer ran"); cache::set("t", $n, 60) })
$i = 0
while ($i < 50000000 && cache::get("t") == null) { $i = $i + 1 }
$t = cache::get("t")
print("timer saw n={$t}")
LK
run "$LK" timer.lk
if [ "$rc" = 0 ] && echo "$out" | grep -qx 'timer ran' && echo "$out" | grep -qx 'timer saw n=5' && [ -z "$err" ]; then
    ok "a timer callback runs on the VM with the globals of the moment it was set"
else bad "timer.lk: rc=$rc out=[$out] err=[$err]"; fi

printf 'ws::send("c", "hi")\n' > "$TMP/ws.lk"
run "$LK" ws.lk
if [ "$rc" = 1 ] && echo "$err" | grep -qF 'ws::send() can be used only in a web application'; then
    ok "ws:: on the command line says it belongs to a web application"
else bad "ws.lk: rc=$rc err=[$err]"; fi

# ── what did not change ─────────────────────────────────────────────────────────────────────
printf '$x = 5\n$x--\n--$x\n$y = 1\n++$y\n$z = $x\n- 1\nprint($x . ":" . $y . ":" . $z)\n' > "$TMP/step.lk"
run "$LK" step.lk
[ "$(echo "$out" | tail -1)" = "3:2:2" ] && ok "lines starting with ++ / -- are statements; a line starting with - still continues" || bad "step.lk: [$out]"

printf 'use json\nuse "inner.lk"\nfunction enc($v) { return json::encode($v) }\n' > "$TMP/lib/util.lk"
printf 'use math\nfunction root($n) { return math::sqrt($n) }\n' > "$TMP/lib/inner.lk"
printf 'use "lib/util.lk"\nprint(enc(["a" => root(16)]))\n' > "$TMP/inc.lk"
run "$LK" inc.lk
[ "$out" = '{"a":4}' ] && [ -z "$err" ] && ok "a script with nested file includes runs" || bad "inc.lk: out=[$out] err=[$err]"

[ $fail = 0 ] && echo "PASS: the command line runs only on the VM" || { echo "FAIL: command line on the VM only"; exit 1; }
