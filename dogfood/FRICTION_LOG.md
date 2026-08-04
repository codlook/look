# Dogfooding sürtünme günlüğü — Görev Yöneticisi (B)

**Kural:** Uygulamayı KAYNAĞA değil DOKÜMANA (codlook.com/docs = docs/index.html) bakarak yazıyorum.
Doküman yetmediğinde bu bir BULGUDUR (gerçek kullanıcının yaşayacağı şey). Her takıldığım yer,
her workaround, her bulamadığım doküman buraya. Amaç çalışan app değil — sürtünmeyi ölçmek.

Kapsam (dikey dilim, 5 ekran):
- [ ] Kayıt + giriş (auth::, session::)
- [ ] Liste sayfası (db::query + template, sayfalama)
- [ ] Ekle/düzenle formu (validator::, POST akışı, hata gösterimi)
- [ ] Sil (CSRF/confirm)
- [ ] JSON endpoint (assoc round-trip)

---

## Bulgular

<!-- Her bulgu: [KATMAN] ne yapmaya çalıştım → ne oldu → doküman ne diyordu → workaround/fix -->

### BULGU #5 — Yayınlanan release asset'leri HEAD'den 30 commit geride (deployment dogfooding, VPS'e dokunmadan)

**Katman:** dağıtım / release süreci / kaynak↔artefakt drift
**Ne yaptım:** Görev yöneticisini gerçek kullanıcı gibi deploy etmek için README "Install (prebuilt)"
yolunu izledim: Releases'ten `look-lang-linux-1.0.0.zip` → `sudo bash install.sh`.
**Ne oldu (ÖLÇÜLDÜ, düzeltilmiş):** Release v1.0 asset'leri GERÇEKTEN var (docs doğru işaret ediyor).
Release *tag* tarihi 2026-07-09 ama **asset'ler `5465074`'e yenilenmiş (2026-08-02)** — yani "bir ay
geride" DEĞİL. Ölçüm: `git rev-list --count 5465074..HEAD` = **30 commit / 2 gün** (HEAD=e47b80b, Aug 4).
İlk yazdığım "~1 ay geride" YANLIŞTI (release tag tarihini asset tarihiyle karıştırdım) — bu turun
"ölçmeden iddia etme" kuralı bulgu metnine de uygulandı, düzeltildi.
**Sonuç (öz tutuyor):** session::has (18bf8f4) o 30-commit farkın İÇİNDE → bugün README'yi takip eden
kullanıcı **session::has içermeyen** binary indirir → dokümandaki auth örneğini yazınca 500.
Dokümanla yönlendirilen install yolu, dokümandaki özelliği çalıştıramayan binary veriyor.
**Kök:** Release asset güncellemesi manuel; her HEAD'de değil. Versiyon 1.0.0 sabit. Memory "yayın
kapısı drift". → release otomasyonu ihtiyacının gerçek gerekçesi (deployment turundan SONRA sıraya).
**Deploy kararı:** VPS'te kaynaktan derle (belgelenmiş ama hiç test edilmemiş yol; docker gerekmez).
Kısıt: canlı `look-test-codlook-com`'a DOKUNMA — ayrı dizin `/opt/dogfood` + ayrı port `:7700`,
`/usr/bin/lk-fcgi` değişmez, mevcut servis durmaz.

### BULGU #6 — Kaynaktan derleme: AlmaLinux 8 default gcc C++23 yapamaz, README demiyor

