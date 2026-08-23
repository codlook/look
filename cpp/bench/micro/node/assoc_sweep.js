function runK(K) {
  const M = 1000000007;
  const map = {};
  for (let k = 0; k < K; k++) map["k" + k] = k;
  let s = 0;
  for (let i = 0; i < 5000000; i++) {
    const key = "k" + (i % K);
    s = (s + map[key]) % M;
  }
  return s;
}
function benchK(K) {
  runK(K);
  const t0 = process.hrtime.bigint();
  const cs = runK(K);
  const t1 = process.hrtime.bigint();
  const ms = Number(t1 - t0) / 1e6;
  console.log(`assoc${K},${cs},${ms.toFixed(3)}`);
}
[5,15,50,100].forEach(benchK);
