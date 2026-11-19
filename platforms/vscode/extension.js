// LOOK Language — VSCode extension
// Codlook. Plain JS (no build). Features: live error checking (lk --check),
// autocompletion, hover signatures, signature help, Run/Serve/REPL commands.
'use strict';

const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

// ── Built-in function database (generated: builtins.json) ──────────────────
let BUILTINS = [];           // [{name, module, fn, sig, detail}]
const BUILTIN_BY_NAME = {};  // "math::sqrt" -> record

const KEYWORDS = [
  'if', 'else', 'elseif', 'while', 'for', 'foreach', 'as', 'break', 'continue',
  'function', 'fn', 'return', 'print', 'write', 'true', 'false', 'null', 'use',
  'global', 'try', 'catch', 'finally', 'throw', 'switch', 'case', 'default',
  'struct', 'const', 'iota'
];
const GLOBALS = ['route', 'parallel', 'channel', 'range', 'len', 'count', 'keys', 'values', 'env', 'exit'];

function loadBuiltins(ctx) {
  try {
    const raw = fs.readFileSync(path.join(ctx.extensionPath, 'builtins.json'), 'utf8');
    BUILTINS = JSON.parse(raw);
    for (const b of BUILTINS) BUILTIN_BY_NAME[b.name] = b;
  } catch (e) {
    BUILTINS = [];
  }
}

// ── Settings ───────────────────────────────────────────────────────────────────
function cfg() { return vscode.workspace.getConfiguration('look'); }

// ═══════════════════════════════════════════════════════════════════════════
// 1) LIVE ERROR CHECKING — lk --check
// ═══════════════════════════════════════════════════════════════════════════
let diagCollection;
let binaryMissingWarned = false;
const debounceTimers = new Map();

function runCheck(doc) {
  if (doc.languageId !== 'look') return;
  if (!cfg().get('diagnostics.enable', true)) { diagCollection.delete(doc.uri); return; }

  const bin = cfg().get('binaryPath', 'lk');
  // Write the content to a temporary .lk file (so unsaved edits are checked too)
  const tmp = path.join(os.tmpdir(), 'look-check-' + Buffer.from(doc.uri.toString()).toString('hex').slice(0, 24) + '.lk');
  try { fs.writeFileSync(tmp, doc.getText()); } catch (e) { return; }

  cp.execFile(bin, ['--check', tmp], { timeout: 5000 }, (err, stdout, stderr) => {
    try { fs.unlinkSync(tmp); } catch (e) {}

    // Binary not found → warn once, turn the check off silently
    if (err && (err.code === 'ENOENT' || /ENOENT|not found|not recognized/i.test(String(stderr)))) {
      if (!binaryMissingWarned) {
        binaryMissingWarned = true;
        vscode.window.showWarningMessage(
          "LOOK: the 'lk' executable was not found — live error checking is off. " +
          "Set its path in the look.binaryPath setting.", 'Open Settings'
        ).then(sel => { if (sel) vscode.commands.executeCommand('workbench.action.openSettings', 'look.binaryPath'); });
      }
      diagCollection.delete(doc.uri);
      return;
    }

    const out = String(stdout).trim();
    const diags = [];
    // Format: "OK"  or  "CHECK <line> <column> <message>"
    const m = out.match(/^CHECK\s+(\d+)\s+(\d+)\s+([\s\S]*)$/);
    if (m) {
      const line = Math.max(0, parseInt(m[1], 10) - 1);
      const col = Math.max(0, parseInt(m[2], 10) - 1);
      const message = m[3].trim();
      // Underline the faulty token: from the column to the end of the line (or the end of the word)
      const textLine = line < doc.lineCount ? doc.lineAt(line).text : '';
      let endCol = textLine.length;
      const wordMatch = /[A-Za-z_$][A-Za-z0-9_:]*/.exec(textLine.slice(col));
      if (wordMatch) endCol = col + wordMatch.index + wordMatch[0].length;
      if (endCol <= col) endCol = col + 1;
      const range = new vscode.Range(line, col, line, endCol);
      const d = new vscode.Diagnostic(range, message, vscode.DiagnosticSeverity.Error);
      d.source = 'lk --check';
      diags.push(d);
    }
    diagCollection.set(doc.uri, diags);
  });
}

function scheduleCheck(doc) {
  const mode = cfg().get('diagnostics.run', 'onType');
  const key = doc.uri.toString();
  if (debounceTimers.has(key)) clearTimeout(debounceTimers.get(key));
  const delay = mode === 'onType' ? 400 : 0;
  debounceTimers.set(key, setTimeout(() => { runCheck(doc); debounceTimers.delete(key); }, delay));
}