**Katman:** dağıtım / build-from-source / doküman eksik ön koşul
**Ne yaptım:** README:215 "Build from source": `cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release`.
Gereksinim: "C++23 compiler + CMake 3.20+".
**Ne oldu:** AlmaLinux 8.10 default `g++ 8.5.0` → `-std=c++23` **"unrecognized command line option"**.
Tier-1 platformunun default derleyicisi C++23 yapamıyor. Çözüm `gcc-toolset-12/13`
(`source /opt/rh/gcc-toolset-12/enable`) ama README bunu SÖYLEMİYOR (bilgi sadece CLAUDE.md'de).
Taze AlmaLinux'ta `dnf install gcc-toolset-12` de gerekir. Doğrulandı: gcc-toolset-12 ile C++23 OK.
**Fix:** README/deployment docs'a "AlmaLinux/RHEL 8: `dnf install gcc-toolset-12` + enable" ön koşulu.

### BULGU #7 — README'nin birincil build komutu kutudan ÇALIŞMIYOR (statik SSL default ON)

**Katman:** dağıtım / build-from-source / CMake default
**Ne oldu:** `cmake -S cpp -B cpp/build` (README:215 birebir) → **"Could NOT find OpenSSL (missing:
OPENSSL_CRYPTO_LIBRARY)"** — `openssl-devel` KURULU olmasına rağmen. Kök: `CMakeLists:210
LOOK_STATIC_SSL=ON` (default) → `OPENSSL_USE_STATIC_LIBS TRUE` → `libcrypto.a` arıyor; `openssl-devel`
sadece `.so` veriyor, `.a` yok. README:231 ince yazıda "install static OpenSSL, or configure with..."
diyor ama **birincil/kopyalanan komut kırık**. Çözüm: `dnf install openssl-static` VEYA
`-DLOOK_STATIC_SSL=OFF` (dinamik — VPS deploy'da yeterli). İkincisiyle build OK (70s, lk-fcgi 4.3MB).
**Fix:** README'nin birincil komutu ya `-DLOOK_STATIC_SSL=OFF` içermeli ya statik-libs ön koşulu net
yazılmalı. "Kopyala-yapıştır çalışsın" — şu an ilk deneyimde patlıyor.

### BULGU #8 — Secure session cookie + düz-http → eski curl'de session kırılıyor

**Katman:** session / cookie / --mode http doğrudan senaryo
**Ne oldu:** VPS'te :7700 (düz http) app'i test ederken kayıt **419 (CSRF)** — GET/POST arası session
tutmadı. Kök: session cookie `Secure` işaretli (`Set-Cookie: ...; HttpOnly; Secure; SameSite=Lax`);
**curl 7.61.1 (RHEL8) Secure cookie'yi düz-http'de göndermiyor** → her istek yeni SID → csrf mismatch.
(Yerel curl 8.x localhost'a Secure gönderiyor → yerelde çalışmıştı; sürüm farkı.) Cookie'yi elle
`-H "Cookie:"` ile geçince TAM akış çalıştı (kayıt/görev/JSON, 0 fallback).
**Değerlendirme:** Üretimde SORUN DEĞİL (test.codlook.com HTTPS/Apache arkasında → tarayıcı cookie'yi
HTTPS'te alır). AMA `--mode http`'yi DOĞRUDAN (TLS'siz, dev/basit-deploy) kullanan biri için gerçek
gotcha: Secure cookie düz-http'de session'ı bozar. Docs "--mode http complete web server" derken bu
sınırı belirtmeli, VEYA localhost/http'de Secure'ı koşullu bırakmayı değerlendir (TLS ardında Secure,
düz-http'de değil — ama bu güvenlik kararı, ölçülmeli).

### DEPLOYMENT TURU SONUÇ (kaynaktan derleme + çalıştırma dogfood edildi)

Görev yöneticisi VPS'te (AlmaLinux 8.10) kaynaktan derlenip :7700'de ÇALIŞTIRILDI, tam CRUD+auth+JSON
akışı doğrulandı (session::has dahil — yayın binary'de yok), canlı `look-test-codlook-com` :9100 +
test.codlook.com→200 DOKUNULMADI (PID-kill temizlik). **3 belgesiz varsayım çıktı** (#6 gcc-toolset,
#7 statik-SSL default, #8 Secure-cookie/http) — analizcinin öngördüğü gibi "deploy her zaman
belgelenmemiş varsayım taşır; kod yazmak dokümanla yönlendirilir ama deploy etmek değil". KALAN
(bu tur yapılmadı): systemd unit kalıcılığı, Apache HTTPS wiring, log/izin/DB-yolu belgelenmesi.

### BULGU #1 — `session::has()` doküman var, implementasyon YOK (500)

**Katman:** session / doküman-implementasyon uyuşmazlığı
**Ne yaptım:** Kayıt sayfasında CSRF token'ı için `if (!session::has("csrf"))` yazdım — dokümandaki
kalıp. Auth örneği de (docs satır 1584) `session::has("admin_id")` kullanıyor.
**Ne oldu:** GET /register → **HTTP 500**, boş gövde. Log:
- VM: `Çağrılabilir değil (fonksiyon bekleniyor)` → route interpreter'a düştü (VM BUG log'u)
- Interpreter: `'session' has no function 'has'`
**Doküman ne diyordu:** `session::has("k")` — "Does the key exist?" (docs satır 1549) + kendi auth
örneğinde kullanılıyor (1584). Yani doküman API'yi VAAT EDİYOR, çekirdek sağlamıyor.
**Kök:** `web_stdlib.cpp` session modülünde `start/regenerate/set/get/destroy` var, `has` yok;
`builtin_names()`'de de yok → VM'de "not callable", interpreter'da "no function".
**Neden değerli:** Dokümanı takip eden HERKES bunu yaşar; auth örneğinin kendisi çalışmıyor. On
turluk denetim görmedi çünkü testler `session::has` çağırmıyordu — "guard'ın kapsamadığı yüzey".
**Fix:** session modülüne `has` ekle (get gibi ama bool), `builtin_names()` SONUNA `session::has`
ekle (ortaya değil — .lkc index kayması). + docs zaten doğru, kod ona yetişti.
**Yan not (ayrı, küçük):** VM fallback hata mesajı "Çağrılabilir değil" — hangi ismin çağrılamadığını
söylemiyor. Interpreter mesajı ("'session' has no function 'has'") çok daha yararlı. VM tarafı da
ismi vermeli (teşhis kolaylığı).

### BULGU #2 — Ceil-bölme papercut: pagination'da float UI'a sızıyor (DX, benim kodum)

**Katman:** aritmetik / DX (dil bug'ı DEĞİL — belgeli davranış, ama sürtünme gerçek)
**Ne yaptım:** `$total_pages = ($total + $PER_PAGE - 1) / $PER_PAGE` — klasik ceil-bölme idiom'u.
**Ne oldu:** Şablonda "Sayfa 1 / **2.2**" göründü. Çünkü `11/5 = 2.2` (int/int → float, belgeli).
**Kök:** LOOK'ta tamsayı-bölme operatörü yok (`//` / `div`). En yaygın web kalıbı (pagination) her
seferinde `int(...)` sarımı gerektiriyor; unutmak kolay ve sessizce float UI'a sızıyor.
**Workaround/fix:** `int((...)/...)` — `int()` pozitif float'ı tabana yuvarlar, ceil-bölme doğrulanır.
**Öneri (dil, ERTELE):** Felsefe "en kısa yol" — pagination bu kadar yaygınken `//` tamsayı-bölme
operatörü değerlendirilebilir. AMA float-bölme bilinçli/belgeli bir karar; şimdi eklemek kapsam dışı.
Bir kullanıcı daha bunu yaşarsa ağırlığı artar. Şimdilik papercut olarak kaydedildi.

