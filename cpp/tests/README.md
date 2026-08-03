# LOOK Testleri — Kalıcı Kurallar

## ⚠️ KURAL: Pozitif kontrol olmadan assertion eklenmez

**Kırmızı görülmeden yeşile güvenilmez.** Bir test/assertion, onun yakaladığı bug'ın
_var olduğu_ bir durumda **FAIL verdiği gösterilmeden** eklenmez. Yeşil geçen ama hiç
kırmızı görülmemiş bir assertion, aslında hiçbir şey test etmiyor olabilir.

Bu desen bu projede **en az dört kez** aynı biçimde ısırdı:

| Ne zaman | Sahte yeşil |
|----------|-------------|
| `t1` (TSan yarış) | Her thread AYRI indekse yazıyordu → hiç yarışmıyordu; CI yeşildi ama bir şey test etmiyordu. Deterministik `t1b` (hepsi aynı elemana + kapı) ile düzeltildi. |
| `t4=24` / ASLR | Yerel libtsan sahte sayı üretti; "0" da sahteydi. CI (ext4) otoriter. |
| `release_gate.lk` non-finite/float | Assertion'lar YAMASIZ binary'de de geçiyordu (yanlış sebeple throw / eski yol da doğruydu) → sahte gate. Ayırt edici mesaj-kontrolüne çevrildi / çıkarıldı. |
| `crypto_vectors.lk` (2026-08-03) | İki KATMAN: (1) `exit(1)` yoktu → bozuk vektörde bile exit 0, CI yeşil (dış-katkı hali). (2) `exit(1)` eklendikten SONRA bile bir dizi boşaltılınca `$run` düşüp "PASS: 13/13" diyordu → **boş-yeşil**. `EXPECTED_VECTORS` sabitiyle kapandı. |

## ⚠️ KURAL 2: Guard eklerken, GUARD'IN KENDİSİ için pozitif kontrol yaz

Bir guard "başarısızlıkta gürültülü" olmak yetmez — "hiç koşmadığında da gürültülü" olmalı.
`exit(1)` fail-loud'u verir ama **boş-yeşil**'i (dizi boşaldı / `foreach` kırıldı / hiç vektör
koşmadı → "PASS: 2/2") vermez. Zincirin her katmanı bir öncekinin _çıktısını_ denetler; kimse
guard'ın **koşturulmayan yollarını** denetlemez. Kural: yeni guard eklerken **iki yönde** kırmızı
göster — (a) bir değeri boz → kırmızı; (b) test dizisini boşalt → kırmızı. Sayaç-sabiti
(`EXPECTED_VECTORS`) ikincisini kapatır: vektör eklerken sayacı bilerek artırmak zorunda kalırsın.

**SINIR (sonsuz gerileme yok): guard'lar KAZAYA karşıdır, KASDA değil.** "İki yönde pozitif kontrol"
evet; "guard'ın guard'ının guard'ı" HAYIR — getiri sıfıra düşer. Yakalanmayacak ve yakalanmaya
ÇALIŞILMAYACAK sınıflar: (a) beklenen değerin kendisi yanlış olabilir → zaten FAIL üretir (güvenli
yön; HMAC case-6'da 131/134 bir kez oldu, yakalandı); (b) biri beklenen değeri buggy çıktıya uydurabilir
veya sayaç+vektörü birlikte silebilir → KASIT, guard değil SÜREÇ (kod inceleme/commit) meselesi.
`EXPECTED_VECTORS` doğru durma noktası: kazara boşalmayı yakalar, kasıtlı sabotajı yakalamaz — ve
yakalamamalı. **Dördüncü tuzak arayışını burada bitir.**

**Pratik:** Yeni bir assertion eklerken, onu yamasız (bug'lı) bir durumda koştur:
- Fix'i geçici geri al → test KIRMIZI olmalı → fix'i koy → YEŞİL olmalı (pozitif kontrol).
- `release_gate.lk` için: **eski bir binary sakla** (ör. bu turun DB işi öncesi). Kapıya her
  yeni assertion, o eski binary'de KIRMIZI verdiği gösterilmeden eklenmez — kapının regresyon testi odur.
- Daha da güçlüsü: assertion, yalnız yamalı kodun ürettiği bir imzayı (ör. yeni bir hata mesajı
  substring'i) kontrol etsin → _inşa gereği_ hiçbir eski binary geçemez.

## release_gate.lk — yayın kapısı

Paketlenen HER binary'nin fix'leri gerçekten içerdiğini KANITLAR. Pakete konmaz (repo'da durur):
zip'i aç → **repodaki** `release_gate.lk`'yi **çıkarılan** `lk` ile koş. Kapıda test EDİLEMEYEN
fix'ler (canlı DB / FCGI env gerektirenler) için tek güvence **build damgası** (`lk --version` =
beklenen git-sha; 4 artefaktın dördünde de gözle gör — damga incremental build'de yalan söyleyebilir,
release build FRESH olmalı).

## Diğer

- `regression_all.lk` — genel regresyon (her motor değişikliğinden sonra).
- `differential_test.sh` / `parallel_db_test.sh` — 3 motor × kategori paritesi + parallel/DB sızıntısı.
- `tsan/enforced/` — yarış testleri ZORUNLU (kırılırsa CI kırmızı). `known_fail/` KALDIRILDI
  (fail-loud'un tersi "başarısızlık kabul bölgesi"ydi).
- `fuzz/` — libFuzzer harness'ları (clang; CI'da her push 30s). Corpus gitignore'da.
- `repro/` — tekil bug pin'leri.
