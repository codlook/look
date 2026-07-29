# LOOK — güvenlik test paketi

Dış denetimde üretilen, **yeniden çalıştırılabilir** test dosyaları.
Referans ortam: Ubuntu 24.04 · GCC 13.3 · OpenSSL 3.0.13 · C++23.

```bash
cd cpp
cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLOOK_SANITIZE=address,undefined
cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLOOK_SANITIZE=thread
cmake --build build-asan --target look -j"$(nproc)"
cmake --build build-tsan --target look -j"$(nproc)"
```

## `tsan/` — yarış testleri (bu dizin)

`build-tsan/lk` ile çalıştırın. Her testi **iki motorda da** koşun
(`LOOK_CLI_VM=0` ile ve olmadan) — bulguların biri yalnızca VM yolunda çıkıyor.

İki gruba ayrıldı — **CI ilk günden yeşil başlasın diye:**

### `enforced/` — yeşil olmalı (kırılırsa CI kırmızı)
| Dosya | Ne test eder | Beklenen |
|---|---|---|
| `t2_cache.lk` | `cache::` 8×200 op | temiz ✅ |
| `t3_queue.lk` | `queue::` 8×200 op | temiz ✅ |
| `t4_sqlite_connect.lk` | Her thread kendi SQLite dosyasına `db::connect` | **`SQLITE_THREADSAFE=2` regresyonu** — 0 yarış |
| `t5_shared_conn.lk` | Tek paylaşılan bağlantı + havuz | temiz ✅ |

### `known_fail/` — bilinen açık (raporlanır, CI'ı kırmaz)
| Dosya | Ne test eder | Durum |
|---|---|---|
| `t1_shared_array.lk` | `parallel()` VM array capture yarışı | R-02 — mimari (S1 masası) |
| `t6_closure.lk` | `shared_ptr<void>` type-confusion yarışı | R-03 — mimari (S1 masası) |

Her düzeltme = bir testi `known_fail/`'dan `enforced/`'a **terfi** ettirmek. İlerleme ölçülebilir.

> **⚠️ Ölçüm ortamı:** SQLite POSIX kilitleri Docker/Windows **overlayfs/bind-mount**'ta
> güvenilmez çalışır — kilit testlerini **ext4** (ör. GitHub Actions runner) üzerinde koşun.
> `t4`/`t5`'in kilit davranışı overlayfs'te hem yanlış-pozitif hem yanlış-negatif verebilir.

## `../repro/`

| Dosya | İçerik |
|---|---|
| `b01_threshold.sh` | Değerlendirme özyineleme sınırını üç yolda tarar (`--check` / VM / interpreter) |
| `sqli_bind_params.lk` | 12 SQLi vektörü + `bind_params` state-machine kör noktaları (backtick, `#`) + NUL truncation |
| `traversal_json_crypto.lk` | Path traversal (4 vektör), int/JSON sınır davranışı, `html::escape`, kripto vektörleri |

`repro/*.lk` dosyalarını `build-asan/lk` ile çalıştırın.

## CI

`.github/workflows/sanitizers.yml` bu testleri her push/PR'da koşar (ASan+UBSan job'ı
regression + B-01'i, TSan job'ı `tsan/*.lk`'yi). Mevcut `ci.yml` yalnız Release derlediği
için bu sınıftaki hataları göremiyordu.
