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
