# LOOK Language — VSCode Eklentisi

[LOOK](https://codlook.com) dilinde (`.lk`) kod yazan herkes için tam editör desteği.
Sıfır bağımlılık, saf JavaScript — derleme gerekmez.

## Özellikler

- **Canlı hata denetimi** — yazarken `lk --check` çalışır; **parse hataları** ve **tanımsız fonksiyon çağrıları** anında kırmızı altı çizili gösterilir. (Kaydetmene gerek yok.)
- **Sözdizimi vurgusu** — anahtar kelimeler, 235 built-in `modül::fonksiyon`, `$değişken`, string interpolation `{$x}`, `//` `/* */` `#` yorumlar, backtick ham stringler.
- **Otomatik tamamlama** — 31 modülün 235 fonksiyonu (gerçek imzalarla), keyword'ler, global'ler ve **dosyandaki kendi değişken/fonksiyonların**. `math::` yazınca o modülün fonksiyonları listelenir.
- **Hover** — herhangi bir built-in'in üzerine gelince imzası + açıklaması.
- **İmza yardımı** — `(` yazınca parametre ipuçları, aktif parametre vurgulu.
- **Snippet'ler** — `route`, `fn`, `foreach`, `try`, `switch`, `struct`, `dbquery`, `httpget`, `session` ve daha fazlası.
- **Komutlar** — sağ tık veya kısayolla:
  - **LOOK: Dosyayı Çalıştır** (`Ctrl+F5`) → `lk dosya.lk`
  - **LOOK: Web Sunucusu Başlat** → `lk-fcgi --mode http --port 7400`
  - **LOOK: REPL Aç** → `lk repl`

## Kurulum

Marketplace'ten **"LOOK Language"** ara, ya da `.vsix` ile:

```bash
code --install-extension look-lang-2.0.1.vsix
```

## Ayarlar

| Ayar | Varsayılan | Açıklama |
|------|-----------|----------|
| `look.binaryPath` | `lk` | `lk` çalıştırılabilir dosyasının yolu (PATH'te değilse tam yol ver) |
| `look.fcgiPath` | `lk-fcgi` | Web sunucusu binary yolu |
| `look.serve.port` | `7400` | "Web Sunucusu Başlat" portu |
| `look.diagnostics.enable` | `true` | Canlı hata denetimini aç/kapat |
| `look.diagnostics.run` | `onType` | Denetim: `onType` (yazarken) / `onSave` (kaydederken) |

> **Canlı denetim için `lk` gerekli.** PATH'te yoksa `look.binaryPath` ayarına tam yolu girin
> (ör. `C:\look\lk.exe` veya `/usr/local/bin/lk`). LOOK binary'sini [codlook.com](https://codlook.com)
> ya da `docker run codlook/look` ile edinebilirsin.

## Lisans

Apache-2.0 · Codlook
