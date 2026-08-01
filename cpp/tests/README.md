# LOOK Testleri — Kalıcı Kurallar

## ⚠️ KURAL: Pozitif kontrol olmadan assertion eklenmez

**Kırmızı görülmeden yeşile güvenilmez.** Bir test/assertion, onun yakaladığı bug'ın
_var olduğu_ bir durumda **FAIL verdiği gösterilmeden** eklenmez. Yeşil geçen ama hiç
kırmızı görülmemiş bir assertion, aslında hiçbir şey test etmiyor olabilir.

Bu desen bu projede **en az üç kez** aynı biçimde ısırdı:

| Ne zaman | Sahte yeşil |
|----------|-------------|
| `t1` (TSan yarış) | Her thread AYRI indekse yazıyordu → hiç yarışmıyordu; CI yeşildi ama bir şey test etmiyordu. Deterministik `t1b` (hepsi aynı elemana + kapı) ile düzeltildi. |
| `t4=24` / ASLR | Yerel libtsan sahte sayı üretti; "0" da sahteydi. CI (ext4) otoriter. |
| `release_gate.lk` non-finite/float | Assertion'lar YAMASIZ binary'de de geçiyordu (yanlış sebeple throw / eski yol da doğruydu) → sahte gate. Ayırt edici mesaj-kontrolüne çevrildi / çıkarıldı. |

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
