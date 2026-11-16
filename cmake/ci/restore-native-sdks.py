"""Restore explicitly selected published SDKs before invoking CMake presets."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile


def run(*args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


def version(name):
    value = os.environ[name]
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?', value):
        raise ValueError(f'{name} must be an exact release version without v')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rid', required=True, choices=PROFILES)
    parser.add_argument('--host-rid', choices=HOST_RIDS)
    parser.add_argument('--profile', choices=SDK_PROFILES)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    profile = args.profile or PROFILES[args.rid][0]
    if SDK_PROFILES[profile][0] != args.rid:
        raise ValueError('The SDK profile must match the target RID')
    triplet = SDK_PROFILES[profile][1]
    host_rid = args.host_rid or args.rid
    if host_rid not in HOST_RIDS:
        raise ValueError('Cross builds require --host-rid for the generator host')
    versions = {repo: version(key) for repo, key in VERSIONS.items()}
    infra = Path(os.environ['VCPKG_CACHE_REPOSITORY_ROOT']).resolve()
    if run('git', '-C', infra, 'rev-parse', 'HEAD') != run(
            'git', '-C', infra, 'rev-parse', 'v' + versions['vcpkg-cache'] + '^{commit}'):
        raise ValueError('The infrastructure checkout must match VCPKG_CACHE_VERSION')
    if run('git', '-C', infra, 'status', '--porcelain'):
        raise ValueError('The selected infrastructure checkout has local changes')
    spec = importlib.util.spec_from_file_location(
        'archive_transport', infra / 'scripts/setup-compiler-toolchain.py')
    transport = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(transport)
    destination = root / 'build/dependencies'
    archives = destination / 'archives'
    archives.mkdir(parents=True, exist_ok=True)
    environment = {key: versions[repo] for repo, key in VERSIONS.items()}
    environment.update(VCPKG_TARGET_TRIPLET=triplet,
                       VCPKG_CACHE_REPOSITORY_ROOT=infra.as_posix())
    # CI supplies the released curl cache policy; local restores retain the
    # read-only NuGet feed and writable default cache unless explicitly set.
    environment['VCPKG_BINARY_SOURCES'] = os.environ.get('VCPKG_BINARY_SOURCES',
        'clear;default,readwrite;nugettimeout,1800;nugetconfig,'
        + (root / 'cmake/vcpkg-cache.nuget.config').as_posix() + ',read')
    if args.rid == 'android-arm64-v8a':
        environment['ANDROID_ABI'] = 'arm64-v8a'
    releases = {}

    def release(repo):
        if repo not in releases:
            selected = json.loads(run('gh', 'api',
                f'repos/qigao/{repo}/releases/tags/v{versions[repo]}'))
            if selected['draft'] or selected['tag_name'] != 'v' + versions[repo]:
                raise ValueError('Expected an exact published release: ' + repo)
            releases[repo] = selected
        return releases[repo]

    def asset(repo, name):
        items = [a for a in release(repo)['assets'] if a['name'] == name]
        if len(items) != 1 or not re.fullmatch(r'sha256:[0-9a-f]{64}', items[0].get('digest', '')):
            raise ValueError(f'Missing unique SHA-256-qualified asset: {repo}/{name}')
        return items[0]

    def download(repo, name):
        item = asset(repo, name)
        archive = archives / name
        if not archive.exists():
            transport.verified_download(item['browser_download_url'], archive,
                                        item['digest'][7:], transport.MAX_ARCHIVE)
        with archive.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != item['digest'][7:] or archive.stat().st_size != item['size']:
            raise ValueError('Release archive size/digest mismatch: ' + name)
        return archive, item

    def sdk(repo, prefix, config, selected_profile):
        name = f'{repo}-{versions[repo]}-{selected_profile}.tar'
        if repo == 'salts-base':
            metadata, _ = download(repo, f'{repo}-{versions[repo]}-manifest.json')
            manifest = json.loads(metadata.read_text(encoding='utf-8-sig'))
            producer, = [s for s in manifest['sdks'] if s['platform'] == selected_profile]
            item = asset(repo, name)
            if (manifest['schemaVersion'] != 1 or manifest['name'] != 'SaltsBase'
                    or manifest['repository'] != 'https://github.com/qigao/salts-base'
                    or manifest['version'] != versions[repo]
                    or manifest['tag'] != release(repo)['tag_name']
                    or manifest['dependencies']['release'] != 'v' + versions['vcpkg-cache']
                    or manifest['ci']['conclusion'] != 'success'
                    or not manifest['ci']['runId'] or producer['file'] != name
                    or producer['triplet'] != triplet
                    or producer['sizeBytes'] != item['size']
                    or 'sha256:' + producer['sha256'] != item['digest']):
                raise ValueError('SaltsBase manifest does not qualify the selected SDK')
        elif repo == 'cmeta':
            metadata, _ = download(repo, f'{repo}-{versions[repo]}-manifest.json')
            manifest = json.loads(metadata.read_text(encoding='utf-8-sig'))
            producer, = [s for s in manifest['sdks'] if s['platform'] == selected_profile]
            item = asset(repo, name)
            if (manifest['schemaVersion'] != 1 or manifest['name'] != 'cmeta'
                    or manifest['version'] != versions[repo]
                    or manifest['repository'] != 'https://github.com/qigao/cmeta'
                    or manifest['ci']['conclusion'] != 'success'
                    or producer['file'] != name or producer['sizeBytes'] != item['size']
                    or 'sha256:' + producer['sha256'] != item['digest']
                    or manifest['dependencies']['vcpkg-cache'] != versions['vcpkg-cache']):
                raise ValueError('CMeta manifest does not qualify the selected SDK')
            required_base = manifest['dependencies']['SaltsBase']
            if required_base != versions['salts-base']:
                raise ValueError(f'CMeta {versions[repo]} release requires SaltsBase '
                                 f'{required_base}; selected {versions["salts-base"]}. '
                                 'A matching published CMeta SDK is required.')
        elif repo == 'chttp':
            metadata, _ = download(repo, f'{repo}-{versions[repo]}-manifest.json')
            manifest = json.loads(metadata.read_text(encoding='utf-8-sig'))
            producer, = [s for s in manifest['sdks'] if s['profile'] == selected_profile]
            item = asset(repo, name)
            dependencies = manifest['dependencies']
            # CMeta publishes SameMinorVersion compatibility. A selected patch
            # upgrade must still match Base exactly via its own release manifest.
            cmeta_version = tuple(map(int, versions['cmeta'].split('.')))
            cmeta_minimum = tuple(map(int, dependencies['CMeta'].split('.')))
            run_info = json.loads(run('gh', 'api',
                f'repos/qigao/chttp/actions/runs/{manifest["producer_run"]}'))
            if (manifest['schema'] != 1 or manifest['repository'] != 'qigao/chttp'
                    or manifest['cmake_package'] != 'Chttp'
                    or manifest['version'] != versions[repo]
                    or run_info['conclusion'] != 'success'
                    or run_info['head_sha'] != manifest['commit']
                    or producer['asset'] != name or producer['sizeBytes'] != item['size']
                    or 'sha256:' + producer['sha256'] != item['digest']
                    or producer['sdk_manifest']['profile'] != selected_profile
                    or cmeta_version[:2] != cmeta_minimum[:2]
                    or cmeta_version < cmeta_minimum
                    or any(dependencies[key] != versions[dependency] for key, dependency in (
                        ('SaltsBase', 'salts-base'),
                        ('SaltsCNet', 'salts-cnet'), ('SaltsUtils', 'salts-utils'),
                        ('vcpkg-cache', 'vcpkg-cache')))):
                raise ValueError('CHttp manifest does not qualify the selected SDK/dependency set')
        archive, item = download(repo, name)
        target = destination / f'{repo}-{versions[repo]}-{selected_profile}'
        receipt = dict(repository=repo, version=versions[repo], profile=selected_profile,
                       sha256=item['digest'][7:])
        if target.exists():
            if json.loads((target / 'receipt.json').read_text()) != receipt:
                raise ValueError('SDK receipt mismatch; choose a fresh destination: ' + str(target))
        else:
            with tempfile.TemporaryDirectory(dir=destination) as temporary:
                staging = Path(temporary) / 'sdk'
                staging.mkdir()
                with tarfile.open(archive) as package:
                    package.extractall(staging, filter='data')
                (staging / 'receipt.json').write_text(json.dumps(receipt) + '\n')
                staging.rename(target)
        if not (target / config).is_file():
            raise ValueError('Incomplete SDK: ' + str(target / config))
        environment[prefix + '_ROOT'] = target.as_posix()
        print(f'Restored {repo} {versions[repo]} ({selected_profile})', file=sys.stderr)

    sdk('salts-base', 'SALTS_BASE', 'lib/cmake/SaltsBase/SaltsBaseConfig.cmake', profile)
    sdk('cmeta', 'CMETA', 'lib/cmake/CMeta/CMetaConfig.cmake', profile)
    sdk('salts-utils', 'SALTS_UTILS', 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake', profile)
    cnet_profile = {'windows-llvm-mingw-x64': 'windows-x64',
                    'linux-clang-x64': 'linux-x64', 'linux-clang-arm64': 'linux-arm64',
                    'macos-clang-arm64': 'macos-arm64'}.get(profile, profile)
    sdk('salts-cnet', 'SALTSCORE', 'lib/cmake/SaltsCNet/SaltsCNetConfig.cmake', cnet_profile)
    sdk('chttp', 'CHTTP', 'lib/cmake/Chttp/ChttpConfig.cmake', profile)
    helper = infra / 'scripts/restore-native-dependency.py'
    os.environ['VCPKG_TARGET_TRIPLET'] = triplet
    lock = destination / f'vcpkg-cache-{triplet}.lock.json'
    run(sys.executable, helper, 'resolve-release', root / 'cmake/dependencies/vcpkg-cache.json',
        '--output-lock', lock)
    receipt = json.loads(run(sys.executable, helper, 'locked', lock,
        '--destination', destination / f'vcpkg-cache-{versions["vcpkg-cache"]}-{triplet}',
        '--payload', 'vcpkg_installed', '--require', f'{triplet}/include/zstd.h', '--reuse'))
    environment['VCPKG_INSTALLED_DIR'] = receipt['root']

    def package(repo, package_id, selected_version, payload, required, prefix, executable=False):
        name = f'{package_id}.{selected_version}.nupkg'
        archive, item = download(repo, name)
        lock = destination / (name + '.lock.json')
        lock.write_text(json.dumps({'schema': 1, 'sha256': item['digest'][7:], 'source': {
            'kind': 'package', 'feed': 'https://nuget.pkg.github.com/qigao/index.json',
            'id': package_id, 'version': selected_version}}) + '\n')
        command = [sys.executable, helper, 'locked', lock, '--archive', archive,
            '--destination', destination / f'{package_id}-{selected_version}-{payload.replace("/", "-")}',
            '--payload', payload, '--require', required, '--reuse']
        if executable:
            command += ['--executable', required]
        receipt = json.loads(run(*command))
        environment[prefix] = receipt['root']

    package('vcpkg-cache', 'Qigao.Re2c.Binary', '4.6.7', 'tools/' + host_rid,
            'bin/re2c.exe' if host_rid.startswith('windows') else 'bin/re2c', 'RE2C_ROOT', True)
    package('vcpkg-cache', 'Qigao.Lemon.Binary', '1.0.0', 'tools/' + host_rid,
            'bin/lemon.exe' if host_rid.startswith('windows') else 'bin/lemon', 'LEMON_ROOT', True)
    output = root / 'build/native-sdk-environment.json'
    output.write_text(json.dumps(environment, indent=2) + '\n')
    print(json.dumps(environment))


VERSIONS = {'vcpkg-cache': 'VCPKG_CACHE_VERSION', 'salts-base': 'SALTS_BASE_VERSION',
            'cmeta': 'CMETA_VERSION', 'salts-utils': 'SALTS_UTILS_VERSION',
            'salts-cnet': 'SALTS_CORE_VERSION', 'chttp': 'CHTTP_VERSION'}
PROFILES = {'windows-x64': ('windows-msvc-x64', 'x64-windows'),
            'linux-x64': ('linux-clang-x64', 'x64-linux'),
            'linux-arm64': ('linux-clang-arm64', 'arm64-linux'),
            'macos-arm64': ('macos-clang-arm64', 'arm64-osx'),
            'android-arm64-v8a': ('android-arm64', 'arm64-android')}
HOST_RIDS = ('windows-x64', 'linux-x64', 'linux-arm64', 'macos-arm64')
SDK_PROFILES = {profile: (rid, triplet) for rid, (profile, triplet) in PROFILES.items()}
SDK_PROFILES.update({'windows-llvm-mingw-x64': ('windows-x64', 'qigao-x64-windows-llvm-mingw'),
                     'linux-gcc-x64': ('linux-x64', 'x64-linux')})

if __name__ == '__main__':
    main()
