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
}