### BULGU #3 — Dosya yükleme: `--mode http`'de multipart yok + iki alt-bug

**Katman:** file:: / request::file() / http server / FELSEFE gerilimi
**Ne yaptım:** Göreve dosya eki eklemek istedim (çekirdek CRUD kalıbı). Docs'a baktım:
`request::file("doc", [...])` + `file::store()`. Docs uyarısı (satır 2432): `--mode http`'de
multipart parse EDİLMİYOR, `request::file()` null döner, base64-in-JSON veya FastCGI kullan.
**Ne oldu (ampirik, docs'tan KÖTÜ):**
- **A (doc↔impl):** docs "null döner" diyor; gerçekte **exception fırlatıyor**:
  `request::file() requires multipart/form-data request`. `if ($f == null)` dalına HİÇ ulaşılmaz
  → hata propagate. session::has sınıfının kardeşi (belge bir sözleşme vaat ediyor, kod başkasını yapıyor).
- **B (robustluk):** multipart POST → **HTTP 000 (bağlantı reset)**, temiz 500 değil. Aynı route'a
  base64-JSON gövde → temiz HTTP 500. Fark: multipart gövde işlenirken bağlantı düşüyor (istek
  gövdesi drain edilmeden yanıt/kapatma → TCP RST). curl yanıtı hiç alamıyor.
**FELSEFE gerilimi (asıl mesele):** "tek-exe web (`--mode http`) + framework kurmadan CRUD" iddiası
ediliyor; ama dosya yükleme çekirdek CRUD kalıbı ve TAM O modda standart HTML `<form
enctype=multipart/form-data>` çalışmıyor. Kullanıcı ya JS+base64 (düz HTML form değil) ya FastCGI
(tek-exe sadeliğini bozar) seçmeli. Bu, "PHP gibi kolay dağıt" hikayesinde bir boşluk.
**Karar noktası (ASSOC gibi — tahminle değil, ölçtükten sonra):** üç yol —
  (1) `--mode http`'de multipart parse et (felsefeyle uyumlu, çekirdek http_server özelliği — büyük),
  (2) sınırı dürüst kabul et: request::file() null DÖNSÜN (docs'a uy, A'yı kapat) + docs'u netleştir,
  (3) her hâlde B'yi (bağlantı reset) düzelt: throw eden route gövdeyi drain edip temiz 500 dönmeli.
  B, upload kararından BAĞIMSIZ bir robustluk bug'ı (herhangi bir hata → RST kabul edilemez).

**SON GÜNCELLEME — B GERİ ÇEKİLDİ (test artefaktı), ASIL BULGU: DOCS BAYAT.**
Ölçüm-önce disiplini (analizcinin ısrarı) üç hipotezi de çürüttü:
- **B YOK — TEST ARTEFAKTI.** "5/5 crash" sanılan `curl -F "@f;type=text/plain"` bu Windows/git-bash
  curl'ünde **exit 26** veriyor (curl dosyayı okuyamıyor — `;type=` soneki path-parse'ı bozuyor),
  istek server'a HİÇ ulaşmıyor → HTTP 000. Server crash'i DEĞİL. UTF-8 mojibake'nin ikizi.
  Doğru ölçümler: hand-crafted `/dev/tcp` (200), `--data-binary @multipart_body` (200), ASan
  SESSİZ (bellek hatası yok), server 20+ isteğe dayandı (hang/DoS yok), parser well-formed
  multipart'ı doğru işliyor.
