#!/usr/bin/env python3
"""Player entry point for the Mac Duo builder: check, fetch, build and sign.

Release inventories bind every shipped file. The Duo profile is selected explicitly,
even when a legacy Mac inventory also contains an iOS profile. A player's generated
bundle identifier is retained across builds so reinstalling preserves app storage.
"""
from __future__ import annotations
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import posixpath
import re
import subprocess
import shutil
import stat
import sys
import tempfile
import urllib.request
import uuid
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import disc_builder as db
import host_support


class Refusal(Exception):
    pass


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def host_id():
    machine = platform.machine().lower()
    machine = {'amd64': 'x86_64', 'aarch64': 'arm64'}.get(machine, machine)
    return f'{sys.platform}-{machine}'


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


def load_release(path):
    r = read_json(path)
    root = path.resolve().parent
    if r.get('schema') != 'builder-release/1' or r.get('host') != host_id():
        raise Refusal(f'wrong or missing release for {host_id()}; obtain the matching builder download')
    if not isinstance(r.get('files'), list) or not r['files']:
        raise Refusal('release inventory is missing; obtain a fully populated, approved builder release')
    seen = set()
    for rec in r['files']:
        p = inside(root, rec['path'])
        key = rec['path'].casefold()
        if key in seen or not re.fullmatch('[0-9a-f]{64}', str(rec.get('sha256'))):
            raise Refusal('invalid release inventory; obtain an intact builder download')
        seen.add(key)
        if not p.is_file() or db.sha256_file(p) != rec['sha256']:
            raise Refusal(f'payload missing or changed: {rec["path"]}; re-extract the approved builder download')
        if rec.get('executable') and os.name != 'nt' and not os.access(p, os.X_OK):
            raise Refusal(f'payload is not executable: {rec["path"]}; re-extract preserving executable permissions')
    listed = [f['path'] for f in r['files']]
    for key in ('profile', 'python'):
        if key in r and r[key] not in listed:
            raise Refusal(f'{key} is not hash-bound in the release; obtain a corrected release')
    targets = r.get('profiles', {})
    if not isinstance(targets, dict) or any(t not in TARGETS or v not in listed for t, v in targets.items()):
        raise Refusal('a target profile is not hash-bound in the release; obtain a corrected release')
    if 'profile' not in r:
        raise Refusal('no production profile; obtain the approved B1/B2 profile')
    return r, root


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


def check_paths(work, cache):
    host_support.private_output(work)
    host_support.private_output(cache)


@contextlib.contextmanager
def environment(values):
    before = {k: os.environ.get(k) for k in values}
    os.environ.update(values)
    try:
        yield
    finally:
        for k, v in before.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v


class DiscNamedAtBuild:
    # Stands for a disc while the plan is checked: a command line may name it, nothing of it can be read.
    def __init__(self, role):
        self.image = f'<the {role} disc>'


SETTINGS_NAME = 'fivepocket-builder-settings.json'
# Accepted inventory vocabulary includes older upstream Mac packages. Only Duo is buildable here.
TARGETS = ('ios', 'duo', 'thor', 'fold')
BUILD_TARGET = 'duo'


