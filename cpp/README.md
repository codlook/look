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
# En kolay — Docker Hub'daki hazır imaj (derleme yok, kaynak gerekmez):
docker run -p 7400:7400 -v "${PWD}/app:/app" codlook/look

# Ya da kaynaktan kendin derle (AlmaLinux 8 tabanlı — Plesk uyumlu):
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
| `extra_stdlib.cpp` | env(), config(), crypto::, mail::, http::, look:: (kod-check), rate limiter |
| `http_client.cpp` | HTTP/HTTPS istemci (Schannel/OpenSSL) — `http::stream` canlı akış dahil |
| `mysql_client.cpp` | MySQL/MariaDB wire protocol istemcisi — `caching_sha2_password` (MySQL 8+/8.4/9.x) + `mysql_native_password`, sunucunun bildirdiği eklentiyle anlaşır |
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

## Yeni

- **`http::stream($method,$url,$body,$headers,$callback[,$opts])`** — canlı streaming HTTP istemcisi. Body parçaları (chunked-decoded) geldikçe `$callback($chunk)` çağrılır. SSE / LLM token akışı için. `route("SSE")` + `channel()` ile eşleşir.
- **`look::check($source)`** — LOOK kaynağını in-process doğrular (çalıştırmadan): `["ok"=>bool,"line"=>int,"col"=>int,"msg"=>string]`. `lk --check` ile aynı motor. LLM/editör araçları için.

**v1.3.0** | Temmuz 2026
