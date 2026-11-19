#!/usr/bin/env bash
set +e
cd /look/cpp
LK=./build/lk; FCGI=./build/lk-fcgi
pass=0; fail=0
chk(){ if [ "$2" = "$3" ]; then echo "  PASS $1 ($2)"; pass=$((pass+1)); else echo "  FAIL $1: beklenen '$3' geldi '$2'"; fail=$((fail+1)); fi; }

echo "=== A) Motor guard'lari (differential + closure) ==="
bash tests/differential_test.sh $LK $FCGI 2>&1 | grep -E '^PASS|^FAIL' | head -1

echo "=== B) int64 karsilastirma (d752afe) ==="
printf '$a=9007199254740992+1\n$b=9007199254740992\nprint(($a==$b) . "," . ($a>$b))\n' > /tmp/t_int.lk
chk "int64-VM"  "$($LK /tmp/t_int.lk 2>&1)" "false,true"
chk "int64-TW"  "$(LOOK_CLI_VM=0 $LK /tmp/t_int.lk 2>&1)" "false,true"

echo "=== C) cache izolasyon (c7c7427) ==="
printf 'use cache;\ncache::set("k",[1,2,3])\n$a=cache::get("k")\npush($a,99)\nprint(count(cache::get("k")))\n' > /tmp/t_cache.lk
chk "cache-VM" "$($LK /tmp/t_cache.lk 2>&1)" "3"

echo "=== D) channel izolasyon (9670c18) ==="
printf '$ch=channel(1)\n$arr=[1,2,3]\nsend($ch,$arr)\npush($arr,99)\nprint(count(receive($ch)))\n' > /tmp/t_chan.lk
chk "channel-VM" "$($LK /tmp/t_chan.lk 2>&1)" "3"

echo "=== E) VM type-confusion throw degil crash (denetci #1, f25955a) ==="
printf 'send([1,2,3],42)\n' > /tmp/t_tc.lk
$LK /tmp/t_tc.lk >/tmp/tc.out 2>&1; ec=$?
if [ $ec -ne 0 ] && [ $ec -ne 139 ] && grep -qiE 'kanal|channel|error|hata' /tmp/tc.out; then echo "  PASS type-confusion (graceful throw ec=$ec)"; pass=$((pass+1)); else echo "  FAIL type-confusion (ec=$ec crash olabilir)"; fail=$((fail+1)); fi

echo "=== F) parser stack-overflow graceful (denetci #2) ==="
printf 'print(%s1)\n' "$(printf '!%.0s' {1..5000})" > /tmp/t_so.lk 2>/dev/null
$LK /tmp/t_so.lk >/tmp/so.out 2>&1; ec=$?
if [ $ec -ne 139 ]; then echo "  PASS stack-overflow (graceful ec=$ec, segfault degil)"; pass=$((pass+1)); else echo "  FAIL stack-overflow (segfault 139)"; fail=$((fail+1)); fi

echo "=== G) installer path-traversal (90c84bc) ==="
out=$($LK install 'github.com/x/y/../../etc/z' 2>&1 | head -1)
if echo "$out" | grep -qiE 'gecersiz|yol-kacis|invalid'; then echo "  PASS installer-traversal (reddedildi)"; pass=$((pass+1)); else echo "  FAIL installer-traversal: $out"; fail=$((fail+1)); fi

echo "=== H) substr/slice clamp (denetci #4) ==="
printf 'print(string::substr("world",5,999))\n' > /tmp/t_sub.lk
$LK /tmp/t_sub.lk >/tmp/sub.out 2>&1; ec=$?
if [ $ec -ne 139 ]; then echo "  PASS substr-clamp (graceful ec=$ec)"; pass=$((pass+1)); else echo "  FAIL substr (crash)"; fail=$((fail+1)); fi

echo ""
echo "=== ARA SONUC (CLI): $pass PASS, $fail FAIL ==="
echo "PASS=$pass FAIL=$fail" > /tmp/cli_result
