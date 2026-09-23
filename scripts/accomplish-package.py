"""Assemble a test-only package from qualified native CI and pinned upstream bytes.

Runs on the Linux publication runner; archive helpers are also tested on Windows.
No Git tags, stable versions, moving dependency pins, or public npm publication.
"""
import argparse
import base64
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import urllib.request

REPOSITORY = 'accomplish-ai/node-pty'
UPSTREAM_COMMIT = '1def5774632305246fe21f0f69e23a664d6c5910'
UPSTREAM_INTEGRITY = 'sha512-20JqtutY6JPXTUnL0ij1uad7Qe1baT46lyolh2sSENDd4sTzKZ4nmAFkeAARDKwmlLjPx6XKRlwRUxwjOy+lUg=='
WINDOWS_REQUIRED = ('conpty.node', 'conpty_console_list.node', 'pty.node', 'winpty-agent.exe', 'winpty.dll', 'conpty/conpty.dll', 'conpty/OpenConsole.exe')
PROVENANCE = 'accomplish-build-provenance.json'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def integrity(data):
    return 'sha512-' + base64.b64encode(hashlib.sha512(data).digest()).decode()


def validate_version(version):
    require(re.fullmatch(r'1\.1\.1-test\.[1-9][0-9]*\.[1-9][0-9]*\.[a-f0-9]{8}', version), 'Expected immutable test prerelease version')


def read_archive(path):
    result = {}
    with tarfile.open(path, 'r:gz') as archive:
        for entry in archive:
            name = PurePosixPath(entry.name)
            require(name.parts and name.parts[0] == 'package' and '..' not in name.parts and '\\' not in entry.name, 'Unsafe archive path')
            if entry.isdir():
                continue
            require(entry.isfile() and len(name.parts) > 1, 'Archive contains a non-file entry')
            relative = '/'.join(name.parts[1:])
            require(relative not in result, 'Duplicate archive entry')
            result[relative] = (archive.extractfile(entry).read(), entry.mode & 0o777)
    return result


