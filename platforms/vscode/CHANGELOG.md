# Değişiklik Günlüğü

## 2.0.1

- Sözdizimi güncellendi: **noktalı virgül kaldırıldı** (LOOK'ta `;` yok); `for` boşlukla ayrılır (`for ($i = 0 $i < 10 $i++)`), `switch` Go-tipi (`break` yok). Gramere `++`/`--` eklendi.
- Katalog tamamlandı: `ws::`, `sse::`, `timer::`, `jobs::run/worker` fonksiyonları eklendi (kaynaktan doğrulandı) — 235 → 248 fonksiyon; grameredeki modül vurgusuna da eklendi.
- Hata düzeltmeleri: hayali `request::query` → `request::get`; WS snippet `$conn->on` → gerçek `ws::on`/`ws::send`; SSE snippet `sse::send`. Docs imzalarındaki HTML kalıntısı temizlendi.
- Yeni marka logosu (look-icon).

## 2.0.0

- **Canlı hata denetimi** `lk --check` üzerine yeniden kuruldu: parse hataları + tanımsız fonksiyon çağrıları, yazarken (onType, debounce) veya kaydederken.
- **IntelliSense** kaynaktan üretildi: 31 modül / 235 built-in fonksiyon, 211'i gerçek imzalı (hover + tamamlama + imza yardımı).
- `modül::` yazınca bağlama duyarlı tamamlama; dosyadaki `$değişken` ve `function` sembolleri.
- Sözdizimi grameri güncellendi: string interpolation `{$x}`, backtick ham string, `#` yorum, `fn` lambda, `iota`/`const`/`struct`.
- Komutlar: Çalıştır (`Ctrl+F5`), Web Sunucusu Başlat, REPL.
- Ayarlar: `look.binaryPath`, `look.fcgiPath`, `look.serve.port`, `look.diagnostics.*`.
- Yeni ikon (LOOK göz motifi).
