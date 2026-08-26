<?php
// Language-core micro-benchmarks — PHP mirror of cpp/bench/micro/SPEC.md.
// Same logic, same N, same integer checksums as look/ and node/ (checksum = equivalence proof).
// Run with a production config: PHP 8.3, OPcache + JIT enabled (see BENCHMARK.md).

const M = 1000000007;

function timeit(string $name, callable $body): void {
    $warm = $body();              // warm-up (discard)
    $t0 = hrtime(true);           // nanoseconds
    $checksum = $body();          // timed
    $t1 = hrtime(true);
    $warm_ms = ($t1 - $t0) / 1e6;
    if ($checksum !== $warm) {
        fwrite(STDERR, "WARN $name: checksum unstable $warm vs $checksum\n");
    }
    printf("%s,%d,%.3f\n", $name, $checksum, $warm_ms);
}

// 1. int_arith
function int_arith() {
    $N = 20000000; $s = 0;
    for ($i = 0; $i < $N; $i++) { $s = ($s + $i * 3 + 7) % M; }
    return $s;
}

// 2. float_arith
function float_arith() {
    $N = 20000000; $x = 1.0;
    for ($i = 0; $i < $N; $i++) {
        $x = $x * 1.0000003 + 0.5;
        if ($x > 1e6) $x = $x - 1e6;
    }
    return (int)floor($x * 1000) % M;
}

// 3. loop
function loop_bench() {
    $N = 100000000; $s = 0;
    for ($i = 0; $i < $N; $i++) { $s = ($s + 1) % M; }
    return $s;
}

// 4. fn_call
function f_fncall($i) { return $i + 1; }
function fn_call() {
    $N = 20000000; $s = 0;
    for ($i = 0; $i < $N; $i++) { $s = ($s + f_fncall($i)) % M; }
    return $s;
}

// 5. nested_fn
function h_nf($x) { return $x + 1; }
function g_nf($x) { return h_nf($x) + 1; }
function f_nf($x) { return g_nf($x) + 1; }
function nested_fn() {
    $N = 10000000; $s = 0;
    for ($i = 0; $i < $N; $i++) { $s = ($s + f_nf($i)) % M; }
    return $s;
}

// 6. recursion
function fib($n) { if ($n < 2) return $n; return fib($n - 1) + fib($n - 2); }
function recursion() { return fib(32); }

// 7. array_create_iterate
function array_create_iterate() {
    $K = 1000; $R = 20000; $s = 0;
    for ($r = 0; $r < $R; $r++) {
        $arr = [];
        for ($i = 0; $i < $K; $i++) $arr[$i] = $i;
        $sum = 0;
        for ($i = 0; $i < $K; $i++) $sum += $arr[$i];
        $s = ($s + $sum) % M;
    }
    return $s;
}

// 8. array_push_pop
function array_push_pop() {
    $K = 1000; $R = 20000; $s = 0;
    for ($r = 0; $r < $R; $r++) {
        $arr = [];
        for ($i = 0; $i < $K; $i++) $arr[] = $i;
        $sum = 0;
        while (count($arr) > 0) $sum += array_pop($arr);
        $s = ($s + $sum) % M;
    }
    return $s;
}

// 9. assoc_access
function assoc_access() {
    $N = 5000000; $map = [];
    for ($i = 0; $i < 100; $i++) $map["k" . $i] = $i;
    $s = 0;
    for ($i = 0; $i < $N; $i++) {
        $v = $map["k" . ($i % 100)];
        $s = ($s + $v) % M;
    }
    return $s;
}

// 10. object_create (PHP assoc array mirrors LOOK assoc — apples-to-apples)
function object_create() {
    $N = 5000000; $s = 0;
    for ($i = 0; $i < $N; $i++) {
        $rec = ['a' => $i, 'b' => $i + 1, 'c' => $i + 2, 'd' => $i + 3, 'e' => $i + 4];
        $s = ($s + $rec['c']) % M;
    }
    return $s;
}

// 11. string_concat
function string_concat() {
    $K = 1000; $R = 20000; $s = 0;
    for ($r = 0; $r < $R; $r++) {
        $str = "";
        for ($i = 0; $i < $K; $i++) $str .= "x";
        $s = ($s + strlen($str)) % M;
    }
    return $s;
}

// 12. string_search
function string_search() {
    $N = 1000000;
    $hay = str_repeat("a", 994) . "needle"; // length 1000, needle near the end
    $s = 0;
    for ($i = 0; $i < $N; $i++) {
        $idx = strpos($hay, "needle");
        $s = ($s + $idx) % M;
    }
    return $s;
}

// 13. string_slice
function string_slice() {
    $N = 2000000;
    $hay = "";
    for ($i = 0; $i < 1000; $i++) $hay .= chr(97 + ($i % 26));
    $s = 0;
    for ($i = 0; $i < $N; $i++) {
        $start = $i % 900;
        $sub = substr($hay, $start, 50);
        $s = ($s + strlen($sub)) % M;
    }
    return $s;
}

// 14. json_parse
function json_parse() {
    $N = 1000000;
    $src = '{"id":42,"name":"look","tags":[1,2,3],"active":true}';
    $s = 0;
    for ($i = 0; $i < $N; $i++) {
        $obj = json_decode($src, true);
        $s = ($s + $obj['id']) % M;
    }
    return $s;
}

// 15. json_serialize
function json_serialize() {
    $N = 1000000;
    $obj = ['id' => 42, 'name' => "look", 'tags' => [1, 2, 3], 'active' => true];
    $s = 0;
    for ($i = 0; $i < $N; $i++) {
        $str = json_encode($obj);
        $s = ($s + strlen($str)) % M;
    }
    return $s;
}

// 16. regex
function regex_bench() {
    $N = 500000;
    $pat = '/[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]+/';
    $text = "contact a@b.com or foo.bar@example.org or x@y.io now";
    $s = 0;
    for ($i = 0; $i < $N; $i++) {
        $count = preg_match_all($pat, $text, $m);
        $s = ($s + $count) % M;
    }
    return $s;
}

// 17. exception
function exception_bench() {
    $N = 2000000; $s = 0;
    for ($i = 0; $i < $N; $i++) {
        try {
            if ($i % 2 === 0) throw new Exception("e");
            $s = ($s + 1) % M;
        } catch (Exception $e) {
            $s = ($s + 2) % M;
        }
    }
    return $s;
}

timeit("int_arith", 'int_arith');
timeit("float_arith", 'float_arith');
timeit("loop", 'loop_bench');
timeit("fn_call", 'fn_call');
timeit("nested_fn", 'nested_fn');
timeit("recursion", 'recursion');
timeit("array_create_iterate", 'array_create_iterate');
timeit("array_push_pop", 'array_push_pop');
timeit("assoc_access", 'assoc_access');
timeit("object_create", 'object_create');
timeit("string_concat", 'string_concat');
timeit("string_search", 'string_search');
timeit("string_slice", 'string_slice');
timeit("json_parse", 'json_parse');
timeit("json_serialize", 'json_serialize');
timeit("regex", 'regex_bench');
timeit("exception", 'exception_bench');
