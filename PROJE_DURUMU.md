# LOOK — Proje Durumu

> **Son güncelleme:** 2026-07-17 · **Uç commit:** `d2df9d3` · **73 commit** (2026-07-10 → 2026-07-17)
> **Canlı:** [look.codlook.com](https://look.codlook.com) · [qrmenu.codlook.com](https://qrmenu.codlook.com) — ikisi de sağlıklı (HTTP 200)

Bu dosya projenin **gerçek** durumunu tutar: ne bitti, ne kaldı, neyi **ölçüp reddettik**, hangi
riskler açık. Ölçümler VPS'te (AlmaLinux 8, gerçek ortam) alındı; iddialar commit'e bağlandı.

---

## 1. Bir bakışta

| | |
|---|---|
| **Felsefe** | "PHP gibi dağılan, Go gibi çalışan" — Go-tarzı ucuz eşzamanlılık + PHP kolaylığında dağıtım |
| **Hedef bant** | Go ile Node.js arası |
| **Hedef kitle** | Küçük ve orta işletmeler (KOBİ) |
| **Bağımlılık** | **Sıfır** — MySQL/PostgreSQL/SQLite/RESP2 gömülü, flag yok |
| **Durum** | **Üretimde çalışıyor.** İki canlı site aynı binary'yi paylaşıyor |
| **Ana açık** | `http_client` fiber-aware değil; fiber default kararı verilmedi |

**Dış dünya karşılaştırması** (VPS, 2 CPU / 4 GB, best-of-3, 200 istek warmup):

| Metrik | LOOK | PHP 8.3 (JIT+FPM) | Sonuç |
|---|---|---|---|
| Throughput (r/s) | **2837** | 2184 | **PHP geçildi** (+%30) |
| RAM | **3–5× daha az** | — | **lider** |
| CPU / istek | **en düşük** | — | **lider** |

> Ölçüm dürüstlüğü: PHP **JIT açık** ve **php-fpm + nginx** ile ölçüldü (dev SAPI `php -S` **değil**).
> Bkz. §7 "Ölçüm metodolojisi ve tuzaklar".

---

## 2. Mimari — iki motor ve C9

LOOK'ta iki yürütme motoru var: **tree-walk interpreter** (referans semantik) ve **bytecode VM**
(hızlı yol). **C9** = VM'i her yerde tek motor yapma işi.

| Bileşen | Motor | Durum |
|---|---|---|
| `lk` (CLI + `-c`) | **VM DEFAULT** — kaçış kapağı `LOOK_CLI_VM=0` | ✅ **~41–51×** (3652ms → 72ms) |
| `lk-fcgi` (web) | VM default + route-bazlı interpreter fallback | ✅ fallback artık **gürültülü** |
| `lk-cgi` (CGI) | yalnız tree-walk | ⬜ VM binary'ye linklenmemiş |
| REPL / `lk test` | yalnız tree-walk | ⬜ ayrı komutlar |

**Fallback neden tehlikeli:** Route sessizce yavaş yola düşer, **doğru sonuç döner** → bug yıllarca
görünmez. 2026-07-16'da bulunan bug'ların çoğu böyle saklanmıştı. Artık fallback **ERROR + "VM BUG"**
basar; `LOOK_VM_STRICT=1` maskelemeyi tamamen kapatır (CI/staging için). Test ederken
**"VM BUG" sayısı 0 olmalı**.

**Fallback güvenliği:** VM yoluna ancak tüm `use`'lar stdlib **ve** compile başarılıysa girilir
(ikisi de execution **öncesi**). Çıktı taahhüt edildikten sonra fallback **yok**.

---

## 3. Performans işleri

| # | İş | Durum | Ölçüm | Commit |
|---|---|---|---|---|
| A1 | regs bounds-check kaldır | ✅ | loop 0.746s→0.663s (~%11) | `1fa2c7e` |
| **B5** | **`Value` 80→32 bayt** (skaler union, STRING pointer arkasında) | ✅ | skaler **+%40**, **RAM 2.5×** | `5aa03c0` |
| B5+ | str_ref hot-path (B5 regresyon geri kazanımı) | ✅ | string-ağır −%15 → −%8 | `b04cdc7` |
| B7 | string concat O(n²)→O(n) | ✅ | 400k `.=`: 10.39s→0.029s (**~358×**) | `0930d04` |
| C9 | CLI-VM default + VM hata konumu (File/Line) | ✅ | **~41–51×** | `82390e8` |
| — | **dispatch kopyası per-request → thread_local** | ✅ | web **2–3×**, gerçek qrmenu **+%26** | `adfc3c0` |
| — | 16-bit `CALL_BUILTIN` (256 duvarı kalktı) + 30 builtin | ✅ | fallback sınıfı kapandı | `edfdaa5` |
| — | worker/DB-pool cgroup CPU limitine uyar | ✅ | tutarlılık | `9da6c2a` |
| A2 | global inline cache | 🟡 **yarım** | `str_ref` yapıldı, **cache yok** | — |
| A3 | computed-goto dispatch | ⬜ | ~%20 bekleniyor | — |
| A4 | builtin arg allocation (stack buffer + span) | ⬜ | düşük-orta | — |
| B8 | arena + non-atomic refcount | ⛔ **güvenli değil** (§6) | — | — |
| B6 | nesne → hash map | ❌ **ölçülüp reddedildi** (§5) | — | — |

**En büyük dersin özeti:** "PHP'yi geçemiyoruz" darboğazı **motorda değildi**. Her istekte
`make_dispatch_copy()` tüm stdlib'i yeniden kuruyordu (dispatch 30µs — route'u çalıştırmaktan pahalı)
ve globals'ı deep-clone ediyordu. `thread_local` kopya ile web throughput **2–3×** arttı. Darboğazı
motorda aramak yerine **PROF logunu okumak** çözdü.

