# LOOK — Bug Avı Haritası

> **Amaç:** Rastgele arama yerine **sistematik av**. Bu dosya "nereye bakılacak, hangi
> yöntemle, hangi testle" sorularının tek kaynağıdır.
> **Son güncelleme:** 2026-07-18 · Kapatılan bug: **21** · Guard: 3 motor × 22 kategori + 20 özel kontrol
>
> **Kardeş dosyalar:** [PROJE_DURUMU.md](PROJE_DURUMU.md) (ne bitti/ne kaldı) · `DENETIM.md` (güvenlik denetimi, yerel)

---

## 0. Avın altın kuralları

Bu 6 kural 20 bug'ın hepsinden çıkarıldı. Her ava başlarken oku.

| # | Kural | Nereden öğrendik |
|---|---|---|
| 1 | **Guard'ın kapsamadığı yüzey = bug'ın saklandığı yüzey.** Yeni bug arıyorsan önce "neyi test etmiyoruz?" diye sor | print/write, sort(comparator), top-level compound, template — hepsi guard boşluğundaydı |
| 2 | **Kopya semantik = kaçınılmaz ayrışma.** Bir kural iki yerde yazılıysa er geç sapar | template truthiness/float dilden kopyalanmıştı, ikisi de saptı |
| 3 | **Sessiz olan, gürültülü olandan tehlikelidir.** Hata veren bug bulunur; yanlış değer döneni kimse görmez | sort comparator, compound assign, `{#if "0"}` — hiçbiri hata vermiyordu |
| 4 | **Bir bug bulduysan SINIFINI ara.** Tek tek düzeltme, deseni grep'le | `== Value::FUNCTION` taraması 3 bug birden çıkardı |
| 5 | **Pozitif kontrol olmadan "temiz" demek anlamsız.** Testin bug'ı yakalayabildiğini kanıtla | U kategorisini fonksiyon içine yazmıştım → bug'ı yakalamazdı, fark edip taşıdım |
| 6 | **Listede yazıyor ≠ kodda öyle.** Her iddiayı kaynakta doğrula | packages.js "http modülünü kur" diyordu, http gömülüydü |

---

## 1. Yüzey envanteri (gerçek, kaynaktan çıkarıldı)

### 1a. Dil çekirdeği — alt sistemler

| Alt sistem | Dosya | Satır | Guard | Risk |
|---|---|---|---|---|
| Interpreter (tree-walk, **referans semantik**) | `interpreter.cpp` | 2192 | ✅ differential | Orta |
| Web stdlib (request/response/db/session) | `web_stdlib.cpp` | 1495 | 🟡 kısmi | **Yüksek** |
| HTTP main (dispatch, hot-reload, rate-limit) | `http_main.cpp` | 1493 | 🟡 kısmi | **Yüksek** |
| **Compiler (AST→bytecode)** | `compiler.cpp` | 1458 | 🟡 kısmi | **Yüksek** |
| SMTP server | `smtp_server.cpp` | 1373 | ⬜ yok | Orta |
| Extra stdlib | `extra_stdlib.cpp` | 1246 | 🟡 kısmi | Orta |
| PostgreSQL wire | `postgres_client.cpp` | 1023 | ⬜ yok | **Yüksek** |
| IMAP server | `imap_server.cpp` | 1013 | ⬜ yok | Orta |
| HTTP server | `http_server.cpp` | 976 | 🟡 kısmi | **Yüksek** |
| Stdlib (çekirdek) | `stdlib.cpp` | 914 | 🟡 kısmi | Orta |
| **VM (bytecode yürütücü)** | `vm.cpp` | 863 | ✅ differential | **Yüksek** |
| FCGI main | `fcgi_main.cpp` | 860 | ⬜ yok | Orta |
| **Parser** | `parser.cpp` | 838 | 🟡 22 kenar durum | **Yüksek** |
| HTTP client | `http_client.cpp` | 822 | 🟡 ODR + fiber | Orta |
| MySQL wire | `mysql_client.cpp` | 761 | 🟡 parallel_db | **Yüksek** |
| Template motoru | `template_stdlib.cpp` | 702 | ✅ yeni eklendi | Orta |
| Event loop | `event_loop.cpp` | 658 | ⬜ yok | Orta |
| Installer (paket/modül) | `installer.cpp` | 617 | ⬜ yok | Orta |
| Test runner | `test_runner.cpp` | 593 | ⬜ yok | Düşük |
| Web context (multipart, cookie) | `web.cpp` | 558 | 🟡 kısmi | **Yüksek** |
| Fiber scheduler | `fiber_posix.cpp` | 483 | 🟡 yük testi | Orta |
| **Lexer** | `lexer.cpp` | 458 | ⬜ yok | Orta |
| Jobs | `jobs_stdlib.cpp` | 457 | ⬜ yok | Orta |
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
| 07-18 | **`array::` uçları (P1.2)** | 3f düşmanca girdi + 3a differential | **1 bug DÜZELTİLDİ** (S9/S14) — `array::set` sayısal dizide veriyi yok ediyordu, geçerli indekste bile: `set([1,2,3],1,"X")` = `{"1":"X"}`. `slice`/`chunk`/`zip`/`flatten`/`unique`/`reverse` uçlarda temiz. **+6 bulgu AÇIK** (aşağıdaki S3 kümesi) |
| 07-18 | Guard'ın kendisi | 3b pozitif kontrol | **1 kırılganlık** (S14) — `differential_test.sh`'a **göreli** binary yolu verilirse TEMPLATE bölümü `cd $TMP` sonrası binary'yi bulamayıp sahte FAIL üretiyor. Mutlak yol zorunlu; script bunu doğrulamıyor |
| 07-19 | **`string::` (22 fn, P1.3)** | 3f düşmanca girdi + Unicode ekseni | **4 bulgu AÇIK** (aşağıda 5b). Motor ayrışması YOK (hepsi C++ builtin). `substr`/`split`/`index_of`/`repeat`/`contains`/`trim` uçlarda temiz |
| — | `request::` çapraz-mod, `session::`, `jobs::`, `db::` | — | **SIRADA** |

