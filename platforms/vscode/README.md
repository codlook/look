# LOOK Language — VS Code Extension

Full editor support for everyone writing code in [LOOK](https://codlook.com) (`.lk`).
Minimal external runtime dependencies, plain JavaScript — no build step required.

## Features

- **Live error checking** — `lk --check` runs as you type; **parse errors** and **calls to undefined functions** are underlined in red immediately. (No need to save.)
- **Syntax highlighting** — keywords, 260 built-in `module::function` names, `$variables`, string interpolation `{$x}`, `//` `/* */` `#` comments, backtick raw strings.
- **Autocompletion** — 260 functions from 33 modules (with real signatures), keywords, globals and **your own variables/functions in the current file**. Typing `math::` lists that module's functions.
- **Hover** — hover over any built-in to see its signature and description.
- **Signature help** — typing `(` shows parameter hints, with the active parameter highlighted.
- **Snippets** — `route`, `fn`, `foreach`, `try`, `switch`, `struct`, `dbquery`, `httpget`, `session` and more.
- **Commands** — from the context menu or a shortcut:
  - **LOOK: Run File** (`Ctrl+F5`) → `lk file.lk`
  - **LOOK: Start Web Server** → `lk-fcgi --mode http --port 7400`
  - **LOOK: Open REPL** → `lk repl`

## Installation

Search the Marketplace for **"LOOK Language"**, or install from a `.vsix`:

```bash
code --install-extension look-lang-2.0.2.vsix
```

## Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `look.binaryPath` | `lk` | Path to the `lk` executable (give the full path if it is not on PATH) |
| `look.fcgiPath` | `lk-fcgi` | Path to the web server binary |
| `look.serve.port` | `7400` | Port used by "Start Web Server" |
| `look.diagnostics.enable` | `true` | Turn live error checking on/off |
| `look.diagnostics.run` | `onType` | When to check: `onType` (while typing) / `onSave` (on save) |

> **Live checking requires `lk`.** If it is not on PATH, enter the full path in the `look.binaryPath` setting
> (e.g. `C:\look\lk.exe` or `/usr/local/bin/lk`). You can get the LOOK binary from [codlook.com](https://codlook.com)
> or with `docker run codlook/look`.

## License

Apache-2.0 · Codlook
