<?php
/**
 * LOOK Language Plesk Extension - Domain yonetim paneli
 */
define('SCRIPTS_DIR', __DIR__ . '/scripts');
define('CONF_DIR', '/opt/look/conf');
define('BINARY', '/opt/look/lk-fcgi');
define('DOMAINS_FILE', CONF_DIR . '/domains.json');

function run_script(string $script, array $args = []): array {
    $safe = array_map('escapeshellarg', $args);
    $cmd = 'sudo ' . escapeshellarg(SCRIPTS_DIR . '/' . $script) . ' ' . implode(' ', $safe) . ' 2>&1';
    exec($cmd, $out, $code);
    return ['ok' => $code === 0, 'out' => implode("\n", $out)];
}
function load_domains(): array {
    if (!file_exists(DOMAINS_FILE)) return [];
    $data = json_decode(file_get_contents(DOMAINS_FILE), true);
    return is_array($data) ? $data : [];
}
function save_domains(array $domains): void {
    if (!is_dir(CONF_DIR)) mkdir(CONF_DIR, 0755, true);
    file_put_contents(DOMAINS_FILE, json_encode($domains, JSON_PRETTY_PRINT));
}
function find_domain(array $domains, string $domain): int {
    foreach ($domains as $i => $d) {
        if (($d['domain'] ?? '') === $domain) return $i;
    }
    return -1;
}
function binary_version(): string {
    if (!file_exists(BINARY)) return 'binary yok';
    exec(escapeshellarg(BINARY) . ' version 2>&1', $out);
    return $out[0] ?? '?';
}
function next_free_port(array $domains): int {
    $used = array_column($domains, 'port');
    for ($p = 9100; $p < 9200; $p++) {
        if (!in_array($p, $used)) return $p;
    }
    return 9100;
}

