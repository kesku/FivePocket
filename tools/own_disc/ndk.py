# SPDX-License-Identifier: GPL-3.0-or-later
"""Pinned Google NDK tools used by the source-built Duo shader pipeline."""
from __future__ import annotations
import contextlib
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request

import disc_builder as db
import host_support

class Refusal(ValueError):
    pass


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def safe_name(name):
    # Same archive vocabulary on NTFS, APFS and ext4. No aliases/ADS/drive names.
    if not isinstance(name, str) or '\\' in name or not name or name.startswith('/'):
        raise Refusal('invalid package path; obtain a corrected release')
    parts = name.rstrip('/').split('/')
    for part in parts:
        if part in ('', '.', '..') or part.endswith(('.', ' ')) or re.search(r'[<>:"|?*\x00-\x1f]', part):
            raise Refusal('invalid package path; obtain a corrected release')
        if re.fullmatch(r'(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?', part, re.I):
            raise Refusal('reserved Windows package path; obtain a corrected release')
    return PurePosixPath(*parts)


def inside(root, name):
    result = root.joinpath(*safe_name(name).parts).resolve()
    if not result.is_relative_to(root.resolve()):
        raise Refusal('package path escapes its root; obtain an intact release')
    return result


def ndk_spec(release):
    n = release.get('ndk')
    if not isinstance(n, dict) or not re.fullmatch('[0-9a-f]{64}', str(n.get('sha256'))) or not isinstance(n.get('size'), int) or n['size'] <= 0:
        raise Refusal('NDK publisher SHA-256/size not pinned; ask the release publisher for a completed release')
    if not re.fullmatch(r'https://dl\.google\.com/android/repository/android-ndk-[a-zA-Z0-9._-]+\.(zip|dmg)', n.get('url', '')):
        raise Refusal('NDK URL is not a pinned Google archive; obtain a corrected release')
    if n['url'].endswith('.dmg'):
        # Google publishes the macOS NDK only as a disk image: image_root names the NDK folder inside it
        # (AndroidNDK<build>.app/Contents/NDK), which is installed as `directory`
        if not isinstance(n.get('image_root'), str) or not n['image_root']:
            raise Refusal('NDK image root not declared; obtain a corrected release')
        safe_name(n['image_root'])
    if n.get('license_url') != 'https://developer.android.com/ndk/downloads':
        raise Refusal('NDK licence link missing; obtain a corrected release')
    safe_name(n['directory'])
    if len(PurePosixPath(n['directory']).parts) != 1 or type(n.get('unpacked_bytes')) is not int or n['unpacked_bytes'] <= 0:
        raise Refusal('NDK unpacked size/root not declared; obtain a corrected release')
    return n


def existing_ndk(cache, n):
    target = cache / n['sha256'][:16]
    record = target / 'installed.json'
    if not record.is_file():
        raise Refusal('compiler missing; read the NDK terms and run builder fetch --accept-ndk-license')
    data = read_json(record)
    if data.get('archive_sha256') != n['sha256'] or not data.get('files'):
        raise Refusal('compiler receipt differs; use a new cache and run builder fetch again')
    for name, digest in data['files'].items():
        p = inside(target, name)
        if not p.is_file() or db.sha256_file(p) != digest:
            raise Refusal('compiler files changed; use a new cache and run builder fetch again')
    return target / n['directory']


def link_target(rel: PurePosixPath, link: str, directory: str) -> str:
    """the archive path a relative link inside the compiler folder names; refused when it leaves the folder"""
    if link.startswith('/') or '\\' in link or ':' in link:
        raise Refusal('archive link escapes its root; obtain a corrected archive')
    linked = safe_name(posixpath.normpath(str(rel.parent / link)))
    if linked.parts[0] != directory:
        raise Refusal('archive link escapes its root; obtain a corrected archive')
    return str(linked)


