#!/usr/bin/env bash
# lk fix rewrites the deprecated push()/pop()/array::pop() into the one way to write them:
#   (a) push(X, v)      statement → X = array::push(X, v)
#   (b) pop(X)          statement → X = array::slice(X, 0, -1)
#   (c) $v = pop(X)     statement → $v = X[-1]  and, on the next line, X = array::slice(X, 0, -1)
#   (d) array::pop(X)   anywhere  → X[-1]   (NOTE per rewrite: an empty list is now an error)
# Anything else (push/pop inside a larger expression) is left as it is and reported
# (`file:line: not rewritten: ...`, exit 1). Untouched lines and comments stay byte for byte; a second
# run changes nothing; `--check` lists what would change and writes nothing; no files = stdin → stdout.
# The program's output is the same before and after (non-empty lists).
# Usage: lk_fix_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

cat > "$TMP/app.lk" <<'LK'
# header comment: push($l, 1) and pop($l) in a comment stay
use array
$l = [1, 2, 3, 4, 5]
$m = ["rows" => [1, 2, 3], "u" => ["tags" => ["a", "b"]]]
push($l, 6)   // trailing comment stays
push($m["rows"], 4)
push($m["u"]["tags"], "c")
pop($l)
$last = pop($l)
if (count($l) > 0) {
    $x = pop($m["rows"])
    print($last . " " . $x)
}
print(array::pop($l))
$y = array::pop($m["rows"]) + array::pop(array::slice($l, 0, 2))
print($y)
push($l, array::pop($m["u"]["tags"]))
function f($a) {
    push($a, 1)
    $z = pop($a)
    return [$a, $z]
}
print(json::encode(f([7])))
print(json::encode($l) . json::encode($m))
LK
cat > "$TMP/want.lk" <<'LK'
# header comment: push($l, 1) and pop($l) in a comment stay
use array
$l = [1, 2, 3, 4, 5]
$m = ["rows" => [1, 2, 3], "u" => ["tags" => ["a", "b"]]]
$l = array::push($l, 6)   // trailing comment stays
$m["rows"] = array::push($m["rows"], 4)
$m["u"]["tags"] = array::push($m["u"]["tags"], "c")
$l = array::slice($l, 0, -1)
$last = $l[-1]
$l = array::slice($l, 0, -1)
if (count($l) > 0) {
    $x = $m["rows"][-1]
    $m["rows"] = array::slice($m["rows"], 0, -1)
    print($last . " " . $x)
}
print($l[-1])
$y = $m["rows"][-1] + array::slice($l, 0, 2)[-1]
print($y)
$l = array::push($l, $m["u"]["tags"][-1])
function f($a) {
    $a = array::push($a, 1)
    $z = $a[-1]
    $a = array::slice($a, 0, -1)
    return [$a, $z]
}
print(json::encode(f([7])))
print(json::encode($l) . json::encode($m))
LK
cp "$TMP/app.lk" "$TMP/orig.lk"
before="$("$LK" "$TMP/app.lk" 2>/dev/null)"

# --check: değişecekleri listeler, dosyaya dokunmaz, çıkış 1
out="$("$LK" fix --check "$TMP/app.lk" 2>&1)"; rc=$?
if [ "$rc" = 1 ] && cmp -s "$TMP/app.lk" "$TMP/orig.lk" \
   && grep -qF "app.lk:5: push(\$l, 6)  ->  \$l = array::push(\$l, 6)" <<< "$out" \
   && grep -qF "app.lk:9: \$last = pop(\$l)  ->  \$last = \$l[-1]; \$l = array::slice(\$l, 0, -1)" <<< "$out"; then
  ok "--check lists the rewrites, exit 1, file unchanged"
else bad "--check: rc=$rc out=[$out]"; fi

# yeniden yazma: beklenen metin bayt bayt; d ve c için NOTE; çıkış 0 (kalan yok)
out="$("$LK" fix "$TMP/app.lk" 2>&1)"; rc=$?
if [ "$rc" = 0 ] && cmp -s "$TMP/app.lk" "$TMP/want.lk"; then ok "cases a-d rewritten, comments and other lines kept"
else bad "rewrite: rc=$rc out=[$out] diff=[$(diff "$TMP/want.lk" "$TMP/app.lk")]"; fi
n="$(grep -c 'NOTE: array::pop(X) became X\[-1\]' <<< "$out")"
if [ "$n" = 4 ] && grep -q '^.*app.lk:14: NOTE: array::pop' <<< "$out" && grep -q 'fixed: .*app.lk' <<< "$out"; then
  ok "one NOTE per array::pop rewrite, with file:line"
else bad "notes: n=$n out=[$out]"; fi

# çıktı aynı, uyarı kalmadı
after="$("$LK" "$TMP/app.lk" 2>"$TMP/err.txt")"
if [ -n "$before" ] && [ "$before" = "$after" ] && ! grep -q 'deprecated\|will be an error' "$TMP/err.txt"; then
  ok "same output before and after, no deprecation warning left"
else bad "output: before=[$before] after=[$after] stderr=[$(cat "$TMP/err.txt")]"; fi

# ikinci koşu hiçbir şey değiştirmez; --check temiz (0)
cp "$TMP/app.lk" "$TMP/once.lk"
out="$("$LK" fix "$TMP/app.lk" 2>&1)"; rc=$?
out2="$("$LK" fix --check "$TMP/app.lk" 2>&1)"; rc2=$?
if [ "$rc" = 0 ] && [ -z "$out" ] && cmp -s "$TMP/app.lk" "$TMP/once.lk" && [ "$rc2" = 0 ] && [ -z "$out2" ]; then
  ok "idempotent: second run and --check change nothing, exit 0"
