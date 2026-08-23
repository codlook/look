const cluster = require('cluster');
const http = require('http');
if (cluster.isPrimary) {
  for (let i = 0; i < 2; i++) cluster.fork();
} else {
  const body = JSON.stringify({ ok: true, msg: "hello" });
  const len = Buffer.byteLength(body);
  http.createServer((req, res) => {
    res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': len });
    res.end(body);
  }).listen(8080, '0.0.0.0', 1024);
}
