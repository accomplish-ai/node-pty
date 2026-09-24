/**
 * Copyright (c) 2017, Daniel Imms (MIT License).
 * Copyright (c) 2018, Microsoft Corporation (MIT License).
 */

import * as fs from 'fs';
import * as assert from 'assert';
import { WindowsTerminal } from './windowsTerminal';
import * as path from 'path';
import * as psList from 'ps-list';

interface IProcessState {
  // Whether the PID must exist or must not exist
  [pid: number]: boolean;
}

interface IWindowsProcessTreeResult {
  name: string;
  pid: number;
  ppid: number;
}

function pollForProcessState(desiredState: IProcessState, intervalMs: number = 100, timeoutMs: number = 2000): Promise<void> {
  return new Promise<void>((resolve, reject) => {
    let tries = 0;
    const interval = setInterval(() => {
      psList({ all: true }).then(ps => {
        let success = true;
        const pids = Object.keys(desiredState).map(k => parseInt(k, 10));
        console.log('expected pids', JSON.stringify(pids));
        pids.forEach(pid => {
          if (desiredState[pid]) {
            if (!ps.some(p => p.pid === pid)) {
              console.log(`pid ${pid} does not exist`);
              success = false;
            }
          } else {
            if (ps.some(p => p.pid === pid)) {
              console.log(`pid ${pid} still exists`);
              success = false;
            }
          }
        });
        if (success) {
          clearInterval(interval);
          resolve();
          return;
        }
        tries++;
        if (tries * intervalMs >= timeoutMs) {
          clearInterval(interval);
          const processListing = pids.map(k => `${k}: ${desiredState[k]}`).join('\n');
          assert.fail(`Bad process state, expected:\n${processListing}`);
        }
      }).catch(error => {
        clearInterval(interval);
        reject(error);
      });
    }, intervalMs);
  });
}

function pollForProcessTreeSize(pid: number, size: number, intervalMs: number = 100, timeoutMs: number = 2000): Promise<IWindowsProcessTreeResult[]> {
  return new Promise<IWindowsProcessTreeResult[]>((resolve, reject) => {
    let tries = 0;
    const interval = setInterval(() => {
      psList({ all: true }).then(ps => {
        const openList: IWindowsProcessTreeResult[] = [];
        openList.push(ps.filter(p => p.pid === pid).map(p => {
          return { name: p.name, pid: p.pid, ppid: p.ppid };
        })[0]);
        const list: IWindowsProcessTreeResult[] = [];
        while (openList.length) {
          const current = openList.shift()!;
          ps.filter(p => p.ppid === current.pid).map(p => {
            return { name: p.name, pid: p.pid, ppid: p.ppid };
          }).forEach(p => openList.push(p));
          list.push(current);
        }
        console.log('list', JSON.stringify(list));
        const success = list.length === size;
        if (success) {
          clearInterval(interval);
          resolve(list);
          return;
        }
        tries++;
        if (tries * intervalMs >= timeoutMs) {
          clearInterval(interval);
          assert.fail(`Bad process state, expected: ${size}, actual: ${list.length}`);
        }
      }).catch(error => {
        clearInterval(interval);
        reject(error);
      });
    }, intervalMs);
  });
}

