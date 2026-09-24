// A terminal owner with a child and grandchild, independent of shell startup.
const { spawn } = require('child_process');
const remaining = Number(process.argv[2]);
if (remaining > 0) {
  spawn(process.execPath, [__filename, String(remaining - 1)], { stdio: 'inherit' })
    .on('error', error => { throw error; });
}
// WindowsTerminal releases deferred operations after the first terminal output.
process.stdout.write('PROCESS_TREE_READY\r\n');
setInterval(() => {}, 1000);
