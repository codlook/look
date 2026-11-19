// builtins.json generator — rerun it when the LOOK language gains a new function.
//   node platforms/vscode/tools/gen-builtins.js
// Sources: cpp/src/*.cpp (function names) + docs/index.html (signatures).
// Must be run from the repo root.
const fs = require('fs');
const cp = require('child_process');

function grep(re, files) {
  const out = new Set();
  for (const f of files) {
    let txt = '';
    try { txt = fs.readFileSync(f, 'utf8'); } catch (e) { continue; }
    // Example names in C++ comments (like "mod::fn") must not become catalog entries: strip the line comment
    // (only a // after whitespace/line start, so the // inside "http://" is left alone).
    txt = txt.replace(/(^|\s)\/\/.*$/gm, '$1');
    let m; const r = new RegExp(re, 'g');
    while ((m = r.exec(txt))) out.add(m[1]);
  }
  return [...out];
}

const srcFiles = fs.readdirSync('cpp/src').filter(f => f.endsWith('.cpp')).map(f => 'cpp/src/' + f);
const names = grep('"([a-z_]+::[a-z_]+)"', srcFiles).sort();

// Reduce docs/index.html to plain text: strip HTML tags + decode entities.
// Otherwise the highlighted signatures inside <pre> are polluted with <span>...</span>.
let docs = fs.readFileSync('docs/look.codlook.com/docs.html', 'utf8');
docs = docs.replace(/<[^>]+>/g, '')
           .replace(/&quot;/g, '"').replace(/&#39;/g, "'")
           .replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
const byName = {};
let m; const rsig = /([a-z_]+::[a-z_]+)\(([^)\n<]*)\)/g;  // single line, no HTML
while ((m = rsig.exec(docs))) {
  const args = m[2].trim();
  if (args.includes('<') || args.includes('=>')) continue; // not a leftover/lambda body
  // parentheses/brackets must be balanced (drop half-cut examples: "..., function($x")
  let d = 0, ok = true;
  for (const c of args) { if (c === '(' || c === '[') d++; else if (c === ')' || c === ']') d--; if (d < 0) { ok = false; break; } }
  if (!ok || d !== 0) continue;
  (byName[m[1]] ||= []).push(args);
}

function bestArgs(c) {
  if (!c || !c.length) return null;
  const d = c.filter(x => x.includes('$') || x.trim() === '');
  const pool = d.length ? d : c;
  return pool.sort((a, b) => {
    const da = (a.match(/\$/g) || []).length, db = (b.match(/\$/g) || []).length;
    return db !== da ? db - da : b.length - a.length;
  })[0];
}
const MOD = {
  math:'Math', string:'String operations', array:'Arrays & collections', type:'Type conversion & checks',
  request:'HTTP request data', response:'HTTP response', db:'Database (MySQL/PG/SQLite)', date:'Date & time',
  http:'HTTP client', file:'File system', crypto:'Cryptography', validator:'Validation', session:'Session',
  cookie:'Cookie', json:'JSON', mail:'Email (SMTP)', cache:'Cache', queue:'Queue', jobs:'Background jobs',
  log:'Logging', template:'Template', parallel:'Parallel execution', ws:'WebSocket', sse:'Server-Sent Events',
  timer:'Timer', route:'Routing', app:'Application state', auth:'Authentication', html:'HTML',
  error:'Error', runtime:'Runtime', module:'Module', mod:'Module', look:'LOOK', csrf:'CSRF protection'
};
// ── SUPPLEMENT: functions that do not appear in the source AS a "mod::fn" string literal but are REAL
//    (the interpreter.cpp `fn == "x"` dispatch + jobs_stdlib functions[]).
//    Each was VERIFIED against interpreter.cpp/jobs_stdlib.cpp (2026-07-29). Signatures are from the source.
const SUPPLEMENT = [
  ['ws::on', '$ws, "message", $fn'], ['ws::send', '$ws, $msg'], ['ws::close', '$ws'],
  ['ws::broadcast', '$msg'], ['ws::clients', ''],
  ['sse::send', '$sse, $data [, $event]'], ['sse::on', '$sse, "close", $fn'],
  ['sse::close', '$sse'], ['sse::clients', ''],
  ['timer::after', '$ms, $fn'], ['timer::every', '$ms, $fn'], ['timer::cancel', '$id'],
  ['jobs::run', '$interval_ms, $fn'], ['jobs::worker', '$name, $fn'],
  // crypto:: — registered separately in the source (verified against the docs reference)
  ['crypto::sha256', '$data'], ['crypto::hmac_sha256', '$data, $key'],
  ['crypto::hmac_sha256_raw', '$data, $key'],
  ['crypto::base64_encode', '$s'], ['crypto::base64_decode', '$s'],
  ['crypto::base64url_encode', '$s'], ['crypto::base64url_decode', '$s'],
  ['crypto::rs256_sign', '$data, $pem_key'], ['crypto::rs256_sign_b64url', '$data, $pem_key'],
  ['crypto::rs256_verify', '$data, $sig, $pem_key'],
  // route::group — the closure body ({...}) in the docs example confuses rsig → hand-written signature.
  ['route::group', '$prefix, [$middleware], $fn'],
];
for (const [nm, args] of SUPPLEMENT) {
  if (!names.includes(nm)) names.push(nm);
  // SUPPLEMENT carries a hand-verified signature. If the docs auto-extraction is polluted with an empty string ("")
  // (bestArgs prefers the empty one over the real args) use the hand signature; if the docs have real args, keep them.
  const good = (byName[nm] || []).filter(a => a && a.trim() !== '');
  byName[nm] = good.length ? good : [args];
}
names.sort();

const out = names.map(n => {
  const [mod, fn] = n.split('::');
  const a = bestArgs(byName[n]);
  return { name: n, module: mod, fn, sig: a != null ? `${n}(${a})` : `${n}(…)`,
           detail: (MOD[mod] || mod) + ' module', documented: !!byName[n] };
});
fs.writeFileSync('platforms/vscode/builtins.json', JSON.stringify(out, null, 1));
console.log(`wrote: ${out.length} functions, ${out.filter(o => o.documented).length} with signatures`);