// ═══════════════════════════════════════════════════════════════════════════
// 2) AUTOCOMPLETION
// ═══════════════════════════════════════════════════════════════════════════
function documentSymbols(doc) {
  const text = doc.getText();
  const vars = new Set();
  const funcs = new Set();
  let m;
  const reVar = /\$([A-Za-z_][A-Za-z0-9_]*)/g;
  while ((m = reVar.exec(text))) vars.add(m[1]);
  const reFn = /\b(?:function|fn)\s+([A-Za-z_][A-Za-z0-9_]*)/g;
  while ((m = reFn.exec(text))) funcs.add(m[1]);
  return { vars: [...vars], funcs: [...funcs] };
}

const completionProvider = {
  provideCompletionItems(doc, pos) {
    const line = doc.lineAt(pos.line).text.slice(0, pos.character);
    const items = [];

    // if "module::" was typed → that module's functions
    const modMatch = /([A-Za-z_][A-Za-z0-9_]*)::([A-Za-z0-9_]*)$/.exec(line);
    if (modMatch) {
      const mod = modMatch[1];
      for (const b of BUILTINS) {
        if (b.module !== mod) continue;
        const it = new vscode.CompletionItem(b.fn, vscode.CompletionItemKind.Function);
        it.detail = b.sig;
        it.documentation = new vscode.MarkdownString('**' + b.sig + '**\n\n' + b.detail);
        it.insertText = new vscode.SnippetString(snippetFromSig(b));
        items.push(it);
      }
      return items;
    }

    // if "$" was typed → the variables in the file
    const sym = documentSymbols(doc);
    if (/\$[A-Za-z0-9_]*$/.test(line)) {
      for (const v of sym.vars) {
        const it = new vscode.CompletionItem(v, vscode.CompletionItemKind.Variable);
        it.insertText = v; // '$' was already typed
        items.push(it);
      }
      return items;
    }

    // General context: keywords + modules + globals + all builtins + user functions
    for (const k of KEYWORDS) items.push(new vscode.CompletionItem(k, vscode.CompletionItemKind.Keyword));
    const modules = [...new Set(BUILTINS.map(b => b.module))];
    for (const mod of modules) {
      const it = new vscode.CompletionItem(mod, vscode.CompletionItemKind.Module);
      it.detail = 'LOOK module';
      it.insertText = new vscode.SnippetString(mod + '::');
      it.command = { command: 'editor.action.triggerSuggest', title: 're-trigger' };
      items.push(it);
    }
    for (const g of GLOBALS) {
      const it = new vscode.CompletionItem(g, vscode.CompletionItemKind.Function);
      it.detail = 'global function';
      items.push(it);
    }
    for (const b of BUILTINS) {
      const it = new vscode.CompletionItem(b.name, vscode.CompletionItemKind.Function);
      it.detail = b.sig;
      it.documentation = new vscode.MarkdownString('**' + b.sig + '**\n\n' + b.detail);
      it.insertText = new vscode.SnippetString(snippetFromSig(b));
      it.filterText = b.name;
      items.push(it);
    }
    for (const f of sym.funcs) {
      const it = new vscode.CompletionItem(f, vscode.CompletionItemKind.Function);
      it.detail = 'function in this file';
      items.push(it);
    }
    return items;
  }
};