if (process.platform === 'win32') {
  describe('Windows process polling', () => {
    it('rejects when the requested process tree never appears', async function () {
      this.timeout(2000);
      await assert.rejects(pollForProcessTreeSize(process.pid, 0, 100, 200), /Bad process state/);
    });
    it('rejects when a required process exit never occurs', async function () {
      this.timeout(2000);
      await assert.rejects(pollForProcessState({ [process.pid]: false }, 100, 200), /Bad process state/);
    });
  });
  [[false, false], [true, false], [true, true]].forEach(([useConpty, useConptyDll]) => {
    describe(`WindowsTerminal (useConpty = ${useConpty}, useConptyDll = ${useConptyDll})`, () => {
      describe('kill', () => {
        it('should not crash parent process', function (done) {
          this.timeout(20000);
          const term = new WindowsTerminal('cmd.exe', [], { useConpty, useConptyDll });
          term.on('exit', () => done());
          term.kill();
        });
        it('should kill the process tree', async function (): Promise<void> {
          this.timeout(20000);
          const fixture = path.resolve(__dirname, '..', 'fixtures', 'process-tree.js');
          const term = new WindowsTerminal(process.execPath, [fixture, '2'], { useConpty, useConptyDll });
          let killRequested = false;
          try {
            const list = await pollForProcessTreeSize(term.pid, 3, 500, 5000);
            for (const entry of list) {
              assert.strictEqual(entry.name.toLowerCase(), path.basename(process.execPath).toLowerCase());
            }
            assert.strictEqual(list[0].pid, term.pid);
            assert.strictEqual(list[1].ppid, list[0].pid);
            assert.strictEqual(list[2].ppid, list[1].pid);
            const desiredState: IProcessState = {};
            desiredState[list[0].pid] = false;
            desiredState[list[1].pid] = false;
            desiredState[list[2].pid] = false;
            const exited = new Promise<void>(resolve => term.once('exit', resolve));
            killRequested = true;
            term.kill();
            await exited;
            await pollForProcessState(desiredState, 1000, 5000);
          } finally {
            if (!killRequested) {
              term.kill();
            }
          }
        });
      });

      describe('resize', () => {
        it('should throw a non-native exception when resizing an invalid value', function(done) {
          this.timeout(20000);
          const term = new WindowsTerminal('cmd.exe', [], { useConpty, useConptyDll });
          assert.throws(() => term.resize(-1, -1));
          assert.throws(() => term.resize(0, 0));
          assert.doesNotThrow(() => term.resize(1, 1));
          term.on('exit', () => {
            done();
          });
          term.kill();
        });
        it('should throw a non-native exception when resizing a killed terminal', function(done) {
          this.timeout(20000);
          const term = new WindowsTerminal('cmd.exe', [], { useConpty, useConptyDll });
          (<any>term)._defer(() => {
            term.once('exit', () => {
              assert.throws(() => term.resize(1, 1));
              done();
            });
            term.destroy();
          });
        });
      });

      describe('Args as CommandLine', () => {
        it('should not fail running a file containing a space in the path', function (done) {
          this.timeout(10000);
          const spaceFolder = path.resolve(__dirname, '..', 'fixtures', 'space folder');
          if (!fs.existsSync(spaceFolder)) {
            fs.mkdirSync(spaceFolder);
          }

          const cmdCopiedPath = path.resolve(spaceFolder, 'cmd.exe');
          const data = fs.readFileSync(`${process.env.windir}\\System32\\cmd.exe`);
          fs.writeFileSync(cmdCopiedPath, data);

          if (!fs.existsSync(cmdCopiedPath)) {
            // Skip test if git bash isn't installed
            return;
          }
          const term = new WindowsTerminal(cmdCopiedPath, '/c echo "hello world"', { useConpty, useConptyDll });
          let result = '';
          term.on('data', (data) => {
            result += data;
          });
          term.on('exit', () => {
            assert.ok(result.indexOf('hello world') >= 1);
            done();
          });
        });
      });

      describe('env', () => {
        it('should set environment variables of the shell', function (done) {
          this.timeout(10000);
          const term = new WindowsTerminal('cmd.exe', '/C echo %FOO%', { useConpty, useConptyDll, env: { FOO: 'BAR' }});
          let result = '';
          term.on('data', (data) => {
            result += data;
          });
          term.on('exit', () => {
            assert.ok(result.indexOf('BAR') >= 0);
            done();
          });
        });
      });

      describe('On close', () => {
        it('should return process zero exit codes', function (done) {
          this.timeout(10000);
          const term = new WindowsTerminal('cmd.exe', '/C exit', { useConpty, useConptyDll });
          term.on('exit', (code) => {
            assert.strictEqual(code, 0);
            done();
          });
        });

        it('should return process non-zero exit codes', function (done) {
          this.timeout(10000);
          const term = new WindowsTerminal('cmd.exe', '/C exit 2', { useConpty, useConptyDll });
          term.on('exit', (code) => {
            assert.strictEqual(code, 2);
            done();
          });
        });
      });

      describe('Write', () => {
        it('should accept input', function (done) {
          this.timeout(10000);
          const term = new WindowsTerminal('cmd.exe', '', { useConpty, useConptyDll });
          term.write('exit\r');
          term.on('exit', () => {
            done();
          });
        });
      });
    });
  });
}