- **Multipart `--mode http`'de ZATEN ÇALIŞIYOR.** `request::file()` gerçek multipart POST'ta dosyayı
  DÖNDÜ (`{"got":"file","size":11}`, 0 fallback). Kod yorumu (http_main.cpp:708-716) de doğruluyor:
  parse eklendi, "README'de 'bilinen sınır' diye yazılmıştı oysa unutulmuş bir daldı".
- **ASIL BULGU (docs↔impl, ters yön): DOCS BAYAT.** Upload bölümü (docs/index.html:2432) hâlâ
  "`--mode http` multipart parse ETMEZ, request::file() null döner, base64/FastCGI kullan" diyor —
  ama parse EDİYOR. Kullanıcı gereksiz yere base64-workaround'a veya FastCGI'ye yönlendiriliyor,
  ya da tek-exe upload'ı çalışmaz sanıp kaçınıyor. docs/ gitignored (website) → kullanıcı düzeltmeli.
- **A ÇÖZÜLDÜ (kalıcı, `8fb8267`):** request::file() non-multipart'ta null döner (docs sözleşmesi).
  Multipart çalışan yolu etkilemez (o content-type multipart → dosyayı döner, kanıtlandı).

**META DERS:** analizcinin "ölçüm-önce, spekülatif güvenlik kodu yazma" ısrarı beni (1) var olmayan
bug'a fix, (2) zaten biten özellik için koca tur, (3) sahte DoS eskalasyonundan kurtardı. Ölçüm
bu turda 4. kez hipotezi çürüttü (mojibake, count-görünürlük, ceil-papercut, şimdi B).

