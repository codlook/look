#!/usr/bin/env python3
# Guard: every LOOK code example in the public docs must PARSE.
#
# The docs<->impl conformance guard checks builtin NAMES (docs mention x -> x exists, and
# vice-versa) but never checked whether the example CODE actually parses. A clean-room dogfood
# (2026) found the very first example failed to parse (three statements on one line) and two
# route snippets used a `:id` param form that silently 404s — none of it caught by any guard.
# This closes that class: extract <pre><code> blocks, keep the LOOK ones, run `lk --check`.
#
# Usage: python3 docs_examples_check.py <lk-binary> <html...>
import re, html, sys, subprocess, tempfile, os

lk = sys.argv[1]
files = sys.argv[2:]

def looks_like_look(t):
    lines = [l for l in t.splitlines() if l.strip()]
    if not lines:
        return False
    first = lines[0].lstrip()
    # shell prompt, REPL transcript (>>> / => output), or shebang — not a .lk program
    if first[:1] in ('$', '>') or first.startswith('#!') or first.startswith('>>>') or first.startswith('=>'):
        return False
    if any(l.lstrip().startswith('>>>') for l in lines):
        return False
    shell = ('docker ', 'sudo ', 'curl ', 'wget ', 'unzip ', 'plesk ', 'cmake', 'systemctl',
             'npm ', 'composer', 'apt ', 'dnf ', 'git ', 'Expand-Archive', 'code --install')
    if any(s in t for s in shell) and 'route(' not in t:
        return False
    # illustrative fragments — an ellipsis means "code omitted here", not a real program
    if '...' in t or '…' in t:
        return False
    # directory trees / diagrams use arrows and box-drawing, not LOOK code
    if any(ch in t for ch in ('←', '→', '↳', '├', '└', '│')):
        return False
    look = ('route(', 'function', 'print(', 'db::', 'json::', 'response::', 'request::',
            'session::', 'template::', 'use (')
    return any(tok in t for tok in look)

blocks = []
for f in files:
    src = open(f, encoding='utf-8').read()
    for m in re.finditer(r'<pre><code>(.*?)</code></pre>', src, re.S):
        # strip only real HTML tags (start with a letter or /); a bare `<`/`<=`/`<<` in code
        # is NOT a tag and must survive, or `$price <= 0` becomes `$price 0` — a false failure.
        text = re.sub(r'</?[a-zA-Z][^>]*>', '', m.group(1))
        text = html.unescape(text)
        blocks.append((f, text))

checked = fails = warns = 0
for f, t in blocks:
    if not looks_like_look(t):
        continue
    checked += 1
    # Some blocks show a shell command to start the server, then the LOOK code below it.
    # Drop the shell lines so the LOOK part is what gets parsed.
    shell_cmd = re.compile(r'^\s*(look-fcgi|lk-fcgi|lk|docker|sudo|curl|wget|cmake|npm|'
                           r'composer|systemctl|bash|git|unzip|plesk|code)\b')
    code = '\n'.join(l for l in t.splitlines() if not shell_cmd.match(l))
    fd, path = tempfile.mkstemp(suffix='.lk')
    os.write(fd, code.encode('utf-8')); os.close(fd)
    r = subprocess.run([lk, '--check', path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    os.unlink(path)
    if r.returncode != 0:
        out = (r.stdout + r.stderr).decode('utf-8', 'replace').strip().replace('\n', ' ')
        snippet = '\n    '.join(t.strip().splitlines()[:5])
        # A parser/lexer error means the documented code is wrong and will not run for anyone.
        # An "Undefined function/variable" means the block references a helper defined elsewhere
        # (an illustrative fragment) — real, but not a syntax bug, so warn instead of failing CI.
        syntax = ("Expect" in out or "Unexpected" in out)
        if syntax:
            fails += 1
            print(f"SYNTAX FAIL [{os.path.basename(f)}]  {out}\n    {snippet}\n")
        else:
            warns += 1
            print(f"warn (fragment?) [{os.path.basename(f)}]  {out}")

print(f"\ndocs examples: {checked} LOOK block(s) checked, {fails} syntax failure(s), {warns} warning(s)")
sys.exit(1 if fails else 0)