---

## 4. Bug avı — kapatılan 16 bug

Renk = etki sınıfı. 🔴 = üretimde veri/erişim kaybı, 🟠 = yanlış davranış, 🟡 = sessiz yavaşlama.

| Bug | Sınıf | Commit |
|---|---|---|
| **ODR: `look::HttpResponse` iki farklı tip** — web route'unda `http::get` **sunucuyu çökertiyordu** | 🔴 crash | `d2df9d3` |
| `array::sort()` comparator'ı VM route'unda **sessizce yok sayılıyordu** — canlı yanlış sıralama | 🔴 veri | `6a58a8a` |
| `parallel()` task'ı DB bağlantısını havuza **iade etmiyordu** — kalıcı sızıntı, endpoint donması | 🔴 erişim | `3459c3e` |
| `db::transaction` VM closure'ını çalıştırmıyordu — **sessiz commit** | 🔴 veri | `4c68f8e` |
| LOOK `throw`'u C++ sınırını geçiyordu (try tabanı) | 🟠 | `4c68f8e` |
| `error::new` tipli payload string'e düşüyordu → `error::is()` sessizce `false` | 🟠 | `235df9e` |
| VM'de `print`/`write` newline+ayraç yok; `exit()`/`die()` çalışmıyor; CLI exit kodu | 🟠 | `4110cbf` |
| Implicit closure capture — `fn($x)=>$x*$m` VM'de `$m`'i görmüyordu (0) | 🟠 | `58eb033` |
| CLI-VM builtin-olmayan `mod::fn`'de tree-walk'a düşmüyordu + 256 sınır guard'ı | 🟠 | `7af8a6c` |
| Tanımsız değişken: iki motor ayrışıyordu → **STRICT** hizalandı + interpolation parity | 🟠 | `9f45233` |
| `type::` / `crypto::` builtin_names'de yoktu | 🟡 | `2b09b8a` |
| `math::` / `string::` (format, regex) yoktu | 🟡 | `29de18a` |
| VM try/catch C++ runtime hatalarını yakalamıyordu + `$`-önekli interpolation | 🟡 | `04a9552` |
| bare `header()` / `redirect()` | 🟡 | `0bb9895` |
| bare `join`, bare `push`/`pop` | 🟡 | `a3682be`, `2fc47ee` |
| higher-order `array::map/filter/reduce` + `array::` modülü + `len`/`json`/`intval` | 🟡 | `58eb033`, `22b8a70`, `c8dfdf0` |