// Generate a snippet with tab stops from the signature: math::sqrt($x) -> math::sqrt(${1:$x})
function snippetFromSig(b) {
  const m = /\(([\s\S]*)\)$/.exec(b.sig);
  if (!m || !m[1].trim()) return b.fn + '()';
  const args = splitArgs(m[1]);
  const parts = args.map((a, i) => '${' + (i + 1) + ':' + a.trim().replace(/[{}$]/g, '') + '}');
  return b.fn + '(' + parts.join(', ') + ')';
}
function splitArgs(s) {
  const out = []; let depth = 0, cur = '';
  for (const ch of s) {
    if (ch === '[' || ch === '(') depth++;
    if (ch === ']' || ch === ')') depth--;
    if (ch === ',' && depth === 0) { out.push(cur); cur = ''; } else cur += ch;
  }
  if (cur.trim()) out.push(cur);
  return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// 3) HOVER
// ═══════════════════════════════════════════════════════════════════════════
const hoverProvider = {
  provideHover(doc, pos) {
    const range = doc.getWordRangeAtPosition(pos, /[A-Za-z_][A-Za-z0-9_]*::[A-Za-z_][A-Za-z0-9_]*/);
    if (!range) return;
    const word = doc.getText(range);
    const b = BUILTIN_BY_NAME[word];
    if (!b) return;
    const md = new vscode.MarkdownString();
    md.appendCodeblock(b.sig, 'look');
    md.appendMarkdown('\n' + b.detail + '  \n_LOOK built-in function_');
    return new vscode.Hover(md, range);
  }
};

// ═══════════════════════════════════════════════════════════════════════════
// 4) SIGNATURE HELP (parameter hint)
// ═══════════════════════════════════════════════════════════════════════════
const signatureProvider = {
  provideSignatureHelp(doc, pos) {
    const line = doc.lineAt(pos.line).text.slice(0, pos.character);
    // Find the function name before the nearest open parenthesis
    let depth = 0, i = line.length - 1, commas = 0;
    for (; i >= 0; i--) {
      const ch = line[i];
      if (ch === ')') depth++;
      else if (ch === '(') { if (depth === 0) break; depth--; }
      else if (ch === ',' && depth === 0) commas++;
    }
    if (i < 0) return;
    const before = line.slice(0, i);
    const nameMatch = /([A-Za-z_][A-Za-z0-9_]*::[A-Za-z_][A-Za-z0-9_]*)\s*$/.exec(before);
    if (!nameMatch) return;
    const b = BUILTIN_BY_NAME[nameMatch[1]];
    if (!b) return;
    const argsMatch = /\(([\s\S]*)\)$/.exec(b.sig);
    const help = new vscode.SignatureHelp();
    const sig = new vscode.SignatureInformation(b.sig, new vscode.MarkdownString(b.detail));
    if (argsMatch && argsMatch[1].trim()) {
      const args = splitArgs(argsMatch[1]);
      sig.parameters = args.map(a => new vscode.ParameterInformation(a.trim()));
      help.activeParameter = Math.min(commas, args.length - 1);
    } else {
      sig.parameters = [];
      help.activeParameter = 0;
    }
    help.signatures = [sig];
    help.activeSignature = 0;
    return help;
  }
};

// ═══════════════════════════════════════════════════════════════════════════
// 5) COMMANDS — Run / Serve / REPL
// ═══════════════════════════════════════════════════════════════════════════
function termFor(name) {
  const existing = vscode.window.terminals.find(t => t.name === name);
  return existing || vscode.window.createTerminal(name);
}
function quote(p) { return /\s/.test(p) ? '"' + p + '"' : p; }

function cmdRun() {
  const ed = vscode.window.activeTextEditor;
  if (!ed || ed.document.languageId !== 'look') { vscode.window.showInformationMessage('No active .lk file.'); return; }
  ed.document.save().then(() => {
    const t = termFor('LOOK Run');
    t.show();
    t.sendText(quote(cfg().get('binaryPath', 'lk')) + ' ' + quote(ed.document.fileName));
  });
}
function cmdServe() {
  const ed = vscode.window.activeTextEditor;
  const port = cfg().get('serve.port', 7400);
  const dir = ed && ed.document.uri.fsPath ? path.dirname(ed.document.uri.fsPath) : undefined;
  const t = termFor('LOOK Serve');
  t.show();
  if (dir) t.sendText('cd ' + quote(dir));
  t.sendText(quote(cfg().get('fcgiPath', 'lk-fcgi')) + ' --mode http --port ' + port + ' --workers 4');
  vscode.window.showInformationMessage('LOOK web server: http://localhost:' + port + '/');
}
function cmdRepl() {
  const t = termFor('LOOK REPL');
  t.show();
  t.sendText(quote(cfg().get('binaryPath', 'lk')) + ' repl');
}
function cmdCheck() {
  const ed = vscode.window.activeTextEditor;
  if (ed && ed.document.languageId === 'look') runCheck(ed.document);
}

// ═══════════════════════════════════════════════════════════════════════════
// ACTIVATE
// ═══════════════════════════════════════════════════════════════════════════
function activate(ctx) {
  loadBuiltins(ctx);
  diagCollection = vscode.languages.createDiagnosticCollection('look');
  ctx.subscriptions.push(diagCollection);

  const sel = { language: 'look', scheme: 'file' };
  ctx.subscriptions.push(
    vscode.languages.registerCompletionItemProvider(sel, completionProvider, ':', '$', '>'),
    vscode.languages.registerHoverProvider(sel, hoverProvider),
    vscode.languages.registerSignatureHelpProvider(sel, signatureProvider, '(', ','),
    vscode.commands.registerCommand('look.run', cmdRun),
    vscode.commands.registerCommand('look.serve', cmdServe),
    vscode.commands.registerCommand('look.repl', cmdRepl),
    vscode.commands.registerCommand('look.check', cmdCheck)
  );

  // Check triggers
  ctx.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(scheduleCheck),
    vscode.workspace.onDidChangeTextDocument(e => {
      if (cfg().get('diagnostics.run', 'onType') === 'onType') scheduleCheck(e.document);
    }),
    vscode.workspace.onDidSaveTextDocument(runCheck),
    vscode.workspace.onDidCloseTextDocument(doc => diagCollection.delete(doc.uri))
  );
  // Check the open files right away
  vscode.workspace.textDocuments.forEach(scheduleCheck);
}

function deactivate() {
  if (diagCollection) diagCollection.dispose();
}

module.exports = { activate, deactivate };