def bundle_id_for(target: str, work: Path, given: str | None = None) -> str:
    """the player's own identifier for target: --bundle-id when given, else the one kept next to the work folder (made once)"""
    if given:
        return given
    settings = work.parent / SETTINGS_NAME
    data = read_json(settings) if settings.is_file() else {}
    player = data.get('player_key')
    if not (isinstance(player, str) and re.fullmatch(r'[0-9a-f]{8}', player)):
        player = uuid.uuid4().hex[:8]
        data['player_key'] = player
        settings.parent.mkdir(parents=True, exist_ok=True)
        tmp = settings.with_name(settings.name + '.part')
        tmp.write_text(json.dumps(data, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
        os.replace(tmp, settings)
    return f'com.fivepocket.{player}.{target}'


def say_bundle_id(bundle_id: str, target: str) -> None:
    print(f'Bundle identifier of your {target} app: {bundle_id}\n'
          'ALWAYS SIGN WITH THIS IDENTIFIER. Signing with another one makes a new app with empty storage: '
          'the imported discs and your saves would be gone.')


def profile_of(release, target=None):
    if target not in (None, BUILD_TARGET):
        raise Refusal('this builder supports only FivePocket Duo')
    profile = release.get('profiles', {}).get(BUILD_TARGET)
    if profile is None:
        raise Refusal('release has no Duo profile; obtain the Mac Duo builder package')
    return profile


def check(release, root, work, cache, target=None):
    check_paths(work, cache)
    estimate = release.get('estimates', {})
    need = estimate.get('min_free_bytes')
    probe = work
    while not probe.exists(): probe = probe.parent
    free = shutil.disk_usage(probe).free
    print(f'Disk available: {free} bytes.')
    if type(need) is not int or need <= 0:
        raise Refusal('disk requirement unmeasured; obtain a release with the B1/B7 disk budget')
    print(f'Disk required: {need} bytes. Build time ESTIMATE: {estimate.get("build_time", "UNKNOWN; B7 measurement pending")}.')
    if free < need:
        raise Refusal('not enough free disk; free space or choose another --work directory')
    ndk = existing_ndk(cache, ndk_spec(release))
    profile = db.load_profile(inside(root, profile_of(release, target)))
    # Check every supported game's plan without pretending either disc was identified.
    with environment({'BUILDER_PYTHON': str(inside(root, release['python'])) if release.get('python') else sys.executable,
                      'BUILDER_NDK_ROOT': str(ndk), 'BUILDER_PACKAGE_ROOT': str(root)}):
        failures = []
        for game in profile['games']:
            ctx = db.Context(profile, work, root, work); ctx.game = game
            ctx.discs = {role: DiscNamedAtBuild(role) for role in ('play', 'install')}  # names only; no stage runs, no disc is read
            db.plan_paths(profile, ctx)
            db.plan_output(profile, ctx)
            failures.extend(db.check_prerequisites(profile, ctx))
    if failures:
        raise Refusal('; '.join(dict.fromkeys(failures)) + '; obtain the complete payload or fix the profile; no stage ran')
    print('Payload, compiler and stage prerequisites verified. Discs still require identification at build time.')
    return ndk


def default_release():
    # The package's inventory lies at the package root, which the launcher names; without a launcher, beside this file.
    root = os.environ.get('BUILDER_PACKAGE_ROOT')
    return Path(root) / 'release.json' if root else Path(__file__).with_name('release.json')


def default_work(root):
    # Inside the package's private folder: the stage tools accept a work folder nowhere else below a package.
    return root / 'local-analysis' / 'builder' / 'work'


def how_to(release_path):
    try:
        release = read_json(release_path)
        ndk = release['ndk']
        compiler = f'{ndk["size"] / 1e6:.0f} MB from {ndk["url"]}'
        space = f'{release["estimates"]["min_free_bytes"] / 2**30:.0f} GiB of free space'
        terms = ndk['license_url']
    except (OSError, ValueError, KeyError, TypeError):
        compiler, space, terms = 'from its publisher', 'the free space reported by check', 'named by the fetch step'
    out = default_work(Path()) / 'out'
    return f'''Build FivePocket Duo from your own two disc images on a Mac with Xcode.

  ./builder fetch --accept-ndk-license
      Fetches the Android NDK shader and decoder tools ({compiler}).
      Read the terms first ({terms}); the switch accepts them.
  ./builder check
      Checks the package, compiler and stage inputs without reading discs.
  ./builder build --play-disc PLAY-DISC.iso --install-disc INSTALL-DISC.iso
      Needs {space}. Writes an unsigned Duo .ipa to {out}.
      An interrupted build resumes from its verified completed stages.
  ./builder sign --ipa UNSIGNED.ipa --identity "Apple Development: you@example.com" --profile-file YOUR.mobileprovision --out SIGNED.ipa
      Signs the app with your own identity and profile.

Keep the printed bundle identifier when signing and reinstalling: a different identifier
creates a different app with empty storage. --bundle-id chooses an identifier for one build.
The app imports game data from your discs on the Duo.'''


def sign_ipa(ipa: Path, identity: str, profile: Path | None, entitlements: Path | None, out: Path) -> int:
    """The player's own signature on a builder .ipa: unpacked into a private temporary folder, signed inside-out and verified
    (ios_sign.py), written again (ipa_writer.py). The unsigned .ipa is left as it is."""
    import ios_sign, ipa_writer
    if out.exists() or out.suffix != '.ipa':
        raise Refusal(f'{out} exists or is not an .ipa name')
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(ipa) as z:
            for name in z.namelist():
                inside(Path(tmp), name)                     # the same refusals as a package path: no .., no absolute names
            z.extractall(tmp)
            for info in z.infolist():
                if (info.external_attr >> 16) & 0o111:
                    (Path(tmp) / info.filename).chmod(0o755)
        apps = sorted((Path(tmp) / 'Payload').glob('*.app'))
        if len(apps) != 1:
            raise Refusal('the .ipa holds no single Payload/<name>.app')
        try:
            done = ios_sign.sign(apps[0], identity, profile, entitlements)
        except ios_sign.Refused as error:
            raise Refusal(str(error))
        except subprocess.CalledProcessError as error:
            print(f'REFUSED: codesign failed: {(error.stderr or error.stdout or "").strip()[-600:]}')
            return 1
        ipa_writer.write_ipa(apps[0], out)
        print(f'{len(done)} signed and verified (codesign --verify --deep --strict)')
    print(f'Signed: {out}')
    return 0


def main(argv=None):
    if (sys.argv[1:] if argv is None else argv) == []:
        print(how_to(default_release()))
        return 0
    ap = argparse.ArgumentParser(description='Build and sign FivePocket Duo on macOS.')
    ap.add_argument('--release', type=Path, default=default_release())
    ap.add_argument('--cache', type=Path, default=Path.home() / '.cache' / 'own-disc-builder')
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check'); c.add_argument('--work', type=Path)
    f = sub.add_parser('fetch'); f.add_argument('--accept-ndk-license', action='store_true'); f.add_argument('--archive', type=Path)
    b = sub.add_parser('build')
    b.add_argument('--work', type=Path); b.add_argument('--play-disc', type=Path, required=True)
    b.add_argument('--install-disc', type=Path, required=True); b.add_argument('--out', type=Path)
    b.add_argument('--allow-heavy', action='store_true', help=argparse.SUPPRESS)
    b.add_argument('--report', type=Path); b.add_argument('--stop-after')
    b.add_argument('--duo', action='store_true', help='build FivePocket Duo (the default)')
    b.add_argument('--bundle-id')
    s = sub.add_parser('sign'); s.add_argument('--ipa', type=Path, required=True); s.add_argument('--identity', required=True)
    s.add_argument('--profile-file', type=Path); s.add_argument('--entitlements', type=Path); s.add_argument('--out', type=Path, required=True)
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'sign':
            return sign_ipa(a.ipa, a.identity, a.profile_file, a.entitlements, a.out)
        release, root = load_release(a.release)
        if a.cmd == 'fetch':
            check_paths(a.cache, a.cache)
            fetch(release, a.cache.resolve(), a.accept_ndk_license, a.archive)
            return 0
        work = a.work or default_work(root)
        selected = inside(root, profile_of(release))
        bundle_id = bundle_id_for(BUILD_TARGET, work.resolve(), a.bundle_id) if a.cmd == 'build' else 'com.fivepocket.check.duo'
        with environment({'BUILDER_BUNDLE_ID': bundle_id}):
            ndk = check(release, root, work.resolve(), a.cache.resolve())
        if a.cmd == 'build':
            command = ['build', '--profile', str(selected), '--work', str(work),
                       '--play-disc', str(a.play_disc), '--install-disc', str(a.install_disc),
                       '--repo', str(root), '--data-root', str(work), '--allow-heavy']
            for name in ('out', 'report', 'stop_after'):
                if getattr(a, name): command += ['--' + name.replace('_', '-'), str(getattr(a, name))]
            with environment({'BUILDER_PYTHON': str(inside(root, release['python'])) if release.get('python') else sys.executable,
                              'BUILDER_NDK_ROOT': str(ndk), 'BUILDER_PACKAGE_ROOT': str(root), 'BUILDER_BUNDLE_ID': bundle_id}):
                code = db.main(command)
            say_bundle_id(bundle_id, BUILD_TARGET)
            return code
        return 0
    except (Refusal, db.BuilderError, OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f'REFUSED: {error}. No automatic retry. Correct the input or use an intact release, then run check again.')
        return getattr(error, 'code', 2)


if __name__ == '__main__':
    raise SystemExit(main())