def zip_payload(zip_path: Path, extracted: Path, n: dict, inventory: dict, links: dict) -> int:
    """Extracts the NDK folder of the verified ZIP: safe names under the declared root, no case-insensitive duplicates,
    links only to files inside the folder (materialized later as copies), no other special files, the declared size
    bound. Returns the bytes written."""
    expanded, seen = 0, set()
    with zipfile.ZipFile(zip_path) as z:
        for member in z.infolist():
            rel = safe_name(member.filename)
            if rel.parts[0] != n['directory']:
                raise Refusal('archive has unexpected root; obtain the pinned archive')
            key = str(rel).casefold()
            if key in seen:
                raise Refusal('archive has duplicate case-insensitive names; obtain the corrected archive')
            seen.add(key)
            mode = member.external_attr >> 16
            if stat.S_ISLNK(mode):
                if member.file_size > 4096:
                    raise Refusal('oversized archive link; obtain a corrected archive')
                links[str(rel)] = link_target(rel, z.read(member).decode('utf-8'), n['directory'])
                continue
            if stat.S_IFMT(mode) and not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)):
                raise Refusal('archive contains unsupported links/devices; obtain a supported publisher archive')
            dest = inside(extracted, str(rel))
            if member.is_dir():
                dest.mkdir(parents=True, exist_ok=True); continue
            expanded += member.file_size
            if expanded > n['unpacked_bytes']:
                raise Refusal('archive exceeds declared extraction bound; obtain a corrected release')
            dest.parent.mkdir(parents=True, exist_ok=True)
            with z.open(member) as source, dest.open('xb') as out:
                shutil.copyfileobj(source, out)
            if mode & 0o111:
                dest.chmod(0o755)
            inventory[str(rel)] = db.sha256_file(dest)
    return expanded


def dmg_payload(dmg: Path, extracted: Path, n: dict, inventory: dict, links: dict) -> int:
    """Copies the NDK folder out of the verified macOS disk image (attached read-only, not shown in Finder, detached
    again; the mount point is a new folder in the system's temporary folder, since macOS refuses to attach an image on
    a volume without owners, as external drives often are) under the rules of the ZIP path: safe names, no case-insensitive duplicates, links only to files inside the
    folder (materialized later as copies), no other special files, the declared size bound. Returns the bytes copied."""
    if sys.platform != 'darwin':
        raise Refusal('this compiler archive is a macOS disk image; use the release for this computer')
    mount = Path(tempfile.mkdtemp(prefix='builder-ndk-image-'))
    subprocess.run(['/usr/bin/hdiutil', 'attach', '-readonly', '-nobrowse', '-noautoopen', '-mountpoint', str(mount), str(dmg)],
                   check=True, capture_output=True)
    expanded, seen = 0, set()
    try:
        src = mount / n['image_root']
        if src.is_symlink() or not src.is_dir():
            raise Refusal('disk image has no NDK folder at the declared root; obtain the pinned archive')
        for here, dirs, files in os.walk(src, followlinks=False):
            for name in sorted(dirs) + sorted(files):
                p = Path(here) / name
                rel = safe_name(str(PurePosixPath(n['directory']) / p.relative_to(src).as_posix()))
                key = str(rel).casefold()
                if key in seen:
                    raise Refusal('archive has duplicate case-insensitive names; obtain the corrected archive')
                seen.add(key)
                mode = p.lstat().st_mode
                if stat.S_ISLNK(mode):
                    links[str(rel)] = link_target(rel, os.readlink(p), n['directory'])
                    continue
                dest = inside(extracted, str(rel))
                if stat.S_ISDIR(mode):
                    dest.mkdir(parents=True, exist_ok=True); continue
                if not stat.S_ISREG(mode):
                    raise Refusal('archive contains unsupported links/devices; obtain a supported publisher archive')
                expanded += p.stat().st_size
                if expanded > n['unpacked_bytes']:
                    raise Refusal('archive exceeds declared extraction bound; obtain a corrected release')
                dest.parent.mkdir(parents=True, exist_ok=True)
                with p.open('rb') as source, dest.open('xb') as out:
                    shutil.copyfileobj(source, out)
                if mode & 0o111:
                    dest.chmod(0o755)
                inventory[str(rel)] = db.sha256_file(dest)
    finally:
        subprocess.run(['/usr/bin/hdiutil', 'detach', str(mount)], capture_output=True)
        with contextlib.suppress(OSError):
            mount.rmdir()                                # only the empty mount point; never anything with content
    return expanded


class GoogleOnlyRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.startswith('https://dl.google.com/android/repository/'):
            raise Refusal('publisher redirect left Google; obtain the pinned archive directly')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def fetch(release, cache, accepted, archive=None, opener=None):
    n = ndk_spec(release)
    print(f'Compiler: {n["url"]}; {n["size"]} bytes; SHA-256 {n["sha256"][:12]}')
    print(f'Android SDK License Agreement and component notices: {n["license_url"]}')
    if not accepted:
        raise Refusal('licence not accepted; read those terms, then rerun fetch with --accept-ndk-license if you agree')
    host_support.private_output(cache)
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / n['sha256'][:16]
    if target.exists():
        existing_ndk(cache, n)
        print('Compiler already verified.'); return
    if shutil.disk_usage(cache).free < n['size'] + n['unpacked_bytes']:
        raise Refusal('not enough disk for archive and extracted compiler; free space or choose --cache elsewhere')
    lock = cache / '.fetch-lock'
    try:
        guard = lock.open('x', encoding='utf-8', newline='\n')
    except FileExistsError:
        raise Refusal('another fetch owns the cache; wait for it or inspect the stale lock before removing it')
    try:
        with guard:
            guard.write(str(os.getpid()))
        staged = cache / ('fetch-' + uuid.uuid4().hex)
        staged.mkdir()
        zip_path = staged / ('download.dmg' if n['url'].endswith('.dmg') else 'download.zip')
        # No request is opened until all release/consent/disk checks above passed.
        stream = open(archive, 'rb') if archive else (opener or urllib.request.build_opener(GoogleOnlyRedirect()).open)(n['url'], timeout=60)
        h, total = hashlib.sha256(), 0
        with stream, zip_path.open('xb') as out:
            while block := stream.read(1 << 20):
                total += len(block)
                if total > n['size']:
                    raise Refusal('archive exceeds pinned size; keep failed evidence and obtain the correct archive')
                h.update(block); out.write(block)
        if total != n['size'] or h.hexdigest() != n['sha256']:
            raise Refusal('archive hash/size mismatch; nothing installed; obtain the pinned publisher archive')
        extracted = staged / 'payload'; extracted.mkdir()
        inventory, links = {}, {}
        payload = dmg_payload if zip_path.suffix == '.dmg' else zip_payload
        expanded = payload(zip_path, extracted, n, inventory, links)
        # Materialize only file links after their validated targets exist. This
        # works on Windows without privilege; no symlink can redirect extraction.
        while links:
            progress = False
            for name, target_name in list(links.items()):
                if target_name in inventory:
                    source, dest = inside(extracted, target_name), inside(extracted, name)
                    expanded += source.stat().st_size
                    if expanded > n['unpacked_bytes']:
                        raise Refusal('archive exceeds declared extraction bound; obtain a corrected release')
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, dest)
                    inventory[name] = db.sha256_file(dest)
                    del links[name]; progress = True
            if not progress:
                raise Refusal('archive has dangling, cyclic or directory links; obtain a supported archive')
        if not inventory:
            raise Refusal('archive has no compiler files; obtain the correct archive')
        db.write_atomic(extracted / 'installed.json', json.dumps({'archive_sha256': n['sha256'], 'files': inventory}, sort_keys=True).encode())
        os.replace(extracted, target)
        # Retain the verified publisher archive as evidence; never delete a pre-existing file.
        print(f'Compiler installed and verified: {target / n["directory"]}')
    finally:
        lock.unlink()
