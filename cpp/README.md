# LOOK C++ Runtime

The C++23 runtime implementation of the LOOK language. LOOK runs on a single engine: source is
compiled to bytecode and executed by the VM (`compiler.cpp` + `vm.cpp`), on the command line and
on the web alike.

## Requirements

- A C++23 (or newer) compiler
- CMake 3.20+
- Windows: Visual Studio 2022 Build Tools
- Linux: GCC 12+ (the Docker build uses `gcc-toolset-12` on AlmaLinux 8)

## Building

### Windows

```powershell
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
# Output: build/Release/lk-fcgi.exe, lk-cgi.exe, lk.exe
```

### Linux — Docker (recommended)

```bash
# Easiest — the ready-made image on Docker Hub (no build, no source needed):
docker run -p 7400:7400 -v "${PWD}/app:/app" codlook/look

# Or build it yourself from source (AlmaLinux 8 based — Plesk compatible):
docker build -f cpp/docker/Dockerfile.production -t look-prod .
docker run -p 7400:7400 --env-file .env look-prod

# Build only (extract the binaries):
docker create --name look-tmp look-prod
docker cp look-tmp:/usr/local/bin/lk       ./lk
docker cp look-tmp:/usr/local/bin/lk-fcgi  ./lk-fcgi
docker cp look-tmp:/usr/local/bin/lk-cgi   ./lk-cgi
docker rm look-tmp
```

## Source Files (`src/`)

| File | Contents |
|------|----------|
| `lexer.cpp` | Tokenizer |
| `parser.cpp` | Precedence-aware parser, AST |
| `compiler.cpp` | Bytecode compiler (AST → bytecode), `use` resolution (modules and file includes) |
| `vm.cpp` | Bytecode VM — the single execution engine |
| `builtins.cpp` | The single source of the VM builtin table |
| `runtime_host.cpp` | Module host: registers the standard library modules |
| `cli_runtime.cpp` | Command-line VM environment (shared by `lk` and `lk test`) |
| `value.cpp` | `Value` type (copy-on-write arrays, maps, structs) |
| `stdlib.cpp` | math::, string::, type::, html::, log::, parallel::, error:: |
| `extra_stdlib.cpp` | auth::, validator::, array::, crypto::, runtime::, look:: (code check) |
| `web.cpp` | Request context: CGI environment, POST body, multipart parsing, status |
| `web_stdlib.cpp` | request::, response::, json::, cookie::, csrf::, session::, route::, db:: |
| `http_stdlib.cpp` | http:: module binding |
| `cache_stdlib.cpp` | cache:: (in-memory, TTL) |
| `queue_stdlib.cpp` | queue:: (named, thread-safe FIFO) |
| `jobs_stdlib.cpp` | jobs:: (SQLite-backed persistent job queue) |
| `template_stdlib.cpp` | template:: |
| `mail_stdlib.cpp` | mail:: (HTTP-API providers and SMTP) |
| `timer_stdlib.cpp` | timer:: |
| `file_stdlib.cpp` | file:: module |
| `date_stdlib.cpp` | date:: module |
| `logger.cpp` | log:: — daily rotation |
| `http_client.cpp` | HTTP/HTTPS client (Schannel/OpenSSL), including `http::stream` live streaming |
| `mysql_client.cpp` | MySQL/MariaDB wire protocol client — `caching_sha2_password` (MySQL 8+/8.4/9.x) + `mysql_native_password`, negotiates the plugin the server announces |
| `sqlite_client.cpp` | SQLite — sqlite3 amalgamation |
| `postgres_client.cpp` | PostgreSQL wire protocol v3 |
| `resp_client.cpp` | Redis (RESP2) client |
| `http_server.cpp` | HTTP/1.1 server (request parsing, keep-alive) |
| `websocket.cpp` | WebSocket framing |
| `sse.cpp` | Server-Sent Events |
| `event_loop.cpp` | Event loop (epoll/IOCP) |
| `fiber_posix.cpp`, `fiber_win.cpp` | Stackful fibers (ucontext / Win32 Fiber API) |
| `mem_budget.cpp` | Memory budget (`LOOK_MEMORY_LIMIT`) |
| `smtp_server.cpp` | Embedded SMTP — event loop, DKIM, SPF, DB auth (PBKDF2), per-user Maildir delivery + shared `mail_user_auth` |
| `imap_server.cpp` | Embedded IMAP4rev1 (RFC 3501/2177) — SELECT/FETCH/STORE/EXPUNGE/APPEND/SEARCH/IDLE, STARTTLS+IMAPS, thread-per-conn |
| `installer.cpp` | `lk install` / `lk module` — package and module installation from GitHub, `look.lock` |
| `test_runner.cpp` | `lk test` |
| `format_src.cpp` | `lk fmt` source formatter |
| `fix_src.cpp` | `lk fix` — rewrites deprecated `push`/`pop`/`array::pop` |
| `repl.cpp` | `lk repl` |
| `http_main.cpp` | HTTP entry (`--mode http`) — shared engine (setup, hot reload, VM), WebSocket, SSE, rate limiter, SMTP/IMAP startup |
| `fcgi_main.cpp` | FastCGI entry — stdin/stdout (mod_fcgid) and TCP modes, warm start |
| `cgi_main.cpp` | CGI entry |
| `main.cpp` | CLI entry (`lk`) |

## Tests

Tests live in `cpp/tests/` (shell scripts, `.lk` scripts and C++ unit tests). The main guard
compares the command-line and web surfaces of the VM:

```bash
bash cpp/tests/differential_test.sh cpp/build/lk cpp/build/lk-fcgi
lk test                                # runs the tests in a project's tests/ directory
```

## Notable APIs

- **`http::stream($method,$url,$body,$headers,$callback[,$opts])`** — live streaming HTTP client. `$callback($chunk)` is called as body chunks (chunked-decoded) arrive. For SSE / LLM token streams. Pairs with `route("SSE")` + `channel()`.
- **`look::check($source)`** — validates LOOK source in-process (without running it): `["ok"=>bool,"line"=>int,"col"=>int,"msg"=>string]`. Same engine as `lk --check`. For LLM/editor tooling.
