# LOOK — XAMPP (Windows)

Self-contained installer (binaries bundled in `bin\`). Extract the zip, then in
an **Administrator PowerShell**:

```powershell
.\install.ps1
```

(or double-click `install.bat`)

What it does:
- copies `lk-cgi.exe` into `xampp\cgi-bin` (backs up any existing one)
- patches `httpd.conf` to handle `.lk` files (Action handler + mod_rewrite)
- creates a sample `htdocs\test.lk`

Then **restart Apache** in the XAMPP Control Panel and open
<http://localhost/test.lk>.

Custom XAMPP path:

```powershell
.\install.ps1 -XamppDir "D:\xampp"
```

Uninstall (removes the httpd.conf block + binaries, keeps your `.lk` files):

```powershell
.\uninstall.ps1
```

Requires XAMPP (Apache) on Windows x64.