### 5b. AÇIK bulgular — `string::` (düzeltilmedi)

| # | Bulgu | Kanıt | Sınıf |
|---|---|---|---|
| 1 | 🔴 **`replace` sonsuz döngü + sınırsız bellek** — `from` boşken `s.find("",pos)` her zaman eşleşir, her turda araya `to` eklenir, string sonsuza kadar büyür (`to` da boşsa `pos` hiç ilerlemez) | `string::replace("abc","","X")` → **asılıyor** (5 sn timeout) | S8 |
| 2 | 🔴 **`pad_left`/`pad_right` GEÇERSİZ UTF-8 üretiyor** — kırpma `s.substr(s.size()-len)` bayt tabanlı, çok baytlı karakteri ortadan bölüyor | `pad_left("şğü",3,"x")` → `0x9F 0xC3 0xBC` (öksüz devam baytı) — `iconv` reddediyor | S9 |
| 3 | 🟠 **`pad_*` bayt sayıyor, kod noktası değil** — modülün geri kalanı (`len`/`substr`/`upper`/`reverse`) kod noktası farkındalıklı; `pad` değil → görünen genişlik yanlış | `pad_left("ş",3,"x")` = `"xş"`, `len()` = **2** (beklenen 3) | S2 |
| 4 | 🟠 **`upper`/`lower` "global" iddiası kodda karşılanmıyor** — kural doğru şekilde locale-bağımsız (Türkçe i↔İ yok ✓) ama TABLO yalnızca ASCII + Latin-1 + 3 Türkçe kod noktası (`ı ğ ş`). Kiril, Yunan ve Latin Ext-A'nın kalan ~125 karakteri sessizce dönüşmeden geçiyor | `upper("привет")`=`привет` (değişmiyor), `upper("ελλάδα")`=`ελλάδα`, `upper("łódź")`=**`łÓDź`** (yarım dönüşüm) | S2 |

Not: 4 numara felsefe açısından önemli — `stdlib.cpp:209` yorumu "dil globaldir" diyor,
yani kod kendi beyan ettiği sözleşmeyi tutmuyor. Kural global, tablo Türkiye'ye özel.

### 5a. `$arr[idx]` operatör kümesi (S3) — 2 kapandı, 4 açık

**DÜZELTİLDİ (23. bug, `90b4487`):** string anahtar vakaları (#1 ve #6). Sözleşme:
sayısal listeye tam-sayı-olmayan string anahtar → listeyi assoc'a **dönüştür ama
mevcut elemanları sayısal indeksleriyle anahtarlayarak koru**; `"1"` gibi tam sayı
metni sayısal indekstir (`look_is_int_key`, iki motorda aynı kural); listede
bulunmayan string anahtar okuması → `null`. `array::set` ile aynı sözleşme →
dil kendi içinde tutarlı. Guard: `differential_test.sh` `$arr[str]` bölümü.

**AÇIK (#2–#5):** sayısal indeks uçları — taşan/negatif indeks.

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

1. **İzole et** — en küçük tekrar üreten örnek, gerçek binary'de doğrula
2. **Sınıfını belirle** (bölüm 2) → **kardeşlerini ara** (3c)
3. **Kök nedeni kaynakta göster** — semptomu değil, satırı
4. **Düzelt** — mümkünse tek kaynağa delege et (S2'yi önler)
5. **Guard ekle** — doğru yere (3a notu) + **pozitif kontrol** (3b)
6. **Tam guard koş** — differential + parallel_db, ikisi de PASS
7. **Commit** — Türkçe, `Fix:` öneki, kök neden + neden kaçtığı + doğrulama. **Yalnız `Codlook` kimliği**
8. **Deploy** — yedekli + otomatik geri-alma, canlı doğrula
9. **Kaydet** — PROJE_DURUMU §7 bug tablosu + bu dosyanın §5 av defteri
