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
# Production image (AlmaLinux 8 tabanlı — Plesk uyumlu)
# Repo kökünden çalıştır:
docker build -f cpp/docker/Dockerfile.production -t look-prod .
docker run -p 7400:7400 --env-file .env look-prod

# Sadece derleme (binary extract):
docker create --name look-tmp look-prod
docker cp look-tmp:/usr/local/bin/lk       ./lk
docker cp look-tmp:/usr/local/bin/lk-fcgi  ./lk-fcgi
docker cp look-tmp:/usr/local/bin/lk-cgi   ./lk-cgi
docker rm look-tmp
```

## Kaynak Dosyalar (`src/`)

| Dosya | İçerik |
|-------|--------|
| `lexer.cpp` | Tokenizer |
| `parser.cpp` | Precedence-aware parser, AST |
| `interpreter.cpp` | Tree-walk interpreter, GC, core modül init |
| `stdlib.cpp` | math::, string::, type::, array::, parallel:: |
| `web.cpp` | route(), request::, response::, json::, session::, cookie:: |
| `web_stdlib.cpp` | db::, auth::, validator::, html::, template::, cache::, queue::, jobs:: |
| `extra_stdlib.cpp` | env(), config(), crypto::, mail::, http::, rate limiter |
| `mysql_client.cpp` | MySQL/MariaDB wire protocol — sıfır bağımlılık |
| `sqlite_client.cpp` | SQLite — sqlite3 amalgamation |
| `postgres_client.cpp` | PostgreSQL wire protocol v3 |
| `smtp_server.cpp` | Gömülü SMTP — event loop, DKIM, SPF, DB auth (PBKDF2), per-user Maildir teslim + ortak `mail_user_auth` |
| `imap_server.cpp` | Gömülü IMAP4rev1 (RFC 3501/2177) — SELECT/FETCH/STORE/EXPUNGE/APPEND/SEARCH/IDLE, STARTTLS+IMAPS, thread-per-conn |
| `file_stdlib.cpp` | file:: modülü |
| `date_stdlib.cpp` | date:: modülü |
| `logger.cpp` | log:: — günlük rotasyon |
| `http_main.cpp` | HTTP entry — epoll/IOCP, WebSocket, SSE, SMTP başlatma |
| `fcgi_main.cpp` | FastCGI entry — warm start, hot reload |
| `cgi_main.cpp` | CGI entry |
| `main.cpp` | CLI / REPL entry |

## Test

```bash
look docs/test/test_lang_deep.lk   # 160 dil testi
look docs/test/test_full.lk        # 177 modül testi
look docs/test/test_db_full.lk     # 204 DB testi (MySQL+SQLite+PostgreSQL)
```

**v1.3.0** | Temmuz 2026
