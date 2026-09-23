// Compare installed terminal behavior against upstream on the same Mac/Node.
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';

assert.equal(process.platform, 'darwin', 'Run this qualification on macOS');
const require = createRequire(import.meta.url);
const cycles = 30;

function descriptorCount() {
  const result = spawnSync('/usr/sbin/lsof', ['-nP', '-a', '-p', String(process.pid), '-F', 'f'], { encoding: 'utf8' });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stderr);
  return result.stdout.split('\n').filter(line => /^f\d/.test(line)).length;
}

async function exercise(packageRoot, scenario) {
  const pty = require(packageRoot);
  const owned = [];
  async function cycle() {
    let output = '';
    const terminal = pty.spawn('/bin/sh', ['-c', scenario === 'natural'
      ? 'printf PTY_COMPARISON_READY; exit 7'
      : 'printf PTY_COMPARISON_READY; while :; do read line; done'], {
      name: 'xterm', cols: 80, rows: 24, cwd: process.cwd(), env: process.env
    });
    owned.push(terminal.pid);
    const done = new Promise((resolveExit, reject) => {
      const watchdog = setTimeout(() => {
        terminal.kill('SIGKILL');
        reject(new Error(`${scenario} terminal did not exit`));
      }, 5000);
      let killed = false;
      const data = terminal.onData(chunk => {
        output += chunk;
        if (scenario === 'kill' && !killed && output.includes('PTY_COMPARISON_READY')) {
          killed = true;
          terminal.kill('SIGHUP');
        }
      });
      const exit = terminal.onExit(event => {
        clearTimeout(watchdog);
        data.dispose();
        exit.dispose();
        resolveExit(event);
      });
    });
    const result = await done;
    assert.ok(output.includes('PTY_COMPARISON_READY'), 'Terminal output was lost');
    if (scenario === 'natural') assert.equal(result.exitCode, 7);
    else assert.equal(result.signal, 1, 'Expected the requested SIGHUP');
    await delay(50);
    assert.throws(() => process.kill(terminal.pid, 0), { code: 'ESRCH' }, 'Owned shell survived exit');
  }
  for (let index = 0; index < 3; index++) await cycle();
  global.gc();
  await delay(200);
  const before = descriptorCount();
  for (let index = 0; index < cycles; index++) await cycle();
  global.gc();
  await delay(200);
  const after = descriptorCount();
  return { scenario, cycles, before, after, growth: after - before, ownedShells: owned.length };
}

if (process.argv[2] === '--child') {
  const result = await exercise(resolve(process.argv[3]), process.argv[4]);
  console.log(JSON.stringify(result));
  // Deliberately no process.exit(): the controller requires natural owner exit.
} else {
  assert.equal(process.argv.length, 4, 'Expected baseline and candidate package directories');
  const results = [];
  for (const scenario of ['natural', 'kill']) {
    const pair = [];
    for (const packageRoot of process.argv.slice(2)) {
      const child = spawnSync(process.execPath, ['--expose-gc', fileURLToPath(import.meta.url), '--child', resolve(packageRoot), scenario], {
        encoding: 'utf8', timeout: 60000, killSignal: 'SIGKILL'
      });
      assert.ifError(child.error);
      assert.equal(child.status, 0, child.stderr);
      assert.equal(child.stderr, '', 'Unexpected terminal diagnostics');
      pair.push(JSON.parse(child.stdout.trim()));
    }
    const [baseline, candidate] = pair;
    results.push({ scenario, baseline, candidate });
    console.log(JSON.stringify({ scenario, baseline, candidate }));
    assert.ok(candidate.growth <= baseline.growth,
      `Descriptor growth regressed: candidate ${candidate.growth}, baseline ${baseline.growth}`);
  }
  console.log(JSON.stringify({ outcome: 'passed', platform: process.platform, arch: process.arch, node: process.version, results }));
}
