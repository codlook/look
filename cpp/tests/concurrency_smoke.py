#!/usr/bin/env python3
# Bounded-concurrency hang smoke-test istemcisi.
#
# AMAÇ: worker-pool (default) ve fiber (LOOK_FIBER_DISPATCH=1) dispatch'inin
# c=50-100 eşzamanlı yükte HANG etmediğini doğrulamak. Geçmişte fiber acceptor'ı
# keep-alive fiber'larını beklerken YENİ bağlantıları accept etmiyordu → c=100
# hang (D Fix #2 ile çözüldü: acceptor artık bir fiber, idle-timeout 15s).
# Bu test o regresyonu yakalayacak POZİTİF-KONTROLE sahiptir (aşağıya bak).
#
# Bu bir THROUGHPUT benchmark'ı DEĞİL — sabit iş yapar, "hepsi bitti mi + sunucu
# hâlâ canlı mı" sorusuna EVET/HAYIR döner. Sayı/rps raporlamaz.
#
# Kullanım: python3 concurrency_smoke.py <port> <concurrency> <reqs_per_conn>
#
# Çıkış kodu 0 = PASS. !=0 = FAIL (stderr'de neden).
import socket, sys, threading, time

PORT   = int(sys.argv[1]) if len(sys.argv) > 1 else 7402
CONC   = int(sys.argv[2]) if len(sys.argv) > 2 else 100
PERCON = int(sys.argv[3]) if len(sys.argv) > 3 else 20
PATH   = "/health"
# Her bağlantı için cömert tavan. Bir HANG olsaydı bağlantılar bu süreyi aşar →
# thread bu süre içinde PERCON isteği bitiremez → eksik sayılır → FAIL.
CONN_TIMEOUT = 8.0

req = ("GET %s HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n" % PATH).encode()

results = [0] * CONC          # bağlantı başına tamamlanan 200 sayısı
errors  = [""] * CONC

def read_response(sock, buf):
    # Content-Length'e göre tam bir yanıtı tüket. keep-alive'da framing şart.
    while b"\r\n\r\n" not in buf:
        d = sock.recv(4096)
        if not d:
            return None, buf
        buf += d
    head, rest = buf.split(b"\r\n\r\n", 1)
    status_ok = head.startswith(b"HTTP/1.1 200") or head.startswith(b"HTTP/1.0 200")
    clen = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":", 1)[1].strip())
            break
    while len(rest) < clen:
        d = sock.recv(4096)
        if not d:
            return None, rest
        rest += d
    body_rest = rest[clen:]
    return status_ok, body_rest

def worker(idx):
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=CONN_TIMEOUT)
        s.settimeout(CONN_TIMEOUT)
        buf = b""
        for _ in range(PERCON):
            s.sendall(req)
            ok, buf = read_response(s, buf)
            if ok is None:
                errors[idx] = "connection closed / short read"
                break
            if not ok:
                errors[idx] = "non-200 status"
                break
            results[idx] += 1
        s.close()
    except Exception as e:
        errors[idx] = repr(e)

def run_wave():
    for i in range(CONC):
        results[i] = 0
        errors[i]  = ""
    t0 = time.time()
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(CONC)]
    for t in threads:
        t.start()
    # Toplam duvar-saati tavanı: CONN_TIMEOUT'tan biraz fazla. HANG'de thread'ler
    # join olmaz → join(timeout) döner → alive thread FAIL sinyali.
    deadline = CONN_TIMEOUT + 4.0
    for t in threads:
        remain = deadline - (time.time() - t0)
        t.join(max(0.1, remain))
    stuck = sum(1 for t in threads if t.is_alive())
    dt = time.time() - t0
    done = sum(results)
    want = CONC * PERCON
    return done, want, stuck, dt

def probe_alive():
    # Yük bittikten SONRA sunucu hâlâ yanıt veriyor mu? (hang → burada takılır)
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=3)
        s.settimeout(3)
        s.sendall(req)
        ok, _ = read_response(s, b"")
        s.close()
        return ok is True
    except Exception:
        return False

def main():
    done, want, stuck, dt = run_wave()
    alive = probe_alive()
    tag = "c=%d x %d req" % (CONC, PERCON)
    if done == want and stuck == 0 and alive:
        print("  PASS [%s]: %d/%d 200 in %.1fs, sunucu canlı" % (tag, done, want, dt))
        return 0
    print("  FAIL [%s]: %d/%d 200, %d thread asılı, %.1fs, post-load-alive=%s"
          % (tag, done, want, stuck, dt, alive), file=sys.stderr)
    for i, e in enumerate(errors):
        if e:
            print("    conn#%d: %s" % (i, e), file=sys.stderr)
            break
    return 1

if __name__ == "__main__":
    sys.exit(main())