$msg = ''; $msg_type = 'info';
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $action = $_POST['action'] ?? '';
    $domains = load_domains();
    if ($action === 'enable') {
        $domain  = trim($_POST['domain'] ?? '');
        $script  = trim($_POST['script'] ?? '/var/www/vhosts/' . $domain . '/httpdocs/index.lk');
        $workers = (int)($_POST['workers'] ?? 4);
        $mode    = in_array($_POST['mode'] ?? 'fcgi', ['fcgi','http']) ? $_POST['mode'] : 'fcgi';
        $port    = (int)($_POST['port'] ?? next_free_port($domains));
        if (!preg_match('/^[a-zA-Z0-9._-]+$/', $domain)) {
            $msg = 'Gecersiz domain adi.'; $msg_type = 'error';
        } else {
            $res = run_script('enable.sh', [$domain, $script, (string)$workers, $mode, (string)$port]);
            if ($res['ok']) {
                $idx = find_domain($domains, $domain);
                $entry = compact('domain','script','workers','mode','port');
                if ($idx >= 0) $domains[$idx] = $entry; else $domains[] = $entry;
                save_domains($domains);
                $msg = "Domain $domain etkinlestirildi (port $port)";
            } else {
                $msg = 'Hata: ' . htmlspecialchars($res['out']); $msg_type = 'error';
            }
        }
    } elseif ($action === 'disable') {
        $domain = trim($_POST['domain'] ?? '');
        $res = run_script('disable.sh', [$domain]);
        $idx = find_domain($domains, $domain);
        if ($idx >= 0) { array_splice($domains, $idx, 1); save_domains($domains); }
        $msg = $res['ok'] ? "Domain $domain devre disi birakildi" : 'Hata: ' . htmlspecialchars($res['out']);
        if (!$res['ok']) $msg_type = 'error';
    } elseif ($action === 'restart') {
        $domain = trim($_POST['domain'] ?? '');
        $idx = find_domain($domains, $domain);
        if ($idx >= 0) {
            $d = $domains[$idx];
            $res = run_script('enable.sh', [$d['domain'], $d['script'], (string)$d['workers'], $d['mode'], (string)$d['port']]);
            $msg = $res['ok'] ? "Domain $domain yeniden baslatildi" : 'Hata: ' . htmlspecialchars($res['out']);
            if (!$res['ok']) $msg_type = 'error';
        }
    }
    $domains = load_domains();
}
$domains = load_domains();
$version = binary_version();
$next_port = next_free_port($domains);
$statuses = [];
foreach ($domains as $d) {
    $res = run_script('status.sh', [$d['domain']]);
    $st = json_decode($res['out'], true);
    $statuses[$d['domain']] = $st ?? ['state' => 'unknown'];
}
?>
<!DOCTYPE html>
<html lang="tr">
<head>
<meta charset="UTF-8">
<title>LOOK Language</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: #f5f7fa; color: #333; }
.header { background: #2c3e50; color: #fff; padding: 18px 30px; display: flex; align-items: center; gap: 14px; }
.header h1 { font-size: 20px; font-weight: 600; }
.ver { font-size: 12px; opacity: .65; margin-left: auto; }
.container { max-width: 960px; margin: 30px auto; padding: 0 20px; }
.card { background: #fff; border-radius: 8px; box-shadow: 0 1px 4px rgba(0,0,0,.1); margin-bottom: 24px; overflow: hidden; }
.card-head { padding: 16px 20px; border-bottom: 1px solid #eee; font-weight: 600; font-size: 14px; }
.card-body { padding: 20px; }
.msg { padding: 12px 16px; border-radius: 6px; margin-bottom: 20px; font-size: 14px; }
.msg.info  { background: #e8f5e9; color: #2e7d32; border-left: 4px solid #4caf50; }
.msg.error { background: #fdecea; color: #c62828; border-left: 4px solid #f44336; }
table { width: 100%; border-collapse: collapse; font-size: 13px; }
th { background: #f9fafb; padding: 10px 12px; text-align: left; font-weight: 600; color: #555; border-bottom: 2px solid #eee; }
td { padding: 10px 12px; border-bottom: 1px solid #f0f0f0; vertical-align: middle; }
.badge { display: inline-block; padding: 2px 8px; border-radius: 12px; font-size: 11px; font-weight: 600; }
.badge.active   { background: #e8f5e9; color: #2e7d32; }
.badge.inactive { background: #fff3e0; color: #e65100; }
.badge.unknown  { background: #f3f4f6; color: #666; }
.btn { display: inline-block; padding: 6px 14px; border-radius: 5px; border: none; cursor: pointer; font-size: 13px; font-weight: 500; }
.btn-primary { background: #2196f3; color: #fff; }
.btn-danger  { background: #f44336; color: #fff; }
.btn-warning { background: #ff9800; color: #fff; }
.btn:hover { opacity: .88; }
form.inline { display: inline; }
.form-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 14px; }
.form-group label { display: block; font-size: 12px; font-weight: 600; color: #555; margin-bottom: 5px; }
.form-group input, .form-group select { width: 100%; padding: 8px 10px; border: 1px solid #ddd; border-radius: 5px; font-size: 13px; }
</style>
</head>
<body>
<div class="header">
    <img src="icon.png" width="28" height="28" alt="">
    <h1>LOOK Language</h1>
    <span class="ver">lk <?php echo htmlspecialchars($version); ?></span>
</div>
<div class="container">
<?php if ($msg): ?>
<div class="msg <?php echo $msg_type; ?>"><?php echo $msg; ?></div>
<?php endif; ?>

<div class="card">
    <div class="card-head">Aktif Domainler</div>
    <div class="card-body">
    <?php if (empty($domains)): ?>
        <p style="color:#888;font-size:13px;">Henuz domain eklenmedi.</p>
    <?php else: ?>
    <table>
        <tr><th>Domain</th><th>Port</th><th>Mod</th><th>Durum</th><th>Uptime</th><th>RAM</th><th>Islemler</th></tr>
        <?php foreach ($domains as $d):
            $st = $statuses[$d['domain']] ?? ['state'=>'unknown'];
            $state = $st['state'] ?? 'unknown';
            $badge = ($state === 'active') ? 'active' : (($state === 'inactive') ? 'inactive' : 'unknown');
        ?>
        <tr>
            <td><strong><?php echo htmlspecialchars($d['domain']); ?></strong><br>
                <small style="color:#888"><?php echo htmlspecialchars($d['script']); ?></small></td>
            <td><?php echo (int)$d['port']; ?></td>
            <td><?php echo htmlspecialchars($d['mode'] ?? 'fcgi'); ?></td>
            <td><span class="badge <?php echo $badge; ?>"><?php echo $state; ?></span></td>
            <td><?php echo htmlspecialchars($st['uptime'] ?? '-'); ?></td>
            <td><?php echo htmlspecialchars($st['ram'] ?? '-'); ?></td>
            <td>
                <form class="inline" method="post">
                    <input type="hidden" name="action" value="restart">
                    <input type="hidden" name="domain" value="<?php echo htmlspecialchars($d['domain']); ?>">
                    <button class="btn btn-warning" type="submit">Yeniden Baslat</button>
                </form>
                &nbsp;
                <form class="inline" method="post">
                    <input type="hidden" name="action" value="disable">
                    <input type="hidden" name="domain" value="<?php echo htmlspecialchars($d['domain']); ?>">
                    <button class="btn btn-danger" type="submit">Durdur</button>
                </form>
            </td>
        </tr>
        <?php endforeach; ?>
    </table>
    <?php endif; ?>
    </div>
</div>

<div class="card">
    <div class="card-head">Domain Ekle / Guncelle</div>
    <div class="card-body">
    <form method="post">
        <input type="hidden" name="action" value="enable">
        <div class="form-grid">
            <div class="form-group">
                <label>Domain Adi</label>
                <input type="text" name="domain" placeholder="api.ornek.com" required>
            </div>
            <div class="form-group">
                <label>Script Yolu (.lk dosyasi)</label>
                <input type="text" name="script" placeholder="/var/www/vhosts/ornek.com/api.ornek.com/httpdocs/index.lk" required>
            </div>
            <div class="form-group">
                <label>Calisan Sayisi</label>
                <input type="number" name="workers" value="4" min="1" max="32">
            </div>
            <div class="form-group">
                <label>Mod</label>
                <select name="mode">
                    <option value="fcgi">FastCGI</option>
                    <option value="http">HTTP Proxy</option>
                </select>
            </div>
            <div class="form-group">
                <label>Port</label>
                <input type="number" name="port" value="<?php echo $next_port; ?>" min="1024" max="65535">
            </div>
        </div>
        <br>
        <button class="btn btn-primary" type="submit">Etkinlestir</button>
    </form>
    </div>
</div>
</div>
</body>
</html>
