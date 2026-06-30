# LOOK Plesk Extension — Kurulum Rehberi

> **Gercek test sonuclarina gore yazilmistir.**  
> AlmaLinux 8.10 + Plesk Obsidian 18.0.78 uzerinde dogrulanmistir.

---

## Gereksinimler

- Plesk Obsidian 18.0+
- AlmaLinux 8 / Rocky Linux 8 veya Ubuntu 22.04/24.04
- Root erisimi (Plesk Terminal veya SSH)
- systemd + nginx (Plesk kurulumunda standart gelir)

---

## Adim 1 — Eklentiyi Kur

Plesk Panel → **Server Management → Terminal** acin ve su komutlari calistirin:

```bash
rm -rf look-lang && \
wget -L -O look-lang.zip "https://github.com/codlook/look/releases/download/v1.2.0/look-lang-plesk-1.2.0.zip" && \
unzip -o look-lang.zip -d look-lang && \
sed -i "s/\r//" look-lang/setup.sh && \
plesk bin extension --uninstall look-lang 2>/dev/null; \
bash look-lang/setup.sh
```

Beklenen cikti:

```
The extension was successfully installed.
```

> **Not:** `plesk bin extension --uninstall` adimi eklenti zaten kuruluysa hata verse bile kurulum devam eder.  
> Alternatif olarak Plesk panelinde **Extensions → Upload Extension** butonu varsa ZIP'i direkt yukleyebilirsiniz.

---

## Adim 2 — LOOK Language Eklentisini Ac

Plesk Panel → **Extensions** → **My Extensions** → **LOOK Language → Ac**

Karsina cikan ekran: **"LOOK Kurulum Gerekli"** — bu normal, bir sonraki adima gec.

---

## Adim 3 — Bootstrap Komutunu Calistir (1 kez)

Ekrandaki komutu **Kopyala** butonuyla kopyalay in.  
Plesk Terminal'e donup yapistirin ve Enter'a basin:

```bash
bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/install.sh
```

Beklenen cikti:

```
[LOOK] Kurulum basliyor...
[LOOK] sudoers yazildi: /etc/sudoers.d/look-lang
[LOOK] Script izinleri verildi
[LOOK] Bundled binary bulundu: .../bin
[LOOK] Binary kopyalandi: /opt/look/look-fcgi
[LOOK] Kurulum tamamlandi
```

**Bu komut ne yapar?**

| Islem | Aciklama |
|-------|----------|
| `/opt/look/look-fcgi` kopyalar | ZIP ici bundled binary. Yoksa GitHub'dan indirir. |
| `/etc/sudoers.d/look-lang` yazar | psaadm kullanicisi domain ekle/kaldir yapabilsin diye |
| Script izinleri verir | `chmod +x enable.sh disable.sh status.sh` |
| Log ve conf dizinleri olusturur | `/var/log/look/`, `/opt/look/conf/` |

**Bu komutu sadece 1 kez calistirman yeterli.**

---

## Adim 4 — Sayfayi Yenile

Bootstrap ekranindaki **"Sayfayi Yenile"** butonuna bas.  
Ana ekran acilir: 0 domain, + Domain Ekle butonu.

---

## Adim 5 — Domain Ekle

1. **+ Domain Ekle** butonuna tikla
2. Dropdown'dan domain sec (Plesk'teki aktif domainler otomatik listelenir)
3. Worker sayisi sec (varsayilan 8, production icin 8-32 arasi)
4. Mod sec:
   - **HTTP** — WebSocket ve SSE destekli, yuksek performans
   - **FastCGI** — Apache/nginx standard, klasik web uygulamasi
5. **Ekle ve Baslat** → ~5 saniye bekle

**Basarili olursa:** domain listesinde "active" olarak gorunur.

---

## Adim 6 — LOOK Uygulamani Yukle

Domain'in httpdocs dizinine `index.lk` dosyani yaz:

```bash
# SSH ile veya Plesk File Manager ile
scp index.lk root@sunucu_ip:/var/www/vhosts/domain.com/httpdocs/
```

Minimum `index.lk`:

```lk
route("GET", "/", function() {
    print("Merhaba LOOK!");
});
```

look-fcgi mtime degisimini algilar, otomatik hot reload yapar — servis restart gerekmez.

---

## Extension Yonetim Ekrani

| Islem | Aciklama |
|-------|----------|
| **+ Domain Ekle** | Yeni domain icin look-fcgi servisi olusturur |
| **Yonet** | Domain ayarlari, log goruntuleme, restart |
| **Durdur / Baslat** | systemctl stop/start |
| **Yeniden Baslat** | systemctl restart (binary guncelleme sonrasi) |
| **Kaldir** | Servisi durdurur, nginx conf siler, systemd unit kaldirir |

---

## Sorun Giderme

### "LOOK Kurulum Gerekli" ekrani kapanmiyor

Bootstrap komutunu calistirdin mi?

```bash
bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/install.sh
```

Komutu calistirdiktan sonra sayfayi yenile. Hala gozukmuyorsa:

```bash
ls -la /opt/look/look-fcgi
```

Dosya varsa Plesk oturumunu kapat → tekrar giris yap → Extensions → LOOK Language.

---

### Domain eklendi ama listede gozukmuyor

Yetki sorunu. SSH ile su komutu calistir:

```bash
chown psaadm:psaadm /opt/look/conf
chmod 755 /opt/look/conf
```

Sonra tekrar domain ekle.

---

### Domain servisi "failed" / "inactive"

```bash
# Servis durumuna bak
systemctl status look-domain-com

# Loglar
journalctl -u look-domain-com -n 50 --no-pager

# index.lk var mi?
ls /var/www/vhosts/domain.com/httpdocs/index.lk
```

Genellikle neden: `index.lk` dosyasi yok veya syntax hatasi var.

---

## Bilinen Sinirlamalar

| Sinir | Aciklama |
|-------|----------|
| Bootstrap 1 kez gerekli | Plesk bu sunucu konfigurasyonunda extension install-script'i otomatik calistirmiyor. Bu nedenle ilk kurulumda 1 terminal komutu gerekiyor. |
| Root erisimi | Bootstrap komutu root olarak calistirilmali. Plesk Terminal veya SSH yeterli. |
| Guncelleme | Yeni ZIP yuklendiginde bootstrap yeniden gerekmez; binary ve sudoers korunur. |
| Upload butonu | Bazi Plesk lisanslarinda "Upload Extension" butonu gozukmez. Bu durumda `plesk bin extension --install-url` komutuyla Terminal'den kurulum yapilir. |

---

## Tam Kurulum Ozeti

```
1. Plesk Terminal'i ac:
   Server Management → Terminal

2. Eklentiyi indir ve kur:
   rm -rf look-lang && \
   wget -L -O look-lang.zip "https://github.com/codlook/look/releases/download/v1.2.0/look-lang-plesk-1.2.0.zip" && \
   unzip -o look-lang.zip -d look-lang && \
   sed -i "s/\r//" look-lang/setup.sh && \
   plesk bin extension --uninstall look-lang 2>/dev/null; \
   bash look-lang/setup.sh

3. Extensions → LOOK Language → Ac
   "LOOK Kurulum Gerekli" ekrani gelir

4. Sayfadaki komutu kopyala → Terminal'e yapistir → Enter
   bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/install.sh

5. "Sayfayi Yenile" → Ana ekran

6. + Domain Ekle → Ekle ve Baslat

7. index.lk yukle → https://domain.com calisir
```

**Toplam sure: ~3 dakika.**  
**Terminal: Tek komut blogu + 1 bootstrap komutu.**
