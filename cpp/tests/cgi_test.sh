#!/usr/bin/env bash
# lk-cgi — the CGI binary (Apache/Plesk runs it once per request) serves a LOOK application.
# Called the way Apache calls it: environment variables, a body on stdin; the output is
# "Status/headers, blank line, body".
#   * a route answers; a path parameter, a query parameter and a JSON body reach the handler
#   * response::json sets Content-Type; response::status and response::header are kept
#   * a path no route matches gets 404, and route("404", fn) answers it with status 404
#   * a script without routes runs whole and its output is the body
#   * a run-time error in a handler is a 500; a script that does not load is an error status
# Usage: cgi_test.sh <lk-cgi>
CGI="$(cd "$(dirname "${1:?usage: $0 <lk-cgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
# req <script> <method> <path> [query] [body] — sets $status, $ctype, $hdrs, $body
req() {
    local script="$1" method="$2" path="$3" query="${4:-}" data="${5:-}"
    local raw
    raw="$(cd "$TMP" && printf '%s' "$data" | env -i PATH="$PATH" APP_ENV=development \
        REQUEST_METHOD="$method" SCRIPT_FILENAME="$TMP/$script" PATH_INFO="$path" REQUEST_URI="$path${query:+?$query}" \
        QUERY_STRING="$query" CONTENT_TYPE="application/json" CONTENT_LENGTH="${#data}" \
        GATEWAY_INTERFACE="CGI/1.1" "$CGI" 2>/dev/null | tr -d '\r')"
    hdrs="$(printf '%s\n' "$raw" | sed '/^$/q')"
    body="$(printf '%s\n' "$raw" | sed '1,/^$/d')"
    status="$(printf '%s\n' "$hdrs" | grep -i '^Status:' | head -1 | awk '{print $2}')"; status="${status:-200}"
    ctype="$(printf '%s\n' "$hdrs" | grep -i '^Content-Type:' | head -1 | cut -d' ' -f2-)"
}

cat > "$TMP/app.lk" <<'LK'
route("GET", "/", fn() => response::text("home"))
route("GET", "/u/{id}", function($id) {
    $q = request::get("q") ?? "none"
    return response::text("user {$id} q={$q}")
})
route("POST", "/echo", function() {
    $data = request::json()
    $name = $data["name"]
    return response::json(["got" => $name])
})
route("GET", "/made", function() {
    response::status(201)
    response::header("X-Made", "yes")
    return response::text("made")
})
route("GET", "/boom", function() { throw "handler broke" })
route("404", fn() => response::text("custom not found"))
LK

req app.lk GET /
[ "$status" = 200 ] && [ "$body" = home ] && ok "GET / answers" || bad "home: status=$status body=[$body]"
req app.lk GET /u/42 "q=x"
[ "$body" = "user 42 q=x" ] && ok "a path parameter and a query parameter reach the handler" || bad "params: [$body]"
req app.lk POST /echo "" '{"name":"ada"}'
if [ "$body" = '{"got":"ada"}' ] && echo "$ctype" | grep -qi 'application/json'; then ok "a JSON body is read; response::json sets Content-Type"
else bad "json: ctype=[$ctype] body=[$body]"; fi
req app.lk GET /made
if [ "$status" = 201 ] && echo "$hdrs" | grep -qi '^X-Made: yes' && [ "$body" = made ]; then ok "response::status and response::header are kept"
else bad "status/header: status=$status hdrs=[$hdrs] body=[$body]"; fi
req app.lk GET /nothing-here
[ "$status" = 404 ] && [ "$body" = "custom not found" ] && ok "an unmatched path gets 404 from route(\"404\", fn)" || bad "404: status=$status body=[$body]"
req app.lk GET /boom
[ "$status" = 500 ] && ok "a run-time error in a handler is a 500" || bad "error: status=$status body=[$body]"

printf 'route("GET", "/", fn() => response::text("home"))\n' > "$TMP/plain404.lk"
req plain404.lk GET /missing
[ "$status" = 404 ] && ok "without route(\"404\") an unmatched path is still 404" || bad "plain 404: status=$status body=[$body]"

printf '$n = 2 + 3\nprint("sum={$n}")\n' > "$TMP/script.lk"
req script.lk GET /
[ "$body" = "sum=5" ] && ok "a script without routes runs whole; its output is the body" || bad "script: [$body]"

printf 'route("GET", "/", fn() => response::text("x")\n' > "$TMP/broken.lk"
req broken.lk GET /
[ "${status:-200}" -ge 500 ] && ok "a script that does not load is an error status ($status)" || bad "broken: status=$status body=[$body]"

[ $fail = 0 ] && echo "PASS: lk-cgi serves a LOOK application" || { echo "FAIL: lk-cgi"; exit 1; }
