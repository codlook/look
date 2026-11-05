// WebSocket echo on Node 22 with the `ws` package, one process per CPU (cluster ×2), production defaults.
const cluster = require('cluster');
const http = require('http');
const { WebSocketServer } = require('ws');
if (cluster.isPrimary) {
  for (let i = 0; i < 2; i++) cluster.fork();
} else {
  const server = http.createServer((req, res) => { res.end('ok'); });
  const wss = new WebSocketServer({ server, path: '/ws', perMessageDeflate: false });
  wss.on('connection', (ws) => { ws.on('message', (m, isBinary) => ws.send(m, { binary: isBinary })); });
  server.listen(8080, '0.0.0.0', 4096);
}
