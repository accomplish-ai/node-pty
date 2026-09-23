const { spawn } = require('node:child_process');
if (process.argv[2] === 'concurrent') {
  process.stdout.write('CONCURRENT_READY\r\n');
  process.stdin.once('data', () => {
    process.stdout.write('CLEANUP_FINAL_OUTPUT\r\n', () => process.exit(7));
  });
} else if (process.argv[2] === 'bulk') {
  const data = Array.from({ length: 20000 }, (_, n) => `DATA:${String(n).padStart(5, '0')}:${'x'.repeat(64)}\r\n`).join('');
  process.stdout.write(data + 'BULK_FINAL_OUTPUT\r\n', () => process.exit(7));
} else if (process.argv[2] === 'descendant') {
  const child = spawn(process.execPath, [__filename, 'idle'], { stdio: 'inherit' });
  process.stdout.write(`DESCENDANT:${child.pid}\r\n`);
  setInterval(() => {}, 1000);
} else {
  process.stdout.write('IDLE_READY\r\n');
  setInterval(() => {}, 1000);
}
