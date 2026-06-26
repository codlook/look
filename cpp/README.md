# LOOK C++ Runtime

LOOK dilinin C++23 runtime implementasyonu.

## Gereksinimler

- C++23 veya daha yeni bir derleyici
- CMake 3.20+
- Windows: Visual Studio 2022 Build Tools
- Linux: GCC 13+ (Docker ile otomatik)

## Derleme

### Windows

```powershell
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
# Çıktı: build/Release/look-fcgi.exe, look-cgi.exe, look.exe
```

### Linux — Docker (önerilen)

```bash
# Ubuntu 24.04
docker build -f Dockerfile.linux-build -t look-linux-builder .
docker run --rm -v "${PWD}:/src" -v "${PWD}/build-linux:/src/build-linux" look-linux-builder \
  sh -c "cd /src/build-linux && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4"

# AlmaLinux 8 (Plesk)
docker build -f Dockerfile.almalinux8-build -t look-almalinux8-builder .
docker run --rm -v "${PWD}:/src" -v "${PWD}/build-almalinux8:/src/build-almalinux8" look-almalinux8-builder \
  sh -c "cd /src/build-almalinux8 && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4"
```

## Kaynak Dosyalar (`src/`)

| Dosya | İçerik |
|-------|--------|
| `lexer.cpp` | Tokenizer |
| `parser.cpp` | Precedence-aware parser, AST |
| `interpreter.cpp` | Tree-walk interpreter, GC |
| `stdlib.cpp` | math::, string::, type::, array:: |
| `web.cpp` | route(), request::, response::, json::, session::, cookie:: |
| `web_stdlib.cpp` | db::, auth::, validator::, html:: |
| `mysql_client.cpp` | MySQL wire protocol — sıfır bağımlılık |
| `sqlite_client.cpp` | SQLite — sqlite3 amalgamation |
| `postgres_client.cpp` | PostgreSQL wire protocol v3 |
| `file_stdlib.cpp` | file:: modülü |
| `date_stdlib.cpp` | date:: modülü |
| `extra_stdlib.cpp` | env(), config() |
| `logger.cpp` | log:: — günlük rotasyon |
| `fcgi_main.cpp` | FastCGI entry — warm start, hot reload |
| `cgi_main.cpp` | CGI entry |
| `main.cpp` | CLI entry |

## Test

```bash
look docs/test/test_lang_deep.lk   # 160 dil testi
look docs/test/test_full.lk        # 177 modül testi
look docs/test/test_db_full.lk     # 204 DB testi (MySQL+SQLite+PostgreSQL)
```

**v0.11.0** | Haziran 2026
