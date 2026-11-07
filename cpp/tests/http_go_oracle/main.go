// Reference server for tests/http_rfc9112_test.py: Go net/http with default settings. Every request
// is answered with its path and Host. Run: go run . <port> — then http_rfc9112_test.py <look_port> <port>.
package main

import (
	"fmt"
	"io"
	"net/http"
	"os"
)

func main() {
	h := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		fmt.Fprintf(w, "path=%s host=%s body=%s", r.URL.Path, r.Host, b)
	})
	srv := &http.Server{Addr: "127.0.0.1:" + os.Args[1], Handler: h}
	fmt.Println(srv.ListenAndServe())
}
