#!/usr/bin/env bash
# installer:: parse-layer guard — NO NETWORK, CI-runnable. parse_pkg (installer.cpp:101-174)
# does all these rejections BEFORE downloading → no network. (Zip-slip/redirect need a real download/
# 3xx → the C++ unit test is a targeted follow-up; the full installer_test.sh needs the network and stays manual.)
# Scope: host restriction (github.com only) · invalid name · path-traversal (user + subdir).
# fail-loud + positive control: a legitimate name must NOT be rejected (otherwise the rejection rejects everything).
set -u
LK="${1:-./build/lk}"
fail=0

expect_reject() {  # $1=label $2=package $3=expected-message-part
  local out; out=$("$LK" module install "$2" 2>&1 | head -2)
  if echo "$out" | grep -qi "$3"; then echo "  OK $1: rejected"
  else echo "  FAIL $1: rejection expected ('$3'), got: [$out]"; fail=1; fi
}

# NOTE: the parts match the ENGLISH runtime strings of installer.cpp (language policy).
expect_reject "host restriction (non-github.com)"  "evil.com/user/repo"                 "Only github.com"
expect_reject "invalid name (single component)"       "github.com/tekbasina"               "user/repo required\|Invalid package"
expect_reject "traversal (user component)"        "github.com/../../etc/passwd"        "path-escape"
expect_reject "traversal (subdir component)"      "github.com/user/repo/../../../evil" "path-escape"

# POSITIVE CONTROL (Rule 1): a legitimate package name must PASS the parse (it must reach the download/404, not the rejection stage).
# Otherwise the parse rejects everything → the rejections above are meaningless (false green).
out=$("$LK" module install "github.com/codlook/look-modules/jwt" 2>&1 | head -3)
if echo "$out" | grep -qiE "path-escape|Only github.com|Invalid package"; then
  echo "  FAIL positive control: a legitimate name was REJECTED at parse (parse over-rejects): [$out]"; fail=1
else
  echo "  OK positive control: a legitimate name passed parse (reached the download/404 stage)"
fi

[ $fail = 0 ] && echo "PASS: installer:: parse layer (host/name/traversal)" || echo "FAIL: installer:: parse layer"
exit $fail
