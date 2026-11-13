"""Prepare published split SDKs once, then restore only verified original TARs.

Base, vcpkg and compiler inputs use the immutable CMeta preparation entry point.
This file supplies CHttp's CNet and Utils release manifests, not a downloader.
CMeta uses the source SDK composition declared by the selected Utils release.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('phase', choices=('prepare', 'restore'))
parser.add_argument('--profile')
parser.add_argument('--prefix', default='')
parser.add_argument('--package', choices=('salts-cnet', 'salts-utils'))
args = parser.parse_args()
root = Path.cwd()
inputs = root / 'build/ci-inputs'
infra = Path(os.environ['VCPKG_CACHE_REPOSITORY_ROOT'])
packages = [('salts-cnet', 'SALTS_CNET', 'SaltsCNet'),
            ('salts-utils', 'SALTS_UTILS', 'SaltsUtils')]
if args.package:
    packages = [package for package in packages if package[0] == args.package]
cnet_profiles = {'windows-llvm-mingw-x64':'windows-x64', 'linux-clang-x64':'linux-x64',
                 'linux-clang-x64-tsan':'linux-x64-tsan', 'macos-clang-arm64':'macos-arm64'}


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def asset(release, name):
    matches = [a for a in release['assets'] if a['name'] == name]
    if len(matches) != 1 or not re.fullmatch(r'sha256:[0-9a-f]{64}', matches[0]['digest']):
        raise ValueError('Missing unique digest-qualified asset: ' + name)
    return matches[0]


def github(repo, path):
    return json.loads(subprocess.check_output(['gh', 'api', f'repos/qigao/{repo}/{path}']))


def sdk_entry(repo, manifest, platform):
    field = 'profile'
    matches = [sdk for sdk in manifest['sdks'] if sdk[field] == platform]
    if len(matches) != 1:
        raise ValueError(repo + ' must publish exactly one SDK for ' + platform)
    sdk = matches[0]
    entry = {'file': sdk['asset'], 'sha256': sdk['sha256']}
    if repo == 'salts-utils':
        entry['size'] = sdk['sizeBytes']
    return entry


if args.phase == 'prepare':
    (inputs / 'blobs').mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('transport', infra / 'scripts/setup-compiler-toolchain.py')
    transport = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(transport)

    def acquire(value):
        path = inputs / 'blobs' / value['digest'][7:]
        transport.verified_download(value['browser_download_url'], path, value['digest'][7:], value['size'])
        return path

    matrix = read(Path(__file__).with_name('sdk-matrix.json'))
    profiles = [args.profile] if args.profile else [p['sdk'] for p in matrix['include']]
    for repo, key, package in packages:
        version = os.environ[key + '_VERSION']
        if not re.fullmatch(r'\d+\.\d+\.\d+', version):
            raise ValueError(key + '_VERSION must select a stable release')
        release = github(repo, f'releases/tags/v{version}')
        if release['draft'] or release['prerelease'] or not release['published_at']:
            raise ValueError('Expected published stable SDK: ' + repo)
        metadata = asset(release, f'{repo}-{version}-manifest.json')
        manifest = read(acquire(metadata))
        if (manifest['schema'] != 1 or manifest['version'] != version
                or manifest['repository'] != 'qigao/' + repo
                or manifest['cmake_package'] != package):
            raise ValueError('Unqualified release manifest')
        tag = github(repo, f'git/ref/tags/v{version}')['object']
        if tag['type'] != 'commit' or tag['sha'] != manifest['commit']:
            raise ValueError('Manifest commit disagrees with release tag')
        producer = github(repo, f"actions/runs/{manifest['producer_run']}")
        if producer['conclusion'] != 'success' or producer['html_url'] != manifest['producer_url']:
            raise ValueError('Release producer did not qualify')
        sources = {manifest['commit'], *(sdk['sdk_manifest']['source'] for sdk in manifest['sdks'])}
        for source in sources:
            if github(repo, f'git/commits/{source}')['tree']['sha'] != manifest['tree']:
                raise ValueError('Release and SDK source trees differ')
        if repo == 'salts-utils':
            required = {'SaltsBase': os.environ['SALTS_BASE_VERSION'], 'CMeta': os.environ['CMETA_VERSION'],
                        'cmetaCommit': os.environ['CMETA_REF'], 'vcpkg-cache': os.environ['VCPKG_CACHE_VERSION']}
            if any(manifest['dependencies'][k] != v for k, v in required.items()):
                raise ValueError('Utils dependency composition differs from selected SDK inputs')
        if repo == 'salts-cnet' and any(
                sdk['sdk_manifest']['base_version'] != os.environ['SALTS_BASE_VERSION']
                or sdk['sdk_manifest']['cache_version'] != os.environ['VCPKG_CACHE_VERSION']
                for sdk in manifest['sdks']):
            raise ValueError('CNet dependency composition differs from selected SDK inputs')
        selected = []
        for profile in profiles:
            platform = cnet_profiles.get(profile, profile) if key == 'SALTS_CNET' else profile
            sdk = sdk_entry(repo, manifest, platform)
            value = asset(release, sdk['file'])
            if value['digest'] != 'sha256:' + sdk['sha256']:
                raise ValueError('SDK manifest/asset mismatch: ' + sdk['file'])
            if repo == 'salts-utils' and value['size'] != sdk['size']:
                raise ValueError('SDK manifest/asset mismatch: ' + sdk['file'])
            acquire(value)
            selected.append({'profile': profile, 'size': value['size']})
        write(inputs / (repo + '-release.json'), {'manifest':manifest,'asset':metadata,'selected':selected})
else:
    if not args.profile:
        parser.error('restore requires --profile')
    values = {}
    for repo, key, package in packages:
        record = read(inputs / (repo + '-release.json'))
        metadata = record['asset']
        original = inputs / 'blobs' / metadata['digest'][7:]
        if original.stat().st_size != metadata['size'] or 'sha256:' + digest(original) != metadata['digest']:
            raise ValueError('Prepared release manifest failed integrity validation: ' + repo)
        manifest = read(original)
        if manifest['version'] != os.environ[key + '_VERSION']:
            raise ValueError('Prepared version disagrees with selected environment: ' + repo)
        platform = cnet_profiles.get(args.profile, args.profile) if key == 'SALTS_CNET' else args.profile
        sdk = sdk_entry(repo, manifest, platform)
        selected = next(s for s in record['selected'] if s['profile'] == args.profile)
        archive = inputs / 'blobs' / sdk['sha256']
        if archive.stat().st_size != selected['size'] or digest(archive) != sdk['sha256']:
            raise ValueError('Prepared SDK failed integrity validation: ' + repo)
        destination = root / 'build/dependencies' / (repo + '-' + manifest['version'] + '-' + args.profile)
        destination.mkdir(parents=True)
        with tarfile.open(archive) as source:
            source.extractall(destination, filter='data')
        producer = manifest['producer_run']
        receipt = {'version':manifest['version'],'source':manifest['commit'], 'sha256':sdk['sha256'],
                   'producer':producer, 'root':destination.as_posix(), 'profile':args.profile}
        write(destination / 'sdk-restore-receipt.json', receipt)
        values[args.prefix + key + '_ROOT'] = destination.as_posix()
    with Path(os.environ['GITHUB_ENV']).open('a',encoding='utf-8') as output:
        for key,value in values.items():
            if '\n' in value or '\r' in value:
                raise ValueError('Invalid environment path')
            output.write(key + '=' + value + '\n')
