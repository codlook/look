#!/usr/bin/env bash
# A list can never pass for a map.
#
# A map is stored as [<marker>, k0, v0, k1, v1, …]. The marker used to be the text "__assoc__",
# so any list whose first element was that text was a map: the JSON body
# ["__assoc__","role","admin"] reached the application as a map with role => admin (an
# authorisation check on $data["role"] passed), a form or query array could do the same, and
# legitimate data that started with that text changed shape. The marker is now recognised by
# identity, not by content: no input and no LOOK code can produce it.
# Checked: JSON, form and query input stay lists; a LOOK list literal stays a list; maps stay maps
# across threads (timer, cache::, channel), through JSON and after `.=` on their values; a map
# prints as [k => v].
# Usage: assoc_marker_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7795}"
fail=0

cat > "$TMP/cli.lk" <<'LK'
use json
use array
use cache
$fails = 0
function chk($name, $got, $want) {
  if ($got != $want) { print("FAIL " . $name . ": got " . json::encode($got) . ", want " . json::encode($want)); return 1 }
  return 0
}
$x = json::decode("[\"__assoc__\", \"role\", \"admin\"]")
$fails = $fails + chk("JSON array stays a list (count)", count($x), 3)
$fails = $fails + chk("JSON array stays a list (element 0)", $x[0], "__assoc__")
$fails = $fails + chk("JSON array re-encodes as an array", json::encode($x), "[\"__assoc__\",\"role\",\"admin\"]")
$l = ["__assoc__", "a", 1]
$fails = $fails + chk("LOOK list literal stays a list", [count($l), $l[2]], [3, 1])
$fails = $fails + chk("keys of such a list are its indexes", array::keys($l), [0, 1, 2])
$m = ["k" => 3, "ad" => "x"]
$fails = $fails + chk("map prints as [k => v]", "" . $m, "[k => 3, ad => x]")
$fails = $fails + chk("nested map prints", "" . [1, ["a" => [2]]], "[1, [a => [2]]]")
$fails = $fails + chk("map JSON round trip", json::decode(json::encode($m))["ad"], "x")
$s = ["t" => "a"]
$s["t"] .= "b"
$fails = $fails + chk("append to a map value", $s["t"], "ab")
$fails = $fails + chk("a new map after .= is still a map", "" . ["z" => 1], "[z => 1]")
cache::set("m", ["role" => "user", "n" => [1, 2]], 60)
$c = cache::get("m")
$fails = $fails + chk("map through cache:: (another copy)", [$c["role"], count($c), count($c["n"])], ["user", 2, 2])
$ch = channel()
send($ch, ["q" => 1])
$r = receive($ch)
$fails = $fails + chk("map through a channel", $r["q"], 1)
$d = ["__assoc__" => 1, "v" => "__assoc__"]
$fails = $fails + chk("the text as a key and a value", [count($d), $d["__assoc__"], $d["v"]], [2, 1, "__assoc__"])
if ($fails > 0) { exit(1) }
print("cli ok")
LK
out=$("$LK" "$TMP/cli.lk" 2>&1)
[ "$out" = "cli ok" ] || { echo "$out" | sed 's/^/  /'; fail=1; }

cat > "$TMP/app.lk" <<'LK'
use json
use cache
route("POST", "/j", function() {
  $d = request::json()
  $role = "none"
  try { $role = "" . $d["role"] } catch ($e) { $role = "error" }
  return response::text(count($d) . " " . $role)
})
route("POST", "/f", function() { $a = request::post("a"); return response::text(json::encode($a)) })
route("GET", "/q", function() { $a = request::get("a"); return response::text(json::encode($a)) })
route("GET", "/t", function() {
  $m = ["role" => "user"]
  timer::after(10, fn() => cache::set("tm", "" . $m["role"] . " " . count($m), 60))
  return response::text("ok")
})
route("GET", "/tr", fn() => response::text("" . (cache::get("tm") ?? "none")))
LK
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log" 2>&1 ) & pid=$!
for i in $(seq 1 100); do curl -s -o /dev/null -m 1 "localhost:$PORT/tr" && break; sleep 0.1; done
j=$(curl -s -m 5 -X POST -H 'Content-Type: application/json' --data '["__assoc__","role","admin"]' "localhost:$PORT/j")
# stays a 3-element list; looking up "role" in a list never yields "admin"
case "$j" in
  "3 "*admin*|*admin*) echo "  FAIL JSON body array gave role=admin: '$j'"; fail=1 ;;
  "3 "*) ;;
  *) echo "  FAIL JSON body array is not a 3-element list: '$j'"; fail=1 ;;
esac
f=$(curl -s -m 5 -X POST --data 'a[]=__assoc__&a[]=role&a[]=admin' "localhost:$PORT/f")
q=$(curl -s -m 5 "localhost:$PORT/q?a[]=__assoc__&a[]=role&a[]=admin")
for got in "$f" "$q"; do
  case "$got" in '{'*) echo "  FAIL a form/query array became a map: $got"; fail=1 ;; esac
done
curl -s -o /dev/null -m 5 "localhost:$PORT/t"; sleep 0.5
t=$(curl -s -m 5 "localhost:$PORT/tr")
[ "$t" = "user 1" ] || { echo "  FAIL map captured by a timer callback: '$t' (want 'user 1')"; fail=1; }
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""

[ $fail = 0 ] && echo "PASS: a list never passes for a map (JSON, form, query, literals; maps survive threads, JSON, .=)" \
              || { echo "FAIL: list/map confusion"; exit 1; }
