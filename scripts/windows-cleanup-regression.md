# Windows terminal cleanup regression

After the TypeScript and native builds, the regression command for Node 20 or 22
on Windows is:

```powershell
$env:NODE_PTY_USE_CONPTY_DLL = '1'
node scripts/windows-cleanup-regression.mjs .
```

The environment switch selects the existing bundled ConPTY provider for these
tests only. A value of `0` selects the system provider. The library default is
unchanged. Optional scenario names after the package path select individual cases.

The seven scenarios cover natural exit and final exit code, repeated kill before
and after an interactive shell responds to input, all
20,000 ordered output rows, descendant termination, concurrent terminals with a
live resize/input handshake, and Worker termination after a shell has exited but
its native exit callback is still queued. Each scenario runs five rounds and
checks OS handle counts after disposing listeners, allowing cleanup time, and
collecting garbage. Concurrent rounds create eight terminals each. A child must
exit naturally without stderr; a timeout kills only its owned process tree and
fails the test. The shutdown scenario does not qualify terminating a Worker
while its shell is still running.

## Fix

Previously, the native exit waiter removed the baton containing the pseudoconsole
without closing it. JavaScript left the input socket and output worker alive on
natural exit. The kill path enumerated descendants after starting console
teardown, causing helper failures and a five-second fallback.

The native waiter now owns pseudoconsole close for both natural exit and kill.
A synchronized registry and shared baton lifetime protect concurrent operations;
resize and clear cannot use a pseudoconsole once close begins. Close runs off the
JavaScript thread so output can drain. The exit code is delivered before console
close can produce output EOF. Shared notification state survives both the waiter
and queued callback, and the finalizer releases the waiter before joining it.
Raw Node-API status handling avoids throwing back into a terminating Worker.
JavaScript closes input, drains output, and disposes the worker on natural exit;
system-provider kill enumerates descendants first and repeated calls are
idempotent. Bundled-provider kill relies on pseudoconsole close to terminate
attached descendants, preserving its original provider behavior. It does not
start an enumeration helper: a bundled shell can still reject `AttachConsole`
with Windows error 31 during startup before it responds to input. Both early
kill and descendant termination remain mandatory regressions.

## Provider qualification

An independent comparison used the same compiled addon on Windows Server 2022
and 2025 with both providers. The Server 2022 system provider retained one exited
`conhost.exe` process handle per terminal. A direct Windows API control without
node-pty reproduced that growth. The bundled provider on 2022 and both providers
on 2025 had flat repeated handle counts. The comparison and control are recorded
in [run 35880631207](https://github.com/accomplish-ai/node-pty/actions/runs/35880631207).

The bundled provider avoids the retained handle on the older host. The resource
assertion remains strict, and the implementation treats `HPCON` as opaque.
Temporary native handle snapshots used to identify the provider defect are not
part of the package API.
