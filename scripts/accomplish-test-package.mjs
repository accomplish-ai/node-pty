// Test installed archive bytes, including package lifecycle and executable modes.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { cpSync, mkdtempSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import { basename, delimiter, join, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';

const source = process.cwd();
const metadataPath = resolve(process.argv[2] ?? 'test-package/publication.json');
const metadata = JSON.parse(readFileSync(metadataPath, 'utf8'));
const archive = resolve('test-package', basename(metadata.archive));
assert.equal(createHash('sha256').update(readFileSync(archive)).digest('hex'), metadata.sha256);
assert.ok(process.env.npm_execpath, 'Run via npm run test:package');
const consumer = mkdtempSync(join(tmpdir(), 'node-pty-packed-'));
writeFileSync(join(consumer, 'package.json'), JSON.stringify({ name: 'qualified-package-consumer', version: '1.0.0', private: true }));

function run(args, cwd = source, env = process.env) {
  const result = spawnSync(process.execPath, args, { cwd, env, stdio: 'inherit', timeout: 600_000 });
  assert.ifError(result.error);
  assert.equal(result.status, 0, `Child failed: ${args.join(' ')}`);
}

run([process.env.npm_execpath, 'install', '--no-audit', '--no-fund', '--save-exact', archive], consumer);
const installed = join(consumer, 'node_modules', '@accomplish-ai', 'node-pty');
const installedPackage = JSON.parse(readFileSync(join(installed, 'package.json'), 'utf8'));
assert.equal(installedPackage.version, metadata.version);
const provenance = JSON.parse(readFileSync(join(installed, 'accomplish-build-provenance.json'), 'utf8'));
for (const [name, expected] of Object.entries(provenance.files)) {
  assert.equal(createHash('sha256').update(readFileSync(join(installed, name))).digest('hex'), expected.sha256,
    `Package installation changed qualified implementation bytes: ${name}`);
}
const pty = createRequire(import.meta.url)(installed);
assert.equal(typeof pty.spawn, 'function');

// npm excludes upstream tests/fixtures. Add only their independently compiled
// test files to the disposable install; all implementation/native files remain
// the bytes from the assembled archive.
for (const name of readdirSync(join(source, 'lib')).filter(name => name.endsWith('.test.js'))) {
  cpSync(join(source, 'lib', name), join(installed, 'lib', name));
}
cpSync(join(source, 'fixtures'), join(installed, 'fixtures'), { recursive: true });
const testEnvironment = {
  ...process.env,
  NODE_ENV: 'test',
  NODE_PATH: [join(source, 'node_modules'), process.env.NODE_PATH].filter(Boolean).join(delimiter)
};
run([join(source, 'node_modules', 'mocha', 'bin', 'mocha.js'), '-R', 'spec', '--exit', 'lib/*.test.js'], installed, testEnvironment);
if (process.platform === 'win32') {
  // This harness requires natural owner exit and does not force the event loop.
  const harness = [join(source, 'scripts', 'windows-cleanup-regression.mjs'), installed];
  run(harness, source, { ...process.env, NODE_PTY_USE_CONPTY_DLL: '0' });
  run(harness, source, { ...process.env, NODE_PTY_USE_CONPTY_DLL: '1' });
}
console.log(JSON.stringify({ outcome: 'passed', version: metadata.version, sha256: metadata.sha256, platform: process.platform, arch: process.arch, node: process.version }));
