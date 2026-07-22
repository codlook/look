# LOOK — Bug Avı Haritası

> **Amaç:** Rastgele arama yerine **sistematik av**. Bu dosya "nereye bakılacak, hangi
> yöntemle, hangi testle" sorularının tek kaynağıdır.
> **Son güncelleme:** 2026-07-19 · Kapatılan bug: **48** · Guard: 3 motor × 22 kategori + 36 özel kontrol + DB sürüm-farkındalık (MySQL matris + PG transaction)
>
> **Bölüm 7 = kapatılan 48 bug'ın tam listesi** (kök neden + çözüm + commit).
>
> **Kardeş dosyalar:** `PROJE_DURUMU.md` (yerel) · `DENETIM.md` (güvenlik denetimi, yerel)

---

## 0. Avın altın kuralları

Bu 7 kural 48 bug'ın hepsinden çıkarıldı. Her ava başlarken oku.

| # | Kural | Nereden öğrendik |
|---|---|---|
| 1 | **Guard'ın kapsamadığı yüzey = bug'ın saklandığı yüzey.** Yeni bug arıyorsan önce "neyi test etmiyoruz?" diye sor | print/write, sort(comparator), top-level compound, template — hepsi guard boşluğundaydı |
| 2 | **Kopya semantik = kaçınılmaz ayrışma.** Bir kural iki yerde yazılıysa er geç sapar | template truthiness/float dilden kopyalanmıştı, ikisi de saptı |
| 3 | **Sessiz olan, gürültülü olandan tehlikelidir.** Hata veren bug bulunur; yanlış değer döneni kimse görmez | sort comparator, compound assign, `{#if "0"}` — hiçbiri hata vermiyordu |
| 4 | **Bir bug bulduysan SINIFINI ara.** Tek tek düzeltme, deseni grep'le | `== Value::FUNCTION` taraması 3 bug birden çıkardı |
| 5 | **Pozitif kontrol olmadan "temiz" demek anlamsız.** Testin bug'ı yakalayabildiğini kanıtla | U kategorisini fonksiyon içine yazmıştım → bug'ı yakalamazdı, fark edip taşıdım |
| 6 | **Listede yazıyor ≠ kodda öyle.** Her iddiayı kaynakta doğrula | packages.js "http modülünü kur" diyordu, http gömülüydü |
| 7 | **Şüpheli sonuçta önce KENDİ ARACINI doğrula.** Kodu suçlamadan önce "test ettiğim şey gerçekten test etmek istediğim şey mi?" | `json::encode` ters bölüyü bozuyor sandım — heredoc ve `printf` `\\`'yi iki kez yutmuş, LOOK'a giden kaynak zaten `"a\b"` (backspace) oluyordu. LOOK doğruydu. Ayrıca **bayat binary** ile ölçüp "düzelmedi" sanmıştım (→ hedef bazlı tazelik kontrolü), ve CRLF'e dönmüş guard script'i sessizce hiç koşmuyordu |

---

## 1. Yüzey envanteri (gerçek, kaynaktan çıkarıldı)

### 1a. Dil çekirdeği — alt sistemler

| Alt sistem | Dosya | Satır | Guard | Risk |
|---|---|---|---|---|
| Interpreter (tree-walk, **referans semantik**) | `interpreter.cpp` | 2192 | ✅ differential | Orta |
| Web stdlib (request/response/db/session) | `web_stdlib.cpp` | 1495 | 🟡 kısmi | **Yüksek** |
| HTTP main (dispatch, hot-reload, rate-limit) | `http_main.cpp` | 1493 | 🟡 kısmi | **Yüksek** |
| **Compiler (AST→bytecode)** | `compiler.cpp` | 1458 | 🟡 kısmi | **Yüksek** |
| SMTP server | `smtp_server.cpp` | 1373 | ✅ smtp_test (yeni) | Orta |
| Extra stdlib | `extra_stdlib.cpp` | 1246 | 🟡 kısmi | Orta |
| PostgreSQL wire | `postgres_client.cpp` | 1023 | ✅ sahte sunucu (yeni) | **Yüksek** |
| IMAP server | `imap_server.cpp` | 1013 | ✅ imap_test (yeni) | Orta |
| HTTP server | `http_server.cpp` | 976 | 🟡 kısmi | **Yüksek** |
| Stdlib (çekirdek) | `stdlib.cpp` | 914 | 🟡 kısmi | Orta |
| **VM (bytecode yürütücü)** | `vm.cpp` | 863 | ✅ differential | **Yüksek** |
| FCGI main | `fcgi_main.cpp` | 860 | ⬜ yok | Orta |
| **Parser** | `parser.cpp` | 838 | 🟡 22 kenar durum | **Yüksek** |
| HTTP client | `http_client.cpp` | 822 | ✅ SSRF + IPv6 (yeni) | Orta |
| MySQL wire | `mysql_client.cpp` | 761 | ✅ iki-sürüm auth (yeni) | **Yüksek** |
| Template motoru | `template_stdlib.cpp` | 702 | ✅ yeni eklendi | Orta |
| Event loop | `event_loop.cpp` | 658 | ✅ fd sızıntı + per-IP (yeni) | Orta |
| Installer (paket/modül) | `installer.cpp` | 617 | ✅ installer_test (yeni) | Orta |
| Test runner | `test_runner.cpp` | 593 | ⬜ yok | Düşük |
| Web context (multipart, cookie) | `web.cpp` | 558 | 🟡 kısmi | **Yüksek** |
| Fiber scheduler | `fiber_posix.cpp` | 483 | 🟡 yük testi | Orta |
| **Lexer** | `lexer.cpp` | 458 | ⬜ yok | Orta |
| Jobs | `jobs_stdlib.cpp` | 457 | ✅ jobs_test — çok süreçli claim (yeni) | Orta |
| DKIM | `dkim.cpp` | 413 | ⬜ yok | Düşük |
| Date | `date_stdlib.cpp` | 348 | ⬜ yok | Orta |
| REPL | `repl.cpp` | 336 | ⬜ yok | Düşük |

### 1b. Builtin yüzeyi — **286 fonksiyon / 30 modül**

Sayı = o modüldeki builtin adedi. **Guard sütunu**: differential gövdesinde test ediliyor mu.

| Modül | # | Guard | Modül | # | Guard |
|---|---|---|---|---|---|
| `array::` | 26 | ✅ E,G,P | `cache::` | 7 | 🟡 T |
| `string::` | 24 | ✅ B,I | `template::` | 6 | ✅ yeni |
| `crypto::` | 20 | ✅ N | `session::` | 5 | ⬜ |
| `request::` | 19 | ⬜ | `parallel::` | 4 | 🟡 parallel_db |
| `math::` | 14 | 🟡 A | `mail::` | 4 | ⬜ |
| `type::` | 12 | ✅ M | `error::` | 4 | ✅ Q |
| `db::` | 12 | 🟡 parallel_db | `cookie::` | 4 | ⬜ |
| `date::` | 12 | ✅ V | `app::` | 4 | ⬜ |
| `jobs::` | 10 | ⬜ | `html::` | 3 | ⬜ |
| `http::` | 9 | 🟡 ODR | `runtime::` | 2 | ⬜ |
| `file::` | 8 | ⬜ | `route::` | 2 | ⬜ |
| `validator::` | 7 | 🟡 T | `json::` | 2 | 🟡 |
| `response::` | 7 | 🟡 | `auth::` | 2 | ⬜ |
| `queue::` | 7 | 🟡 T | `uuid::`/`look::` | 2 | ⬜ |

---

## 2. Bug sınıfı kataloğu — **desen → başka nerede olabilir**

Her sınıf gerçek bir bug'dan çıkarıldı. **Yeni av = bu desenleri henüz bakılmamış yüzeylerde aramak.**

### S1 · Callback tip kontrolü fazla dar 🔴
**Desen:** `if (v.type() == Value::FUNCTION)` — VM closure'ı `BYTECODE_FN` geldiği için **sessizce reddedilir**, callback hiç çalışmaz.
**Yakalanan:** `db::transaction` (sessiz COMMIT — veri kaybı), `array::sort` comparator (yanlış sıralama), `type::is_function` (false).
**Nerede ara:** callback alan HER builtin. Tarama: `grep -n '== Value::FUNCTION' src/*.cpp`
**Test:** her callback'li builtin'i **VM route'unda** (BYTECODE_FN) + CLI'de (FUNCTION) çağır, sonuçlar aynı olmalı.

### S2 · Kopya semantik ayrışması 🔴
**Desen:** Dilin bir kuralı (truthiness / to_string / karşılaştırma / escaping) ikinci bir yerde yeniden yazılmış → zamanla sapar.
**Yakalanan:** template `is_truthy` (`"0"` kuralı eksik), template float formatı (bilimsel gösterim + hassasiyet kaybı).
**Nerede ara:** `grep -n 'is_truthy\|to_string\|format_double\|escape' src/*.cpp` → dil dışındaki her implementasyon şüpheli.
**Şüpheliler:** `json_encode` (kendi sayı formatı?), `db` parametre bağlama (kendi to_string?), log biçimleme, `template::escape` vs `html::escape`.
**Test:** aynı değer kümesini iki yoldan geçir, çıktıları karşılaştır.

### S3 · Motor ayrışması (VM ≠ interpreter) 🔴
**Desen:** Aynı kod iki motorda farklı sonuç → biri sessizce yanlış.
**Yakalanan:** top-level bileşik atama, `print`/`write` ayracı, `{$var}` interpolation, tanımsız değişken, implicit closure capture.
**Yöntem:** **differential test** (bölüm 3a). Bu sınıfın tek güvenilir avı budur.
**Henüz taranmamış:** `date::`, `jobs::`, `session::`, `cookie::`, `request::`, `file::` — hiçbiri differential gövdesinde yok.

### S4 · Compiler dalı bir durumu atlıyor 🔴
**Desen:** `compile_*` fonksiyonunun N dalından biri bir bayrağı/op'u işlemiyor.
**Yakalanan:** `compile_assign` top-level global dalı `e.op`'u yok sayıyordu (`$t += 2` → `$t = 2`).
**Nerede ara:** `compiler.cpp`'de çok dallı her fonksiyon — her dal tüm op/case'leri ele alıyor mu?
**Tarama:** `grep -n 'e.op != "="' src/compiler.cpp` → kaç dalda var, kaç dalda yok?
**Test:** aynı işlemi 4 kapsamda dene: global, local, fonksiyon-içi, dizi-elemanı, nesne-alanı.

