const cluster = require('cluster');
const http = require('http');
const mysql = require('mysql2/promise');
if (cluster.isPrimary) {
  for (let i = 0; i < 2; i++) cluster.fork();
} else {
  const pool = mysql.createPool({
    host: process.env.DB_HOST, user: 'root', password: process.env.DB_PASS,
    database: 'bench', connectionLimit: 16, waitForConnections: true, queueLimit: 0,
  });
  // Pre-warm the pool BEFORE listening: open every connection and complete the MySQL
  // auth handshake up front, and tolerate MySQL not being ready yet (retry). Otherwise
  // the first burst of load hits a cold, lazily-connecting pool and the early
  // concurrency levels measure connection setup, not steady-state throughput.
  async function warmup() {
    for (let attempt = 0; attempt < 60; attempt++) {
      try {
        await Promise.all(Array.from({ length: 16 }, () => pool.query('SELECT 1')));
        return;
      } catch (e) {
        await new Promise(r => setTimeout(r, 500));
      }
    }
    throw new Error('MySQL never became reachable');
  }
  warmup().then(() => {
    http.createServer(async (req, res) => {
      try {
        const [rows] = await pool.query("SELECT id, name, email FROM users WHERE id = ?", [42]);
        const body = JSON.stringify(rows);
        res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) });
        res.end(body);
      } catch (e) {
        res.writeHead(500); res.end(JSON.stringify({ error: String(e) }));
      }
    }).listen(8080, '0.0.0.0', 1024);
  }).catch(e => { console.error(e); process.exit(1); });
}
