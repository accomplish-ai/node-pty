// Run with Node 20/22 on Windows. The child must exit without forced process.exit.
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { dirname, join } from 'node:path';
import { Worker } from 'node:worker_threads';

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const [command, packagePath, mode] = process.argv.slice(2);
if (command === 'child') {
  const pty = createRequire(import.meta.url)(packagePath);
  const nativeTrace = process.env.NODE_PTY_TRACE_CLEANUP === '1'
    ? createRequire(import.meta.url)(join(dirname(packagePath), '../build/Release/conpty.node')) : undefined;
  if (nativeTrace) console.log(JSON.stringify({ handlesAtStart: nativeTrace._traceHandles() }));
  const samples = [];
  async function exercise() {
    if (mode === 'shutdown') {
      const worker = new Worker(`
        const { parentPort, workerData } = require('node:worker_threads');
        const pty = require(workerData);
        const terminal = pty.spawn(process.env.ComSpec, ['/d', '/c', 'exit /b 7']);
        parentPort.postMessage(terminal.pid);
        // Hold JS while native exit notification queues, then destroy this env.
        Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0);
      `, { eval: true, workerData: packagePath });
      const shell = await new Promise((resolve, reject) => {
        worker.once('message', resolve);
        worker.once('error', reject);
      });
      let exited = false;
      for (let n = 0; n < 100 && !exited; n++) {
        try { process.kill(shell, 0); await delay(50); }
        catch (error) { assert.equal(error.code, 'ESRCH'); exited = true; }
      }
      assert.ok(exited, 'shell did not exit before worker termination');
      await worker.terminate();
      return;
    }
    const natural = mode === 'natural' || mode === 'concurrent';
    const payload = mode === 'bulk' || mode === 'descendant';
    const terminal = pty.spawn(payload ? process.execPath : process.env.ComSpec, payload
      ? [fileURLToPath(new URL('./windows-cleanup-payload.cjs', import.meta.url)), mode]
      : natural ? ['/d', '/c', 'echo CLEANUP_FINAL_OUTPUT & exit /b 7'] : ['/d', '/q'], { cols: 120, rows: 24 });
    let output = '';
    const data = terminal.onData(value => {
      output += value;
      if (mode === 'concurrent') {
        try { terminal.resize(121, 25); }
        catch (error) { assert.match(error.message, /Cannot resize a pty that has already exited/); }
      }
    });
    let exit;
    const exited = new Promise(resolve => exit = terminal.onExit(resolve));
    let descendant;
    if (mode === 'descendant') {
      for (let n = 0; n < 100 && !output.includes('DESCENDANT:'); n++) { await delay(50); }
      descendant = Number(/DESCENDANT:(\d+)/.exec(output)?.[1]);
      assert.ok(descendant > 0, 'descendant did not start');
    }
    if (!natural && mode !== 'bulk') {
      await delay(500);
      terminal.kill();
      terminal.kill();
    }
    const result = await exited;
    if (natural) {
      assert.equal(result.exitCode, 7);
      assert.ok(output.includes('CLEANUP_FINAL_OUTPUT'), 'final output was lost');
    }
    if (mode === 'bulk') {
      assert.equal(result.exitCode, 7);
      assert.ok(output.includes('BULK_FINAL_OUTPUT'), 'bulk final output was lost');
      const rows = [...output.matchAll(/DATA:(\d{5}):x{64}/g)].map(match => Number(match[1]));
      assert.equal(rows.length, 20000, 'bulk output was truncated');
      rows.forEach((row, index) => assert.equal(row, index, 'bulk output was reordered'));
    }
    if (descendant) {
      let alive = true;
      for (let n = 0; n < 100 && alive; n++) {
        try { process.kill(descendant, 0); await delay(50); }
        catch (error) { assert.equal(error.code, 'ESRCH'); alive = false; }
      }
      assert.equal(alive, false, `descendant ${descendant} survived terminal kill`);
    }
    data.dispose();
    exit.dispose();
  }
  for (let round = 0; round < 5; round++) {
    if (mode === 'concurrent') await Promise.all(Array.from({ length: 8 }, () => exercise()));
    else await exercise();
    await delay(2500);
    for (let n = 0; n < 3; n++) { global.gc(); await delay(50); }
    const count = spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command',
      `(Get-Process -Id ${process.pid}).HandleCount`], { encoding: 'utf8', windowsHide: true });
    assert.equal(count.status, 0, count.stderr);
    const handles = Number(count.stdout.trim());
    assert.ok(handles > 0);
    samples.push(handles);
    if (nativeTrace) console.log(JSON.stringify({ round, nativeHandles: nativeTrace._traceHandles() }));
    console.log(JSON.stringify({ round, handles, resources: process.getActiveResourcesInfo() }));
  }
  // Ignore one-time initialization: no per-terminal native handle growth thereafter.
  assert.ok(Math.max(...samples.slice(1)) - Math.min(...samples.slice(1)) <= 1,
    `native handles grew across disposed terminals: ${samples.join(', ')}`);
} else {
  assert.equal(process.platform, 'win32');
  const entry = resolve(command || '.', 'lib/index.js');
  let failed = false;
  for (const scenario of (process.argv.slice(3).length ? process.argv.slice(3) : ['natural', 'kill', 'bulk', 'descendant', 'concurrent', 'shutdown'])) {
    const child = spawn(process.execPath, ['--expose-gc', fileURLToPath(import.meta.url), 'child', entry, scenario],
      { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '', stderr = '', timedOut = false;
    child.stdout.on('data', data => stdout += data);
    child.stderr.on('data', data => stderr += data);
    const timeout = setTimeout(() => {
      timedOut = true;
      spawnSync('taskkill.exe', ['/pid', String(child.pid), '/t', '/f'], { windowsHide: true, timeout: 5000 });
    }, 45000);
    const code = await new Promise((resolve, reject) => { child.once('error', reject); child.once('close', resolve); });
    clearTimeout(timeout);
    console.log(JSON.stringify({ scenario, code, timedOut, stdout, stderr }));
    failed ||= code !== 0 || timedOut || stderr.length > 0;
  }
  process.exitCode = failed ? 1 : 0;
}
