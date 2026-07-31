// builtins.json ureticisi — LOOK dili yeni fonksiyon kazandiginda yeniden calistir.
//   node platforms/vscode/tools/gen-builtins.js
// Kaynaklar: cpp/src/*.cpp (fonksiyon adlari) + docs/index.html (imzalar).
// Repo kokunden calistirilmali.
const fs = require('fs');
const cp = require('child_process');

function grep(re, files) {
  const out = new Set();
  for (const f of files) {
    let txt = '';
    try { txt = fs.readFileSync(f, 'utf8'); } catch (e) { continue; }
    let m; const r = new RegExp(re, 'g');
    while ((m = r.exec(txt))) out.add(m[1]);
  }
  return [...out];
}

const srcFiles = fs.readdirSync('cpp/src').filter(f => f.endsWith('.cpp')).map(f => 'cpp/src/' + f);
const names = grep('"([a-z_]+::[a-z_]+)"', srcFiles).sort();

// docs/index.html'i düz metne indir: HTML etiketlerini sök + varlıkları çöz.
// Aksi halde <pre> içindeki vurgulu imzalar <span>...</span> ile kirlenir.
let docs = fs.readFileSync('docs/index.html', 'utf8');
docs = docs.replace(/<[^>]+>/g, '')
           .replace(/&quot;/g, '"').replace(/&#39;/g, "'")
           .replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
const byName = {};
let m; const rsig = /([a-z_]+::[a-z_]+)\(([^)\n<]*)\)/g;  // tek satır, HTML yok
while ((m = rsig.exec(docs))) {
  const args = m[2].trim();
  if (args.includes('<') || args.includes('=>')) continue; // kalıntı/lambda gövdesi değil
  // dengeli parantez/köşeli olmalı (yarım kesilmiş örnekleri ele: "..., function($x")
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
  math:'Matematik', string:'Metin işlemleri', array:'Dizi/koleksiyon', type:'Tip dönüşüm & sorgu',
  request:'HTTP istek verisi', response:'HTTP yanıt', db:'Veritabanı (MySQL/PG/SQLite)', date:'Tarih & saat',
  http:'HTTP istemci', file:'Dosya sistemi', crypto:'Kriptografi', validator:'Doğrulama', session:'Oturum',
  cookie:'Çerez', json:'JSON', mail:'E-posta (SMTP)', cache:'Önbellek', queue:'Kuyruk', jobs:'Arka plan işleri',
  log:'Günlükleme', template:'Şablon', parallel:'Paralel çalıştırma', ws:'WebSocket', sse:'Server-Sent Events',
  timer:'Zamanlayıcı', route:'Yönlendirme', app:'Uygulama durumu', auth:'Kimlik doğrulama', html:'HTML',
  error:'Hata', runtime:'Çalışma zamanı', module:'Modül', mod:'Modül', look:'LOOK'
};
// ── SUPPLEMENT: kaynakta "mod::fn" string literali OLARAK geçmeyen ama GERÇEK olan
//    fonksiyonlar (interpreter.cpp `fn == "x"` dispatch'i + jobs_stdlib functions[]).
//    Her biri interpreter.cpp/jobs_stdlib.cpp'den DOĞRULANDI (2026-07-29). İmzalar kaynaktan.
const SUPPLEMENT = [
  ['ws::on', '$ws, "message", $fn'], ['ws::send', '$ws, $msg'], ['ws::close', '$ws'],
  ['ws::broadcast', '$msg'], ['ws::clients', ''],
  ['sse::send', '$sse, $data [, $event]'], ['sse::on', '$sse, "close", $fn'],
  ['sse::close', '$sse'], ['sse::clients', ''],
  ['timer::after', '$ms, $fn'], ['timer::every', '$ms, $fn'], ['timer::cancel', '$id'],
  ['jobs::run', '$interval_ms, $fn'], ['jobs::worker', '$name, $fn'],
  // crypto:: — kaynakta ayrı kayıtlı (docs referansından doğrulandı)
  ['crypto::sha256', '$data'], ['crypto::hmac_sha256', '$data, $key'],
  ['crypto::hmac_sha256_raw', '$data, $key'],
  ['crypto::base64_encode', '$s'], ['crypto::base64_decode', '$s'],
  ['crypto::base64url_encode', '$s'], ['crypto::base64url_decode', '$s'],
  ['crypto::rs256_sign', '$data, $pem_key'], ['crypto::rs256_sign_b64url', '$data, $pem_key'],
  ['crypto::rs256_verify', '$data, $sig, $pem_key'],
];
for (const [nm, args] of SUPPLEMENT) {
  if (!names.includes(nm)) { names.push(nm); byName[nm] = [args]; }
}
names.sort();

const out = names.map(n => {
  const [mod, fn] = n.split('::');
  const a = bestArgs(byName[n]);
  return { name: n, module: mod, fn, sig: a != null ? `${n}(${a})` : `${n}(…)`,
           detail: (MOD[mod] || mod) + ' modülü', documented: !!byName[n] };
});
fs.writeFileSync('platforms/vscode/builtins.json', JSON.stringify(out, null, 1));
console.log(`yazildi: ${out.length} fonksiyon, ${out.filter(o => o.documented).length} imzali`);