### S5 · ODR / tip çakışması 💥
**Desen:** Aynı isim iki header'da farklı layout, aynı namespace → LTO birleştirir → bellek bozulması.
**Yakalanan:** `look::HttpResponse` (web'de `http::get` sunucuyu çökertiyordu).
**Nerede ara:** `comm -12 <(header1 tipleri) <(header2 tipleri)` — aynı namespace'te tekrar eden tip adı.
**Kritik ders:** **ASan bunu GÖREMEZ** (o build'de LTO kapalı). Release-only crash → ilk şüphe ODR.

### S6 · Parser/operatör ikililiği 🟠
**Desen:** Bir token iki anlama geliyor, parser açgözlü davranıyor.
**Yakalanan:** `.` hem concat hem üye erişimi → `$out . fn()` parse hatası.
**Nerede ara:** Çift görevli tokenlar: `.` (concat/üye), `-` (çıkarma/unary), `:` (ternary/assoc), `[` (indeks/dizi literal), `{` (blok/interpolation/map), `/` (bölme/regex?).
**Test:** her token için "iki anlamın çarpıştığı" ifadeler yaz (bölüm 3d).

### S7 · Mutlu-olmayan yolda kaynak sızıntısı 🔴
**Desen:** `acquire()` var, alternatif çıkış yolunda `release()` yok.
**Yakalanan:** `parallel()` DB bağlantısını iade etmiyordu → havuz tükeniyor, endpoint sonsuza dek asılıyor (sessiz).
**Nerede ara:** `grep -n 'acquire\|open\|lock\|new ' src/*.cpp` → RAII guard var mı? erken `return`/`throw` yolları?
**Test:** havuz boyutundan **fazla** istek (tek istek YETMEZ — sızıntı ancak tükenince görünür).

### S8 · Sınırsız büyüme (OOM/DoS) 🟠
**Desen:** Kullanıcı girdisiyle büyüyen container'da tavan yok.
**Yakalanan:** `cache::` sınırsızdı → `LOOK_CACHE_MAX_ENTRIES` eklendi. `string::format` `%500000000d` → 500MB.
**Nerede ara:** `queue::`, `jobs::`, session store, log buffer, WS/SSE bağlantı listesi, template include derinliği.
**Test:** 100k+ girdi, çok büyük tek değer, derin iç içe yapı → RSS izle.

### S9 · Sınır/negatif indeks 💥
**Desen:** Kullanıcıdan gelen tamsayıyla indeks aritmetiği, negatif/taşma kontrolü yok.
**Yakalanan:** `array::slice([1,2,3], -10)` segfault.
**Nerede ara:** `array::` (26 fn — slice/chunk/zip/flatten), `string::` (24 fn — substr/pad/index_of).
**Test matrisi:** `-1, -büyük, 0, len, len+1, INT64_MIN/MAX` × her indeks alan fonksiyon.

### S10 · Yol içerme (prefix ≠ ayırıcı-sınırı) 🔴
**Desen:** `path.startsWith(root)` → `/proj` öneki `/proj-evil` ile eşleşir → dizin dışına çıkış.
**Yakalanan:** installer zip-slip, fcgi/cgi script çözümü (RCE), session cookie, template render.
**Nerede ara:** kullanıcı girdisiyle yol birleştiren HER yer: `file::`, `template::`, upload, installer, log yolu.
**Test:** `../`, `..\\`, mutlak yol, sibling-prefix (`/root-evil`), sembolik link, URL-encoded (`%2e%2e`).

### S11 · Gevşek ayrıştırma 🟠
**Desen:** `stoi("123abc")` → 123, çöp sessizce kabul.
**Yakalanan:** `validator::integer/numeric` bypass.
**Nerede ara:** `grep -n 'stoi\|stol\|atoi\|strtol\|stod' src/*.cpp` → tam-string kontrolü var mı?
**Test:** `"12abc"`, `" 12"`, `"12 "`, `"0x1F"`, `"1e5"`, `"+-1"`, `""`, çok uzun sayı.

### S12 · Moda özgü boşluk 🟠
**Desen:** Özellik bir sunum yolunda var, diğerinde yok — belge fark etmiyor.
**Yakalanan:** `request::file` multipart **FastCGI'de var, `--mode http`'de YOK** (sessizce null).
**Nerede ara:** `fcgi_main.cpp` ↔ `http_main.cpp` fark analizi (ikisi de WebContext kuruyor).
**Test:** her request:: fonksiyonunu **iki modda** çağır, sonuç aynı olmalı.

### S13 · builtin_names boşluğu 🟡
**Desen:** Fonksiyon interpreter modülünde var, `builtin_names()`'de yok → VM route'u **kalıcı interpreter'a düşer** (doğru sonuç, ~40× yavaş, sessiz).
**Yakalanan:** array/math/string/type/crypto/request/db — toplam ~70 fonksiyon.
**Tarama:** interpreter modül fonksiyonlarını `builtin_names()` ile diff'le.
**Kritik:** körlemesine ekleme YOK — her biri (a) interpreter fn'ine map oluyor mu (b) callback alıyor mu diye doğrulanmalı (`db::` bunu kanıtladı; 256 duvarına körlemesine dayanmak `cache::size`'ı `print`e çevirmişti).

### S14 · Guard kör noktası (meta-sınıf) 🔴
**Desen:** Test edilmeyen yüzey. **Diğer 13 sınıfın hepsi buradan geldi.**
**Yöntem:** Bölüm 1b tablosundaki ⬜ satırları = bugün avlanacak yerler.

---

## 3. Yöntem kataloğu — hangi testi nasıl yaparım

### 3a · Differential test (motor ayrışması için TEK güvenilir yol)
Aynı kodu 3 motorda çalıştır, çıktılar **birebir** aynı olmalı:
```bash
bash cpp/tests/differential_test.sh <lk> <lk-fcgi>
```
- tree-walk (`LOOK_CLI_VM=0`) = **referans semantik**
- CLI-VM (default) · web-VM (route içinde)
- **Neden 3:** builtin wiring iki ayrı elle-yazılmış yerde (`build_cli_builtins`, `req_builtins`) → sessizce kayabilir.
- **Yeni test eklerken:** kapsamı doğru yere koy — gövde `run_all()` **fonksiyonu** içinde çalışır; top-level/global davranışı test etmek istiyorsan `differential_test.sh`'e **ayrı** blok ekle.

### 3b · Pozitif kontrol (testin işe yaradığını kanıtla)
Bir test yazdın, PASS verdi — **bu hiçbir şey ifade etmez** ta ki testin FAIL edebildiğini görene kadar.
1. Fix'i geçici geri al (veya bug'lı binary kullan) → test **FAIL** vermeli
2. Fix'i koy → **PASS**
Yapmazsan: yanlış yere yazılmış (bkz. U kategorisi hatam), yanlış değer bekleyen veya hiç çalışmayan test elde edersin.

### 3c · Sınıf taraması (bir bug → tüm kardeşleri)
Bug bulunca **tek tek düzeltme** — deseni grep'le:
```bash
grep -n '== Value::FUNCTION' src/*.cpp      # S1
grep -n 'stoi\|atoi\|strtol'   src/*.cpp    # S11
grep -n 'is_truthy\|to_string' src/*.cpp    # S2
grep -n 'e.op != "="'          src/compiler.cpp  # S4
```
Sonra her hit'i "bu da aynı hatayı yapıyor mu?" diye incele.

