import http from 'node:http';

const [, , address, portArg] = process.argv;

if (!address || !portArg) {
  console.error(`Usage: node ${process.argv[1]} <address> <port>`);
  process.exit(1);
}

const port = Number.parseInt(portArg, 10);

if (!Number.isInteger(port) || port < 1 || port > 65535) {
  console.error(`Invalid port: ${portArg}`);
  process.exit(1);
}

let total = 0;
let withAddress = 0;
let withoutAddress = 0;
let timerProgress = undefined;
function progress() {
  if (timerProgress) return;

  timerProgress = setTimeout(() => {
    const percent = total ? ((withoutAddress / total) * 100).toFixed(1) : '0.0';

    process.stdout.write(
      `\rTotal: ${total}  ` + `address: ${withAddress}  ` + `undefined: ${withoutAddress}  ` + `failure: ${percent}%`
    );

    if (process.stdout.isTTY) process.stdout.clearLine(1);

    timerProgress = undefined;
  }, 1000);
}

const server = http.createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.end('ok');
});

server.on('connection', (socket) => {
  total++;

  const remoteAddress = socket.remoteAddress;

  if (remoteAddress === undefined) {
    withoutAddress++;
  } else {
    withAddress++;
  }

  progress();
});

server.listen(port, address, () => {
  console.log(`Listening on ${address}:${port}`);
});

process.on('SIGINT', () => {
  console.log(
    `\n\nFinal result:\n` +
      `  Total:     ${total}\n` +
      `  Address:   ${withAddress}\n` +
      `  Undefined: ${withoutAddress}\n`
  );

  server.close(() => process.exit(0));
});