### BULGU #4 — Upload KULLANILABİLİR ama magic-byte MIME düz metni reddediyor

**Katman:** file:: / request::file / MIME doğrulama / DX
**Ne yaptım:** Göreve dosya-ekleme ekranı yazdım (attach.html multipart form + request::file +
file::store + allow_mime + boyut sınırı + hata gösterimi). "Parse ediliyor mu"yu değil
"KULLANILABİLİR mi"yi ölçtüm (analizcinin dediği).
**Happy path ÇALIŞIYOR (PNG, magic-byte):** upload→302 flash, storage/task-1/<sha256>.png yazıldı,
DB url kaydedildi, 📎 listede, 0 fallback. Tam yol (request::file + file::store + web-root koruması
+ DB) `--mode http`'de tam çalışıyor → docs:2432 uyarısı KESİN bayat (tam yol doğrulandı).
**Ne oldu (BULGU):** `allow_mime:["text/plain"]` ile .txt yükleyince → **"File type not allowed:
application/octet-stream"**. Magic-byte dedektörü düz metni (magic-byte'ı YOK) octet-stream görüyor
→ text/plain asla eşleşmiyor. Docs örnekleri hep RESİM kullandığı için (jpeg/png magic-byte'lı) bu
kör-nokta maskeleniyor. **Sonuç:** metin/csv/log gibi magic-byte'sız formatları allow_mime ile izin
vermek İMKANSIZ (tek yol "application/octet-stream" eklemek = her binary'ye izin = güvenlik amacını
bozar).
**Öneri (dil, DEĞERLENDİR):** (a) magic-byte eşleşmezse "tümü yazdırılabilir bayt → text/plain"
heuristiği, VEYA (b) text-tabanlı tipler için beyan edilen Content-Type'a güvenme opsiyonu, VEYA
(c) docs'ta "allow_mime yalnız magic-byte'lı formatlarda çalışır" notu. En azından docs örneği
sadece resim kullanarak yanıltıyor.
**Test-harness notu:** curl `-F "@f;type=..."` bu ortamda exit 26 → upload testleri hand-crafted
multipart + `--data-binary` ile yapıldı (güvenilir). Ayrıca data.db'yi silmeyi unutunca "email
zaten kayıtlı" sessiz register-fail → 302/login (yine kendi state hatam, LOOK değil).

--- (aşağısı geri çekilen B analizinin kaydı — tarihsel) ---
**B kesinleşti (ayrı tura):**
- **A ÇÖZÜLDÜ:** `web_stdlib.cpp` request::file() artık non-multipart istekte throw yerine null
  döner (docs sözleşmesi). Doğrulandı: JSON/urlencoded POST → `file=null`, temiz HTTP 200.
- **B KESİN REPRO (ayrı odaklı tura — analizcinin dediği güvenlik yüzeyi):** İlk sandığım "throw→RST"
  DEĞİL. Deterministik: multipart dosya part'ı **kendi `Content-Type` başlığını** içerince
  (`curl -F "doc=@f;type=text/plain"`) → **5/5 HTTP 000** (worker hang/reset, log yok, process
  ayakta). Başlıksız (`-F doc=@f`) → 5/5 HTTP 200. **KRİTİK: gerçek TARAYICILAR dosya part'ında
  HER ZAMAN Content-Type gönderir** → bu curl edge-case değil, NORMAL tarayıcı upload yolu worker'ı
  düşürüyor (`--mode http`). Muhtemel worker-tükenme DoS'u (N eşzamanlı upload → N hung worker).
  parse_multipart header-loop'u Content-Type satırını yok sayıyor gibi görünüyor ama davranış
  aksini söylüyor — parser gövde-offset veya Windows-özgü. Multipart TURUNDA teşhis: (a) hang mı
  crash mı, (b) worker-tükenme DoS ölç, (c) Linux'ta tekrarlanıyor mu (docker gelince).
