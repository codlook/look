# LOOK — Proje Durumu, Yol Haritası ve Deployment Etki Analizi

> **Son güncelleme:** 2026-07-17 · **Uç commit:** `f1b2619` · **74 commit** (2026-07-10 → 2026-07-17)
> **Canlı:** [look.codlook.com](https://look.codlook.com) · [qrmenu.codlook.com](https://qrmenu.codlook.com) — ikisi de sağlıklı (HTTP 200)

Bu dosya projenin **tek doğru kaynağıdır**: ne bitti, ne kaldı, neyi **ölçüp reddettik**, hangi
riskler açık, hangi kısıtlar korunacak. (2026-07-17'de `PERFORMANS_YOL_HARITASI.md` buraya
birleştirildi — iki ayrı dosya birbirinden kayıyordu.)

**Yöntem sözleşmesi:** İddialar kaynak kod okunarak veya **ölçülerek** çıkarıldı, spekülasyonla
değil; her iddia bir commit'e bağlı. Tahmin ile ölçüm çeliştiğinde **ölçüm kazanır** — bu dosyada
"ölçüm tahmini çürüttü" başlığı **dört kez** geçiyor (§8).

---

## 1. Bir bakışta

| | |
|---|---|
| **Felsefe** | "PHP gibi dağılan, Go gibi çalışan" |
| **Hedef bant** | **Go ile Node.js arası** (Elixir/Phoenix, Bun/Deno bandı) |
| **PHP'den alınan** | **Yalnız dağıtım kolaylığı + kitle alışkanlığı.** PHP bir hız/tasarım hedefi **değil**, bir benimsenme köprüsü |
| **Hedef kitle** | Küçük ve orta işletmeler (KOBİ) |
| **Bağımlılık** | **Sıfır** (OpenSSL/sqlite hariç) — MySQL/PostgreSQL/SQLite/RESP2 gömülü, flag yok |
| **Durum** | **Üretimde çalışıyor.** İki canlı site aynı binary'yi paylaşıyor |
| **Ana açık** | Web asıl hedefi için **yok** (üretimde çalışıyor). Vizyon-ilerlemesi: `go{}`/`select` **ertelendi** (§9 mimari karar); tek latent teknik açık = fiber modunda kanallar fiber-aware değil (default POOL'da devre dışı) |

---

## 2. Vizyon ve stratejik kalibrasyon

**Konumlandırma: Go ile Node.js arasındaki banda girmek.**

- **Node.js tarafından:** dinamik ergonomi (kolay yazım), non-blocking I/O.
- **Go tarafından:** ucuz eşzamanlılık, tek-binary deploy, öngörülebilir hız, küçük çekirdek.
- **LOOK'un nişi:** Node-tarzı ergonomi + Go-tarzı deploy/eşzamanlılık + PHP-tarzı benimsenme,
  "yeterince hızlı" compute ile.

### Bu hedef önceliği değiştirir

Go↔Node bandını tanımlayan şey **mikro-hız değil, eşzamanlılık modeli + throughput**:

| Grup | Rolü |
|---|---|
| **A / B / C** (motor hızı) | **Masa payı.** Gerekli ama **ayırt edici değil** — bandın *altında* kalmanı önler, *içine* sokmaz |
| **D** (eşzamanlılık/async) | **ASIL fark yaratan.** "Go-tarzı ucuz eşzamanlılık" vizyonun kalbi — LOOK'u banda yerleştiren tek özellik |

**Dürüst tavan:** JIT olmadan Go'nun compute'unu veya V8'i yakalayamazsın. Ama bu bandın bileti "en
hızlı compute" değil; **eşzamanlılık + ergonomi + deploy** üçlüsü. O üçlü ulaşılabilir.

> **Pratik okuma:** A/B/C'yi hızlı ve ucuz geç; asıl mühendislik yatırımını **D'ye** ayır.
> D "en son/opsiyonel" değil, **en stratejik** olan.

---

## 3. Mimari — iki motor ve C9

İki yürütme motoru var: **tree-walk interpreter** (referans semantik) ve **bytecode VM** (hızlı yol).
**C9** = VM'i her yerde tek motor yapma işi.

| Bileşen | Motor | Durum |
|---|---|---|
| `lk` (CLI + `-c`) | **VM DEFAULT** — kaçış kapağı `LOOK_CLI_VM=0` | ✅ **~41–51×** (3652ms → 72ms) |
| `lk-fcgi` (web) | VM default + route-bazlı interpreter fallback | ✅ fallback artık **gürültülü** |
| `lk-cgi` (CGI) | yalnız tree-walk | ⬜ VM binary'ye linklenmemiş |
| REPL / `lk test` | yalnız tree-walk | ⬜ ayrı komutlar |

**Fallback neden tehlikeli:** Route sessizce yavaş yola düşer, **doğru sonuç döner** → bug yıllarca
görünmez. Bulduğumuz bug'ların çoğu böyle saklanmıştı. Artık fallback **ERROR + "VM BUG"** basar;
`LOOK_VM_STRICT=1` maskelemeyi kapatır. Test ederken **"VM BUG" sayısı 0 olmalı**.

**Fallback güvenliği:** VM yoluna ancak tüm `use`'lar stdlib **ve** compile başarılıysa girilir
(ikisi de execution **öncesi**). Çıktı taahhüt edildikten sonra fallback **yok**.

**Yapısal borç:** builtin wiring **iki ayrı elle-yazılmış yerde** (`build_cli_builtins` ve
`req_builtins`). Birleştirilebilir ama web yolu production → refactor riski > getiri. Bunun yerine
3 motorlu guard kaymayı ampirik yakalıyor (§11).

---

## 4. Deployment etki analizi ve mimari takas

### Deployment kaybımız var mı? → A/B/C'de YOK, D'de dikkat

| Grup | Ne değişir | Plesk/XAMPP/FastCGI | Hot-reload | Kayıp? |
|---|---|:---:|:---:|:---:|
| A (mikro-opt) | yalnız VM iç döngüsü | Aynı | Aynı | **Yok** |
| B (Value/nesne/string) | yalnız motor iç veri yapıları | Aynı | Aynı | **Yok** |
| C9 (VM her yerde) | CLI/CGI de VM kullanır | Aynı | Korunmalı (kolay) | **Yok** — sadece hızlanır |
| D (async) | eşzamanlılık modeli | **Dikkatli tasarım gerekir** | Korunmalı | Riskli olabilir |

**A/B/C'de kayıp yok** çünkü hepsi `lk-fcgi` binary'sinin **içinde**: aynı binary, aynı FastCGI
protokolü, aynı servis modeli, aynı `.lk` dosyaları. Apache/nginx/Plesk hiçbir değişiklik fark etmez.

**D neden dikkat ister:** Tek-thread event-loop modeli, uygulamanın kendisinin sunucu olduğu yapıya
(nginx reverse-proxy) yakışır; Apache + `mod_proxy_fcgi` / Plesk shared-hosting ile daha az hizalı.
`lk-fcgi` zaten `--mode http` destekliyor → ikisi de mümkün, ama FastCGI/Plesk uyumu **bilinçli**
korunmalı. Fiber şu an yalnız `--mode http` Linux worker'ında ve opt-in.

### Hot-reload çalışıyor mu? → EVET (doğrulandı)

`http_main.cpp` her istekte `.lk` dosyasının `mtime`'ını kontrol eder; değiştiyse setup yeniden çalışıp
bytecode yeniden derlenir. **PHP-tarzı "dosyayı kaydet, restart yok" gerçekten çalışıyor.** `.lkc`
bytecode cache = PHP opcache muadili, reload'da geçersiz kılınıyor. Hiçbir optimizasyon bunu
kaldırmaz — recompile yollarında bu davranışı korumak bir **kısıt**, kayıp değil.

### ⚖️ Dürüst mimari takas (deployment kararının kalbi)

Model **PHP-FPM'den farklı** ve bu bilinçli:

| | PHP-FPM | LOOK `lk-fcgi` |
|---|---|---|
| Model | istek-başına-process + opcache | **kalıcı, çok-thread'li tek process** |
| İzolasyon | Bir istekteki crash diğerlerini etkilemez | **Bir segfault tüm worker'ı/process'i düşürür** |
| State | paylaşılan mutable state yok | **paylaşılan global state** (routes, globals_) → thread-safety yükü |
| Hız | istek-başına bootstrap | bootstrap yok → **daha hızlı** |

**Sonuç:** Kalıcı-paylaşılan-process modeli PHP'den hızlı ama **daha az izole/dayanıklı** ve
thread-safety yükü taşıyor (atomic `shared_ptr` → B8'in maliyeti). Bu bir kayıp değil, bir **tercih** —
ama farkında olmak lazım: **"PHP kadar çökme-toleranslı" değilsin, karşılığında "PHP'den hızlı"sın.**
Bulduğumuz ODR/heap bug'larının PHP'dekinden **daha kritik** olmasının sebebi tam olarak budur (§7).

### Korunacak kısıtlar (her faz için)

- FastCGI servis modeli (Plesk `look-fcgi` port 9100/9101) **değişmez**.
- `mtime` tabanlı hot-reload çalışmaya devam eder.
- `.lkc` bytecode cache reload'da geçersiz kılınır (stale çalıştırma yok).
- Sıfır 3rd-party bağımlılık korunur.
- Semantik: her değişiklik VM/interpreter **differential** ile doğrulanır.

---

## 5. Ölçümler — LOOK vs PHP 8.3 vs Node 20

Gerçek VPS (Docker değil), gerçek QR menü DB yükü (100 firma / 1000 kategori / 50k ürün, JOIN).

### Güncel: adil kurulum (1 CPU + 1 GB, eşit pool=32, no-TLS, c=100, en-iyi-3 + 200 ısınma)

| Motor | Mod | req/s | RAM | CPU/istek |
|---|---|---|---|---|
| **LOOK** | naive | **2760** | **29 MB** | **375 µs** |
| **LOOK** | pro (w=8) | **2837** | **9 MB** | **353 µs** |
| PHP 8.3 | naive | 2206 | 43 MB | 465 µs |
| PHP 8.3 | pro (persistent) | 2184 | 47 MB | 466 µs |
| Node 20 | naive | 283 | 52 MB | 3517 µs |
| Node 20 | pro (pool=32) | 1520 | 49 MB | 623 µs |

**LOOK +%25–30 önde**, CPU/istek en düşük, RAM'de **3–5× lider**. PHP **JIT açık + php-fpm**
ile ölçüldü (dev SAPI değil).

> **Kazanç motor mikro-opt'undan DEĞİL** — per-request sabit maliyetin kaldırılmasından (`adfc3c0`, §8).
> Eski ölçüm JOIN paritesiydi (2175 vs 2140).

### Kötü-kod cezası (naive → pro farkı)

| Motor | Ceza | Yorum |
|---|---|---|
| **LOOK** | **1.03×** | defaults zaten iyi — "batteries-included" iddiası **doğrulandı** |
| PHP | 1.0× | — |
| Node | **5.4×** | havuzsuz 283 r/s → pool/cluster/async **şart** |

### 2 CPU + 4 GB + c=200 (naive vs pro)

| | normal r/s | RAM | kötü-kod cezası |
|---|---|---|---|
| LOOK naive→pro | 4102 → ~5800 (w=8) | **13–24 MB** | **1.15×** |
| PHP naive→pro | 4468 → 5837 | 88–94 MB | 1.3× |
| Node naive→pro | 537 → 2243 | 97–164 MB | **4.2×** |

**Genel sonuç:** Ham hız değil — **toplam paket + RAM + kötü-kod toleransı**.

### Motor dışı tavan

| Konu | Ölçüm |
|---|---|
| MySQL tavanı | ~6900 JOIN/s — **gerçek web DB-bound** |
| TLS zinciri | ~750 r/s (nginx→Apache **çift TLS**); direkt port ~9.8k → **darboğaz LOOK değil** |
| Gerçek web-throughput için sıradaki | DB katmanı: sorgu sonuç cache'i, prepared-statement reuse |

---

## 6. Performans işleri

| # | İş | Durum | Ölçüm | Commit |
|---|---|---|---|---|
| A1 | regs bounds-check kaldır | ✅ | loop 0.746s→0.663s (~%11) | `1fa2c7e` |
| **B5** | **`Value` 80→32 bayt** | ✅ | skaler **+%40**, **RAM 2.5×** | `5aa03c0` |
| B5+ | str_ref hot-path | ✅ | string-ağır −%15 → −%8 | `b04cdc7` |
| B7 | string concat O(n²)→O(n) | ✅ | 400k `.=`: 10.39s→0.029s (**~358×**) | `0930d04` |
| C9 | CLI-VM default + VM hata konumu | ✅ | **~41–51×** | `82390e8` |
| — | **dispatch kopyası → thread_local** | ✅ | web **2–3×**, qrmenu **+%26** | `adfc3c0` |
| — | 16-bit `CALL_BUILTIN` (256 duvarı) + 30 builtin | ✅ | fallback sınıfı kapandı | `edfdaa5` |
| — | worker/DB-pool cgroup CPU limitine uyar | ✅ | tutarlılık | `9da6c2a` |
| A2 | global inline cache | ✅ | LOAD/STORE_GLOBAL slot çivileme → global-ağır **4.16s→1.81s (2.3×)**, register hızına yakın; web DB-bound'da nötr ama sıfır downside | `c524b32` |
| A3 | computed-goto dispatch | ⬜ | ~%20 bekleniyor | — |
| A4 | builtin arg allocation | ⬜ | düşük-orta | — |
| B8 | arena + non-atomic refcount | ⛔ **güvenli değil** (§9) | — | — |
| B6 | nesne → hash map | ❌ **ölçülüp reddedildi** (§8) | — | — |

**B7 notu:** İki ayrı O(n²) kaynağı vardı — (1) `concat` her adımda akümülatörü kopyalıyordu →
`append_in_place`; (2) atama-statement'ı gereksiz `MOVE` emit ediyordu → kaldırıldı (**tüm atamaları**
iyileştirir). `$s.=$s` aliasing test edildi.

**`9da6c2a` notu:** `hardware_concurrency()` **fiziksel** çekirdek dönüyordu → kısıtlı ortamda
(container/systemd/vCPU) 2 CPU'ya 32-64 worker. `look::available_cpus()` artık cgroup v2/v1 limitini
okur → min(fiziksel, cgroup)×4. **Naive kullanıcı tam performansı otomatik alır.**

---

## 7. Bug avı — kapatılan 16 bug

🔴 = üretimde veri/erişim kaybı · 🟠 = yanlış davranış · 🟡 = sessiz yavaşlama

| Bug | Sınıf | Commit |
|---|---|---|
| **ODR: `look::HttpResponse` iki farklı tip** — web'de `http::get` **sunucuyu çökertiyordu** | 🔴 crash | `d2df9d3` |
| `array::sort()` comparator'ı VM route'unda **sessizce yok sayılıyordu** — canlı yanlış sıralama | 🔴 veri | `6a58a8a` |
| `parallel()` DB bağlantısını **iade etmiyordu** — kalıcı sızıntı, endpoint donması | 🔴 erişim | `3459c3e` |
| `db::transaction` VM closure'ını çalıştırmıyordu — **sessiz commit** | 🔴 veri | `4c68f8e` |
| LOOK `throw`'u C++ sınırını geçiyordu (try tabanı) | 🔴 veri | `4c68f8e` |
| `error::new` tipli payload string'e düşüyordu → `error::is()` sessizce `false` | 🟠 | `235df9e` |
| VM `print`/`write` ayraç+newline yok; `exit()`/`die()` yok; CLI exit kodu | 🟠 | `4110cbf` |
| Implicit closure capture — `fn($x)=>$x*$m` VM'de `$m`'i görmüyordu | 🟠 | `58eb033` |
| `{$var}` interpolation **tüm VM route'larında sessizce "null"** | 🟠 | `04a9552` |
| Tanımsız değişken: iki motor ayrışıyordu → **STRICT** hizalandı | 🟠 | `9f45233` |
| `type::is_function` VM closure'ını tanımıyordu | 🟠 | `2b09b8a` |
| CLI-VM builtin-olmayan `mod::fn` + 256 sınır guard'ı | 🟠 | `7af8a6c` |
| `type::`/`crypto::`, `math::`/`string::` builtin_names'de yoktu | 🟡 | `2b09b8a`, `29de18a` |
| VM try/catch C++ runtime hatalarını yakalamıyordu | 🟡 | `04a9552` |
| bare `header`/`redirect`, `join`, `push`/`pop` | 🟡 | `0bb9895`, `a3682be`, `2fc47ee` |
| higher-order `array::map/filter/reduce` + `array::` + `len`/`json`/`intval` | 🟡 | `58eb033`, `22b8a70`, `c8dfdf0` |

### Bir BUG SINIFI: callback `Value::FUNCTION`-only kontrolü

`db::transaction`, `type::is_function`, `array::sort` **aynı kökten**: callback yalnız
`Value::FUNCTION` kabul ediyor, VM closure'ı (`BYTECODE_FN`) **reddediliyordu**. `array::sort` zaten
builtin olduğu için **canlıydı**: `sort([3,1,4,1,5], fn($a,$b)=>$b-$a)` → interpreter `[5,4,3,1,1]`,
VM `[1,1,2,3,4,5]` — **hata yok, log yok, sadece yanlış veri** (fiyat/tarih sıralayan her route).

**Yöntem dersi:** Tek tek düzeltmek yerine `== Value::FUNCTION` **sistematik taraması** yapıldı; bunun
bir sınıf olduğu `db::transaction`'dan sonra anlaşıldı. Köprü (`interp->invoke()`) zaten iki tipi de
ele alıyordu — korumalar kaldırıldı.

### ODR bug'ı — neden aylarca görünmedi (kalıcı ders)

`look::HttpResponse` **aynı namespace'te**, **farklı layout** ile iki kez tanımlıydı:

| istemci (`http_client.h`) | sunucu (`http_server.h`) |
|---|---|
| `status`, `body`, `headers`, `error` | `status_code`, `status_text`, `headers`, `set_cookies`, `body`, `keep_alive`, `build()` |

- `lk` (CLI) yalnız `http_client.cpp` linkler → **tek tanım** → sorun yok.
- `lk-fcgi` **ikisini de** linkler → **ODR ihlali** → LTO iki layout'u tek tip sanıp birleştiriyor →
  nesne bir düzenle kurulup ötekiyle yıkılıyor → `free(): invalid pointer` / segfault. **Tek istek**
  sunucuyu düşürüyordu (bkz. §4 mimari takas: segfault **tüm worker'ı** düşürür).

**Kör noktamız:** ASan build'inde **LTO kapalı** (`CMakeLists.txt`: `if(LOOK_SANITIZE) … else() IPO ON`)
→ **ASan bu sınıfı yapısal olarak göremez.** → **"ASan temiz" bellek hatası yokluğunun kanıtı DEĞİLDİR.**
Release-only crash görürsen ilk hipotez **ODR / link farkı** olsun: hedeflerin hangi `.cpp`'leri
linklediğini karşılaştır, header tip adlarını `comm -12` ile çakıştır.

**Çözüm:** istemci tarafı `look::HttpClientResponse`. **Guard:** ODR kategorisi eklendi.

---

## 8. Ölçülüp REDDEDİLEN / çürütülen tahminler

Bunlar "yapılmadı" değil — **yapılmaması gerektiği ölçümle gösterildi**. Kayda geçiyor ki tekrar
gündeme gelmesin.

### B6 (nesne → hash map) — ÖLÇÜLDÜ ve REDDEDİLDİ

Roadmap B6'yı **"Web'de en yüksek etki"** diye işaretliyordu. **Yapmadan önce ölçtük.**

Saf field-lookup maliyeti (1M iterasyon, **en kötü durum** = son alanı oku):

| Alan sayısı | Süre | 4-alana göre |
|---|---|---|
| 4 | 143 ms | 1.00× |
| 8 | 155 ms | 1.08× |
| 16 | 161 ms | **1.12×** |
| 32 | 183 ms | 1.27× |
| 64 | 263 ms | 1.83× |
| 128 | 389 ms | 2.72× |

Gerçekçi web yükü (500 DB satırı → 8 alanlı nesne → `array::map` → JSON): **1851 r/s, 0 route-disable**.

**Neden reddedildi:**
1. **Web nesneleri küçük** (5–20 alan) → O(n) tarama maliyeti **+%8–12**, darboğaz değil. Bitişik
   `vector`'de kısa string karşılaştırması ~3ns, cache-dostu; asıl maliyet dispatch + `Value` kopyası.
2. **Hash map bu aralıkta ZARARLI olurdu:** nesne başına bucket allocation + hash → tipik route'ta
   (500 nesne × 1-2 okuma) linear taramadan **pahalı**. Başabaş **~32–64 alan**.
3. **Fayda alanı gerçekçi değil:** ancak 64+ alanlı ve çok okunan nesnelerde kazandırır.

**Karar: B6 YAPILMAYACAK.** Yeniden değerlendirme koşulu: gerçek uygulama profilinde 64+ alanlı
nesnelerde yoğun lookup görülürse → hibrit (n>32 için lazy hash index).

### İstek-başına sabit maliyet — "PHP'yi geç" engeli buydu

Vizyondaki son madde motor hızı sanılmıştı. **Profil bunu çürüttü: darboğaz istek-başına SABİT maliyetti.**

| | ÖNCE | SONRA |
|---|---|---|
| sade app | copy=30µs dispatch=12µs → 21,203 r/s | copy=**0µs** → **43,213 r/s** (+%104) |
| global-ağır app | copy=63µs dispatch=23µs → 12,842 r/s | copy=**0µs** → **38,069 r/s** (+%196) |
| **gerçek qrmenu (VPS+MySQL)** | 3,220 r/s | **4,051 r/s (+%26)** |

- **Kök neden 1:** `make_dispatch_copy()` route'u çalıştırmaktan **2.5× pahalı** — içindeki
  `make_unique<Interpreter>()` **tüm stdlib'i her istekte** kuruyordu.
- **Kök neden 2:** app'in **global verisi her isteği vergilendiriyordu** (globals deep-clone): route
  aynıyken 200 öğelik dizi eklenince throughput **%39 düştü**. PHP-FPM bunu ödemez.

**Çözüm:** dispatch kopyası **thread_local** (worker başına bir kez) + `HttpApp::generation` ile
hot-reload tazelemesi. İzolasyon: interpreter yolunda `reset_globals_from`.

### Diğer reddedilenler

| İş | Beklenti | Ölçüm | Karar |
|---|---|---|---|
| Apache `enablereuse` | keep-alive kazancı | Apache serileştirmesi → baseline'dan **yavaş** | ❌ Kapalı |
| stdlib "preload" | VM'i `use`'suz çalıştır | interpreter `use` zorunlu tutuyor → **yeni divergence** | ❌ Geri alındı |
| Fiber'i **her yerde** default yap | tek model kolaylığı | Hızlı route'ta POOL ~%15 hızlı + daha iyi kuyruk (§9) | ❌ Reddedildi — default POOL kalır, FIBER yük-tipine göre opt-in |

> **DERS (dört kez tekrarlandı):** A1/B5/B7 gerçek DB-bound yükte throughput'a **nötr** çıktı ·
> B6'nın "en yüksek etki" tahmini **çürüdü** · asıl engel **per-request sabit maliyet**ti ·
> "motor yavaş" varsayımı **üçüncü kez** yanıldı. **ÖLÇMEDEN OPTİMİZE ETME — önce profille.**
> PROF logu (`copy=Xus dispatch=Yus`) zaten vardı, okumak yetiyordu.

**Kalibrasyon:** Gerçek web **DB-bound** → ham-motor micro-opt'ları (A2/A3/A4/B6) throughput'a nötr;
değerleri yalnız CPU-bound edge-case + doğruluk. **Asıl kazanç: tutarlılık + RAM + async.**

---

## 9. Async / Fiber (Grup D) — vizyonun kalbi

**Hedef Go modeli** — Node taklidi değil: goroutine'ler (ucuz yeşil-thread), channel'larla CSP, kod
düz-blocking yazılır, runtime ucuzca multiplex eder. `async/await` **yok** (colored functions yok).

**Substrat kısmen Go-goroutine:** stackful fiber (guard-page), per-fiber local, `wait_readable`
(epoll-park/resume = **Go netpoller**), double-enqueue race'ini kapatan SchedState makinesi,
**HTTP soket katmanı** fiber-aware (Fix#1/#2 + http_client). Ciddi ve doğru — ama **eksik**:
⚠️ **kanal primitifi fiber-aware DEĞİL** (`interpreter.cpp` `cv.wait` thread bloklar) ve per-fiber
DB izolasyonu yok. Yani "substrat tam" değil, "I/O katmanı fiber-aware, kanal+DB katmanı değil".
Bu, gerçek go-substratının ilk adımı ve go{} maliyetinin bir parçası (bkz. karar, aşağı).

| İş | Durum | Kanıt / not |
|---|---|---|
| Fix #1 — `recv` fiber-aware | ✅ | c=100 **hang kök nedeni**: `handle_connection` bloklayan `::recv()` kullanıyordu → keep-alive'da fiber sonraki isteği beklerken **tüm worker** bloklaniyordu (`ec8dc12`) |
| Fix #2 — acceptor fiber + 15s idle-timeout | ✅ | c=200 / c=500 / c=1000 → **0 hata** (eski: c=100 hang) (`4b311c0`) |
| **`http_client` fiber-aware** | ✅ | recv/SSL_read yield eder (Go netpoller). Ölçüm (yavaş upstream 0.5s, 1 worker, 10 eşz): POOL 5.53s/3.62 r/s → **FIBER 1.52s/13.19 r/s** (upstream tavanı 13.21 — **tek thread'le tavana ulaştı**), 0 hata (`d5919ce`) |
| `parallel()` ↔ DB köprüsü (fiber task) | ⬜ | intra-request paralel query — artık **zorunlu değil** (sızıntı `3459c3e` ile çözüldü), opsiyonel iyileştirme |
| **Fiber default kararı** | ✅ **VERİLDİ** | Default **POOL**, FIBER yük-tipine göre opt-in (aşağıda) |
| **`go { }` + `select` + paylaşımlı kanal** | 🔴 **ERTELENDİ (karar, 2026-07-18)** | Aşağıda — mimari karar, talep bekliyor |
| `await_all` (thread+kopya, gather) | 📐 **Hazır tasarım** | Talep gelince — aşağıda |

### 🔴 go{}/select KARARI (mimari, 2026-07-18) — ERTELE, spekülatif inşa etme

Üç iddia **kodda doğrulandı** (liste değil, gerçek): (1) kanallar **thread-blocking** —
`interpreter.cpp` `not_empty.wait`/`not_full.wait` (`condition_variable`), fiber-aware değil →
fiber modunda boş kanaldan recv **tüm OS thread'i bloklar**, o thread'in goroutine'leri açlıkta;
(2) DB bağlantısı **`thread_local`** (`web_stdlib.cpp` `thread_local std::map<..., DbConnection>`)
→ aynı thread'teki 2 fiber **aynı bağlantıyı** paylaşır → MySQL stateful protokol bozulur =
**sessiz veri bozulması**; (3) Value içeriği **kilitsiz** (`shared_ptr<vector<Value>>`, refcount
atomic ama container değil) → 2 fiber/thread aynı array'i mutate → heap bozulması. `parallel()`
bunu **deep-copy** ile aşıyor (vm.cpp:725,736).

**KARAR: go{}/select YAPILMAYACAK (şimdilik).** Gerekçe (dört ayak):
1. **Hedef pazar istemiyor** — KOBİ/PHP-yerine geliştiricisi tek request'te 50 API'yi eşzamanlı
   çağırmaz. go{}/select bu pazarın **kullanmayacağı** özellik.
2. **Öldürdüğümüz sınıfı geri getirir** — bu turun her kazancı *sessiz-bozulmayı öldür* ekseninde
   (ODR, sort comparator, DB sızıntı, STRICT). DB `thread_local`→fiber-paylaşımı **yeni** sessiz-
   bozulma vektörü açar. **Kopya modeli bunu ÇÖZMEZ** (per-fiber DB izolasyonu ayrı, tüm stdlib'i
   etkileyen yeniden-yapılanma).
3. **Proje asıl hedefi için bitti** — go{} bir *eksik* değil, *vizyon-genişletmesi*.
4. **Talep kanıtı yok** — kullanıcı/yıldız yokken 20-30 saatlik ileri eşzamanlılık = **erken
   optimizasyon**. Doğru sıra: biteni yayınla → kullanıcı edin → gerçek talep karar versin.

### 📐 await_all — in-request fan-out'un HAZIR tasarımı (talep gelince, ~6-10s)

go{}'nun alıntılanan tek gerçek kullanım durumu ("bir request'te N dış API'yi eşzamanlı çağır")
bir **fan-out/gather** desenidir — kanala/select'e/paylaşımlı belleğe **gerek yok**. Tasarım:

- **`await_all([closure,...], timeout_ms?)` → sonuç dizisi.** Kanal yok, select yok.
- **`parallel()`'in thread+kopya modeli üstüne** (yeni değil): bounded pool (task_acquire /
  `LOOK_PARALLEL_LIMIT` zaten var) + her closure kendi thread'inde + `DbGuard` ile per-thread DB
  iadesi (vm.cpp:736 — kanıtlı). **await_all'ı FIBER üstüne kurma** — o zaman per-fiber DB belasını
  miras alır; thread modeli izolasyonu zaten çözüyor.
- **Kopya semantiği, paylaşımsız** → Value data-race yapısal olarak **yok**. **SIFIR yeni güvenlik
  yüzeyi** (kanıtlı izolasyonu yeniden kullanır).
- KOBİ ölçeği (5-10 eşzamanlı dış çağrı) için thread maliyeti önemsiz; "ucuz fiber" avantajı yalnız
  50+ fan-out'ta önemli (KOBİ yapmaz). Eksik olan tek şey `select` kompozisyonu — onu da
  `timeout_ms` parametresi fan-in'in %90'ını karşılar.

### 🟡 Kanal fiber-aware (latent bug — spekülatif düzeltme değil, kayıt)

Kanallar fiber modunda thread bloklıyor (yukarıda #1). Ama fiber **default OFF** ve fiber+kanal
**nadir kombo** → şimdilik **bilinen-latent-sorun** olarak kaydedildi; go{}/await_all gündeme
gelmeden düzeltmek spekülatif. Gerçek go-substratı istenirse ilk adım budur (park/resume).

### ✅ Fiber default KARARI (ölçümle verildi, 2026-07-17)

`http_client` fiber-aware olunca (`d5919ce`) iki uç da ölçülebilir hale geldi. Aynı 4-worker,
hızlı JSON route (dış I/O yok) vs yavaş dış API senaryosunda:

| Senaryo | POOL | FIBER | Kim kazanır |
|---|---|---|---|
| Hızlı route, c=50 (dış I/O yok) | **11506 r/s**, p95 6ms | 9724 r/s, p95 8ms | POOL **+%18** |
| Hızlı route, c=200 | **9210 r/s**, p95 28ms | 8097 r/s, p95 31ms | POOL **+%14** |
| **Yavaş dış API** (0.5s), 1 worker, 10 eşz. | 3.62 r/s, 5.53s | **13.19 r/s**, 1.52s | **FIBER 3.6×** |

**KARAR: Default POOL kalır; FIBER opt-in (`LOOK_FIBER_DISPATCH=1`).**

- **Neden default POOL:** Tipik KOBİ web yükü (hızlı/DB-bound route) POOL'da ~%15 daha hızlı ve
  kuyruğu (p95/p99) daha iyi. Bu, hedef kitlenin **ortalama** isteği.
- **Ne zaman FIBER aç:** Route'un çoğu zamanını **yavaş dış çağrıda** geçirdiğinde (dış API,
  webhook, yavaş upstream) — orada 3.6× kazanç POOL'un %15'ini kat kat aşar. Fiber c=200/500/1000'de
  **0 hata** (D Fix #2), yani stabilitesi kanıtlı.
- **Basit eşik kuralı:** route ağırlıklı `http::*` ile yavaş dış servis bekliyorsa → **FIBER**;
  CPU/DB-bound ve hızlıysa → **POOL** (default).

> **Düzeltilen kayıt:** Eski not "fiber CPU-bound'da **2× yavaş**" diyordu; o rakam sentetik
> CPU-bound mikro-bench'tendi. Gerçekçi hızlı JSON route'ta ceza **~%15** — çok daha ılımlı.
> (Yine "ölç, tahmin etme": eski tahmini taze ölçüm düzeltti.)

**Neden fiber DB-yükünde kazanmıyor:** **Async DB pool zaten concurrency sağlıyor.** LOOK'un iki
eşzamanlılık alt-sistemi (`parallel()` = compute, async-DB-pool = request-arası) birbirine bağlı değil.
Fiber'in kattığı yeni eksen: **`http_client` üzerinden yavaş dış I/O** — DB değil, dış servis.

### ⛔ B8 neden "güvenli değil"

Başlamadan önce doğrulandı: **`Value`'lar thread'ler arası geçiyor** — channel `send_val`/`recv_val`
(queue'ya move, başka thread'de recv) ve `PARALLEL_CALL` (`std::thread`'e globals kopyası). shared_ptr'ın
**atomic** refcount'u bu yüzden **gerekli**. Naive non-atomic → channel/parallel'de **data race →
use-after-free** (yeni kapattığımız memory-safety işini bozar). Güvenli B8 = arena (request-local,
thread geçmeyen) + tagged-pointer → **haftalar + yüksek risk**. **Ertelendi.** B5 string regresyonunun
kalan %8'i buna bağlı ama aceleye gelmez.

---

## 10. Ölçüm metodolojisi ve tuzaklar

**Yanlış ölçüp yanlış karar vermemek** için bedeli ödenerek öğrenildi.

| Tuzak | Ne oluyordu | Doğrusu |
|---|---|---|
| `php -S` | **varsayılan tek süreç** → DB beklemesini örtüştüremez → sahte-düşük (641 r/s) | `PHP_CLI_SERVER_WORKERS=16`; gerçeği: **nginx + php-fpm** |
| JIT sessizce kapalı | FPM pool'da `php_admin_value[opcache.jit_buffer_size]` **çalışmaz** (INI_SYSTEM) → `jit_enabled:false` | php-fpm `-d opcache.jit_buffer_size=128M -d opcache.jit=tracing` → `opcache_get_status()` ile **doğrula** |
| Tek koşum | aynı ayarla 2179 vs 1074 (**2× gürültü**) | **en-iyi-3** + 200 istek ısınma |
| `systemctl show CPUUsageNSec` | bu systemd'de **boş** döner | cgroup dosyaları (`cpuacct.usage` / `cpu.stat`); RAM'i **yük altında** örnekle |
| `systemd-run --working-directory` | **yok** | `--property=WorkingDirectory=` |
| `cmp` / `diff` yok | AlmaLinux 8 minimal'de diffutils yok → "command not found" non-zero → **"baytlar farklı" yanlış alarmı** | `od` dump'larını **dosyaya** yaz, shell string karşılaştır (`$(od)` trailing newline'ı yutar) |
| Uydurma slug ile test | 404 dönüyordu = **asıl veri yolu hiç çalışmamıştı** | gerçek slug + **pozitif kontrol** (bilerek bug'lı app → tespit ediliyor mu?) |
| "ASan temiz" | ASan build'inde **LTO kapalı** → ODR sınıfını göremez | Release-only crash'te **ODR/link farkına** bak |
| FCGI portuna `curl` | FastCGI protokolü — çalışmaz | https://domain veya `--mode http` |
| Worker eşitsizliği | "LOOK 2× > PHP" eski artefaktı **bu sınıftandı** | karşılaştırılan tarafları **eşitle** |

**Karar yöntemi (tanımsız-değişken flip'i — model alınmalı):** Karar tartışarak değil **ölçerek**
verildi. ① Davranışı değiştirmeyen teşhis aracı eklendi (`LOOK_WARN_UNDEF=1`, `9995458`) →
② **pozitif kontrolle** doğrulandı (bilerek bug'lı app → 1 tespit; yoksa "0 bulundu" **anlamsız**
olurdu) → ③ gerçek uygulamalar **gerçek slug'larla** tarandı → **0 tanımsız okuma** → ④ flip'in
risksizliği **veriyle** gösterildi → ⑤ flip yapıldı → ⑥ **aynı uygulamalarla strict binary'de tekrar
doğrulandı** (look 200/200/200/404, qrmenu 200×6/404, 0 hata, 0 fallback). **Risk tahmin değil, ölçüm.**

**Gerekçe (neden strict):** hedef band Go↔Node ve **ikisi de strict** (Go: derleme hatası, Node:
ReferenceError). PHP'den **yalnız dağıtım** alınıyor, **semantik değil** → lenient'in felsefi dayanağı
yok. Interpreter referans motor ve zaten strict'ti — **sapan taraf VM'di**, sessiz null üretiyordu
(bu turun tüm bug'larıyla aynı sınıf). `LOOK_WARN_UNDEF=1` artık **geçiş modu**.

---

## 11. Güvenlik ağı — regresyon guard'ları

**Her motor değişikliğinden (opcode/builtin/Value/köprü) SONRA çalıştır.**

```bash
bash cpp/tests/differential_test.sh <lk> <lk-fcgi>   # canonical
bash cpp/tests/parallel_db_test.sh  <lk-fcgi>        # tek istek YETMEZ
```

| Guard | Kapsam |
|---|---|
| `differential_test.sh` | **3 motor × 20 kategori (A–T)** — tree-walk == CLI-VM == web-VM **birebir** |
| ↳ CLI yüzeyi | `print`/`write` **bayt-bayt** (od) + `exit(3)` kodu |
| ↳ fallback gürültüsü | ERROR + "VM BUG"; `LOOK_VM_STRICT=1` maskelemiyor |
| ↳ izolasyon | thread_local dispatch kopyası istekler arası **sızmamalı** (`s=1` ×5) |
| ↳ **ODR** | web route'unda `http::get` → `{"st":200}` **ve sunucu ayakta** |
| `parallel_db_test.sh` | parallel+DB **sızıntısı**, havuz baskısı (vm + interp) |

**3 motor neden şart:** builtin wiring iki ayrı yerde (§3) — sessizce kayabilir ve **kaydı da**:
`math::round` bir dönem web'de çalışırken CLI-VM'de çalışmıyordu. **2 motorlu test bu sınıfı göremez.**

**Operasyonel:** guard'larda temizlik **PID ile** — geniş `pkill -f` **ASLA** (§14).

### 🎯 Guard'ın kör noktası dersi

Differential **dil semantiğini** kapsıyordu ama **CLI-özgü yüzeyi** (print/exit/argv/stdin)
kapsamıyordu → CLI-VM'i default yapmadan önceki **ilk CLI testi 3 bug çıkardı** (`4110cbf`), biri
**web-VM'i de etkiliyordu**: `print($a,$b)` → interpreter `"a b\n"`, VM `"ab"`.

**Neden kaçmıştı:** gövde tek kez print ediyor + `$(...)` trailing newline'ı yutuyor → print semantiği
**görünmüyordu**. Aynı şekilde differential `map/filter/reduce`'u kapsıyor ama `sort(comparator)`'ı
kapsamıyordu → `6a58a8a` canlı bug'ı **kaçmıştı**.

> **GENEL DERS: guard'ın kapsamadığı yüzey = bug'ın saklandığı yüzey.**

**Gerçek MySQL doğrulaması** (VPS ayrı port, prod'a dokunulmadan): parallel+DB sızıntı **10/10**,
havuz baskısı **sum=8**, transaction commit/rollback **`rows=[a],err=rolled:boom`** (InnoDB veri
bütünlüğü), `array::sort` comparator **`[5,4,3,1,1]`**, **0 route fallback**. Fix'ler yalnız SQLite'ta
değil, **production'ın kullandığı MySQL'de** de doğrulandı.

---

## 12. Sıradaki işler (öncelik sırasıyla)

| Sıra | İş | Neden şimdi |
|---|---|---|
| ✅ | ~~`http_client` fiber-aware~~ | **BİTTİ** (`d5919ce`) — tek thread'le upstream tavanına ulaştı, 3.6× throughput |
| ✅ | ~~Fiber default kararı~~ | **VERİLDİ** — default POOL, FIBER yük-tipine göre opt-in (§9); hızlı route'ta ceza sanılan 2× değil ~%15 çıktı |
| ✅ | ~~A2 inline cache~~ | **BİTTİ** (`c524b32`) — global-ağır 2.3×, güvenli (nested-run doğrulandı) |
| **1** | `lk-cgi` / REPL / `lk test` → VM | C9'un kapanışı; motor ikiliğini bitirir |
| 2 | A3 computed-goto | ~%20, düşük risk, DB-bound'da görünmez |
| 3 | `parallel()` ↔ DB köprüsü (fiber task) | intra-request paralel query (opsiyonel) |
| 🔴 | ~~`go { }` + `select`~~ | **ERTELENDİ** (§9 mimari karar) — pazar kullanmıyor, felsefeyi ters çeviriyor, talep kanıtı yok |
| 📐 | `await_all` fan-out-gather | **Hazır tasarım** (§9) — talep gelince, thread+kopya, sıfır yeni risk |
| ❌ | ~~DB prepared-statement reuse~~ | **İncelendi ve REDDEDİLDİ (2026-07-18):** hot yol `query()` text protokolü kullanıyor (prepared değil); prepared yalnız parse/plan'ı kurtarır, darboğaz MySQL JOIN execution (~6900/s) → dokunmuyor. Bkz. §8 |
| ⏸ | DB otomatik result cache | App sorumluluğu (`cache::` modülü var); otomatik = bayat-veri/veri-bütünlüğü riski |
| — | A4 / B8 | ertelendi (B8 güvenli değil) |

**Kalan builtin boşluğu (~16, düşük öncelik):** `jobs::` (11), `queue::` (3), `template::` (2),
`mail::` (2), `log::` (2), `http::` (2), `cache::` (2), `validator::check`, `file::upload_dir`,
`runtime::` (2), `route::` (2), `look::check`. Her biri **interpreter fn'ine map oluyor mu + callback
alıyor mu** diye doğrulanmalı — **körlemesine ekleme YOK** (`db::` bunu kanıtladı; 256 duvarına
körlemesine dayanmak `cache::size`'ı `print`e çevirmişti).

---

## 13. Bilinen sınırlar ve açık riskler

| Konu | Durum |
|---|---|
| Fallback güvenlik ağı | Hâlâ açık — bug **maskeleyebilir**. Test ederken "VM BUG" **0 olmalı** |
| Çökme toleransı | Kalıcı-process modeli → **segfault tüm worker'ı düşürür** (§4). PHP-FPM'den daha az izole |
| VM hata konumu | `Line` var, **`Column` yok** (tree-walk'ta var) |
| `lk-cgi` / REPL / `lk test` | Hâlâ tree-walk → motorlar bu yüzeylerde ayrışabilir |
| B8 | Non-atomic refcount **güvenli değil** (§9) |
| B5 string takası | skaler +%40 ama string-ağır −%8 (gerçek DB yükünde **net/parite**); B8 ile kapanır |
| Tek makineden yük testi | ab tavanı c≈20k / ~50k ephemeral port → **"1M eş zamanlı" tek kaynaktan test edilemez**; 1M **toplam istek** dayanıklılığı test edilir |
| Ekosistem | Yeni dil → kütüphane ekosistemi yok. **Argüman olarak kullanılmıyor**; hedef KOBİ ve ihtiyaçlar dilin içinde |

**Yüksek concurrency (c=5000+) tuning:** kernel duvarı `tcp_max_syn_backlog=512` → c=5000 SYN
patlamasında bağlantı düşer. VPS'te kalıcı: `/etc/sysctl.d/99-look.conf` (somaxconn=8192,
tcp_max_syn_backlog=8192, port_range 15000-64999). Kod backlog 8192. Test eden shell'de
`ulimit -n 200000` (hem ab hem sunucu inherit eder).

---

## 14. Operasyonel dersler (bedeli ödendi)

| Ders | Olay |
|---|---|
| **Paylaşımlı/prod sunucuda ASLA geniş `pkill -f`** | `pkill -f 'mode http'` prod servisleri (look+qrmenu) öldürdü → **~20 dk kesinti**. PID kullan (`$!`, `ss -tlnp`) |
| **Deploy yolunu her seferinde doğrula** | Gerçek dosya **`/usr/bin/lk-fcgi`**; `/opt/look/lk-fcgi` ona işaret eden **symlink**. Kayıt bir dönem **ters** yazıyordu → deploy symlink'i düz dosyayla ezecekti (guard yakaladı). `readlink -f` ile **gerçek yola** yaz |
| Daha eski bir kayıt `/usr/local/bin/lk-fcgi` diyordu | **Öyle bir dosya yok** → oraya kopyalayan deploy hiçbir şey değiştirmez (**"deploy ettim" yanılgısı**). `systemctl cat <servis> \| grep ExecStart` + `readlink -f` ile teyit et |
| **İki servis aynı binary'yi paylaşır** | `look-look-codlook-com` (**9101**) + `look-qrmenu-codlook-com` (**9100**) → binary değişimi **her iki canlı siteyi birden** etkiler → deploy **yedekli + otomatik geri-alma** |
| Prod'a dokunmadan test | Binary'yi ayrı isimle (`/tmp/lk-fcgi-test`) **ayrı portta** çalıştır |
| Deploy sonrası | Stale `.lkc` bytecode cache'lerini sil (`.look_cache/`) |
| Uzak komutlar | Karmaşık komutu **script dosyasına yaz → pscp → `bash /tmp/x.sh`** (tırnak/escape sorunları) |

---

## 15. Katkı ve kimlik

- Commit/push/release'de **yalnız `Codlook`** görünür; **Co-Authored-By satırı eklenmez**.
- Tek geçerli kimlik: `Codlook <91369165+codlook@users.noreply.github.com>`.
- **GitHub katkıyı ada değil E-POSTAYA göre bağlar** — ad doğru olsa bile yanlış e-posta katkıyı
  **başka bir hesaba** yazar (bir kez yaşandı; amend + force-push gerektirdi ve GitHub'ın önbelleği
  bir süre kaldı).
- `.git/hooks/commit-msg` bunu **zorunlu kılıyor** (yanlış ad/e-posta veya Co-Authored-By → commit reddi).
  ⚠️ Hook `.git/hooks` içinde → **klonla taşınmaz**, yeni klonda yeniden kurulmalı.
- Commit mesajları Türkçe, `Fix:` / `Feat:` / `Perf:` önekiyle.
