# Accomplish Windows terminal cleanup candidate

This fork qualifies the Sandbox Engine's Windows x64 terminal cleanup fix. Its
baseline is node-pty 1.1.0, upstream commit
`1def5774632305246fe21f0f69e23a664d6c5910`. The source package retains the upstream
name/version so the original build and tests continue to work.

## Private test packages

The existing **CI** workflow accepts a manual dispatch on
`codex/windows-conpty-cleanup`. Set `publish_test_package` to `true` and supply a
successful **Windows cleanup candidate** `native_run_id` for that exact commit.
Publication remains disabled by default and cannot run on pull requests.
The Linux, macOS and Windows Server 2022 source test jobs must all pass. The
final assembled archive is then installed on Windows Server 2025 and macOS 14
with Node 20 and Node 22. The upstream tests run against its installed
implementation. Windows also runs the natural-exit cleanup harness twice:
once with the system ConPTY provider and once with the bundled ConPTY DLL.
All four installed-package jobs must pass before publication starts.

Native qualification separately checks both providers on Windows Server 2025
and the bundled provider on Windows Server 2022. Diagnostics found that Server
2022's system provider retains a console-host reference; that provider is not
qualified by these cleanup checks. The Server 2025 evidence does not replace
final acceptance on a clean Windows 11 host.

The assembler checks the native run's repository, source commit, workflow,
successful Node 20 and Node 22 jobs, artifact provenance and archive checksums.
It uses the Node 22 build's Windows x64 prebuilds and preserves the original
node-pty 1.1.0 macOS arm64/x64 prebuild bytes. The original registry archive
records `spawn-helper` as mode 0644; assembly corrects both helpers to executable
mode 0755. Other modes are retained. The
upstream npm archive is pinned by SHA-512, and unchanged Unix native sources
are required. No `build/` payload can shadow the selected prebuilds.

Packages are published privately to GitHub Packages as
`@accomplish-ai/node-pty@1.1.1-test.<publication-run>.<attempt>.g<source-sha8>`.
The `g` prefix keeps an all-numeric SHA fragment valid as a SemVer prerelease
identifier even when it begins with zero.
Each publication has a unique `test-<run>-<attempt>` tag. The workflow creates
no Git tags or stable releases and does not update `latest`.
Every file's checksum and mode, both native build receipts and the source
commit are embedded in `accomplish-build-provenance.json`. The workflow then
downloads the exact registry version and verifies it matches the assembled
archive. Keep the publication receipt with integration evidence.

Engine can keep its existing imports using an exact npm alias:

```json
"node-pty": "npm:@accomplish-ai/node-pty@1.1.1-test.<run>.<attempt>.g<sha8>"
```

Use the actual immutable version returned by a successful publication. Do not
install a moving test tag. Consumers need GitHub Packages read access; CI
publication uses its own scoped `GITHUB_TOKEN`, not a developer's credentials.
The new package's Actions access must explicitly allow the Sandbox Engine and
Agent Runtime repositories to read it. Existing access to other Accomplish
packages does not grant access to this package. After initial publication,
verify an exact-version install using each consumer repository's CI token
before considering cross-repository adoption ready. A developer-token download
or the publisher's registry check does not prove those consumer permissions.

## Supported candidate payloads

The package includes prebuilds for Windows x64 and macOS arm64/x64. It excludes
upstream Windows arm64 binaries because they do not implement the new native
cleanup contract. Other targets retain the upstream source-build fallback;
Windows arm64 is not qualified by this candidate. Linux retains source builds
and the existing Linux CI lane.

This publication step is test-only. A successful package build does not replace
Sandbox integration tests, natural terminal-owner exit and handle-count checks,
or the final clean Windows 11 host acceptance.

## Stable release preparation

The CI workflow accepts `prepare_stable_package=true` on the candidate branch.
This creates version `1.1.1` as a local archive and tests its installation on
Windows and macOS with Node 20 and 22. It does not publish. The source archive
is the qualified immutable test package from run `35889321781`, attempt 2,
pinned by SHA-256. Promotion changes only package metadata and its checksum
receipt. All implementation bytes and modes remain identical. CI rejects
source or dependency changes relative to the qualified implementation.

Set `compare_macos_package=true` to compare that exact qualified terminal
package with upstream 1.1.0 on the same Mac. Each Node version exercises
30 natural exits and 30 terminations per package after warmup. Checks require
output/status, shell cleanup, natural owner exit, and no additional descriptor
growth over upstream. This comparison does not claim upstream has no leaks.

After review, merge, and explicit release approval, dispatch CI on
`accomplish-1.1` with `publish_stable_package=true`. Publication requires the
source tests, archive checks, and all four installed stable package jobs to
pass. It publishes the exact qualified archive privately and verifies registry
bytes. Stable publication is disabled on PRs, pushes, and the candidate branch.
It updates the private package's `latest` tag only during this explicit release.
An existing immutable version cannot be overwritten; inspect registry evidence
before retrying an interrupted publication. Keep the branch until release is
complete. Engine and Runtime must pin the published stable version exactly.

## Local package tests

Run `python scripts/accomplish-package-test.py` with Python 3.12 or later. The
tests create real tar archives and check executable modes, platform selection,
native-source matching, immutable version rules, tampering and unsafe archives.
