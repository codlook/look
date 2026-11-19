# LOOK — security test suite

**Re-runnable** test files produced during an external audit.
Reference environment: Ubuntu 24.04 · GCC 13.3 · OpenSSL 3.0.13 · C++23.

```bash
cd cpp
cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLOOK_SANITIZE=address,undefined
cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLOOK_SANITIZE=thread
cmake --build build-asan --target look -j"$(nproc)"
cmake --build build-tsan --target look -j"$(nproc)"
```

## `tsan/` — race tests (this directory)

Run with `build-tsan/lk` (single engine: VM).

Originally split into two groups so that **CI would start green from day one**. Every test now lives in
`enforced/`; the former `known_fail/` group has been removed.

### `enforced/` — must be green (CI goes red if one breaks)
| File | What it tests | Expected |
|---|---|---|
| `t2_cache.lk` | `cache::` 8×200 ops | clean ✅ |
| `t3_queue.lk` | `queue::` 8×200 ops | clean ✅ |
| `t4_sqlite_connect.lk` | Each thread calls `db::connect` on its own SQLite file | **`SQLITE_THREADSAFE=2` regression** — 0 races |
| `t5_shared_conn.lk` | One shared connection + pool | clean ✅ |

The directory now also holds `t1b_contended.lk`, `t6_closure.lk` (both formerly in `known_fail/`, which
covered the `parallel()` VM array capture race and the `shared_ptr<void>` type-confusion race),
`t8_nested_closure_capture.lk` and `t9_struct_field_cache.lk`. CI runs every `tests/tsan/enforced/*.lk`.

> **⚠️ Measurement environment:** SQLite POSIX locks behave unreliably on Docker/Windows
> **overlayfs/bind-mounts**; run lock tests on **ext4** (e.g. a GitHub Actions runner).
> On overlayfs the lock behavior of `t4`/`t5` can give both false positives and false negatives.

## `../repro/`

| File | Contents |
|---|---|
| `b01_threshold.sh` | Scans the evaluation recursion limit across three paths (`--check` / VM / interpreter) |
| `sqli_bind_params.lk` | 12 SQLi vectors + `bind_params` state-machine blind spots (backtick, `#`) + NUL truncation |
| `traversal_json_crypto.lk` | Path traversal (4 vectors), int/JSON boundary behavior, `html::escape`, crypto vectors |

Run the `repro/*.lk` files with `build-asan/lk`.

## CI

`.github/workflows/sanitizers.yml` runs these tests on every push/PR (the ASan+UBSan job runs the
regression suite + B-01, the TSan job runs `tsan/*.lk`). The existing `ci.yml` only builds Release, so it
could not see this class of bugs.
