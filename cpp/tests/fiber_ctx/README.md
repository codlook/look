# Fiber-mode web-context correctness guard

Proves that under `LOOK_FIBER_DISPATCH=1` each request's `request::`/`response::`
context stays bound to its own fiber, even when handlers yield on I/O and many
fibers interleave on one worker thread.

## The bug this guards against

`look_app_dispatch()` binds the per-request web modules with
`copy->set_web_context(&web)`. Under the default worker-pool model one thread
serves one request at a time, so a thread-local `copy` is safe. Under fiber
mode a single worker thread interleaves many connection fibers: when handler A
yields (e.g. `db::query`, `http_client`), handler B runs and rebinds the shared
copy to *its* request — so when A resumes, `request::get()` reads B's data. One
user's response could leak into another's.

The VM (bytecode) dispatch path was already safe: it snapshots the per-request
builtins into a fiber-local vector. The **tree-walk interpreter path** was not —
it went straight through the shared `copy->modules_`. The fix gives every fiber
its own dispatch copy in fiber mode.

## Why it was never caught before

The existing fiber load tests ("0 hata") measured error/crash counts under load,
not per-response *output correctness*. Same blind spot class as the string
index-immutability gap: differential/load tests didn't assert this behavior.

## Files

- `app.lk` — `/echo?v=N` yields at `http::get()` (to `/slow` via the slow
  upstream) then returns `request::get("v")`. Correct → returns its own `N`.
- `slow_server.py` — deliberately slow HTTP responder that widens the yield
  window so fibers reliably interleave.
- `run.sh <lk-fcgi> [fiber|worker-pool]` — fires 80 concurrent unique requests,
  asserts every response equals its own input. `LOOK_BYTECODE=0` selects the
  interpreter path (the one that used to fail 80/80).
- `guard.sh <lk-fcgi>` — CI entry point; runs interpreter+fiber and VM+fiber,
  fails on any leak. Skips cleanly off-Linux or without python3/curl.

## Run

```bash
docker run --rm -v "$PWD/cpp:/look/cpp" -w /look/cpp look-build \
  bash tests/fiber_ctx/guard.sh build/lk-fcgi
```
