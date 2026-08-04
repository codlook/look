# LOOK Plesk Eklentisi — Kurulum Rehberi

> AlmaLinux 8.10 + Plesk Obsidian üzerinde doğrulanmıştır.
> Ayrıntılı referans: [plesk-extension.md](plesk-extension.md).

## Gereksinimler

- Plesk Obsidian 18.0+
- systemd Linux (AlmaLinux/RHEL/Rocky/CloudLinux · Ubuntu/Debian Plesk)
- Root erişimi (SSH veya Plesk → Tools & Settings → Terminal)

---

## Desteklenen sistemler

Gömülü binary **tek portatif statik build** (OpenSSL + libstdc++ içeride; yalnız glibc + libz
dinamik). **glibc ≥ 2.28** gerektirir — destekte olan tüm dağıtımlarda çalışır:

| Dağıtım | Durum |
|---|---|
| AlmaLinux / Rocky / RHEL / CloudLinux **8, 9, 10+** | ✅ Destekli |
| Ubuntu **18.10 → 24.04+** | ✅ Destekli |
| Debian **10 (Buster) ve üstü** | ✅ Destekli |
| **CentOS 7 / RHEL 7** (glibc 2.17, EOL 2024-06) | ❌ Desteklenmiyor — **AlmaLinux 8**'e geçin (ücretsiz, birebir halef) |

**Tasarım gereği ileriye-dönük uyumlu.** Binary *eski* glibc'de (2.28) derlendiği ve OpenSSL'i
statik gömdüğü için **gelecek** dağıtımlarda kendiliğinden çalışır:
- **Yeni glibc** (2.38, 2.40…): glibc ileri-uyumlu → eski-glibc binary yeni glibc'de çalışır.
  AlmaLinux 10, Ubuntu 26 için yeni build gerekmez.
- **Yeni OpenSSL** (3.x, 4.x): binary kendi OpenSSL'ini taşır → sistemdeki sürüm önemsiz.

Yani yeni distro sürümleri **yeni binary gerektirmez.** İki uzun-vadeli bakım notu (per-distro değil):
1. **En eski destekli glibc'de** (AlmaLinux 8) derlemeye devam et — tabanı asla yükseltme.
2. Gömülü OpenSSL'i ara sıra güvenlik için tazele (1.1.1 → 3.x).

---

## Adım 1 — Kur

Sunucuda root olarak:

```bash
plesk bin extension --uninstall look-lang 2>/dev/null
wget -O /tmp/look-lang-plesk-1.0.0.zip "https://github.com/codlook/look/releases/download/v1.0/look-lang-plesk-1.0.0.zip"
plesk bin extension --install /tmp/look-lang-plesk-1.0.0.zip
```

Beklenen çıktı: `The extension was successfully installed.`

Alternatif — Plesk UI: **Extensions → Upload Extension → `look-lang-plesk-1.0.0.zip`**.

### ⚠️ Kurulum sonrası — tek komut (YENİ sunucularda ZORUNLU)

Plesk, eklentinin `post-install` hook'unu **güvenilir çalıştırmıyor** (doğrulandı:
`plesk bin extension --install` CLI/sideload kurulumu hook'u hiç koşmuyor). Bu yüzden
kurulum **motoru (`/opt/look`) + sudoers kuralını otomatik ayarlamaz.** Kurulumdan
**hemen sonra, root olarak bir kez** şunu çalıştır — portatif binary'yi `/opt/look`'a
kurar **ve** sudoers kuralını yazar (kur/kaldır döngülerinde kalıcı):

```bash
plesk php /usr/local/psa/admin/plib/modules/look-lang/scripts/post-install.php
```

**Bu komut çalıştırılmazsa** domain enable ederken şu hatayı alırsın:

```
sudo: a terminal is required to read the password ...
sudo: a password is required
```

Sebep: sudoers kuralı yoksa panelin `sudo systemctl …` çağrısı parola ister, ama web
bağlamında terminal yoktur. Komut `/opt/look/lk-fcgi` + `/etc/sudoers.d/look-lang`
oluşturur → enable sorunsuz çalışır. (Aynı işi doğrudan da yapabilirsin:
`bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/setup.sh`.)

---

## Adım 2 — Paneli Aç

**Plesk → Extensions → LOOK Language**, veya `https://<sunucu>:8443/modules/look-lang/`.

Çalışma alanı sol kenar çubuğu: **Dashboard · Applications · Logs · Documentation**.

---

## Adım 3 — Domain Ekle

1. **Applications** sekmesi → **Add New Domain** formu.
2. Domain seç (Plesk'teki domainler listelenir) — script yolu ve boş port otomatik dolar.
3. Mod seç: **FastCGI** (üretim önerisi) veya **HTTP**.
4. **Add & Start** → birkaç saniye içinde listede **Running** görünür.

---

## Adım 4 — Kodu Yaz

Uygulama tablosunda **Edit Code** → `index.lk`'i düzenle → **Save & Redeploy**
(dosya yazılır + servis yeniden başlatılır):

```lk
route("GET", "/", fn() => response::json(["ok" => true, "app" => "LOOK"]))
route("GET", "/ping", fn() => response::json(["status" => "ok"]))
route("404", fn() => response::error(404, "Not found"))
```

---

## Panel Özellikleri

| Özellik | Açıklama |
|---|---|
| **Dashboard** | Canlı CPU/Memory/Disk/Uptime + mini grafikler |
| **Add New Domain** | systemd servisi + Apache proxy config oluşturur |
| **Edit Code** | Tarayıcı-içi `index.lk` editörü → Save & Redeploy |
| **Monitor** | Canlı per-domain: CPU/RSS/PID/bağlantı/uptime + log kuyruğu |
| **View Logs** | `journalctl` görüntüleyici (renkli + kopyala) |
| **Configure / Restart / Stop / Remove** | Servis yönetimi (Actions menüsü) |

---

## Sorun Giderme

| Belirti | Çözüm |
|---|---|
| `sudo: .../enable.sh: command not found` | Adım 1'deki sudoers bloğunu root ile çalıştır |
| Domain eklendi ama listede yok | `ls -l /usr/local/psa/var/modules/look-lang/domains.json` (psaadm sahipli olmalı) |
| Site 502/403 | `systemctl status look-<domain>` + `journalctl -u look-<domain> -n 50` |

Ayrıntı: [plesk-extension.md](plesk-extension.md).
