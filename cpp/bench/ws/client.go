// WebSocket load client for ws-race.sh.
//   hold: open -n connections (-c dialing at once), report how many opened and how fast, keep them
//         open -hold seconds (RSS is read meanwhile), then check that a sample still echoes.
//   echo: -n connections each send a -size byte text frame and wait for its echo, for -d seconds;
//         report messages per second and latency percentiles.
package main

import (
	"flag"
	"fmt"
	"os"
	"sort"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
)

func main() {
	url := flag.String("url", "ws://127.0.0.1:8080/ws", "")
	mode := flag.String("mode", "echo", "hold | echo")
	n := flag.Int("n", 1000, "connections")
	c := flag.Int("c", 200, "dial concurrency")
	d := flag.Duration("d", 15*time.Second, "echo duration")
	hold := flag.Duration("hold", 10*time.Second, "hold duration")
	size := flag.Int("size", 64, "message bytes")
	flag.Parse()

	dialer := websocket.Dialer{HandshakeTimeout: 20 * time.Second}
	conns := make([]*websocket.Conn, *n)
	var failed int64
	sem := make(chan struct{}, *c)
	var wg sync.WaitGroup
	start := time.Now()
	for i := 0; i < *n; i++ {
		wg.Add(1)
		sem <- struct{}{}
		go func(i int) {
			defer wg.Done()
			defer func() { <-sem }()
			cn, _, err := dialer.Dial(*url, nil)
			if err != nil {
				atomic.AddInt64(&failed, 1)
				return
			}
			conns[i] = cn
		}(i)
	}
	wg.Wait()
	dial := time.Since(start)
	open := *n - int(failed)
	msg := make([]byte, *size)
	for i := range msg {
		msg[i] = 'a' + byte(i%26)
	}

	if *mode == "hold" {
		fmt.Printf("HOLD open=%d failed=%d dial_s=%.1f\n", open, failed, dial.Seconds())
		os.Stdout.Sync()
		time.Sleep(*hold)
		alive, sample := 0, 0
		for i := 0; i < *n && sample < 200; i += 1 + *n/200 {
			if conns[i] == nil {
				continue
			}
			sample++
			conns[i].SetReadDeadline(time.Now().Add(5 * time.Second))
			conns[i].SetWriteDeadline(time.Now().Add(5 * time.Second))
			if conns[i].WriteMessage(websocket.TextMessage, msg) == nil {
				if _, r, err := conns[i].ReadMessage(); err == nil && len(r) == len(msg) {
					alive++
				}
			}
		}
		fmt.Printf("ALIVE %d/%d\n", alive, sample)
		return
	}

	var total int64
	lat := make([][]time.Duration, *n)
	stop := time.Now().Add(*d)
	var ew sync.WaitGroup
	for i := 0; i < *n; i++ {
		if conns[i] == nil {
			continue
		}
		ew.Add(1)
		go func(i int) {
			defer ew.Done()
			cn := conns[i]
			for time.Now().Before(stop) {
				t0 := time.Now()
				if cn.WriteMessage(websocket.TextMessage, msg) != nil {
					return
				}
				if _, _, err := cn.ReadMessage(); err != nil {
					return
				}
				lat[i] = append(lat[i], time.Since(t0))
				atomic.AddInt64(&total, 1)
			}
		}(i)
	}
	ew.Wait()
	var all []time.Duration
	for _, l := range lat {
		all = append(all, l...)
	}
	sort.Slice(all, func(a, b int) bool { return all[a] < all[b] })
	p := func(q float64) float64 {
		if len(all) == 0 {
			return 0
		}
		return float64(all[int(float64(len(all)-1)*q)].Microseconds()) / 1000
	}
	fmt.Printf("ECHO open=%d failed=%d msgs_s=%.0f p50_ms=%.2f p99_ms=%.2f max_ms=%.2f\n",
		open, failed, float64(total)/d.Seconds(), p(0.50), p(0.99), p(1.0))
}