else bad "second run: rc=$rc out=[$out] check rc=$rc2 out=[$out2]"; fi

# yeniden yazılamayanlar: raporlanır, dokunulmaz, çıkış 1 (yazarken de --check'te de)
cat > "$TMP/rest.lk" <<'LK'
$l = [1, 2, 3]
print(pop($l))
if (push($l, 9) > 0) { print("pushed") }
$l = pop($l)
push($l, 4)
LK
out="$("$LK" fix "$TMP/rest.lk" 2>&1)"; rc=$?
if [ "$rc" = 1 ] && grep -qF 'rest.lk:2: not rewritten: pop() is used inside a larger expression' <<< "$out" \
   && grep -qF 'rest.lk:3: not rewritten: push() is used inside a larger expression' <<< "$out" \
   && grep -qF 'rest.lk:4: not rewritten: the result of pop() is assigned to a variable the list uses' <<< "$out" \
   && grep -qx '\$l = array::push(\$l, 4)' "$TMP/rest.lk" && grep -qx 'print(pop(\$l))' "$TMP/rest.lk"; then
  ok "calls inside larger expressions are reported (file:line: not rewritten), exit 1"
else bad "not rewritten: rc=$rc out=[$out] file=[$(cat "$TMP/rest.lk")]"; fi
"$LK" fix --check "$TMP/rest.lk" > /dev/null 2>&1; rc=$?
[ "$rc" = 1 ] && ok "--check exits 1 while calls are left" || bad "--check on leftovers: rc=$rc"

# stdin → stdout (dosya yok); raporlar stderr'e
o="$(printf 'use array\n$l = [1, 2]\npop($l)\nprint(json::encode($l))\n' | "$LK" fix 2>"$TMP/err.txt")"; rc=$?
if [ "$rc" = 0 ] && [ "$o" = "$(printf 'use array\n$l = [1, 2]\n$l = array::slice($l, 0, -1)\nprint(json::encode($l))')" ] && [ ! -s "$TMP/err.txt" ]; then
  ok "stdin to stdout"
else bad "stdin: rc=$rc out=[$o] err=[$(cat "$TMP/err.txt")]"; fi

# CRLF dosya: eklenen ikinci satır da CRLF
printf 'use array\r\n$l = [1, 2]\r\n$v = pop($l)\r\nprint($v)\r\n' > "$TMP/crlf.lk"
"$LK" fix "$TMP/crlf.lk" > /dev/null 2>&1; rc=$?
if [ "$rc" = 0 ] && [ "$(cat "$TMP/crlf.lk")" = "$(printf 'use array\r\n$l = [1, 2]\r\n$v = $l[-1]\r\n$l = array::slice($l, 0, -1)\r\nprint($v)\r')" ]; then
  ok "a CRLF file keeps CRLF line ends"
else bad "crlf: rc=$rc file=[$(od -c "$TMP/crlf.lk" | head -5)]"; fi

# `use array` yoksa eklenir (yeni yazım array:: kullanır; yoksa "Module 'array' not loaded.");
# shebang ilk satırda kalır; yalnız array::pop → X[-1] ise modül gerekmez, eklenmez.
printf '#!/usr/bin/env lk\n$l = [1, 2]\n$m = ["a" => [1]]\npush($m["a"], 2)\npop($l)\nprint(json::encode($l) . json::encode($m))\n' > "$TMP/nouse.lk"
want_out="$("$LK" "$TMP/nouse.lk" 2>/dev/null)"
"$LK" fix "$TMP/nouse.lk" > /dev/null 2>&1; rc=$?
if [ "$rc" = 0 ] && [ "$(head -2 "$TMP/nouse.lk")" = "$(printf '#!/usr/bin/env lk\nuse array')" ] \
   && [ "$("$LK" "$TMP/nouse.lk" 2>&1)" = "$want_out" ] && [ "$want_out" = '[1]{"a":[1,2]}' ]; then
  ok "use array is added when missing (after a shebang) and the program still runs"
else bad "missing use array: rc=$rc file=[$(cat "$TMP/nouse.lk")] run=[$("$LK" "$TMP/nouse.lk" 2>&1)]"; fi
printf '$l = [1, 2]\nprint(array::pop($l))\n' > "$TMP/onlyap.lk"
"$LK" fix "$TMP/onlyap.lk" > /dev/null 2>&1
[ "$(cat "$TMP/onlyap.lk")" = "$(printf '$l = [1, 2]\nprint($l[-1])')" ] && ok "array::pop alone needs no use array" \
  || bad "array::pop alone: file=[$(cat "$TMP/onlyap.lk")]"

# ayrışmayan dosya: dokunulmaz, çıkış 2
printf 'push($l, \n' > "$TMP/broken.lk"; cp "$TMP/broken.lk" "$TMP/broken0.lk"
"$LK" fix "$TMP/broken.lk" > /dev/null 2>&1; rc=$?
[ "$rc" = 2 ] && cmp -s "$TMP/broken.lk" "$TMP/broken0.lk" && ok "a file that does not parse is left alone, exit 2" || bad "broken file: rc=$rc"

[ $fail = 0 ] && echo "PASS: lk fix rewrites push/pop/array::pop and reports what it cannot" || { echo "FAIL: lk fix"; exit 1; }
