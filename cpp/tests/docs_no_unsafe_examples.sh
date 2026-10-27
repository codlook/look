#!/usr/bin/env bash
# Two examples must never appear in the documentation again, in any file:
#   1. template::render_string("...") with a DOUBLE-quoted template. LOOK substitutes an
#      in-scope variable into the template source before the engine runs: the value skips
#      auto-escaping and is then executed as template code. The template goes in single quotes.
#   2. A SQL statement with a value interpolated into it ({$id}). Values go in as parameters.
# Both were fixed in llms-full.txt in 1.0.1 and stayed on the human-readable page for three
# days, because the same content is kept in two files by hand. Until the two are generated
# from one source, this rule covers every documentation file.
cd "$(dirname "$0")/../.." || exit 1
files="$(git ls-files 'docs/*.html' 'docs/*.txt' 'docs/*.md' 'docs/**/*.html' 'docs/**/*.txt' 'docs/**/*.md' README.md cpp/README.md 2>/dev/null | sort -u)"
fail=0
# In the HTML pages the call is marked up: render_string</span>(<span class="str">"...
bad="$(grep -nE 'render_string(</span>)?\((<span class="str">)?("|&quot;)' $files /dev/null || true)"
[ -z "$bad" ] || { echo "FAIL: template::render_string with a double-quoted template:"; echo "$bad" | cut -c1-170 | sed 's/^/    /'; fail=1; }
bad="$(grep -nE '\b(WHERE|AND|OR|SET|VALUES)\b[^`"]*\{(<span class="var">)?\$' $files /dev/null || true)"
[ -z "$bad" ] || { echo "FAIL: a value interpolated into SQL:"; echo "$bad" | cut -c1-170 | sed 's/^/    /'; fail=1; }
# 3. Kurulum: "curl … | sudo bash" yok ve indirme yalnız projenin adreslerinden. Ubuntu rehberi
#    kayıtlı olmayan bir alan adından (get.look-lang.org) root olarak betik çalıştırtıyordu:
#    o adı alan herkes rehberi izleyenin sunucusunda kod çalıştırırdı.
ifiles="$( { echo "$files"; git ls-files '*.md' 'platforms/**/*.md' 'platforms/**/*.sh' 2>/dev/null | grep -v '^cpp/tests/'; } | sort -u)"
bad="$(grep -nE '(curl|wget)[^|]*\|[[:space:]]*(sudo[[:space:]]+)?(ba|z)?sh\b' $ifiles /dev/null || true)"
[ -z "$bad" ] || { echo "FAIL: a download piped into a shell:"; echo "$bad" | cut -c1-170 | sed 's/^/    /'; fail=1; }
bad="$(grep -nE '(curl|wget)[^#]*https?://' $ifiles /dev/null | grep -vE 'https?://(github\.com/codlook/|([a-z0-9-]+\.)?codlook\.com\b|(127\.0\.0\.1|localhost|\$|<|\{))' | grep -E '(curl|wget)[^#]*https?://[a-z]' | grep -vE 'https?://[^/ ]*(example|myapp|sunucu|domain)' || true)"
[ -z "$bad" ] || { echo "FAIL: a download from an address that is not the project's (github.com/codlook, codlook.com):"; echo "$bad" | cut -c1-170 | sed 's/^/    /'; fail=1; }
# 4. Belgedeki her systemd servis örneği bir kullanıcıyla çalışır, root'la değil. Plesk elle kurulum
#    rehberi User= olmadan servis yazdırıyor ve "root yaygın ve pratik" diyordu (eklenti 1.2.0'da
#    kapattığımız açığın belgedeki kopyası).
mdfiles="$(echo "$ifiles" | grep '\.md$')"
bad="$(awk 'FNR==1{inb=0} /^```/{ if(inb){ if(svc && exe && !usr) print FILENAME":"start": systemd unit without User= (or User=root)"; inb=0 } else { inb=1; svc=0; exe=0; usr=0; start=FNR } next }
     inb && /^\[Service\]/{svc=1} inb && /^ExecStart=.*(lk|look)-(fcgi|cgi)/{exe=1} inb && /^User=/ && !/^User=root/{usr=1}' $mdfiles /dev/null || true)"
[ -z "$bad" ] || { echo "FAIL: a service example that runs LOOK as root:"; echo "$bad" | sed 's/^/    /'; fail=1; }
# 5. Web sunucusu lk-fcgi'dir; "lk --mode http" bir dosya adı sanılır ve çalışmaz (SMTP/IMAP belgeleri).
bad="$(grep -nE '(^|[[:space:]`>])lk --mode http' $ifiles /dev/null | grep -v '^dogfood/' || true)"
[ -z "$bad" ] || { echo "FAIL: 'lk --mode http' (the server is lk-fcgi):"; echo "$bad" | cut -c1-170 | sed 's/^/    /'; fail=1; }
[ $fail = 0 ] && echo "PASS: no unsafe examples in the documentation ($(echo "$files" | wc -l) files)" || exit 1
