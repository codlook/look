# Yerleşik × bağlam: argüman YEREL değişken, çağrıdan sonra değişken yeniden okunur
# (builtin_arg_intact_test.sh). Çıktı satırı: "<ad>|<örnek>|<arity>|same" ya da "...|CHANGED <json>".
# Kullanım: python3 builtin_arg_intact_gen.py <src/builtins.cpp> <çıkış dizini>
import os
import re
import sys

src = open(sys.argv[1], encoding='utf-8').read()
i = src.index('builtin_names()')
j = src.index('};', i)
names = re.findall(r'"([a-z_][a-z0-9_:]*)"', src[i:j])

# Dış sunucu, süreç sonu, dosyaya yazma ya da istek bağlamı isteyenler dışarıda.
SKIP_PREFIX = ('db::', 'http::', 'mail::', 'session::', 'cookie::', 'response::', 'csrf::', 'timer::',
               'ws::', 'sse::', 'jobs::', 'template::render', 'template::include', 'template::block',
               'template::extends', 'crypto::rs256', 'route::', 'request::')
SKIP = {'route', 'before_route', 'stop', 'exit', 'die', 'send', 'receive', 'close', 'channel', 'chan_size',
        'file::put', 'file::append', 'file::remove', 'file::store', 'parallel::wait', 'parallel::limit',
        'log::configure', 'runtime::gc', 'app::set', 'app::db', 'a', 'b', 'sqlite', 'mysql', 'postgres',
        'args', 'auth::hash', 'auth::verify', 'print', 'write', 'push', 'pop', 'queue::clear',
        'cache::flush', 'look::check', 'config', 'env', 'file::upload_dir'}
CALLBACK = {'array::map', 'array::filter', 'array::reduce', 'array::find', 'array::sort', 'array::all',
            'array::any'}
keep = [n for n in names if n not in SKIP and not n.startswith(SKIP_PREFIX)]
skipped = [n for n in names if n not in keep]
SAMPLES = [('s', '"abc"'), ('i', '3'), ('l', '[3, 1, 2]'), ('m', '["k" => "v"]')]


def block(n, tag, lit, arity):
    if n in CALLBACK:
        call = f'{n}($a, fn($x) => $x)' if arity == 1 else f'{n}($a, fn($x, $y) => $x, $b)'
    else:
        call = f'{n}($a)' if arity == 1 else f'{n}($a, $b)'
    return (f'  $a = {lit}\n  $b = "b"\n'
            f'  try {{ $r = {call} }} catch ($e) {{ $r = null }}\n'
            f'  print("{n}|{tag}|{arity}|" . (json::encode($a) == json::encode({lit}) ? "same" : '
            f'"CHANGED " . json::encode($a)) . ($b == "b" ? "" : " B-CHANGED"))\n')


def builtin_body(n):
    return ''.join(block(n, t, l, a) for t, l in SAMPLES for a in (1, 2))


def print_body():
    out = []
    for tag, lit in SAMPLES:
        same = f'json::encode($a) == json::encode({lit})'
        out.append(f'  $a = {lit}\n  print($a)\n  print("print-stmt|{tag}|1|" . ({same} ? "same" : "CHANGED"))\n')
        out.append(f'  $a = {lit}\n  write($a)\n  print("")\n'
                   f'  print("write-stmt|{tag}|1|" . ({same} ? "same" : "CHANGED"))\n')
        out.append(f'  $a = {lit}\n  $b = "b"\n  print($a, $b)\n'
                   f'  print("print-2|{tag}|1|" . ({same} && $b == "b" ? "same" : "CHANGED"))\n')
    return ''.join(out)


mods = sorted({n.split('::')[0] for n in keep if '::' in n} | {'json'})
header = ''.join(f'use {m}\n' for m in mods)
out_dir = sys.argv[2]
os.makedirs(out_dir, exist_ok=True)


def write(name, text):
    with open(os.path.join(out_dir, name), 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


# Bağlam 1: fonksiyon (her yerleşik ayrı fonksiyonda — register sınırı)
fn = [header]
for k, n in enumerate(keep):
    fn.append(f'function t{k}() {{\n{builtin_body(n)}}}\nt{k}()\n')
fn.append('function tp() {\n' + print_body() + '}\ntp()\n')
write('fn.lk', ''.join(fn))
# Bağlam 2: üst düzey blok (v2'de bloğun değişkenleri yereldir)
top = [header]
for n in keep:
    top.append('if (true) {\n' + builtin_body(n) + '}\n')
top.append('if (true) {\n' + print_body() + '}\n')
write('top.lk', ''.join(top))
# Bağlam 3: route (her yerleşik bir rota)
rt = [header]
for k, n in enumerate(keep):
    rt.append(f'route("GET", "/t{k}", function() {{\n{builtin_body(n)}}})\n')
rt.append('route("GET", "/tp", function() {\n' + print_body() + '})\n')
write('route.lk', ''.join(rt))
write('routes.txt', '\n'.join([f't{k}' for k in range(len(keep))] + ['tp']) + '\n')
print(f'builtins total={len(names)} tested={len(keep)} skipped={len(skipped)}')