def write_archive(path, entries):
    # Fixed archive order, timestamps, and ownership make identical inputs reproducible.
    with open(path, 'wb') as file:
        with gzip.GzipFile(filename='', mode='wb', fileobj=file, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w', format=tarfile.PAX_FORMAT) as archive:
                for name, (data, mode) in sorted(entries.items()):
                    entry = tarfile.TarInfo('package/' + name)
                    entry.size, entry.mode = len(data), mode
                    archive.addfile(entry, io.BytesIO(data))


def validate_payload(entries):
    require(not any(name.startswith('build/') for name in entries), 'Forbidden build/ payload can shadow qualified prebuilds')
    for name in WINDOWS_REQUIRED:
        require(f'prebuilds/win32-x64/{name}' in entries, f'Missing Windows native payload: {name}')


def assemble(candidate, original, output, version, provenance):
    validate_version(version)
    entries, upstream = read_archive(candidate), read_archive(original)
    validate_payload(entries)
    package = json.loads(entries['package.json'][0])
    require(package['name'] == 'node-pty' and package['version'] == '1.1.0', 'Unexpected candidate package identity')
    # The preserved macOS binaries are safe only while their native sources are unchanged.
    # Windows Git checkouts can use CRLF without changing the native source.
    unix = {name: value[0].replace(b'\r\n', b'\n') for name, value in entries.items() if name.startswith('src/unix/')}
    upstream_unix = {name: value[0].replace(b'\r\n', b'\n') for name, value in upstream.items() if name.startswith('src/unix/')}
    require(unix and unix == upstream_unix, 'Unix native source differs from the preserved upstream prebuild')
    entries = {name: value for name, value in entries.items() if not name.startswith('prebuilds/') or name.startswith('prebuilds/win32-x64/')}
    for arch in ('arm64', 'x64'):
        prefix = f'prebuilds/darwin-{arch}/'
        preserved = {name: value for name, value in upstream.items() if name.startswith(prefix)}
        for name in ('pty.node', 'spawn-helper'):
            require(prefix + name in preserved, f'Missing upstream macOS payload: {prefix}{name}')
        # Upstream 1.1.0's registry archive incorrectly records this executable as
        # 0644. Preserve its bytes and explicitly repair the executable mode.
        helper = prefix + 'spawn-helper'
        preserved[helper] = (preserved[helper][0], 0o755)
        entries.update(preserved)
    package.update(name='@accomplish-ai/node-pty', version=version,
                   repository={'type': 'git', 'url': 'https://github.com/accomplish-ai/node-pty.git'},
                   publishConfig={'registry': 'https://npm.pkg.github.com', 'access': 'restricted'})
    entries['package.json'] = (json.dumps(package, indent=2).encode() + b'\n', 0o644)
    receipt = {**provenance, 'schemaVersion': 1, 'upstreamCommit': UPSTREAM_COMMIT,
               'upstreamIntegrity': UPSTREAM_INTEGRITY, 'packageVersion': version,
               'prebuiltPlatforms': ['darwin-arm64', 'darwin-x64', 'win32-x64'],
               'files': {name: {'sha256': sha256(data), 'mode': mode} for name, (data, mode) in sorted(entries.items())}}
    entries[PROVENANCE] = (json.dumps(receipt, indent=2, sort_keys=True).encode() + b'\n', 0o644)
    write_archive(output, entries)
    verify(output)


def verify(path):
    entries = read_archive(path)
    validate_payload(entries)
    receipt = json.loads(entries.pop(PROVENANCE)[0])
    package = json.loads(entries['package.json'][0])
    validate_version(package['version'])
    require(package['name'] == '@accomplish-ai/node-pty' and receipt['packageVersion'] == package['version'], 'Package identity mismatch')
    require(set(entries) == set(receipt['files']), 'Package checksum file set mismatch')
    require(not any(name.startswith('prebuilds/') and name.split('/')[1] not in ('win32-x64', 'darwin-arm64', 'darwin-x64') for name in entries), 'Unqualified prebuild in package')
    for name, (data, mode) in entries.items():
        require(receipt['files'][name] == {'sha256': sha256(data), 'mode': mode}, f'Package checksum mismatch: {name}')
    return receipt


def validate_run(run, jobs, commit):
    require(run['head_sha'] == commit and run['head_branch'] == 'codex/windows-conpty-cleanup', 'Native run source mismatch')
    require(run['event'] == 'push' and run['status'] == 'completed' and run['conclusion'] == 'success', 'Native run has not passed trusted push qualification')
    require(run['head_repository']['full_name'] == REPOSITORY and run['path'] == '.github/workflows/windows-cleanup-candidate.yml', 'Unexpected native workflow/repository')
    for major in (20, 22):
        matches = [job for job in jobs if job['name'] == f'Windows native cleanup (Node {major})']
        require(len(matches) == 1 and matches[0]['conclusion'] == 'success', f'Missing successful Node {major} native lane')


def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', f'repos/{REPOSITORY}/{path}'], text=True))


def build_from_run(run_id, output_directory):
    require(re.fullmatch(r'[1-9][0-9]*', run_id), 'Invalid native run ID')
    require(os.environ['GITHUB_REPOSITORY'] == REPOSITORY and os.environ['GITHUB_REF'] == 'refs/heads/codex/windows-conpty-cleanup', 'Publication is restricted to the maintained candidate branch')
    commit = os.environ['GITHUB_SHA']
    run = api(f'actions/runs/{run_id}')
    jobs = api(f'actions/runs/{run_id}/attempts/{run["run_attempt"]}/jobs?per_page=100')
    require(jobs['total_count'] == len(jobs['jobs']), 'Unexpected paginated native jobs')
    validate_run(run, jobs['jobs'], commit)
    output = Path(output_directory)
    output.mkdir(parents=True, exist_ok=False)
    receipts = []
    archives = []
    for major in (20, 22):
        artifact_name = f'node-pty-windows-node{major}-{commit}-{run["run_attempt"]}'
        lane = output / f'node{major}'
        subprocess.run(['gh', 'run', 'download', run_id, '--repo', REPOSITORY, '--name', artifact_name, '--dir', str(lane)], check=True)
        receipt = json.loads((lane / 'build-provenance.json').read_text(encoding='utf-8-sig'))
        archives.append(lane / 'node-pty-1.1.0.tgz')
        require(receipt['sourceCommit'] == commit and receipt['upstreamCommit'] == UPSTREAM_COMMIT and str(receipt['runId']) == run_id and str(receipt['runAttempt']) == str(run['run_attempt']), 'Native artifact provenance mismatch')
        require(receipt['platform'] == 'win32' and receipt['architecture'] == 'x64' and receipt['nodeVersion'].startswith(f'v{major}.'), 'Native artifact target mismatch')
        require(sha256(archives[-1].read_bytes()) == receipt['archiveSha256'], 'Native archive checksum mismatch')
        validate_payload(read_archive(archives[-1]))
        receipts.append({'artifactName': artifact_name, **receipt})
    original = output / 'upstream-node-pty-1.1.0.tgz'
    with urllib.request.urlopen('https://registry.npmjs.org/node-pty/-/node-pty-1.1.0.tgz') as response:
        original.write_bytes(response.read())
    require(integrity(original.read_bytes()) == UPSTREAM_INTEGRITY, 'Upstream npm archive integrity mismatch')
    version = f'1.1.1-test.{os.environ["GITHUB_RUN_ID"]}.{os.environ["GITHUB_RUN_ATTEMPT"]}.{commit[:8]}'
    archive = output / f'accomplish-ai-node-pty-{version}.tgz'
    assemble(archives[1], original, archive, version, {'sourceCommit': commit, 'nativeRunId': run_id,
             'nativeRunAttempt': run['run_attempt'], 'nativeArtifacts': receipts,
             'publicationRunId': os.environ['GITHUB_RUN_ID'], 'publicationRunAttempt': os.environ['GITHUB_RUN_ATTEMPT']})
    metadata = {'version': version, 'archive': str(archive.resolve()), 'sha256': sha256(archive.read_bytes()), 'integrity': integrity(archive.read_bytes())}
    (output / 'publication.json').write_text(json.dumps(metadata, indent=2) + '\n')
    with open(os.environ['GITHUB_OUTPUT'], 'a') as file:
        file.write(f'version={version}\narchive={archive.resolve()}\ntag=test-{os.environ["GITHUB_RUN_ID"]}-{os.environ["GITHUB_RUN_ATTEMPT"]}\n')
    print(json.dumps(metadata, indent=2))


def check_registry(metadata_path):
    metadata = json.loads(Path(metadata_path).read_text())
    validate_version(metadata['version'])
    package = '@accomplish-ai/node-pty@' + metadata['version']
    registry = 'https://npm.pkg.github.com'
    published_integrity = json.loads(subprocess.check_output(['npm', 'view', package, 'dist.integrity', '--json', '--registry', registry], text=True))
    require(published_integrity == metadata['integrity'], 'Published registry integrity mismatch')
    destination = Path(metadata_path).parent / 'registry-download'
    destination.mkdir(exist_ok=False)
    downloaded = json.loads(subprocess.check_output(['npm', 'pack', package, '--ignore-scripts', '--json', '--registry', registry, '--pack-destination', str(destination)], text=True))
    require(len(downloaded) == 1, 'Expected one downloaded package')
    filename = downloaded[0]['filename']
    require(Path(filename).name == filename, 'Unsafe downloaded package filename')
    archive = destination / filename
    require(sha256(archive.read_bytes()) == metadata['sha256'], 'Registry archive differs from qualified archive')
    receipt = verify(archive)
    evidence = {'package': package, 'registry': registry, 'integrity': published_integrity,
                'sha256': metadata['sha256'], 'sourceCommit': receipt['sourceCommit'], 'outcome': 'passed'}
    Path(metadata_path).with_name('registry-verification.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(evidence, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-run-id')
    parser.add_argument('--output', default='test-package')
    parser.add_argument('--verify')
    parser.add_argument('--registry-check')
    args = parser.parse_args()
    if args.verify:
        verify(args.verify)
    elif args.registry_check:
        check_registry(args.registry_check)
    else:
        require(args.native_run_id, 'An explicit successful native run ID is required')
        build_from_run(args.native_run_id, args.output)
