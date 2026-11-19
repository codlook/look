// JSON referee for json_differential_test.py: Go's encoding/json decides what a JSON text means.
//
// stdin: one base64-encoded JSON text per line. stdout: one line per input:
//   X                 Go rejects the text (json.Unmarshal into interface{} fails)
//   <tree>            the decoded value as a tagged tree, itself JSON:
//                       null / true / false
//                       {"n": "<number text exactly as written>"}
//                       {"s": "<string after Go's decoding>"}
//                       {"a": [<tree>, ...]}
//                       {"o": {"<key>": <tree>, ...}}   (Go keeps the last value of a duplicated key)
// Validity uses a plain Unmarshal (so 1e999 is rejected as out of range); the tree uses UseNumber
// (so the number text is kept for the comparison).
package main

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
)

func tag(v interface{}) interface{} {
	switch x := v.(type) {
	case nil, bool:
		return x
	case json.Number:
		return map[string]interface{}{"n": string(x)}
	case string:
		return map[string]interface{}{"s": x}
	case []interface{}:
		out := make([]interface{}, len(x))
		for i, e := range x {
			out[i] = tag(e)
		}
		return map[string]interface{}{"a": out}
	case map[string]interface{}:
		out := make(map[string]interface{}, len(x))
		for k, e := range x {
			out[k] = tag(e)
		}
		return map[string]interface{}{"o": out}
	}
	panic(fmt.Sprintf("unexpected %T", v))
}

func main() {
	in := bufio.NewScanner(os.Stdin)
	in.Buffer(make([]byte, 1<<20), 64<<20)
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	for in.Scan() {
		data, err := base64.StdEncoding.DecodeString(in.Text())
		if err != nil {
			fmt.Fprintln(os.Stderr, "bad base64:", err)
			os.Exit(2)
		}
		var plain interface{}
		if json.Unmarshal(data, &plain) != nil {
			fmt.Fprintln(out, "X")
			continue
		}
		dec := json.NewDecoder(bytes.NewReader(data))
		dec.UseNumber()
		var v interface{}
		if err := dec.Decode(&v); err != nil {
			fmt.Fprintln(os.Stderr, "decode after a valid Unmarshal failed:", err)
			os.Exit(2)
		}
		b, err := json.Marshal(tag(v))
		if err != nil {
			fmt.Fprintln(os.Stderr, "marshal:", err)
			os.Exit(2)
		}
		out.Write(b)
		out.WriteByte('\n')
	}
	if err := in.Err(); err != nil {
		fmt.Fprintln(os.Stderr, "read:", err)
		os.Exit(2)
	}
}
