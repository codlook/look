#!/usr/bin/env bash
# A test script that changes directory makes its binary arguments absolute first.
#
# CI calls the tests from cpp/ with relative paths (./build/lk). A script that changes directory
# (into its temporary folder, often only inside a subshell) and then runs "$LK" no longer finds
# the binary: "No such file or directory" — and a test that checks for a wrong answer reports a
# failure of LOOK, or passes for the wrong reason. Run locally with absolute paths, the mistake
# never shows; it broke CI three times.
# Rule: in a script with a `cd` other than `cd "$(dirname "$0")..."`, a variable read from a
# positional argument ($1, ${2:-...}) and run as a command ("$LK" ..., exec/env/timeout "$LK")
# is made absolute: X="$(cd "$(dirname "$X")" && pwd)/$(basename "$X")" (or realpath /
# readlink -f), on its own line or in the reading line.
# Usage: test_path_guard.sh   (run from anywhere)
cd "$(dirname "$0")" || exit 1
bad=""
n=0
for f in *.sh; do
    case "$f" in _*|test_path_guard.sh) continue ;; esac   # _*.sh: yerel karalama betikleri
    n=$((n + 1))
    out=$(awk -v F="$f" '
        { L[NR] = $0 }
        END {
            for (i = 1; i <= NR; i++) {
                s = L[i]; sub(/#.*$/, "", s)
                if (s ~ /(^|[;&|({ \t])cd[ \t]/ && s !~ /cd[ \t]+"?\$\(dirname[ \t]+"?\$0/) movecd = 1
            }
            if (!movecd) exit
            # En üst düzeyde konum argümanından okunan değişkenler: NAME="$1" / NAME="${1:-…}"
            for (i = 1; i <= NR; i++) {
                s = L[i]
                if (s ~ /^[ \t]*local[ \t]/) continue                     # fonksiyon argümanı
                if (s ~ /="?\$\{[1-9]:-[0-9]+\}"?/) continue              # sayısal argüman (port)
                while (match(s, /(^|[; \t])[A-Za-z_][A-Za-z0-9_]*="?\$\{?[1-9]/)) {
                    seg = substr(s, RSTART, RLENGTH); sub(/^[; \t]/, "", seg)
                    v = seg; sub(/=.*/, "", v)
                    if (!(v in line)) line[v] = i
                    s = substr(s, RSTART + RLENGTH)
                }
            }
            for (v in line) {
                absd = 0; ran = 0
                for (i = 1; i <= NR; i++) {
                    s = L[i]; sub(/^[ \t]*#.*$/, "", s)
                    if (s ~ ("(^|[; \t])" v "=\"?\\$\\(cd[ \t]+\"?\\$\\(dirname[ \t]+\"?\\$\\{?(" v "|[1-9])")) absd = 1
                    if (s ~ ("(^|[; \t])" v "=.*(realpath|readlink[ \t]+-f)")) absd = 1
                    if (s ~ ("(^|[; \t])" v "=\"?\\$\\(pwd\\)/\\$\\{?" v)) absd = 1          # göreliyse $(pwd)/ önüne
                    # komut olarak çalıştırılıyor mu: satır/alt kabuk başında, exec/env/timeout/&&/; sonrası
                    if (s ~ ("(^|[;&|(]|exec|env[^|;]*|timeout[ \t]+[0-9.]+[a-z]?)[ \t]*\"?\\$\\{?" v "\\}?\"?([ \t]|$)")) ran = 1
                }
                if (ran && !absd) print F ":" line[v] ": " v " is run as a command but not made absolute: " L[line[v]]
            }
        }' "$f")
    [ -n "$out" ] && bad="$bad$out"$'\n'
done
bad="$(echo "$bad" | sed '/^$/d')"
if [ -n "$bad" ]; then
    echo "FAIL: a test script changes directory but runs a binary argument as given (a relative ./build/lk breaks):"
    echo "$bad" | sed -E 's/[ \t]+/ /g; s/^/    /'
    echo "  Make it absolute at the top: X=\"\$(cd \"\$(dirname \"\$X\")\" && pwd)/\$(basename \"\$X\")\""
    exit 1
fi
echo "PASS: every test script that changes directory runs its binary arguments by an absolute path ($n scripts)"