### ODR bug'ı — neden aylarca görünmedi (kalıcı ders)

`look::HttpResponse` **hem** `http_client.h` **hem** `http_server.h` içinde, **aynı namespace'te**,
**farklı layout** ile tanımlıydı:

| | istemci (`http_client.h`) | sunucu (`http_server.h`) |
|---|---|---|
| alanlar | `status`, `body`, `headers`, `error` | `status_code`, `status_text`, `headers`, `set_cookies`, `body`, `keep_alive`, `build()` |

- `lk` (CLI) yalnız `http_client.cpp` linkler → **tek tanım** → sorun yok.
- `lk-fcgi` (web) `http_client.cpp` **ve** `http_server.cpp` linkler → **ODR ihlali** → LTO iki
  layout'u tek tip sanıp birleştiriyor → nesne bir düzenle kurulup ötekiyle yıkılıyor →
  `free(): invalid pointer` / segfault. **Tek istek** sunucuyu düşürüyordu.

**Kör noktamız buydu:** ASan build'inde **LTO kapalı** (`CMakeLists.txt`: `if(LOOK_SANITIZE) … else()
IPO ON`). Yani **ASan bu bug sınıfını yapısal olarak göremez**. → **"ASan temiz" bellek hatası
yokluğunun kanıtı DEĞİLDİR.** Release-only crash görürsen ilk hipotez **ODR / link farkı** olsun:
hedeflerin hangi `.cpp`'leri linklediğini karşılaştır, header'lardaki tip adlarını `comm -12` ile çakıştır.

**Çözüm:** istemci tarafı `look::HttpClientResponse`'a taşındı. Çakışan tek isim buydu (doğrulandı).
**Guard:** `differential_test.sh`'ye ODR kategorisi eklendi — web route'unda `http::get` `{"st":200}`
dönmeli **ve sunucu ayakta kalmalı**.

---

## 5. Ölçülüp REDDEDİLEN işler

Bunlar "yapılmadı" değil — **yapılmaması gerektiği ölçümle gösterildi**. Kayda geçiyor ki tekrar
gündeme gelmesin.

| İş | Beklenti | Ölçüm | Karar |
|---|---|---|---|
| **B6 — nesne → hash map** | "Web'de en yüksek etki" | 4→16 alanda yalnız **+%12**; başabaş **~32-64 alan** | ❌ **Reddedildi** — web kayıtları 5–20 alan → hash map **zararlı** olurdu |
| Apache `enablereuse` | keep-alive kazancı | Apache serileştirmesi → baseline'dan **yavaş** | ❌ Kapalı |
| Fiber'i default yapmak | eşzamanlılık | Eşzamanlılığı fiber değil **ConnPool** sınırlıyor; p95 ~2× kötü | ⏸ **Karar verilmedi** (§6) |

**Kalibrasyon (VPS kanıtı):** Gerçek web **DB-bound**. Ham-motor micro-opt'ları (A2/A3/A4/B6)
throughput'a **nötr** çıktı; değerleri yalnız CPU-bound edge-case + doğruluk. **Asıl kazanç
tutarlılık + RAM + async'te.**

---

## 6. Async / Fiber (Grup D)

**Hedef Go modeli** — Node taklidi değil: goroutine'ler (ucuz yeşil-thread), channel'larla CSP, kod
düz-blocking yazılır, runtime ucuzca multiplex eder. `async/await` **yok** (colored functions yok).

**Mimari zaten Go-goroutine:** stackful fiber (guard-page), per-fiber local, `wait_readable`
(epoll-park/resume = Go netpoller), channel/pool entegrasyonu. Substrat ciddi ve doğru — sadece kapalıydı.

| İş | Durum | Kanıt / not |
|---|---|---|
| Fix #1 — `recv` fiber-aware | ✅ | c=100 **hang kök nedeni**: `handle_connection` bloklayan `::recv()` kullanıyordu → keep-alive'da worker bloklanıyordu (`ec8dc12`) |
| Fix #2 — acceptor fiber (accept scheduler epoll'unda) + 15s idle-timeout | ✅ | c=200 / c=500 / c=1000 → **0 hata** (eski: c=100 hang) (`4b311c0`) |
| **`http_client` fiber-aware** | ⬜ **SIRADAKİ** | şu an thread'i blokluyor → fiber'in "dış API" değer önerisi bunsuz eksik |
| Fiber default kararı | ⏸ **verilmedi** | veri toplandı, aşağıda |
| `go { }` ergonomisi + channel select | ⬜ | — |

**Fiber default neden hâlâ kapalı (dürüst tablo):**

| Ölçüm | Sonuç |
|---|---|
| Eşzamanlılık tavanı | **fiber sayısı değil, ConnPool (= CPU sayısı) belirliyor.** Pool=64 iken FIBER w=8 → 1154 r/s (~58 eşzamanlı) **yalnız 8 thread'le**; pool bunun için w=64 gerektiriyor |
| Non-keepalive throughput | fiber 8.1k vs pool 8.5k → **parite** |
| Keep-alive | 13.6k r/s |
| p95 gecikme | fiber **~2× kötü** |
| RSS | fiber **daha yüksek** |

→ **Default pool kalıyor** (kısa CPU-bound istekte hızlı). Fiber'in değeri **yavaş/uzun I/O
eşzamanlılığında** — ve o değer `http_client` fiber-aware olmadan ortaya çıkmıyor. Sıralama bu yüzden.

`LOOK_FIBER_DISPATCH=1` ile opt-in. Deployment: fiber yalnız `--mode http` Linux worker'ında;
FastCGI/Plesk uyumu korunuyor.

### ⛔ B8 neden "güvenli değil"

B8'e (non-atomic refcount) başlamadan önce doğrulandı: **`Value`'lar thread'ler arası geçiyor** —
channel `send_val`/`recv_val` (queue'ya move, başka thread'de recv) ve `PARALLEL_CALL`
(`std::thread`'e globals kopyası). shared_ptr'ın **atomic** refcount'u bu yüzden **gerekli**. Naive
non-atomic → channel/parallel'de **data race → use-after-free** (yeni kapattığımız memory-safety işini
bozar). Güvenli B8 = arena (request-local, thread geçmeyen) + tagged-pointer → **haftalar + yüksek
risk**. **Ertelendi.** B5 string regresyonunun kalan %8'i buna bağlı ama aceleye gelmez.

---

## 7. Ölçüm metodolojisi ve tuzaklar

Bu bölüm, **yanlış ölçüp yanlış karar vermemek** için bedelini ödeyerek öğrendiklerimiz.

| Tuzak | Ne oluyordu | Doğrusu |
|---|---|---|
| `php -S` | **tek süreç** → PHP sahte-düşük (641 r/s) | `PHP_CLI_SERVER_WORKERS=16` — ya da gerçek kurulum: **nginx + php-fpm** |
| JIT sessizce kapalı | FPM pool'da `php_admin_value[opcache.jit_buffer_size]` **çalışmaz** (INI_SYSTEM) → `jit_enabled:false` | php-fpm `-d opcache.jit_buffer_size=128M -d opcache.jit=tracing` → `opcache_get_status()` ile **doğrula** |
| Tek koşum | aynı config 2179 vs 1074 r/s (**2× gürültü**) | **best-of-3** + 200 istek warmup |
| `systemctl show CPUUsageNSec` | bu systemd'de **boş** | cgroup dosyaları (`cpuacct.usage` / `cpu.stat`) |
| `cmp` / `diff` yok | AlmaLinux 8 minimal'de **diffutils yok** → "command not found" non-zero → **"baytlar farklı" yanlış alarmı** | `od` dump'larını **dosyaya** yaz, shell string karşılaştır (`$(od)` trailing newline'ı yutar) |
| Uydurma slug ile test | 404 dönüyordu = **asıl veri yolu hiç çalışmamıştı** | gerçek slug'lar + **pozitif kontrol** (bilerek bug'lı app → tespit ediliyor mu?) |
| "ASan temiz" | ASan build'inde **LTO kapalı** → ODR sınıfını göremez | Release-only crash'te ODR/link farkına bak |
| FCGI portuna `curl` | FastCGI protokolü — çalışmaz | https://domain üzerinden veya `--mode http` |
| TLS zinciri | ~750 r/s tavan (nginx→Apache **çift TLS**); direkt port ~9.8k | **Darboğaz LOOK değil** |

**Karar yöntemi (tanımsız-değişken flip'i örneği):** karar tartışarak değil **ölçerek** verildi. Önce
davranışı değiştirmeyen teşhis aracı eklendi (`LOOK_WARN_UNDEF=1`, `9995458`), **pozitif kontrolle**
doğrulandı, gerçek uygulamalar gerçek slug'larla tarandı → **0 tanımsız okuma** → flip'in risksizliği
veriyle gösterildi, sonra flip yapıldı ve **aynı uygulamalarla strict binary'de tekrar doğrulandı**.

---

## 8. Güvenlik ağı — regresyon guard'ları

**Her motor değişikliğinden (opcode/builtin/Value/köprü) SONRA çalıştır.** Dilin semantik sözleşmesi
artık repoda taşınabilir (`42647e4`).

```bash
bash cpp/tests/differential_test.sh <lk> <lk-fcgi>   # canonical
bash cpp/tests/parallel_db_test.sh  <lk-fcgi>        # tek istek YETMEZ
```

| Guard | Kapsam |
|---|---|
| `differential_test.sh` | **3 motor × 20 kategori** (A–T) — tree-walk == CLI-VM == web-VM **birebir** |
| ↳ CLI yüzeyi | `print`/`write` **bayt-bayt** (od) + `exit(3)` kodu |
| ↳ fallback gürültüsü | fallback olursa ERROR + "VM BUG"; `LOOK_VM_STRICT=1` maskelemiyor |
| ↳ izolasyon | thread_local dispatch kopyası istekler arası **sızmamalı** (`s=1` ×5) |
| ↳ **ODR** | web route'unda `http::get` → `{"st":200}` **ve sunucu ayakta** |
| `parallel_db_test.sh` | parallel + DB **sızıntısı**, havuz baskısı (vm + interp) |

**Neden 3 motor:** builtin wiring **iki ayrı elle-yazılmış yerde** duruyor (`build_cli_builtins` ve
`req_builtins`). Sessizce birbirinden kayabilir — nitekim kaydı da (`math::round` bir dönem web'de
çalışıp CLI-VM'de çalışmadı). **2 motorlu test bu kaymayı göremez.**

---

## 9. Sıradaki işler (öncelik sırasıyla)

| Sıra | İş | Neden şimdi |
|---|---|---|
| **1** | **`http_client` fiber-aware** | Fiber'in tek gerçek değer önerisi (yavaş dış I/O) bunsuz ölçülemez. ODR fix'i bu yolu yeni açtı |
| **2** | Fiber default kararı | ConnPool tavanı (§6) çözülmeden fiber'in anlamı yok |
| 3 | A2 inline cache | `str_ref` var, cache yok — yarım iş |
| 4 | `lk-cgi` / REPL / `lk test` → VM | C9'un kapanışı; motor ikiliğini bitirir |
| 5 | A3 computed-goto | ~%20, düşük risk |
| 6 | `go { }` ergonomisi + channel select | Go-eşzamanlılık ergonomisi |
| — | A4 / B8 | ertelendi (B8 güvenli değil) |

---

## 10. Bilinen sınırlar ve açık riskler

| Konu | Durum |
|---|---|
| Fallback güvenlik ağı | Hâlâ açık — bug **maskeleyebilir**. Test ederken `route disable` / "VM BUG" **0 olmalı** |
| VM hata konumu | `Line` var, **`Column` yok** (tree-walk'ta var) |
| `lk-cgi` / REPL / `lk test` | Hâlâ tree-walk → motorlar bu yüzeylerde ayrışabilir |
| B8 | Non-atomic refcount **güvenli değil** (§6) |
| Tek makineden yük testi | ab tavanı c≈20k / ~50k ephemeral port → **"1M eş zamanlı" tek kaynaktan test edilemez**; 1M **toplam istek** dayanıklılığı test edilir |
| Ekosistem | Yeni dil → kütüphane ekosistemi yok. **Argüman olarak kullanılmıyor**; hedef KOBİ ve ihtiyaçlar dilin içinde |

---

## 11. Operasyonel dersler (bedeli ödendi)

| Ders | Olay |
|---|---|
| **Paylaşımlı/prod sunucuda ASLA geniş `pkill -f`** | `pkill -f 'mode http'` prod servisleri (look+qrmenu) öldürdü → **~20dk kesinti**. PID kullan (`$!`, `ss -tlnp`) |
| **Deploy yolunu her seferinde doğrula** | Gerçek dosya **`/usr/bin/lk-fcgi`**; `/opt/look/lk-fcgi` ona işaret eden **symlink**. Kayıt bir dönem **ters** yazıyordu → deploy symlink'i düz dosyayla ezecekti (guard yakaladı). `readlink -f` ile **gerçek yola** yaz |
| Daha eski bir kayıt `/usr/local/bin/lk-fcgi` diyordu | **Öyle bir dosya yok** → oraya kopyalayan deploy hiçbir şey değiştirmez ("deploy ettim" yanılgısı). `systemctl cat <servis> \| grep ExecStart` ile teyit et |
| **İki servis aynı binary'yi paylaşır** | `look-look-codlook-com` (**9101**) + `look-qrmenu-codlook-com` (**9100**) → binary değişimi **her iki canlı siteyi birden** etkiler. Deploy **yedekli + otomatik geri-alma** yapılmalı |
| Prod'a dokunmadan test | Binary'yi ayrı isimle (`/tmp/lk-fcgi-test`) **ayrı portta** çalıştır |
| Deploy sonrası | Stale `.lkc` bytecode cache'lerini sil (`.look_cache/`) |

---

## 12. Katkı ve kimlik

- Commit/push/release'de **yalnız `Codlook`** görünür; **Co-Authored-By satırı eklenmez**.
- Tek geçerli kimlik: `Codlook <91369165+codlook@users.noreply.github.com>`.
- **GitHub katkıyı ada değil E-POSTAYA göre bağlar** — ad doğru olsa bile yanlış e-posta katkıyı
  başka bir hesaba yazar (bir kez yaşandı; amend + force-push gerektirdi).
- `.git/hooks/commit-msg` bunu **zorunlu kılıyor** (yanlış ad/e-posta veya Co-Authored-By → commit reddi).
  ⚠️ Hook `.git/hooks` içinde olduğu için **klonla taşınmaz** — yeni klonda yeniden kurulmalı.
- Commit mesajları Türkçe, `Fix:` / `Feat:` / `Perf:` önekiyle.