### 3d · Parser sondası (izole dosya başına bir vaka)
Parse hatası tüm dosyayı öldürür → **her vaka ayrı `.lk`**, hepsini döngüde çalıştır:
```bash
for f in _probe/*.lk; do printf "%-20s %s\n" "$(basename $f)" "$(./build/lk $f 2>&1|head -1)"; done
```
**Not:** parser bug'ları differential'a **görünmez** (iki motor da aynı parser'ı kullanır) → ayrı sonda şart.

### 3e · Çapraz-mod testi
Aynı çağrıyı farklı yollarda dene, sonuç aynı olmalı:
| Eksen | Değerler |
|---|---|
| Motor | tree-walk · CLI-VM · web-VM |
| Sunum | FastCGI · `--mode http` |
| Dispatch | worker pool · `LOOK_FIBER_DISPATCH=1` |
| Kapsam | global · local · fonksiyon-içi · dizi elemanı · nesne alanı |

### 3f · Düşmanca girdi matrisi
Her fonksiyon için: `null`, `""`, `"0"`, `0`, `-1`, çok büyük, çok uzun string, unicode/emoji (4-bayt), `INT64_MIN/MAX`, derin iç içe (1000), yanlış tip, eksik argüman, fazla argüman.

### 3g · Sanitizer + fuzz
```bash
cmake -S cpp -B build-asan -DLOOK_SANITIZE=ON   # ASan+UBSan
```
**KRİTİK KISIT:** ASan build'inde **LTO kapalı** → **ODR sınıfını (S5) göremez.** "ASan temiz" bellek hatası yokluğunun kanıtı DEĞİLDİR.

### 3h · Yük/sızıntı testi
```bash
ab -n 20000 -c 200 -k http://127.0.0.1:PORT/route ; ps -o rss= -C lk-fcgi
```
Sızıntı **tek istekte görünmez** — havuz boyutundan fazla istek şart (S7).

---

## 4. Tarama planı — öncelik sırasıyla

Öncelik = (etki × kullanım sıklığı) ÷ guard kapsamı.

### 🔴 Öncelik 1 — hiç guard'ı olmayan, sık kullanılan yüzeyler

| # | Hedef | Aranacak sınıf | Somut test |
|---|---|---|---|
| ~~1.1~~ | ~~**`date::`**~~ ✅ **TARANDI** (1 bug) | S3, S9, S11 | Artık yıl (29 Şubat), DST geçişi, ay-sonu taşması (31 Ocak +1 ay), negatif diff, `parse` çöp girdi, timezone, 1970 öncesi, 2038 |
| 1.2 | **`array::` (26 fn)** | S9, S3 | Negatif/taşan indeks × slice/chunk/zip/flatten/slice; boş dizi; tek eleman; assoc karışık; iç içe 1000 derinlik |
| 1.3 | **`string::` (24 fn)** | S9, S2, S8 | `substr` negatif/taşan; `pad` çok büyük genişlik; `repeat` büyük N (OOM); `format` `%N` uçları; regex catastrophic backtracking; UTF-8 kesme (emoji ortasından) |
| 1.4 | **`request::` (19 fn)** | S12, S3 | Her fonksiyonu FastCGI **ve** `--mode http`'de çağır — fark = bug (multipart böyle bulundu) |
| 1.5 | **`session::` + `cookie::`** | S10, S8, S3 | Session id traversal, çok büyük session, eşzamanlı yazma, cookie enjeksiyonu (CRLF), süre dolumu |
| 1.6 | **`jobs::` (10 fn)** | S7, S8, S1 | Worker callback (BYTECODE_FN!), kuyruk sınırsız mı, retry sonsuz döngü, crash sonrası kurtarma |

### 🟠 Öncelik 2 — yüksek etkili ama kısmi guard'lı

| # | Hedef | Aranacak sınıf | Somut test |
|---|---|---|---|
| 2.1 | **Compiler dal denetimi** | S4 | `compiler.cpp`'deki her `compile_*` fonksiyonunda tüm dallar tüm op/case'leri ele alıyor mu — özellikle nesne-alanı (`$o.f += 1`) ve capture |
| 2.2 | **`db::` (12 fn)** | S1, S7, S2 | `transaction` iç içe/savepoint; parametre bağlama tip dönüşümü (S2!); bağlantı kopması sonrası; çok büyük sonuç kümesi |
| 2.3 | **PostgreSQL wire** | S9, S11 | MySQL guard'ı var, PG'de **yok** — aynı testleri PG'ye uygula (protokol ayrı kod!) |
| 2.4 | **`file::` (8 fn)** | S10, S8 | Traversal matrisi (S10 testleri), büyük dosya, eşzamanlı yazma, sembolik link |
| 2.5 | **`http::` (9 fn)** | S12, S7 | Timeout, redirect döngüsü, çok büyük yanıt, TLS hatası, fiber modunda kanal etkileşimi |

### 🟡 Öncelik 3 — daha az sık ama gerçek

| # | Hedef | Aranacak sınıf |
|---|---|---|
| 3.1 | Lexer (458 satır, guard yok) | Bozuk UTF-8, çok uzun token, iç içe string kaçışı, sayı literal uçları |
| 3.2 | `json::` encode/decode | S2 (sayı formatı!), derin iç içe, unicode, büyük sayı, döngüsel referans |
| 3.3 | WS/SSE | S7, S8 (bağlantı listesi sınırsız mı), eşzamanlı broadcast |
| 3.4 | `mail::` + SMTP/IMAP | S10 (Maildir yolu), S8, protokol ayrıştırma uçları |
| 3.5 | Installer | S10 (zip-slip tekrar), imza doğrulama, ağ hatası |
| 3.6 | REPL / `lk test` | S3 — **hâlâ tree-walk**, production VM ile ayrışabilir |

### ⬜ Öncelik 4 — bilinen açıklar (bug değil, eksik)
- Kanallar fiber-aware değil (fiber modunda `receive` worker'ı bloklar) — PROJE_DURUMU §9
- `request::file` `--mode http`'de yok — S12, belgelendi
- Proje-yerel `include` yok — dil eksiği

---

## 5. Av defteri — ne tarandı, ne bulundu

| Tarih | Yüzey | Yöntem | Sonuç |
|---|---|---|---|
| 07-16/17 | VM↔interpreter köprüsü | 3a differential | **10 bug** (S1, S3, S13) |
| 07-17 | ODR / link farkı | 5 (gdb + link analizi) | **1 crash** (S5) |
| 07-18 | Parser `.` operatörü | 3d sonda (22 vaka) | **1 bug** (S6) — 21/22 temiz |
| 07-18 | Bileşik atama | 3e çapraz-kapsam | **1 bug** (S4) — global dal |
| 07-18 | Template motoru | 3e (kod↔şablon karşılaştırma) | **2 bug** (S2) — truthiness + float |
| 07-18 | **`date::` (12 fn, P1.1)** | 3f düşmanca girdi + 3e çapraz-motor | **1 bug** (S11/S14) — `parse` imkânsız takvim tarihlerini sessizce kaydırıyordu (31 Nis → 1 May). `is_valid` titiz çıktı, `diff` işaretli, motor ayrışması yok, uçlarda çökme yok |
| 07-18 | **`array::` uçları (P1.2)** | 3f düşmanca girdi + 3a differential | **1 bug DÜZELTİLDİ** (S9/S14) — `array::set` sayısal dizide veriyi yok ediyordu, geçerli indekste bile: `set([1,2,3],1,"X")` = `{"1":"X"}`. `slice`/`chunk`/`zip`/`flatten`/`unique`/`reverse` uçlarda temiz. **+6 bulgu** (S3 kümesi, 5a) — **hepsi kapandı** (23 ve 31. bug) |
| 07-18 | Guard'ın kendisi | 3b pozitif kontrol | **1 kırılganlık** (S14) — `differential_test.sh`'a **göreli** binary yolu verilirse TEMPLATE bölümü `cd $TMP` sonrası binary'yi bulamayıp sahte FAIL üretiyor. Mutlak yol zorunlu; script bunu doğrulamıyor |
| 07-19 | **`string::` (22 fn, P1.3)** | 3f düşmanca girdi + Unicode ekseni | **4 bulgu — hepsi DÜZELTİLDİ** (24–26, ayrıntı 5b). Motor ayrışması YOK (hepsi C++ builtin). `substr`/`split`/`index_of`/`repeat`/`contains`/`trim` uçlarda temiz |
| 07-19 | **`request::` (19 fn, P1.4)** | 3f düşmanca HTTP girdisi + 3e çapraz-mod | **1 GÜVENLİK bug** (5c) + 1 minör. `header()` büyük/küçük harf duyarsız ✓, URL çözme (Türkçe/`%20`) ✓, boş `""` ↔ eksik `null` ayrımı ✓, `all()`/`method`/`path`/POST form/JSON ✓ |
| 07-19 | **`session::` + `cookie::` (9 fn, P1.5)** | 3f düşmanca girdi (enjeksiyon ekseni) | **2 GÜVENLİK bug DÜZELTİLDİ** (28, 29) — session verisi enjeksiyonu (`\n` ile yetki alanı uydurma) + çerez öznitelik enjeksiyonu (`;` ile `Domain=`). Geri kalanı sağlam: `valid_sid` traversal guard'ı her fonksiyonda, `gen_session_id` `/dev/urandom` (yetersiz okumada **hata fırlatıyor**), `regenerate` fixation savunması doğru, oturum çerezinde `HttpOnly+Secure+SameSite`, CRLF enjeksiyonu zaten engelliydi |
| 07-19 | `json::encode` kaçışları | 3f | **TEMİZ** — ters bölü/tırnak/satır sonu/Türkçe hepsi doğru. (Şüphelendim ama suçlu kendi test aracımdı, bkz. altın kural 7) |
| 07-19 | **`db::` (12 fn, P2)** | 3f enjeksiyon matrisi + parametre kenar durumları (SQLite `:memory:`, sunucusuz) | **2 bug DÜZELTİLDİ** (30) — literal içindeki `?` placeholder sanılıyordu + parametre sayısı uyuşmazlığı iki yönde de sessizdi. **SQL enjeksiyonu TEMİZ**: `' OR '1'='1`, UNION, DROP, ters bölülü yük — altısı da engelli, `escape_str` doğru. Tipler doğru (int/float locale-güvenli/bool/null/Türkçe) |
| 07-19 | **`file::` (8 fn) — sandbox iddiası** | 3f düşmanca yol + **sembolik link** (gerçek Linux dizininde) | **TEMİZ** — `SECURITY.md`'nin iddiası doğrulandı. `../`, mutlak yol, iç içe `..`, `....//`, ve **sembolik link** (dosya + dizin) hepsi reddedildi; link üzerine **yazma** da engellendi (`/etc/passwd` bozulmadı). Guard 8 fonksiyonun 6'sında; `store`/`upload_dir` kullanıcı yolu almıyor (kendi `subdir` doğrulaması var). `LOOK_FILE_ROOT=*` opt-out'u çalışıyor. Önek karşılaştırması **bileşen bazlı** → S10 sınıfı doğru yapılmış |
| 07-19 | Hata mesajı kalitesi | 3a differential (kazara — kendi probe'umda `use` unuttum) | **1 bug DÜZELTİLDİ** (33) — `use` unutulunca VM `bad_function_call` (C++ iç terimi) sızdırıyordu, tree-walk ise doğrusunu diyordu. **1 bulgu AÇIK** (aşağıda) |
| 07-19 | **`http::` (9 fn) — SSRF ekseni** | 3f düşmanca URL (11 bypass varyantı) + gerçek IPv6/IPv4 sunucusu | **2 bug DÜZELTİLDİ** (36, 37). **SSRF koruması TEMİZ**: kısa form, ondalık, sekizlik, IPv6, IPv4-mapped, metadata (169.254), 10.x, 192.168 — hepsi engelli. Çözümlenmiş adres kontrol ediliyor (string değil) → hostname/kodlama hileleri otomatik düşüyor; bağlantı **kontrol edilen adrese** yapılıyor → DNS rebinding yok. Yönlendirme takibi hiç yok → redirect bypass da yok |
| 07-19 | **PostgreSQL wire (1023 satır, guard YOKTU)** | 3f düşmanca girdi + **sahte PG sunucusu** (`tests/fake_pg_server.py`) | **1 bug DÜZELTİLDİ** (34) — bozuk DataRow sessizce yanlış veri üretiyordu. Mesaj okuma yolu sağlam çıktı (uzunluk `<4` red, `LOOK_PG_MAX_MSG` tavanı). Çökme yok, bellek güvenliği sorunu yok |
| 07-20 | **MySQL auth (39) + PG transaction (40)** | gerçek sürüm konteynerleri + sunucu logu | MySQL 8.0–9.x `caching_sha2_password` (RSA tam yol) — 5 sürüm + MariaDB doğrulandı; PG transaction içinde sequence'siz INSERT veri kaybı kapandı |
| 07-21 | **Event loop (`event_loop.cpp`, 658 satır — guard'ı YOKTU)** | 3h yük/sızıntı: 240 anormal kopma + 320 paralel bağlantı + per-IP sınırı | **TEMİZ — bug yok.** fd yaşam döngüsü kusursuz: RST (SO_LINGER 0), yarım komut (CRLF'siz), DATA ortasında kopma, 150 eşzamanlı bağlantı — hepsinde **fd sızıntısı 0**. 8 thread × 40 tur = 320 bağlantı 0.1 sn'de, **0 hata**, log temiz, RSS 8.6 MB sabit. `close_fd` idempotent ve fd-yeniden-kullanım yarışına karşı korumalı (kaynakta belgeli). Per-IP limiti **doğru uygulanıyor**: varsayılan 10 → `220 kabul: 10 / 421 red: 20`; `LOOK_SMTP_MAX_CONNS_IP=3` → `3/27`; bağlantı kapanınca sayaç serbest. **Kendi ölçüm hatam:** ilk turda `recv`'den geleni "banner" saydım ve `421` reddini kabul sanıp "limit çalışmıyor" sonucuna varmıştım — `220`/`421` ayırt edilince kodun doğru olduğu görüldü (7. altın kural) |
| — | manuel `db::begin` nested tutarlılığı, prepared statement bağlama, `jobs::`, lexer, `installer::`, SMTP/IMAP | — | **SIRADA** |

**~~AÇIK — çağrılamayan ad mesajı~~ → DÜZELTİLDİ (38. bug, `31ee310`):** `olmayan_fn(1)`
artık iki motorda `Undefined variable: <ad>`; `$x=5; $x(1)` iki motorda
`'$x' çağrılabilir değil`. Ad, compiler tarafından `NOP` hint'ine gömülüp VM'e taşınıyor.
(Bu vakada tree-walk **da** yanılıyordu — tanımlı değere "is not defined" diyordu.)

**`db::` notu (bug değil, sözleşme):** parametreler wire-protocol prepared statement
değil, **kaçırılıp metin olarak** SQL'e gömülüyor. Güvenlik tümüyle `escape_str`'e
dayanıyor ve o doğru çalışıyor. İki uç durum haritada dursun:
- **MySQL** `NO_BACKSLASH_ESCAPES` modundaysa `\'` kaçış sayılmaz → şema kırılır (sıra dışı mod)
- **PostgreSQL** `standard_conforming_strings` kapalıysa (9.1 öncesi varsayılan) ters bölü aktifleşir

### 5c. 🔴 GÜVENLİK — `request::ip()` sahtelenebilir (`--mode http`)

`http_main.cpp:675` `ctx.remote_addr`'ı **yalnızca** `X-Forwarded-For` başlığından dolduruyor;
`http_server.cpp`'nin doğru şekilde doldurduğu gerçek peer IP'ye (`req.remote_addr`) hiç
düşmüyor ve **güvenilir-proxy kontrolü yok**:

| istek | `request::ip()` |
|---|---|
| başlık yok | `''` — gerçek peer yok sayılıyor |
| `X-Forwarded-For: 9.9.9.9` | `'9.9.9.9'` — **istemci kendi IP'sini seçiyor** |
| `X-Forwarded-For: 1.1.1.1, 2.2.2.2` | zincir ayrıştırılmıyor |
| `X-Forwarded-For: not-an-ip; DROP TABLE` | çöp aynen geçiyor (log/sorgu/ban listesi) |

**Etki:** IP'ye dayalı yetkilendirme, ban listesi ve denetim kaydı güvenilmez. Uygulama
`request::ip()` ile rate-limit yapıyorsa saldırgan her istekte IP değiştirip sınırı aşar.

**Doğrusu AYNI DOSYADA zaten var** (`http_main.cpp:727–737`, rate limiter yolu):
`req.remote_addr`'dan başla → yalnız `is_trusted_proxy()` ise XFF/X-Real-IP'ye bak →
zinciri virgülden böl. Mantık kopyalanmış ve kopya sapmış (**S2**). Aynı kusur WS/SSE
yollarında da var (`1358`, `1388`).

**Kapsam:** Üretim FastCGI modunda (`fcgi_main.cpp:562` → `REMOTE_ADDR`) — canlı siteler
**etkilenmiyor**. Açık `--mode http` ve WS/SSE yollarında.

**DÜZELTİLDİ (32. bug, `8e363e0`):** `request::get("k", "varsayılan")` ikinci argümanı
sessizce yok sayıyordu. Dilin **kendi kalıbı** zaten buydu — `env(key, varsayılan)` ve
`config(key, varsayılan)` destekliyor — yani tutarsızlıktı. 4. altın kural gereği tek
fonksiyon değil **erişimci ailesi**: `request::get` / `request::post` / `request::header` /
`cookie::get` / `session::get`. Geriye dönük uyumlu: varsayılan verilmezse yine `null`.

### 5b. `string::` bulguları — **4'ü de KAPANDI** (24–26. bug)

Çözüm ekseni **Go**: bu dört sorunun da Go'da tanımlı bir cevabı vardı ve
tutarlı bir bütün oluşturdular.

| # | Bug | Go sözleşmesi | Commit |
|---|---|---|---|
| 1 | `replace` boş arama dizesiyle asılıyordu (DoS) | `strings.Replace(s,"",new,-1)` — başta ve her UTF-8 dizisinden sonra eşleşir, `k+1` ekleme, **sınırlı** | `e125dad` |
| 2+3 | `pad_*` geçersiz UTF-8 + bayt/kod noktası | `fmt "%Ns"` — genişlik **asgari** ve **kod noktası**; kırpma yok → bölünecek yer de yok, 2 bug tek kökten öldü ve **kod azaldı** | `071bc27` |
| 4 | `upper/lower` global kapsam eksik | `unicode.ToUpper` yarım tablo ile çıkmaz — Latin Ext-A + Yunan + Kiril eklendi | `24fec50` |

**Kalıcı ders (S14):** `.gitattributes` eklendi. `core.autocrlf=true` rebase
sonrası `differential_test.sh`'i CRLF yapmıştı ve bash onu çalıştıramıyordu —
**guard sessizce koşmuyordu**, ben ise PASS sanıyordum. Git'te saklanan hâl LF'ti,
bozulma yalnızca çalışma ağacındaydı. Guard'ın kendisi de av alanıdır.

<details><summary>Bulguların ilk kaydı (tarama anı)</summary>

| # | Bulgu | Kanıt | Sınıf |
|---|---|---|---|
| 1 | 🔴 **`replace` sonsuz döngü + sınırsız bellek** — `from` boşken `s.find("",pos)` her zaman eşleşir, her turda araya `to` eklenir, string sonsuza kadar büyür (`to` da boşsa `pos` hiç ilerlemez) | `string::replace("abc","","X")` → **asılıyor** (5 sn timeout) | S8 |
| 2 | 🔴 **`pad_left`/`pad_right` GEÇERSİZ UTF-8 üretiyor** — kırpma `s.substr(s.size()-len)` bayt tabanlı, çok baytlı karakteri ortadan bölüyor | `pad_left("şğü",3,"x")` → `0x9F 0xC3 0xBC` (öksüz devam baytı) — `iconv` reddediyor | S9 |
| 3 | 🟠 **`pad_*` bayt sayıyor, kod noktası değil** — modülün geri kalanı (`len`/`substr`/`upper`/`reverse`) kod noktası farkındalıklı; `pad` değil → görünen genişlik yanlış | `pad_left("ş",3,"x")` = `"xş"`, `len()` = **2** (beklenen 3) | S2 |
| 4 | 🟠 **`upper`/`lower` "global" iddiası kodda karşılanmıyor** — kural doğru şekilde locale-bağımsız (Türkçe i↔İ yok ✓) ama TABLO yalnızca ASCII + Latin-1 + 3 Türkçe kod noktası (`ı ğ ş`). Kiril, Yunan ve Latin Ext-A'nın kalan ~125 karakteri sessizce dönüşmeden geçiyor | `upper("привет")`=`привет` (değişmiyor), `upper("ελλάδα")`=`ελλάδα`, `upper("łódź")`=**`łÓDź`** (yarım dönüşüm) | S2 |

Not: 4 numara felsefe açısından önemli — `stdlib.cpp:209` yorumu "dil globaldir" diyor,
yani kod kendi beyan ettiği sözleşmeyi tutmuyor. Kural global, tablo Türkiye'ye özel.

</details>

### 5a. `$arr[idx]` operatör kümesi (S3) — hepsi kapandı (23 ve 31. bug)

**DÜZELTİLDİ (23. bug, `90b4487`):** string anahtar vakaları (#1 ve #6). Sözleşme:
sayısal listeye tam-sayı-olmayan string anahtar → listeyi assoc'a **dönüştür ama
mevcut elemanları sayısal indeksleriyle anahtarlayarak koru**; `"1"` gibi tam sayı
metni sayısal indekstir (`look_is_int_key`, iki motorda aynı kural); listede
bulunmayan string anahtar okuması → `null`. `array::set` ile aynı sözleşme →
dil kendi içinde tutarlı. Guard: `differential_test.sh` `$arr[str]` bölümü.

**DÜZELTİLDİ (31. bug, `8e363e0`):** sayısal indeks uçları (#2–#5). VM tree-walk'a
(referans semantik) hizalandı — negatif = sondan, `size`'a eşit yazma = sona ekleme,
diğerleri **hata**. VM bunlarda sessizdi: aralık dışı okuma `null` dönüyor, aralık dışı
yazma **sessizce atlanıyordu** ("yazdım ama yazılmadı"). Gerekçe tanımsız-değişken
STRICT kararıyla aynı. Guard: `$arr[sayi]` uçları bölümü, iki motor.

Dizi **indeks operatörünün** kendisi iki motorda ayrışıyor. `array::set` düzeltildi ama
bunlar **kasıtlı olarak bekletiliyor**: dilin en sıcak yolu (`vm.cpp` `array_get`/`array_set`,
`interpreter.cpp` okuma+yazma) ve davranış değişikliği canlı uygulamaları etkiler
(ör. VM'de aralık dışı okuma `null`→hata olursa mevcut route'lar 500 döner).
Ayrı bir karar + ayrı bir kapsam olarak ele alınacak.

| # | İfade | CLI-VM | tree-walk | Not |
|---|---|---|---|---|
| ~~1~~ | ~~`$a=[1,2,3]; $a["k"]="X"`~~ | ~~`{"1":2,"3":"k"}`~~ | ~~`["X",2,3]`~~ | ✅ **DÜZELTİLDİ** → `{"0":1,"1":2,"2":3,"k":"X"}` (iki motor) |
| 2 | `$a[99]="X"` | sessizce yok sayar | hata | |
| 3 | `$a[-1]="X"` | sessizce yok sayar | `[1,2,"X"]` (sondan) | |
| 4 | `$a[99]` oku | `null` | hata | |
| 5 | `$a[-1]` oku | `null` | `3` (sondan) | |
| ~~6~~ | ~~`$a["k"]` oku~~ | ~~`null`~~ | ~~`1` (indeks 0)~~ | ✅ **DÜZELTİLDİ** → `null` (iki motor) |

Kök neden yerleri: `cpp/src/vm.cpp` `array_get`/`array_set`, `cpp/src/interpreter.cpp`
index okuma + index atama dalları. Sözleşme kararı verilmeden dokunulmayacak.

---

## 6. Bug bulunca akış (zorunlu adımlar)

0. **ARACI DOĞRULA** (altın kural 7 — ölçümden ÖNCE, her seferinde)
   - **Binary taze mi?** Hedef bazlı bak: `lk` ≠ `lk-fcgi`. `lk` hedefi `http_main.cpp`
     içermez; global "en yeni kaynak" karşılaştırması yanlış alarm verir.
     Derleyici konteynerde kurulu değilse `cmake --build` **sessizce hiçbir şey yapmaz**
     ve eski binary'yi ölçersin.
   - **Test girdisi gerçekten yazdığın şey mi?** Kabuk `\\`'yi yutar (heredoc, `printf`).
     Şüphelenirsen `od -c` ile dosyanın baytlarına bak; ters bölü/tırnak içeren
     probe'ları `Write` ile oluştur, kabuktan geçirme.
   - **Guard gerçekten koşuyor mu?** CRLF'e dönmüş `.sh` bash'te çalışmaz ve PASS/FAIL
     hiç basmadan düşer — "PASS görmedim" ile "FAIL görmedim" aynı şey değildir.
     Çıktıda `PASS:` satırını **gördüğünü** doğrula. Script'e **mutlak** binary yolu ver
     (bazı bölümler `cd $TMP` yapar, göreli yol orada kırılır).
1. **İzole et** — en küçük tekrar üreten örnek, gerçek binary'de doğrula
2. **Sınıfını belirle** (bölüm 2) → **kardeşlerini ara** (3c)
3. **Kök nedeni kaynakta göster** — semptomu değil, satırı.
   Kod doğru görünüyorsa **0. adıma dön**: büyük ihtimalle aracın yanıltıyor
   (`json::encode` böyle boşuna suçlandı).
4. **Düzelt** — mümkünse tek kaynağa delege et (S2'yi önler)
5. **Guard ekle** — doğru yere (3a notu) + **pozitif kontrol** (3b).
   Guard'ın kendisi de yanılır: kontrolün *yapıyı* mı yoksa yalnız *metni* mi
   aradığına dikkat et (kodlanmış değer içindeki `HttpOnly` kelimesi zararsızdır,
   `; HttpOnly` ise özniteliktir — ilk yazdığım kontrol tam bunu karıştırmıştı).
6. **Tam guard koş** — differential + parallel_db, ikisi de PASS
7. **Commit** — Türkçe, `Fix:` öneki, kök neden + neden kaçtığı + doğrulama. **Yalnız `Codlook` kimliği**
8. **Deploy** — yedekli + otomatik geri-alma, canlı doğrula.
   *(Motor/stdlib düzeltmeleri canlı iki siteyi birden etkiler — binary paylaşılıyor.
   Deploy edilmediyse commit'te belirt: "deploy EDİLMEDİ" yazmak, sessiz bırakmaktan iyidir.)*
9. **Kaydet** — bu dosyanın §5 av defteri (`PROJE_DURUMU.md` artık **yerel**, repoda değil)

---

## 7. Kapatılan bugların tam listesi — **33 bug, kök neden + çözüm**

Tek kaynak. Ayrıntılı analiz ilgili **commit mesajında**; burada her bug tek satırda:
neydi, neden kaçtı, nasıl çözüldü.

🔴 = veri kaybı/bozulma veya erişim kaybı · 🛡️ = güvenlik · 🟠 = yanlış davranış · 🟡 = sessiz eksiklik

### 7a. Motor ayrışması ve derleyici turu (1–21)

| # | Bug | Sınıf | Çözüm | Commit |
|---|---|---|---|---|
| 1 | `array::sort()` comparator'ı VM route'unda **sessizce yok sayılıyordu** — canlı yanlış sıralama | 🔴 | Callback `FUNCTION`-only kontrolü kaldırıldı; köprü iki tipi de alıyor | `6a58a8a` |
| 2 | `db::transaction` VM closure'ını çalıştırmıyordu → **sessiz COMMIT** | 🔴 | Aynı sınıf (S1) | `4c68f8e` |
| 3 | LOOK `throw`'u C++ sınırını geçiyordu (try tabanı yok) | 🔴 | `try_floor_` + `LookVmThrow` | `4c68f8e` |
| 4 | `parallel()` DB bağlantısını **iade etmiyordu** → havuz tükeniyor, endpoint donuyor | 🔴 | Mutlu-olmayan yolda `release()` (S7) | `3459c3e` |
| 5 | `type::is_function` VM closure'ını tanımıyordu | 🟠 | Aynı sınıf (S1) | `2b09b8a` |
| 6 | `error::new` tipli payload string'e düşüyordu → `error::is()` sessizce `false` | 🟠 | Payload korundu | `235df9e` |
| 7 | VM `print`/`write` ayraç+newline yok; `exit()`/`die()` yok; CLI çıkış kodu | 🟠 | Dile hizalandı | `4110cbf` |
| 8 | Implicit closure capture — `fn($x)=>$x*$m` VM'de `$m`'i görmüyordu | 🟠 | Capture düzeltildi | `58eb033` |
| 9 | `{$var}` interpolation **tüm VM route'larında sessizce `"null"`** | 🟠 | Fragment eval'i hizalandı | `04a9552` |
| 10 | Tanımsız değişken iki motorda ayrışıyordu | 🟠 | **STRICT** hizalandı (Go/Node bandı) | `9f45233` |
| 11 | VM `try/catch` C++ runtime hatalarını yakalamıyordu | 🟡 | Operatör istisnaları da yakalanıyor | `04a9552` |
| 12 | CLI-VM `mod::fn` + **256 indeks sınırı**: index 256 sessizce 0'a kırpılıp `print` çağrılıyordu | 🟠 | 16-bit builtin indeksi | `7af8a6c` |
| 13 | `type::`/`crypto::`, `math::`/`string::` `builtin_names`'de yoktu → route kalıcı interpreter'a düşüyordu | 🟡 | Tabloya eklendi | `2b09b8a`, `29de18a` |
| 14 | bare `header`/`redirect`, `join`, `push`/`pop` eksikti | 🟡 | Eklendi | `0bb9895`, `a3682be`, `2fc47ee` |
| 15 | higher-order `array::map/filter/reduce`, `len`/`json`/`intval` | 🟡 | Eklendi | `58eb033`, `22b8a70`, `c8dfdf0` |
| 16 | **ODR: `look::HttpResponse` iki farklı tip** → web'de `http::get` **sunucuyu çökertiyordu** | 🔴 çökme | Tip tekilleştirildi. **Ders: ASan bunu göremez** (o build'de LTO kapalı) | `d2df9d3` |
| 17 | **Parser `.` ikililiği** — `$out . html::escape(...)` parse hatası (concat mı üye erişimi mi) | 🟠 | İleriye bakış: `(` veya `::` geliyorsa concat | `be6b72b` |
| 18 | **Compiler top-level bileşik atamada sol operandı atıyordu** — `$t=1; $t+=2` → VM'de **2**. Yalnız global dal bozuktu → web route'ları etkilenmiyordu, bu yüzden yıllarca görünmedi | 🔴 | `e.op` global dalda da işleniyor | `3892f5e` |
| 19 | **Template motoru dilden sapmıştı (2 ayrışma)** — `{#if "0"}` şablonda TRUE/kodda FALSE (DB'den gelen `"0"`); float `1234567.5` → `1.23457e+06` (hassasiyet kaybı, fatura bozar) | 🔴 | Kopya semantik silindi, **dile delege** edildi (S2) | `69dcd50` |
| 20 | **`date::parse` imkânsız tarihleri sessizce kaydırıyordu** — `2024-04-31` → 1 Mayıs; `is_valid` aynı girdiye `false` diyordu (yarım kalmış fix) | 🟠 | `mktime` sonrası takvim doğrulaması | `4b7b338` |
| 21 | `$arr[2^32]` — `int` daraltması bounds kontrolünü baypas ediyordu | 🟠 | `int64_t` | `9f45233` |

### 7b. Sistematik tarama turu (22–33) — bu tur haritayla yapıldı

| # | Bug | Sınıf | Kök neden → çözüm | Commit |
|---|---|---|---|---|
| 22 | **`array::set` sayısal dizide veriyi YOK EDİYORDU** — geçerli indekste bile: `set([1,2,3],1,"X")` = `{"1":"X"}` | 🔴 | "Convert to assoc" dalı dönüştürmüyor **değiştiriyordu**: yalnız yeni anahtar/değeri içeren yeni assoc kurup orijinali atıyordu → geçerli indekste yerinde değiştir, `size`'a eşitte ekle, aksi hâlde **elemanları koruyarak** çevir | `18e5170` |
| 23 | **`$a=[1,2,3]; $a["k"]="X"` iki motorda İKİ FARKLI ŞEKİLDE bozuyordu** — VM `{"1":2,"3":"k"}` (veri uçuyor, yazılan değer bile erişilemez), tree-walk `["X",2,3]` (`to_int("k")=0` → indeks 0 eziliyor) | 🔴 | Liste→assoc dönüşümü elemanları **sayısal indeksleriyle** korur; `"1"` gibi tam sayı metni sayısal indekstir (iki motorda aynı kural) | `90b4487` |
| 24 | **`string::replace("abc","","X")` ASILIYORDU** — sonsuz döngü + sınırsız bellek | 🛡️ DoS | `s.find("",pos)` hep eşleşiyor, her turda `to` ekleniyordu. Web'de `replace($metin, $kullanıcıGirdisi, $x)` yaygın → tek istekle worker kilitleniyordu. **Go sözleşmesi**: boş arama başta ve her UTF-8 dizisinden sonra, `k+1` ekleme, **sınırlı** | `e125dad` |
| 25 | **`string::pad_*` GEÇERSİZ UTF-8 üretiyordu** — `pad_left("şğü",3,"x")` → `0x9F C3 BC` (öksüz devam baytı); ayrıca uzun metni **kırpıyor**, genişliği **bayt** sayıyordu | 🔴 | **Go `fmt "%Ns"`**: genişlik ASGARİ ve KOD NOKTASI. Kırpma kalkınca bölünecek yer kalmadı — 3 sorun tek kökten öldü, **kod azaldı** (12 satır → 3) | `071bc27` |
| 26 | **`upper`/`lower` "global" iddiasını karşılamıyordu** — Kiril/Yunan hiç dönmüyor, Lehçe **yarım**: `upper("łódź")` = `łÓDź` | 🟠 | Kural globaldi ama **tablo Türkiye'ye özeldi** (ASCII+Latin-1+3 kod noktası). Latin Ext-A + Yunan + Kiril eklendi. **Dikkat**: Ext-A'nın genel kuralı `ı`→`İ` verir (Türkçe locale'i geri getirir) → istisnalar kuraldan ÖNCE | `24fec50` |
| 27 | **`request::ip()` SAHTELENEBİLİYORDU** — `X-Forwarded-For: 9.9.9.9` gönderen istemci kendi IP'sini seçiyordu → IP'ye dayalı yetki/ban/rate-limit bypass | 🛡️ | Doğru mantık **aynı dosyada** (rate limiter yolunda) vardı; `request::ip()` yoluna koşulsuz tek satır konmuştu (S2). `resolve_client_ip()` tek kaynağı — **4 çağrı yeri** bağlandı | `716db47` |
| 28 | **Session verisi ENJEKTE EDİLEBİLİYORDU** — `?isim=bob␊rol=admin␊admin=1` → `session::get("admin")` = `"1"`; hiç var olmayan yetki alanı uyduruluyordu | 🛡️ | Blob `anahtar=değer␊` biçiminde ve **hiç kaçış yoktu**. Kaçırarak sakla: meşru satır sonu (textarea) veri olarak korunur, ayraç anlamı taşımaz | `3658cb0` |
| 29 | **Çerez ÖZNİTELİK enjeksiyonu** — `?v=x; HttpOnly; Domain=evil.com` → çerez tüm alt alan adlarına sızabiliyordu | 🛡️ | CR/LF kapalıydı ama `;` **ayrı bir kanaldı** ve açıktı. PHP `setcookie()` gibi: yazarken yüzde-kodla, okurken çöz | `d60179c` |
| 30 | **SQL literalindeki `?` parametre sanılıyordu** — `"SELECT 'Hazır mı?' AS s, ? AS p"` patlıyordu. Ayrıca parametre sayısı uyuşmazlığı **iki yönde de sessizdi** (fazlası atılıyor, eksiği `null` veriyordu) | 🔴 | Tırnak/yorum durumu izlenip yalnız **gövdedeki** `?` bağlanıyor; sayı uyuşmazlığı **hata** (Go `db.Query` gibi). **Enjeksiyon zaten temizdi** — 6 yük denendi | `d7d85cf` |
| 31 | **VM dizi indeks uçları sessizdi** — `$a[99]` → `null` (tree-walk hata), `$a[-1]` → `null` (tree-walk sondan), **`$a[99]="X"` yazmayı sessizce atlıyordu** ("yazdım ama yazılmadı") | 🔴 | VM referans semantiğe (tree-walk) hizalandı: negatif sondan, `size`'a eşit ekleme, aksi **hata**. Gerekçe tanımsız-değişken STRICT kararıyla aynı | `8e363e0` |
| 32 | **Erişimci varsayılan argümanı yutuluyordu** — `request::get("sayfa", 1)` → `null` | 🟡 | Dilin **kendi kalıbı** zaten `env(key, varsayılan)`. 4. kural gereği tek fonksiyon değil **aile**: `request::get`/`post`/`header`, `cookie::get`, `session::get`. Geriye dönük uyumlu | `8e363e0` |
| 33 | **`use` unutulunca VM `bad_function_call` sızdırıyordu** — C++ iç terimi; ne modülü ne fonksiyonu söylüyor. tree-walk ise `Module 'string' not loaded.` diyordu | 🟠 | `use` unutmak **en sık yapılan hata**, yani varsayılan motor en kötü mesajı veriyordu. `builtin_names()` ile ad çözülüp **aynı metin** üretiliyor — hata metni de sözleşmenin parçası | `d6c7e5a` |
| 34 | **PostgreSQL wire: bozuk DataRow sessizce yanlış veri üretiyordu** — uzunluk 100 der 2 bayt gönderir → alan `""`; int32-max uzunluk → `""`; 3 alan vaat 1 alan gönderir → satır 1 sütunla döner. Hiçbirinde hata yok | 🔴 | Çökme yoktu (sınır kontrolü tutuyordu) ama uygulama boş değeri gerçek veri sanıyordu. Bozuk satır artık **net hata**; ayrıca `p + field_len <= end` işaretçi taşmasına açıktı → `field_len > end - p`. **Sahte PG sunucusuyla** bulundu ve o sunucu kalıcı guard oldu — bu dosyanın ilk guard'ı | bu commit |
| 35 | **`request::file` (multipart) `--mode http`'te YOKTU** — FastCGI'de çalışıyor, aynı uygulama `--mode http`'te "requires multipart/form-data request" hatası veriyordu. Ayrıca `http_main` gövde ayrıştırmaya POST kapısı koyuyordu, FastCGI koymuyordu → PUT/PATCH gövdeleri de modlara göre ayrışıyordu | 🟠 | Gövde ayrıştırma AYNI 6 SATIR **üç yerde** yazılıydı; `http_main` kopyasında multipart dalı hiç yoktu (S2 → S12). `WebContext::parse_post_body()` tek kaynağı; üç giriş noktası da ona bağlandı. README'de "bilinen sınır" diye duruyordu — sınır değil, unutulmuş daldı | bu commit |
| 36 | **IPv6 literal URL'ler HİÇ çalışmıyordu** — `parse_url` köşeli parantezleri sıyırmıyor, port ayracını `rfind(':')` ile arıyordu: `"[::1]:8080"` → host `"[::1]"` (parantezli, çözümlenemez); `"[::1]"` → host `"[:"` (adresin İÇİNDEKİ iki nokta port sanıldı) | 🟠 | Hata "DNS çözümlenemedi" olduğu için sebep görünmüyordu. RFC 3986 biçimi ayrıştırılıyor; kapanış `]` yoksa net hata. Doğrulama: gerçek IPv6 sunucusuna `status=200` (aynı adrese curl ile teyitli) | bu commit |
| 37 | **SSRF engeli gerçek ağ hatasından ayırt edilemiyordu** — `tcp_connect` DNS hatası, SSRF engeli, socket ve bağlantı hatası için aynı `INVALID`'i dönüyor, çağıran hepsine `"connection failed"` diyordu | 🟠 | İç servisine ulaşamayan geliştirici bir **güvenlik politikasının** engellediğini göremiyor, boşuna ağ/DNS/firewall araştırıyordu (33. bug ile aynı sınıf). Sebep `t_conn_error` ile taşınıyor. **36. bug bu düzeltme sayesinde görünür oldu** — mesajlar ayrışınca IPv6'nın "DNS hatası" verdiği fark edildi | bu commit |
| 38 | **Çağrılamayan ad hatası — VM adı söylemiyordu, tree-walk YANILTIYORDU** — `olmayan_fn(1)` VM'de `"Çağrılabilir değil (BYTECODE_FN bekleniyor)"` (hangi ad?); `$x=5; $x(1)` tree-walk'ta `"'$x' is not defined"` (yanlış — `$x` **tanımlı**) | 🟠 | Bu vakada **referans motor da yanılıyordu**, o yüzden yön tek taraflı alınmadı: iki durum ayrıldı, iki motor aynı metni kullanıyor. Ad `LOAD_GLOBAL`'da kaybediyordu (`$`'sız adlar orada ıskalamak **zorunda** — `mod::fn` genel CALL yoluna düşsün diye); compiler adı `NOP` hint'ine gömüp VM'e taşıyor | `31ee310` |
| 39 | **MySQL 8.0–9.x ile HİÇ bağlanılamıyordu** — yalnızca `mysql_native_password` uygulanmış, eklenti adı handshake yanıtına **sabit** yazılmış, sunucunun bildirdiği eklenti okunmuyor, `AuthSwitchRequest` (0xFE) hiç ele alınmıyordu. `CLIENT_PLUGIN_AUTH` bayrağı set edilmesine rağmen | 🔴 | Dil pratikte **MySQL 5.7 diliydi**: 8.0 (2018'den beri varsayılan `caching_sha2_password`), 8.4 (native kapalı), 9.x (kaldırıldı) — hiçbirine bağlanamıyordu. Eklenti adı okunuyor, 0xFE/0x01 diyaloğu işleniyor, `caching_sha2_password` hızlı + **RSA tam yol** uygulandı. **Doğrulanan matris:** 5.7.44, 8.0.46, 8.2.0, 8.4.10 (native DISABLED), 9.1.0 (native kaldırılmış), MariaDB 10.11 + 11.4 — hepsi bağlanıyor. Kripto OpenSSL'den (9. SHA-256 kopyası açılmadı) | bu commit |
| 40 | **PostgreSQL: transaction içinde sequence'siz tabloya INSERT sessizce VERİ KAYBEDİYORDU** — `db::exec` her INSERT sonrası otomatik `SELECT lastval()` çağırıyor (last_insert_id için); tablo SERIAL/sequence kullanmıyorsa `lastval()` hata verir, **açık transaction'ı ABORTED yapar**, sonraki `COMMIT` sessizce ROLLBACK'e döner → INSERT kaybolur | 🔴 | Autocommit'te zararsızdı (INSERT ayrı statement'ta kalıcı); yalnız `BEGIN…COMMIT` içinde. MySQL/SQLite'ta yok. C++ `catch(...)` hatayı yutuyordu ama PG bağlantısı zaten zehirli. Çözüm: transaction bloğundaysak (`ReadyForQuery` status='T') `lastval`'i **SAVEPOINT ile koru** — hata olsa da savepoint'e dönüp transaction'ı kurtar; autocommit'te doğrudan çağır. `affected_rows_`/`last_insert_id_` savepoint komutlarınca ezilmesin diye yerelde tutulup en sonda yazılıyor. **Gerçek PG sunucusu + sunucu logu** ile bulundu | bu commit |
| 41 | 🛡️ **MySQL kaçışı `\'` kullanıyordu — `NO_BACKSLASH_ESCAPES` modunda SQL ENJEKSİYONU** — o modda MySQL ters bölüyü kaçış karakteri saymaz, `\'` tırnağı **kapatır**. Ölçüldü: oturum o moda alınınca `' OR 1=1 -- ` yükü **tüm tabloyu** döndürdü (3/3 satır) | 🛡️ | Güvenliği tutan tek şey `do_connect()`'teki `SET SESSION sql_mode = REPLACE(...)`'ın başarılı olmasıydı — yeterli zemin değil: ProxySQL/bağlantı çoklayıcıları oturum durumunu sessizce kaybedebilir (SET başarılı olur, koruma yok olur); tek ifadenin reddedilmesi bağlantının bozuk olduğu anlamına gelmez. **Asimetri belirleyici: düzeltme tek satır, yanılmanın bedeli enjeksiyon.** Çözüm: `''` (standart SQL) — her iki modda güvenli, `SET`'ten **bağımsız**; PostgreSQL zaten `''` kullanıyordu, iki sürücü artık aynı zeminde. `SET` kalıyor ama artık güvenlik için değil **veri sadakati** için (NBE'de `\\` ikilemesi veriyi bozar). 6 özel-karakter vakası gidiş-dönüşte korunuyor | bu commit |
| 42 | **SMTP adres ayrıştırma TERS yönde gevşekti — çöp veri DİSKE yazılıyordu** — `extract_addr` ilk boşluktan sonrasını alıyordu: `MAIL FROM:` (adressiz) → adres **`"FROM:"`** → maildir'e `Return-Path: <FROM:>`; `MAIL FROM: bare@x.com` → `<FROM: bare@x.com>` (bare adres desteği kırık); `MAIL FROM:<a@b<c>` → bozuk adres kabul. Buna karşılık **`MAIL FROM:<>` (null sender) REDDEDİLİYORDU** — oysa RFC 5321 §4.5.5'te bounce/DSN için kabulü zorunlu | 🟠 | Yani çöpü alıp meşru olanı reddediyordu. Bozuk `Return-Path` bounce'ları yanlış yönlendirir ve başlığı ayrıştıran alıcı yazılımı şaşırtır (adres içinde boşluk/`<` olabiliyordu). Çözüm: SMTP komut sözdizimine göre ayrıştırma (`:` sonrası, açı parantezi kapanmalı, iç içe `<`/boşluk red, ESMTP parametreleri atlanır); null sender boş-ama-geçerli olarak ayırt ediliyor. `validate_rcpt` boş adresi zaten reddettiği için RCPT tarafı etkilenmedi. **`smtp_server.cpp`'nin (1373 satır) İLK guard'ı** — gerçek sunucu ayağa kaldırılıp ham SMTP konuşuluyor | bu commit |
| 43 | **IMAP mailbox adı doğrulaması eksikti + var olmayan mailbox `OK` dönüyordu** — `SELECT "INBOX; rm -rf /"` **kabul ediliyordu** (diğer traversal denemeleri reddedilirken); kontroller yalnız `\0`, baştaki ayırıcı ve `..` bakıyor, `;`/boşluk/**içerideki** ayırıcı geçiyordu. Ayrıca var olmayan mailbox `OK (0 EXISTS)` dönüyordu — RFC 3501 §6.3.1: `NO` olmalı | 🟠 | Mailbox adı bir **yol bileşeni** olarak dizin adına dönüşüyor; kabuk üzerinden işlenen bir yedekleme/log betiği için tehlike, en iyi ihtimalle çöp dizin. Var olmayan kutunun `OK` dönmesi istemciyi yanıltıyordu ("Sent" seçilir, boş kutu görünür — oysa kutu yok). Çözüm: ayırıcı/`;`/kontrol karakteri reddi + `is_directory` kontrolü. **`imap_server.cpp`'nin (1013 satır) İLK guard'ı** | bu commit |
| 44 | **IMAP kalıcı olmayan UID'yi "UID" diye döndürüyordu** — `FETCH n (UID)` o anki *sequence* numarasını veriyordu. Ölçüldü: 3 mesaj `UID 1,2,3` → ortadaki silindi → kalanlar `UID 1,2` (olması gereken `1,3`); **UID 2 artık başka bir mesaj** (önce M1, sonra M2) | 🔴 | Bunu önbelleğe alan istemci/webmail silinen mesajı görmeye devam eder ve açınca **başka mesajın içeriğini** alır — sessiz veri bozulması. Kalıcı UID deposu gelene kadar doğru davranış **gürültülü hata**: `UID` veri öğesi ve `UID` komutu artık `NO [CANNOT] … (Milestone 1)` ile reddediliyor; `CREATE/DELETE/RENAME/SUBSCRIBE` de kapsamı söyleyerek reddediliyor (eskiden `BAD bilinmeyen komut` — teşhis ettirmiyordu). Yetenek kontrolü sequence-set kontrolünden **önce**: boş INBOX'ta bile istemci "UID var mı" cevabını alır. **README gerçekle hizalandı** — IMAP `✅` yerine `🟡 Milestone 1`, standart MUA istemcilerinin çalışmayacağı açıkça yazıldı | bu commit |
| 45 | **Mail sunucuları IPv6'sız ortamda hiç başlamıyordu + "started" logu YALAN söylüyordu** — SMTP/IMAP yalnız `socket(AF_INET6, …)` deniyordu (IPv4 yedeği yok); IPv6 desteği derlenmemiş/kapalı ortamlarda (sertleştirilmiş VPS, bazı konteynerler) `EAFNOSUPPORT` ile başlamıyorlardı. HTTP `AF_INET` kullandığı için o ortamlarda **çalışıyordu** — fark buradan geliyordu. Üstelik `http_main` "server started" logunu **koşulsuz** basıyordu | 🔴 | Ölçüldü (port meşgul edilerek, IPv6'dan bağımsız): `[ERROR] port 7171 dinlenemedi` hemen ardından `[INFO] IMAP server started on port 7171`. Kullanıcı log'a bakıp çalıştığını sanıyordu — bu turun ekseni olan **sessiz/yanıltıcı başarısızlık**. Çözüm: her iki sunucuda IPv4 yedeği (`socket()` ve `bind()` başarısızlığında), `ImapServer::start()` artık `bool`, `SmtpServer::listening()` eklendi, "started" logu **gerçek duruma bağlandı** (başarısızsa `ERROR … (port meşgul mü? yetki? IPv6/IPv4?)` + nesne serbest bırakılıyor). **Analizci bağımsız ortamında buldu** (onun konteynerinde IPv6 yoktu, bende vardı — bu yüzden benim testlerim geçmişti) | bu commit |
| 46 | **`jobs::next()` ÇOK SÜREÇLİ ortamda aynı işi birden fazla worker'a veriyordu (ÇİFT İŞLEME)** — claim `SELECT … WHERE status='pending' LIMIT 1` + ayrı `UPDATE … WHERE id=?` şeklindeydi; UPDATE'te durum kontrolü yoktu, transaction yoktu, aradaki `std::lock_guard` yalnız **süreç içi** mutex (FastCGI multi-worker'da her worker **ayrı süreç**). Ayrıca `busy_timeout` ve `WAL` yoktu | 🔴 | **Ölçüldü** (4 süreç / 40 iş, ortak `jobs.db`): düzeltme öncesi **42 claim / 30 tekil → 12 iş çift işlendi**, üstelik 10 iş hiç alınamadı (kilit çekişmesi). Gerçek etkisi: aynı e-posta iki kez gider, aynı ödeme iki kez çekilir. Çözüm: **tek ifadelik atomik claim** — `UPDATE … WHERE id=(SELECT … LIMIT 1) AND status='pending' RETURNING …` (SQLite 3.35+, gömülü sürüm 3.47.2) + `journal_mode=WAL` + `busy_timeout=5000`. Sonra: **40 claim / 40 tekil**, kayıp yok. Analizcinin okul sisteminde açık bıraktığı soru buydu | bu commit |
| 47 | **`sqlite3_busy_timeout()` çok geç çağrılıyordu — worker'lar açılışta çöküyordu** — sıra `open → PRAGMA synchronous → PRAGMA foreign_keys → PRAGMA journal_mode=WAL → busy_timeout → create_schema` şeklindeydi. WAL geçişi ve `create_schema()` **özel kilit** ister; `busy_timeout` henüz ayarlı olmadığı için çekişmede anında `SQLITE_BUSY` → süreç `"jobs:: schema hatası: database is locked"` ile ölüyordu | 🔴 | **Analizcinin ortamında ölçüldü: 48 denemede 37 kilit hatası (%77) + 3 çift claim.** İki katmanlı sonuç: (a) worker'lar açılışta ölüyor, (b) **ölen worker'lar claim yarışını maskeliyor** — az süreç kalınca yarış görünmez olur; hayatta kalan sayısı artınca çift-claim ortaya çıkar (WAL geçişi başarısız olan süreç rollback-journal modunda kalır, izolasyon farklılaşır). Düzeltme: `busy_timeout` `sqlite3_open`'dan **hemen sonra**, tüm PRAGMA'lardan önce + WAL'ın gerçekten etkin olduğu okunup doğrulanıyor (değilse uyarı — sessizce rollback modunda kalmasın). **Benim ortamımda tekrar üretilemedi** (48/48 temiz) — yarış penceresi I/O hızına bağlı; asimetri belirleyici oldu: düzeltme tek satır taşıma, bedeli süreç çökmesi + çift işleme | bu commit |
| 48 | 🛡️ **Paket kurulumunda yönlendirme hedefi doğrulanmıyordu** — `download_follow` gelen `Location` başlığını **körlemesine** takip ediyordu: `http://…` (şema düşürme → sonraki istek düz metin, TLS bütünlüğü kaybolur) ve **farklı host** kabul ediliyordu | 🛡️ | Paket = **çalıştırılabilir LOOK kodu**, yani tedarik zinciri riski. İlk istek zaten güvenliydi (`zip_url()` https sabit, `parse_pkg` host'u `github.com`'a kısıtlı, TLS'te `SSL_VERIFY_PEER` + `SSL_set1_host`) — açık yalnız yönlendirme zincirindeydi. Çözüm: yönlendirmede **https zorunlu** + host allowlist (`github.com`, `*.github.com`, `*.githubusercontent.com`). **Ölçüldü:** meşru zincir (`api.github.com` → `codeload.github.com`) bozulmadan çalışıyor, modül gerçekten kuruluyor. Kötü yönlendirmenin reddi **ölçülemedi** (sahte GitHub geçerli TLS sertifikası gerektirir) — mantık + asimetri gerekçesiyle uygulandı | bu commit |

### 7c. Bu turda TEMİZ çıkanlar (aynı derecede önemli)

Bug bulunamaması da sonuçtur — nereye bakıldığı kaydedilmezse aynı yere tekrar bakılır.

| Yüzey | Sonuç |
|---|---|
| **SQL enjeksiyonu** (`db::`) | 6 yük (`OR 1=1`, `UNION`, `DROP`, ters bölülü, alt sorgu, yorumla kesme) — hepsi engelli, tablo yerinde. `escape_str` doğru |
| **SQL kaçış uç durumları** | MySQL `NO_BACKSLASH_ESCAPES` ve PG `standard_conforming_strings=off` sunucularında **gerçek ölçüm**: LOOK ikisini de bağlantı düzeyinde kapatıyor (session `SET` / startup parametresi), 7 ters-bölü yükü engelli. Önceden "açık uç durum" sanılıyordu — değilmiş |
| **`file::` sandbox** | `../`, mutlak yol, iç içe `..`, `....//`, **sembolik link (dosya + dizin)**, link üzerine yazma — hepsi reddedildi. Önek karşılaştırması **bileşen bazlı** (S10 doğru). `LOOK_FILE_ROOT=*` opt-out'u çalışıyor |
| **`session::` çekirdeği** | `valid_sid` traversal guard'ı her fonksiyonda; `gen_session_id` `/dev/urandom` ve **yetersiz okumada hata fırlatıyor** (zayıf rastgelelik yaymıyor); `regenerate` fixation savunması doğru; `HttpOnly+Secure+SameSite` |
| **`json::encode` kaçışları** | Ters bölü/tırnak/satır sonu/Türkçe hepsi doğru. Şüphelenmiştim — suçlu kendi test aracımdı (7. kural) |
| **`string::` geri kalanı** | `substr`, `split`, `index_of`, `repeat`, `contains`, `trim` uçlarda temiz |
| **`request::` geri kalanı** | `header()` harf duyarsız, URL çözme (Türkçe/`%20`), boş `""` ↔ eksik `null` ayrımı, `all()`/`method`/`path`/POST form/JSON |
| **`array::` geri kalanı** | `slice` negatif/taşma, `chunk(0)`/`chunk(-1)` hata, `zip`/`flatten`/`unique`/`reverse` |

### 7d. Bilinen, açık bırakılanlar

| Konu | Neden bekliyor |
|---|---|
| Manuel `db::begin/commit/rollback` nested davranışı: MySQL `[1,3]`, PG `[3]` (aynı kod, iki DB farklı) | **KARAR (2026-07-20): belgelendi, değiştirilmedi.** Manuel API bilinçli olarak **düz** transaction (klasik SQL/PHP alışkanlığı); nested/savepoint isteyen `db::transaction($c, fn)` closure'ını kullanmalı — o `tx_depth`+`SAVEPOINT` ile doğru çalışır (web_stdlib.cpp:1520). Düz API'yi sessizce savepoint'e çevirmek yeni sürprizler doğururdu. README'de belirtildi |
| `db::` parametreleri prepared statement değil, kaçırılıp **metin olarak** gömülüyor | **KARAR (2026-07-21): ertelendi — ve gerekçe ÖLÇÜMLE güçlendi.** `execute()` prepared yolu üç sürücüde de yazılı ama `db::query` çağırmıyor (tek kullanıcı SMTP). Bağlamak bir bug sınıfını kökten silerdi (Go/Rust'ın yolu) ama: (1) sıcak yol — her web isteği buradan geçer; (2) `execute()` gerçek yükte hiç test edilmedi; (3) statement cache olmadan sorgu başına ek round-trip. **Kaçış katmanı ölçüldü ve sanılandan sağlam çıktı** — bkz. aşağıdaki not |

**ÖLÇÜM (2026-07-21) — iki aşamalı: önce varsayım çürüdü, sonra DAHA DERİN bir bug çıktı.** Bu tabloda daha önce
"iki uç durum açık: MySQL `NO_BACKSLASH_ESCAPES`, PG `standard_conforming_strings=off`"
yazıyordu. **Yanlıştı** — LOOK ikisini de proaktif kapatıyor ve gerçek sunucularla
doğrulandı:

| uç durum | LOOK'un savunması | ölçüm |
|---|---|---|
| MySQL `NO_BACKSLASH_ESCAPES` | bağlanırken kendi *session*'ında modu kaldırıyor (`mysql_client.cpp:221`, her reconnect'te tekrar; global'e dokunmuyor) | Sunucu o modda başlatıldı → LOOK bağlantısında mod **yok**, 4 ters-bölü yükü engelli |
| PG `standard_conforming_strings=off` | **startup paketinde** `on` gönderiyor (`postgres_client.cpp:636`) — sonradan `SET`'e bile gerek yok | Sunucu global `off` → LOOK bağlantısında `on`, 3 yük engelli |


**Neden iki aşama:** İlk ölçüm *korumanın çalıştığını* gösterdi (sunucu NBE modundayken
LOOK'un oturumunda mod yok, yükler engelli) — bu doğru ama **eksik soru**ydu. Analizci
farkı yakaladı: *koruma olmasaydı* ne olurdu? Oturum elle NBE'ye alınıp saldırıldığında
`' OR 1=1 -- ` **tüm tabloyu döndürdü**. Yani zafiyet teorik değildi; onu tutan tek şey
tek bir `SET` komutunun başarısıydı. 41. bug bu yüzden açıldı ve kapatıldı.

**Ders (guard'a da işlendi):** "koruma çalışıyor mu?" ile "koruma olmasaydı ne olurdu?"
farklı sorulardır. İkincisi sorulmazsa, tek bir komutun sessiz başarısızlığına dayanan
savunmalar sağlam sanılır. Guard artık korumayı **kasten kaldırıp** saldırıyor.
**Kalan not:** MySQL'deki `SET SESSION` `catch(...)` ile sessizce yutuluyor — ama artık **güvenlik ona bağlı değil** (41. bug). `SET` yalnızca veri sadakati için: NBE modunda `\` ikilemesi veriyi bozardı.

### 7e. IMAP4rev1 uyumluluk açığı — **ÖLÇÜLDÜ, DÜZELTİLMEDİ** (2026-07-21)

CAPABILITY'de `IMAP4rev1` ilan ediliyor ama sözleşmenin zorunlu parçaları eksik.
Bu bir "ilan edip desteklememek" durumu — SIZE/literal sınırlarının aksine (onlar
ilan edilip **zorlanıyor**, ölçüldü).

| # | Eksik | Ölçülen davranış | Etki |
|---|---|---|---|
| ~~1~~ | ~~**UID kalıcı değil**~~ → **44. bugla kapatıldı: artık reddediliyor** (RFC 3501 §2.3.1.1) | `UID` = o anki sequence numarası (`imap_server.cpp` FETCH: `"UID " + std::to_string(i)`). Ölçüm: 3 mesaj `UID 1,2,3` → ortadaki silindi → kalan mesajlar `UID 1,2` (beklenen `1,3`). **UID 2 artık BAŞKA bir mesaj**: önce `M1`, sonra `M2` | 🔴 İstemci önbelleği sessizce bozulur: silinmiş mesaj görünmeye devam eder, açılınca başka mesajın içeriği gelir |
| ~~2~~ | ~~**`UID` komutu yok**~~ → **açıklayıcı `NO [CANNOT]` veriyor** (RFC 3501 §6.4.8) | `UID FETCH 1` → `BAD bilinmeyen komut`. Komut dağıtımında `cmd == "UID"` dalı hiç yok; UID yalnızca *veri öğesi* ve *arama ölçütü* olarak var | 🔴 Thunderbird/Outlook/iOS Mail neredeyse hep `UID FETCH` kullanır → bağlanamazlar |
| 3 | `CREATE`/`DELETE`/`RENAME` yok | Komut listesinde yok; kullanıcı yeni kutu oluşturamaz (yalnız INBOX) | 🟠 İstemci "Sent"/"Drafts" oluşturamaz |
| 4 | `BODY[HEADER.FIELDS (...)]` desteklenmiyor | `FETCH 1 BODY[HEADER.FIELDS (SUBJECT)]` → `* 1 FETCH ()` (boş). `BODY[HEADER]` (tümü) çalışıyor | 🟠 İstemcilerin liste görünümü için en çok kullandığı biçim budur |

**Neden düzeltilmedi:** 1 ve 2 mimari — kalıcı UID için mailbox'ta bir UID deposu
gerekir (Dovecot'un `dovecot-uidlist`'i / Courier'in dosya adına `,U=N` gömmesi
gibi), ve SMTP teslim tarafıyla birlikte tasarlanmalı. Küçük diff'le çözülmez;
"değişiklik kapsamını küçük tut" kuralına göre ayrı bir iş turu.

**Bu turda ölçülüp TEMİZ çıkanlar:** kimlik doğrulama zorunluluğu (auth'suz
SELECT/FETCH/APPEND hepsi `NO`), APPEND literal sınırı **zorlanıyor** (2GB →
`TOOBIG`, negatif/bozuk → `BAD`), SEARCH bozuk ölçüt → `BAD`, FETCH sınır
durumları çökmüyor (0 / 99999 / -1 / 2^63 / `abc`), traversal 6/7 varyant
reddedilmişti (7.'si 43. bugla kapandı), LOGIN brute-force gecikmesi var,
oturum sonrası sunucu sağlam, RSS 8.8 MB.

**Dürüst özet:** LOOK'un IMAP'i şu an **temel okuma** seviyesinde (webmail
arka ucu için yeterli), tam IMAP4rev1 istemci uyumluluğu için 1–4 gerekir.
`CAPABILITY`'de `IMAP4rev1` ilan etmek bu hâliyle yanıltıcı.
| **Hiç taranmamış yüzeyler** | lexer, `installer::`, lexer, `installer::`, REPL, test runner. *(`http::`, PostgreSQL wire, `file::`, `session/cookie`, `request::`, `db::` çekirdeği bu turda tarandı.)* |

### 7f. `installer::` — AÇIK: indirilen paketin bütünlüğü doğrulanmıyor (2026-07-22)

Tarama sonucu: yönlendirme açığı kapatıldı (48. bug), ama **checksum/imza yok**.

| Katman | Durum |
|---|---|
| İlk isteğin şeması | `zip_url()` **https sabit** ✅ |
| Host kısıtı | `parse_pkg` yalnız `github.com` ✅ |
| TLS | `SSL_VERIFY_PEER` + `SSL_set1_host` (hostname doğrulama) ✅ |
| Yönlendirme | https zorunlu + host allowlist ✅ *(48. bug)* |
| Zip-Slip | `weakly_canonical` + **bileşen sınırı** (sibling-prefix escape'e karşı) ✅ |
| **İndirilen ZIP'in checksum/imzası** | **YOK** ❌ |
| Atomik kurulum | `remove_all` → çıkar; yarıda kesilirse hem eski hem yeni yarım kalır 🟡 |

**Neden şimdi düzeltilmedi:** bütünlük doğrulaması bir *altyapı* kararı — paketlerin
nasıl imzalanacağı (yayıncı anahtarı? `look.lock`'a SHA256 sabitleme?) ve anahtarın
nasıl dağıtılacağı belirlenmeden kod yazmak yanlış olur. `resolve_sha` GitHub commit
SHA'sını çözüyor ama bu **sürüm sabitleme**, indirilen ZIP'in bütünlük kanıtı değil.

**Bugünkü güvence:** GitHub'a doğrulanmış TLS + host kısıtı + yönlendirme allowlist.
Yani "GitHub'a ve TLS'e güveniyoruz" — makul bir taban, ama `look.lock`'a ZIP SHA256
yazıp sonraki kurulumlarda karşılaştırmak ucuz ve büyük kazanç olurdu (TOFU modeli).

**Ölçülemeyen:** kötü yönlendirmenin gerçekten reddedildiği canlı olarak
doğrulanamadı — sahte GitHub geçerli sertifika ister. Meşru zincirin bozulmadığı
ise ölçüldü (gerçek kurulum yapıldı).
