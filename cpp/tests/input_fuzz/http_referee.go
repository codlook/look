// HTTP request referee for http_differential_test.py: Go's net/http server with default settings.
// Every request is answered with what Go read from it, in the same form as the LOOK echo app:
//   <method>|<path>|<body length>|<body>
// The test sends the same raw bytes to this server and to `lk-fcgi --mode http`, and compares
// what each one accepted, rejected and read.
// Run: http_referee <port>
package main

import (
	"fmt"
	"io"
	"net/http"
	"os"
)

func main() {
	h := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, err := io.ReadAll(r.Body)
		if err != nil {
			// The body could not be read as framed (a bad chunk, a short body): Go's server answers
			// the request but the connection is not reused; report it so the test sees the failure.
			w.WriteHeader(400)
			fmt.Fprintf(w, "BODY-ERROR %v", err)
			return
		}
		fmt.Fprintf(w, "%s|%s|%d|%s", r.Method, r.URL.Path, len(b), b)
	})
	srv := &http.Server{Addr: "127.0.0.1:" + os.Args[1], Handler: h}
	fmt.Println(srv.ListenAndServe())
}
