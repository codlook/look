// multipart/form-data referee for multipart_differential_test.py: Go's mime/multipart reads the body.
// POST /m answers with what Go read, as JSON, in the same shape as the LOOK app:
//   {"post": {name: value, ...}, "files": {name: [filename, size], ...}}   or   {"error": "..."}
// A part with a file name (and a field name) is a file; a part with a field name only is a field; a part with
// no field name is skipped (Go's ReadForm does the same). A repeated name keeps its last value, as in LOOK.
// Run: multipart_referee <port>
package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
)

func main() {
	http.HandleFunc("/m", func(w http.ResponseWriter, r *http.Request) {
		out := map[string]interface{}{}
		post := map[string]string{}
		files := map[string][]interface{}{}
		mr, err := r.MultipartReader()
		if err != nil {
			out["error"] = "reader: " + err.Error()
		} else {
			for {
				p, err := mr.NextPart()
				if err == io.EOF {
					break
				}
				if err != nil {
					out["error"] = "next part: " + err.Error()
					break
				}
				data, err := io.ReadAll(p)
				if err != nil {
					out["error"] = "part body: " + err.Error()
					break
				}
				name := p.FormName()
				if name == "" {
					continue
				}
				if fn := p.FileName(); fn != "" {
					files[name] = []interface{}{fn, len(data)}
				} else {
					post[name] = string(data)
				}
			}
		}
		if _, bad := out["error"]; !bad {
			out["post"] = post
			out["files"] = files
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(out)
	})
	if err := http.ListenAndServe("127.0.0.1:"+os.Args[1], nil); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
