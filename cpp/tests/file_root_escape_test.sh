#!/usr/bin/env bash
# Path escape through file::, template::, use and file::store (Go 1.24 os.Root model: no path and
# no symbolic link — not even one whose target does not exist yet — leads outside the root).
#   * file::put through a dangling link ("dangle" -> /outside/new.txt) used to create the file
#     outside LOOK_FILE_ROOT: the old check could not resolve a target that did not exist;
#   * file::store took "path" and "sha256" from the array it was given: a hand-made array moved any
#     file (.env) into the upload directory, and "../" in sha256 wrote outside it. Only a file from
#     request::file() in the same request is stored now, with LOOK's own path, name and type.
# Links that stay inside the root keep working, dangling ones included.
# Usage: file_root_escape_test.sh <lk> <lk-fcgi> [port]
set -u
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage}")" && pwd)/$(basename "$2")"
PORT="${3:-7912}"
T="$(mktemp -d)"; pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
A="$T/app"; O="$T/out"
mkdir -p "$A/data" "$A/views" "$A/real" "$O"
echo "SECRET-OUT" > "$O/secret.txt"; echo "SECRET-TPL" > "$O/secret.html"; echo 'print("OUTSIDE-RAN")' > "$O/mod.lk"
echo "inside" > "$A/data/ok.txt"; echo "<p>page</p>" > "$A/views/page.html"; echo "hi" > "$A/real/f.txt"
ln -s "$O/secret.txt" "$A/link_out";  ln -s "$O" "$A/dirlink"
ln -s "$O/new_by_dangle.txt" "$A/dangle"; ln -s ../out/rel_dangle.txt "$A/rel_dangle"
ln -s "$O/secret.html" "$A/views/evil.html"; ln -s "$O/mod.lk" "$A/modlink.lk"
ln -s real "$A/alias"; ln -s "$A/real/f.txt" "$A/abslink"; ln -s real/new.txt "$A/dangle_in"
fail=0
run() {   # ad beklenen-metin lk-kaynağı
    printf '%s\n' "$3" > "$A/t.lk"
    out="$(cd "$A" && "$LK" t.lk 2>&1 | grep -v '^\[20' | tr '\n' ' ')"
    if echo "$out" | grep -qF -- "$2"; then echo "  OK   $1"; else echo "  FAIL $1: want \"$2\", got: $(echo "$out" | cut -c1-160)"; fail=1; fi
}
D="access denied"
run "read ../out"                 "$D" 'use file
print(file::read("../out/secret.txt"))'
run "read absolute outside"       "$D" "use file
print(file::read(\"$O/secret.txt\"))"
run "read link to outside"        "$D" 'use file
print(file::read("link_out"))'
run "read through dir link"       "$D" 'use file
print(file::read("dirlink/secret.txt"))'
run "put through dir link"        "$D" 'use file
file::put("dirlink/w.txt", "x")'
run "put through dangling link"   "$D" 'use file
file::put("dangle", "PWNED")'
run "put through relative dangling link" "$D" 'use file
file::put("rel_dangle", "PWNED")'
run "append through dangling link" "$D" 'use file
file::append("dangle", "PWNED")'
run "remove through dir link"     "$D" 'use file
file::remove("dirlink/secret.txt")'
[ -e "$O/new_by_dangle.txt" ] || [ -e "$O/rel_dangle.txt" ] && { echo "  FAIL a file was created outside the root"; fail=1; }
[ -f "$O/secret.txt" ] || { echo "  FAIL the outside file was removed"; fail=1; }
run "inside: ../ that stays in"   "inside" 'use file
print(file::read("data/../data/ok.txt"))'
run "inside: relative dir link"   "hi" 'use file
print(file::read("alias/f.txt"))'
run "inside: absolute link"       "hi" 'use file
print(file::read("abslink"))'
run "inside: dangling link"       "created-inside" 'use file
file::put("dangle_in", "created-inside")
print(file::read("real/new.txt"))'
run "template ../"                "escape outside" 'use template
print(template::render("../out/secret", []))'
run "template link to outside"    "escape outside" 'use template
print(template::render("views/evil", []))'
run "template inside"             "<p>page</p>" 'use template
print(template::render("views/page", []))'
run "use ../"                     "outside the application directory" 'use "../out/mod.lk"
print("main")'
run "use link to outside"         "outside the application directory" 'use "modlink.lk"
print("main")'

# file::store with a hand-made array, in a web request
echo "DB_PASS=topsecret" > "$A/.env"
cat > "$A/st.lk" <<LK
use file
route("GET", "/steal", function() {
    try { \$r = file::store(["path" => "$A/.env", "mime" => "text/plain", "sha256" => "0123456789abcdef0123456789abcdef"], "u"); return response::text("STORED") }
    catch (\$e) { return response::text("REFUSED " . \$e) }
})
route("GET", "/escape", function() {
    try { \$r = file::store(["path" => "$A/.env", "mime" => "text/plain", "sha256" => "../../../out/escaped"], "u"); return response::text("STORED") }
    catch (\$e) { return response::text("REFUSED " . \$e) }
})
LK
( cd "$A" && UPLOAD_DIR="$A/uploads" LOOK_SECRET=escape-test-secret-0123456789abcdef0123 exec "$FCGI" --mode http --port "$PORT" --workers 1 st.lk > "$T/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/steal" && break; sleep 0.1; done
for r in steal escape; do
    b="$(curl -s -m 5 "localhost:$PORT/$r")"
    case "$b" in "REFUSED"*"request::file()"*) echo "  OK   file::store refuses a hand-made array (/$r)";;
      *) echo "  FAIL file::store /$r: $b"; fail=1;; esac
done
[ "$(cat "$A/.env" 2>/dev/null)" = "DB_PASS=topsecret" ] || { echo "  FAIL .env was moved"; fail=1; }
ls "$O" | grep -q escaped && { echo "  FAIL a file was written outside the upload directory"; fail=1; }
[ $fail = 0 ] && echo "PASS: no path or link leads outside the root" || { echo "FAIL: path escape"; exit 1; }
