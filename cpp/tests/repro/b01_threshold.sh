#!/usr/bin/env bash
# B-01: degerlendirme ozyinelemesi sinirsiz — cokme esigini bul.
# Kullanim: ./b01_threshold.sh /yol/lk
LK="${1:?kullanim: $0 /yol/lk}"
gen(){ python3 -c "print('\$x = ' + '+'.join(['1']*$1))" > /tmp/b01.lk; }
probe(){ gen "$1"
  out=$(env $2 ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 timeout 120 $LK $3 /tmp/b01.lk 2>&1)
  rc=$?
  if echo "$out" | grep -qE "stack-overflow|SEGV|Segmentation"; then echo CRASH
  elif [ $rc -ge 128 ]; then echo "CRASH(sig$((rc-128)))"
  else echo OK; fi; }
printf "%-8s %-9s %-9s %-9s\n" N --check VM interp
for n in 200 400 500 1000 2000 4000 8000 16000 32000; do
  printf "%-8s %-9s %-9s %-9s\n" "$n" \
    "$(probe $n '' --check)" "$(probe $n '' '')" "$(probe $n LOOK_CLI_VM=0 '')"
done
echo
echo "Beklenen dogru davranis: her sutun, parser guard'i (max 150) ile TUTARLI bir"
echo "noktada temiz 'cok derin' hatasi vermeli — hicbir N'de CRASH olmamali."
