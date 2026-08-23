'use strict';

const M = 1000000007;

function timeit(name, body) {
  // warm-up (discard)
  const warmChecksum = body();
  // timed
  const t0 = process.hrtime.bigint();
  const checksum = body();
  const t1 = process.hrtime.bigint();
  const warm_ms = Number(t1 - t0) / 1e6;
  if (checksum !== warmChecksum) {
    // should not happen for deterministic bodies
    console.error(`WARN ${name}: checksum unstable ${warmChecksum} vs ${checksum}`);
  }
  console.log(`${name},${checksum},${warm_ms.toFixed(3)}`);
}

// 1. int_arith
function int_arith() {
  const N = 20000000;
  let s = 0;
  for (let i = 0; i < N; i++) {
    // i*3 <= 60_000_000, s < M (~1e9), s + i*3 + 7 < ~1.06e9 < 2^53. safe.
    s = (s + i * 3 + 7) % M;
  }
  return s;
}

// 2. float_arith
function float_arith() {
  const N = 20000000;
  let x = 1.0;
  for (let i = 0; i < N; i++) {
    x = x * 1.0000003 + 0.5;
    if (x > 1e6) x = x - 1e6;
  }
  return Math.floor(x * 1000) % M;
}

// 3. loop
function loop() {
  const N = 100000000;
  let s = 0;
  for (let i = 0; i < N; i++) {
    s = (s + 1) % M;
  }
  return s;
}

// 4. fn_call
function f_fncall(i) { return i + 1; }
function fn_call() {
  const N = 20000000;
  let s = 0;
  for (let i = 0; i < N; i++) {
    s = (s + f_fncall(i)) % M;
  }
  return s;
}

// 5. nested_fn
function h_nf(x) { return x + 1; }
function g_nf(x) { return h_nf(x) + 1; }
function f_nf(x) { return g_nf(x) + 1; }
function nested_fn() {
  const N = 10000000;
  let s = 0;
  for (let i = 0; i < N; i++) {
    s = (s + f_nf(i)) % M;
  }
  return s;
}

// 6. recursion
function fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
function recursion() {
  return fib(32);
}

// 7. array_create_iterate
function array_create_iterate() {
  const K = 1000, R = 20000;
  let s = 0;
  for (let r = 0; r < R; r++) {
    const arr = new Array(K);
    for (let i = 0; i < K; i++) arr[i] = i;
    let sum = 0;
    for (let i = 0; i < K; i++) sum += arr[i];
    s = (s + sum) % M;
  }
  return s;
}

// 8. array_push_pop
function array_push_pop() {
  const K = 1000, R = 20000;
  let s = 0;
  for (let r = 0; r < R; r++) {
    const arr = [];
    for (let i = 0; i < K; i++) arr.push(i);
    let sum = 0;
    while (arr.length > 0) sum += arr.pop();
    s = (s + sum) % M;
  }
  return s;
}

// 9. assoc_access
function assoc_access() {
  const N = 5000000;
  const map = {};
  for (let i = 0; i < 100; i++) map["k" + i] = i;
  let s = 0;
  for (let i = 0; i < N; i++) {
    const v = map["k" + (i % 100)];
    s = (s + v) % M;
  }
  return s;
}

// 10. object_create
function object_create() {
  const N = 5000000;
  let s = 0;
  for (let i = 0; i < N; i++) {
    const rec = { a: i, b: i + 1, c: i + 2, d: i + 3, e: i + 4 };
    s = (s + rec.c) % M;
  }
  return s;
}

// 11. string_concat
function string_concat() {
  const K = 1000, R = 20000;
  let s = 0;
  for (let r = 0; r < R; r++) {
    let str = "";
    for (let i = 0; i < K; i++) str += "x";
    s = (s + str.length) % M;
  }
  return s;
}

// 12. string_search
function string_search() {
  const N = 1000000;
  let hay = "";
  for (let i = 0; i < 994; i++) hay += "a";
  hay += "needle"; // length 1000, needle near the end
  let s = 0;
  for (let i = 0; i < N; i++) {
    const idx = hay.indexOf("needle");
    s = (s + idx) % M;
  }
  return s;
}

// 13. string_slice
function string_slice() {
  const N = 2000000;
  let hay = "";
  for (let i = 0; i < 1000; i++) hay += String.fromCharCode(97 + (i % 26));
  let s = 0;
  for (let i = 0; i < N; i++) {
    const start = i % 900;
    const sub = hay.substring(start, start + 50);
    s = (s + sub.length) % M;
  }
  return s;
}

// 14. json_parse
function json_parse() {
  const N = 1000000;
  const src = '{"id":42,"name":"look","tags":[1,2,3],"active":true}';
  let s = 0;
  for (let i = 0; i < N; i++) {
    const obj = JSON.parse(src);
    s = (s + obj.id) % M;
  }
  return s;
}

// 15. json_serialize
function json_serialize() {
  const N = 1000000;
  const obj = { id: 42, name: "look", tags: [1, 2, 3], active: true };
  let s = 0;
  for (let i = 0; i < N; i++) {
    const str = JSON.stringify(obj);
    s = (s + str.length) % M;
  }
  return s;
}

// 16. regex
function regex() {
  const N = 500000;
  const pat = /[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]+/g;
  const text = "contact a@b.com or foo.bar@example.org or x@y.io now";
  let s = 0;
  for (let i = 0; i < N; i++) {
    const m = text.match(pat);
    const count = m ? m.length : 0;
    s = (s + count) % M;
  }
  return s;
}

// 17. exception
function exception() {
  const N = 2000000;
  let s = 0;
  for (let i = 0; i < N; i++) {
    try {
      if (i % 2 === 0) throw new Error("e");
      s = (s + 1) % M;
    } catch (e) {
      s = (s + 2) % M;
    }
  }
  return s;
}

timeit("int_arith", int_arith);
timeit("float_arith", float_arith);
timeit("loop", loop);
timeit("fn_call", fn_call);
timeit("nested_fn", nested_fn);
timeit("recursion", recursion);
timeit("array_create_iterate", array_create_iterate);
timeit("array_push_pop", array_push_pop);
timeit("assoc_access", assoc_access);
timeit("object_create", object_create);
timeit("string_concat", string_concat);
timeit("string_search", string_search);
timeit("string_slice", string_slice);
timeit("json_parse", json_parse);
timeit("json_serialize", json_serialize);
timeit("regex", regex);
timeit("exception", exception);
