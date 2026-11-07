#!/usr/bin/env python3
"""Build the Duo pipeline from owned disc images and publish its final stage file.

    disc_builder.py build --profile FILE --play-disc ISO --install-disc ISO --work DIR
    disc_builder.py identify --profile FILE --play-disc ISO

Before execution, identify the disc, resolve every stage input/output and check
prerequisites. Verified receipts allow unchanged stages to resume. Build outputs
and reports must remain under local-analysis when written in a Git checkout.
The profile's output names the file delivered by one of its declared stages.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))      # the host helper lies beside this file
import host_support  # noqa: E402

TOOL_ROOT = host_support.package_root(__file__)
PROFILE_SCHEMA = 'own-disc-pipeline/1'
RECEIPT_SCHEMA = 'own-disc-receipt/1'
ENGINE_KEY_VERSION = 3          # bump when the meaning of a stage key changes (2: tree digests skip TREE_IGNORED; 3: the engine's and the host helper's own hashes are part of every key, and a tree's files are ordered by their names with '/', the same on every host)
BLOCK = 1 << 20
PRIVATE_DIR = 'local-analysis'  # the git-ignored folder of a checkout; the only place in one the builder writes to
WORK_MARKER = '.own-disc-work'  # a work directory made by this builder holds this file
TREE_IGNORED = frozenset(('__pycache__', '.DS_Store'))

EXIT_OK, EXIT_STAGE, EXIT_PREREQ, EXIT_HEAVY, EXIT_GAME, EXIT_USAGE = 0, 1, 2, 3, 4, 64


class BuilderError(Exception):
    def __init__(self, message: str, code: int = EXIT_STAGE):
        super().__init__(message)
        self.code = code


# ------------------------------------------------------------------------------------------------------- hashing

def sha256_stream(f, size: int | None = None) -> str:
    h = hashlib.sha256()
    left = size
    while left is None or left > 0:
        block = f.read(BLOCK if left is None else min(BLOCK, left))
        if not block:
            if left:
                raise BuilderError('unexpected end of data while hashing')
            break
        h.update(block)
        if left is not None:
            left -= len(block)
    return h.hexdigest()


def sha256_file(path: Path) -> str:
    with open(path, 'rb') as f:
        return sha256_stream(f)


def sha256_tree(directory: Path) -> tuple[str, int]:
    """One digest for a directory: the sorted list of (relative path, file SHA-256). Returns (digest, file count).
    Interpreter caches (__pycache__) and Finder files (.DS_Store) are left out: they appear by themselves (another
    Python version writes other cache files) and are not content."""
    h = hashlib.sha256()
    count = 0
    for p in sorted((q for q in directory.rglob('*') if q.is_file()), key=lambda q: q.relative_to(directory).as_posix()):
        rel = p.relative_to(directory)
        if TREE_IGNORED.intersection(rel.parts):
            continue
        h.update(rel.as_posix().encode() + b'\0' + sha256_file(p).encode() + b'\n')
        count += 1
    return h.hexdigest(), count


def sha256_path(path: Path) -> dict:
    if path.is_dir():
        digest, count = sha256_tree(path)
        return {'kind': 'dir', 'sha256': digest, 'files': count}
    if path.is_file():
        return {'kind': 'file', 'sha256': sha256_file(path), 'size': path.stat().st_size}
    raise BuilderError(f'missing: {path}')


def canonical(obj) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(',', ':')).encode()


def short(digest: str) -> str:
    return digest[:12]


def write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


# ------------------------------------------------------------------------------------------------- disc sources

XDVDFS_MAGIC = b'MICROSOFT*XBOX*MEDIA'
SECTOR = 2048
# Start of the game partition inside an image: 0 for a trimmed image ("XISO"), then the full-disc layouts
# (XGD2, XGD3, XGD1). Values from public format descriptions; NOT yet checked against the pinned extract-xiso
# (that submodule is not checked out in this repository).
XDVDFS_PARTITION_OFFSETS = (0, 0x0FD90000, 0x02080000, 0x18300000)
MAX_DIR_BYTES = 16 << 20


class FolderDisc:
    """An extracted disc: a folder. File names are matched without regard to case."""

    kind = 'folder'

    def __init__(self, root: Path):
        self.root = root

    def _resolve(self, rel: str) -> Path | None:
        cur = self.root
        for part in [p for p in rel.replace('\\', '/').split('/') if p]:
            if part in ('.', '..'):
                raise BuilderError(f'bad disc path: {rel}', EXIT_USAGE)
            if not cur.is_dir():
                return None
            match = [c for c in cur.iterdir() if c.name.lower() == part.lower()]
            if not match:
                return None
            if len(match) != 1:
                raise BuilderError('ambiguous case-insensitive disc path: use an unambiguous extracted disc or ISO', EXIT_USAGE)
            cur = match[0]
        return cur if cur.is_file() else None

    def size(self, rel: str) -> int | None:
        p = self._resolve(rel)
        return None if p is None else p.stat().st_size

    def open(self, rel: str):
        p = self._resolve(rel)
        if p is None:
            raise BuilderError(f'{rel}: not on the disc folder')
        return open(p, 'rb'), p.stat().st_size


class XisoDisc:
    """An Xbox 360 disc image holding an XDVDFS volume (full or trimmed image). Read-only."""

    kind = 'xiso'

    def __init__(self, image: Path):
        self.image = image
        self.partition = None
        size = image.stat().st_size
        with open(image, 'rb') as f:
            for off in XDVDFS_PARTITION_OFFSETS:
                if off + 33 * SECTOR > size:
                    continue
                f.seek(off + 32 * SECTOR)
                head = f.read(SECTOR)
                if head[:20] == XDVDFS_MAGIC and head[-20:] == XDVDFS_MAGIC:
                    self.partition = off
                    self.root_sector, self.root_size = struct.unpack_from('<II', head, 20)
                    break
        if self.partition is None:
            raise BuilderError(f'{image.name}: no Xbox disc file system found (not an XISO image)', EXIT_GAME)
        self.image_size = size

    def _read_dir(self, f, sector: int, size: int) -> dict[str, tuple[int, int, bool]]:
        if size == 0:
            return {}
        if size > MAX_DIR_BYTES or self.partition + sector * SECTOR + size > self.image_size:
            raise BuilderError('disc image: directory outside the image')
        f.seek(self.partition + sector * SECTOR)
        data = f.read(size)
        entries: dict[str, tuple[int, int, bool]] = {}
        todo, seen = [0], set()
        while todo:
            off = todo.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            left, right, start, fsize, attr, nlen = struct.unpack_from('<HHIIBB', data, off)
            if left == 0xFFFF and right == 0xFFFF and start == 0xFFFFFFFF:
                continue                                    # padding
            name = data[off + 14: off + 14 + nlen].decode('latin-1')
            if len(name) != nlen or not name:
                raise BuilderError('disc image: damaged directory entry')
            entries[name.lower()] = (start, fsize, bool(attr & 0x10))
            for child in (left, right):
                if child not in (0, 0xFFFF):
                    todo.append(child * 4)
        return entries

    def _find(self, f, rel: str) -> tuple[int, int] | None:
        sector, size = self.root_sector, self.root_size
        parts = [p for p in rel.replace('\\', '/').split('/') if p]
        for i, part in enumerate(parts):
            entry = self._read_dir(f, sector, size).get(part.lower())
            if entry is None:
                return None
            start, fsize, is_dir = entry
            if i == len(parts) - 1:
                return None if is_dir else (start, fsize)
            if not is_dir:
                return None
            sector, size = start, fsize
        return None

    def size(self, rel: str) -> int | None:
        with open(self.image, 'rb') as f:
            hit = self._find(f, rel)
        return None if hit is None else hit[1]

    def open(self, rel: str):
        f = open(self.image, 'rb')
        hit = self._find(f, rel)
        if hit is None:
            f.close()
            raise BuilderError(f'{rel}: not on the disc image')
        start, fsize = hit
        if self.partition + start * SECTOR + fsize > self.image_size:
            f.close()
            raise BuilderError(f'{rel}: the image is cut off inside this file')
        f.seek(self.partition + start * SECTOR)
        return f, fsize


def open_disc(path: Path):
    if path.is_dir():
        return FolderDisc(path)
    if path.is_file():
        return XisoDisc(path)
    raise BuilderError(f'disc not found: {path}', EXIT_USAGE)


def disc_hash(disc, rel: str) -> tuple[str, int]:
    f, size = disc.open(rel)
    with f:
        return sha256_stream(f, size), size


def disc_copy(disc, rel: str, dest: Path) -> tuple[str, int]:
    f, size = disc.open(rel)
    dest.parent.mkdir(parents=True, exist_ok=True)
    h = hashlib.sha256()
    left = size
    with f, open(dest, 'wb') as out:
        while left > 0:
            block = f.read(min(BLOCK, left))
            if not block:
                raise BuilderError(f'{rel}: short read from the disc')
            h.update(block)
            out.write(block)
            left -= len(block)
    return h.hexdigest(), size


# ------------------------------------------------------------------------------------------------------ profile

VAR = re.compile(r'\$\{([^}]+)\}')
STAGE_ID = re.compile(r'[a-z][a-z0-9-]{0,39}')
OUTPUT_NAME = re.compile(r'[a-z][a-z0-9_-]{0,39}')
HEX64 = re.compile(r'[0-9a-f]{64}')
ADAPTERS = ('disc-file', 'command', 'supplied')


def load_profile(path: Path) -> dict:
    try:
        profile = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as e:
        raise BuilderError(f'profile {path}: {e}', EXIT_USAGE)
    if not isinstance(profile, dict) or profile.get('schema') != PROFILE_SCHEMA:
        raise BuilderError(f'profile {path}: schema must be {PROFILE_SCHEMA}', EXIT_USAGE)
    ids = [s.get('id', '') for s in profile.get('stages', [])]
    for s in profile.get('stages', []):
        if not STAGE_ID.fullmatch(s.get('id', '')) or s.get('adapter') not in ADAPTERS:
            raise BuilderError(f'profile: bad stage {s.get("id")!r} / adapter {s.get("adapter")!r}', EXIT_USAGE)
        outs = s.get('outputs')
        if not isinstance(outs, dict) or not outs or not all(OUTPUT_NAME.fullmatch(n) for n in outs):
            raise BuilderError(f'profile: stage {s["id"]} needs outputs with simple lower-case names', EXIT_USAGE)
        if s['adapter'] == 'command' and not (isinstance(s.get('argv'), list) and s['argv']):
            raise BuilderError(f'profile: command stage {s["id"]} has no argv', EXIT_USAGE)
        if s['adapter'] != 'command' and len(outs) != 1:
            raise BuilderError(f'profile: stage {s["id"]} copies one file, so it declares exactly one output', EXIT_USAGE)
        if s['adapter'] == 'disc-file' and not s.get('path'):
            raise BuilderError(f'profile: disc-file stage {s["id"]} names no path on the disc', EXIT_USAGE)
    if len(set(ids)) != len(ids):
        raise BuilderError('profile: stage ids must be unique', EXIT_USAGE)
    if not profile.get('games'):
        raise BuilderError('profile: no games table', EXIT_USAGE)
    for g in profile['games']:
        if not HEX64.fullmatch(str(g.get('executable_sha256', ''))):
            raise BuilderError('profile: every game needs executable_sha256 (64 hex digits)', EXIT_USAGE)
    profile['_dir'] = path.resolve().parent
    return profile


class Context:
    """Paths and facts a run knows; expands ${...} variables of the profile."""

    def __init__(self, profile: dict, work: Path, repo: Path | None = None, data: Path | None = None):
        self.profile = profile
        self.work = work.resolve()
        paths = profile.get('paths', {})
        base = profile['_dir']
        self.repo = (repo or self._rel(base, paths.get('repo')) or TOOL_ROOT).resolve()
        self.data = (data or self._rel(base, paths.get('data')) or self.repo).resolve()
        self.game: dict = {}
        self.discs: dict = {}
        self.outputs: dict[str, dict[str, Path]] = {}       # filled by plan_paths, stage by stage

    @staticmethod
    def _rel(base: Path, value: str | None) -> Path | None:
        if not value:
            return None
        p = Path(value)
        return p if p.is_absolute() else base / p

    def expand(self, text: str) -> str:
        def sub(m: re.Match) -> str:
            name = m.group(1)
            if name == 'python':
                return os.environ.get('BUILDER_PYTHON', sys.executable)
            if name == 'ndk':
                if not os.environ.get('BUILDER_NDK_ROOT'):
                    raise BuilderError('NDK missing: run builder fetch after reading its licence', EXIT_PREREQ)
                return os.environ['BUILDER_NDK_ROOT']
            if name == 'exe':
                return '.exe' if sys.platform == 'win32' else ''
            if name == 'ndkhost':                           # this host's folder inside the NDK
                return host_support.ndk_host_tag()
            if name in ('xcode', 'iossdk', 'appleclang'):   # the Apple toolchain of an iOS profile (Mac only)
                return str(xcode_path(name))
            if name == 'bundleid':                          # the player's bundle identifier for an iOS app (builder --bundle-id)
                value = os.environ.get('BUILDER_BUNDLE_ID', '')
                if not re.fullmatch(r'[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+', value):
                    raise BuilderError('bundle id missing or malformed: give --bundle-id in reverse-DNS form '
                                       '(for example com.yourname.fivepocket)', EXIT_USAGE)
                return value
            if name == 'hosttools':                         # this host's builds of the project's own tools
                if not os.environ.get('BUILDER_HOST_TOOLS'):
                    raise BuilderError('host tools missing: BUILDER_HOST_TOOLS names no folder', EXIT_PREREQ)
                return os.environ['BUILDER_HOST_TOOLS']
            if name == 'repo':
                return str(self.repo)
            if name == 'data':
                return str(self.data)
            if name == 'work':
                return str(self.work)
            if name == 'profile':
                return str(self.profile.get('id', ''))
            if name == 'flavour':
                return str(self.profile.get('flavour', ''))
            if name.startswith('game:'):
                if name[5:] not in self.game:
                    raise BuilderError(f'profile: unknown game field in {m.group(0)}', EXIT_USAGE)
                return str(self.game[name[5:]])
            if name.startswith('disc:'):                    # the image file (or folder) of a disc named on the command line
                disc = self.discs.get(name[5:])
                if disc is None:
                    raise BuilderError(f'profile: {m.group(0)} needs that disc on the command line', EXIT_USAGE)
                return str(getattr(disc, 'image', None) or disc.root)
            if name.startswith('out:'):
                _, stage, out = (name.split(':') + ['', ''])[:3]
                if stage not in self.outputs or out not in self.outputs[stage]:
                    raise BuilderError(f'profile: {m.group(0)} names no output of an earlier stage', EXIT_USAGE)
                return str(self.outputs[stage][out])
            raise BuilderError(f'profile: unknown variable {m.group(0)}', EXIT_USAGE)
        return VAR.sub(sub, text)


XCODE_PARTS = {'xcode': '', 'iossdk': 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk',
               'appleclang': 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'}


def xcode_path(name: str) -> Path:
    """${xcode} (Xcode's Developer folder), ${iossdk}, ${appleclang}. Taken from BUILDER_XCODE, else DEVELOPER_DIR, never from
    xcode-select: on a Mac where xcode-select names the Command Line Tools (which have no iPhoneOS SDK) the run would otherwise
    pick a toolchain that cannot build for iOS. The folder must hold the iPhoneOS platform."""
    raw = os.environ.get('BUILDER_XCODE') or os.environ.get('DEVELOPER_DIR')
    if not raw:
        raise BuilderError('Xcode missing: set DEVELOPER_DIR (or BUILDER_XCODE) to Xcode\'s Contents/Developer folder, '
                           'e.g. /Applications/Xcode.app/Contents/Developer', EXIT_PREREQ)
    root = Path(raw)
    if root.name != 'Developer' and (root / 'Contents/Developer').is_dir():
        root = root / 'Contents/Developer'               # the .app folder itself was given
    if not (root / XCODE_PARTS['iossdk']).is_dir():
        raise BuilderError(f'Xcode missing: {root} holds no iPhoneOS SDK (the Command Line Tools have none; '
                           'install Xcode and point DEVELOPER_DIR at it)', EXIT_PREREQ)
    return root / XCODE_PARTS[name] if XCODE_PARTS[name] else root


def check_private_path(path: Path, what: str) -> None:
    """What the builder writes is game-derived: inside a git checkout it must stay under its ignored local-analysis/."""
    path = path.resolve()
    for parent in [path, *path.parents]:
        if (parent / '.git').exists():
            if not path.is_relative_to(parent / PRIVATE_DIR):
                raise BuilderError(f'{what} {path} is inside the git checkout {parent} but not under its '
                                   f'{PRIVATE_DIR}/: refusing to write game-derived files there', EXIT_USAGE)
            return


def check_work_dir(work: Path) -> None:
    """The builder removes and rewrites folders in its work directory, so it only takes one that is new, empty or its
    own (marked by WORK_MARKER)."""
    if work.exists() and (not work.is_dir() or (any(work.iterdir()) and not (work / WORK_MARKER).is_file())):
        raise BuilderError(f'work directory {work} already holds files and was not made by this builder (no '
                           f'{WORK_MARKER} file in it): give an empty or new directory', EXIT_USAGE)


def work_target(ctx: Context, stage_id: str, value: str, what: str) -> Path:
    """A folder or file the builder itself removes or creates for a stage: always strictly inside the work directory."""
    p = (ctx.work / ctx.expand(value)).resolve()
    if p == ctx.work or not p.is_relative_to(ctx.work):
        raise BuilderError(f'stage {stage_id}: {what} {value!r} is outside the work directory', EXIT_USAGE)
    return p


def tool_paths(stage: dict) -> list[str]:
    return [t['path'] if isinstance(t, dict) else t for t in stage.get('tools', [])]


def plan_paths(profile: dict, ctx: Context) -> None:
    """Resolves every stage's output paths into ctx.outputs and expands every other ${...} of the stages once, so a
    misspelt variable, a reference to a later stage or a path the builder must not write stops the run before any
    stage has run. Relative outputs live in the work directory."""
    for stage in profile['stages']:
        sid = stage['id']
        outs: dict[str, Path] = {}
        for name, value in stage['outputs'].items():
            p = (ctx.work / ctx.expand(value)).resolve()
            if p == ctx.work:
                raise BuilderError(f'profile: output {name} of stage {sid} is the work directory itself', EXIT_USAGE)
            if not p.is_relative_to(ctx.work):
                if stage['adapter'] != 'command' or stage.get('external_outputs') is not True:
                    raise BuilderError(f'profile: output {name} of stage {sid} ({p}) is outside the work directory; only '
                                       f'a command stage that says "external_outputs": true may write there', EXIT_USAGE)
                check_private_path(p, f'output {name} of stage {sid}')
            outs[name] = p
        for text in tool_paths(stage) + list(stage.get('inputs', {}).values()) + [stage.get('source') or '']:
            ctx.expand(text)                    # what a stage reads comes from earlier stages only
        ctx.outputs[sid] = outs                 # its command line may name its own outputs
        for text in list(stage.get('argv', [])) + list(stage.get('expect', {}).values()) + [stage.get('cwd') or '']:
            ctx.expand(text)
        for value in stage.get('clean', []):
            work_target(ctx, sid, value, 'clean target')
        for value in stage.get('make_dirs', []):
            work_target(ctx, sid, value, 'folder to make')


# ----------------------------------------------------------------------------------------------- identification

def xex_details(path_or_bytes) -> str:
    """Non-secret header facts for the refusal message (title, media id, version), when the reader is available."""
    try:
        sys.path.insert(0, str(TOOL_ROOT / 'tools' / 'xex_info'))
        import xex_info  # type: ignore
        info = xex_info.parse_xex2(path_or_bytes)
        exe = info.get('execution_info') or {}
        facts = [f'{k} {exe[k]}' for k in ('title_id', 'media_id', 'version', 'base_version', 'disc_number') if k in exe]
        return ', '.join(facts)
    except Exception:
        return ''
    finally:
        if sys.path and sys.path[0].endswith('xex_info'):
            sys.path.pop(0)


def identify(profile: dict, play) -> dict:
    """Returns the matching entry of the profile's games table plus the measured hash; refuses anything else."""
    names = sorted({g.get('executable', 'default.xex') for g in profile['games']})
    seen = []
    for name in names:
        if play.size(name) is None:
            continue
        digest, size = disc_hash(play, name)
        seen.append((name, digest, size))
        for g in profile['games']:
            if g.get('executable', 'default.xex') == name and g['executable_sha256'] == digest and \
                    g.get('executable_size', size) == size:
                return dict(g, executable=name, executable_sha256=digest, executable_size=size)
    if not seen:
        raise BuilderError(f'No game executable ({", ".join(names)}) on the play disc: is this the play disc of the '
                           f'game, and is the image complete?', EXIT_GAME)
    name, digest, size = seen[0]
    details = ''
    if size <= 256 << 20:
        f, _ = play.open(name)
        with f:
            details = xex_details(f.read(size))
    raise BuilderError('Unsupported game version: ' + (details + ', ' if details else '') +
                       f'executable hash {short(digest)} ({size} bytes). This build supports only: ' +
                       '; '.join(str(g.get('label', g.get('id', '?'))) for g in profile['games']) + '.', EXIT_GAME)


def check_disc_files(game: dict, discs: dict, hash_files: bool) -> list[str]:
    """Optional presence / size (/ hash) check of other disc files listed for the game. Returns problems."""
    problems = []
    for rec in game.get('disc_files', []):
        disc = discs.get(rec.get('disc', 'play'))
        if disc is None:
            continue                       # that disc was not given; a stage that needs it will say so
        size = disc.size(rec['path'])
        if size is None:
            problems.append(f'{rec.get("disc", "play")} disc: {rec["path"]} is missing')
        elif 'size' in rec and size != rec['size']:
            problems.append(f'{rec.get("disc", "play")} disc: {rec["path"]} has {size} bytes, expected {rec["size"]}')
        elif hash_files and 'sha256' in rec and disc_hash(disc, rec['path'])[0] != rec['sha256']:
            problems.append(f'{rec.get("disc", "play")} disc: {rec["path"]} has a different SHA-256')
    return problems


# ------------------------------------------------------------------------------------------------ prerequisites

def version_tuple(text: str) -> tuple[int, ...]:
    return tuple(int(x) for x in re.findall(r'\d+', text)[:3])


def check_prerequisites(profile: dict, ctx: Context, supply: dict | None = None) -> list[str]:
    """Every check runs (so one report lists everything that is missing). Returns the failures."""
    failures: list[str] = []

    def check_file(label: str, raw: str, pinned: str | None = None, executable: bool = False) -> None:
        if '${out:' in raw:
            return                            # made by an earlier stage during the run
        p = Path(ctx.expand(raw))
        if not p.exists():
            failures.append(f'{label}: missing {p}')
        elif executable and not os.access(p, os.X_OK):
            failures.append(f'{label}: not executable {p}')
        elif pinned and p.is_file() and sha256_file(p) != pinned:
            failures.append(f'{label}: {p.name} is not the pinned version (SHA-256 differs)')

    for s in profile.get('stages', []):
        for tool in s.get('tools', []):
            if isinstance(tool, dict):
                check_file(f'stage {s["id"]} tool', tool['path'], tool.get('sha256'), tool.get('executable', False))
            else:
                check_file(f'stage {s["id"]} tool', tool)
        for name, raw in s.get('inputs', {}).items():
            check_file(f'stage {s["id"]} input {name}', raw)
        if s['adapter'] == 'supplied':
            given = (supply or {}).get(s['id'])
            if given or s.get('source'):
                check_file(f'stage {s["id"]} supplied file', given or s['source'], None if given else s.get('sha256'))
            else:
                failures.append(f'stage {s["id"]}: nothing supplied (give --supply {s["id"]}=PATH)')
        if s['adapter'] == 'disc-file' and s.get('disc', 'play') not in ctx.discs:
            failures.append(f'stage {s["id"]}: needs the {s.get("disc", "play")} disc (not given)')
    for pre in profile.get('prerequisites', []):
        kind, label = pre.get('kind'), f'prerequisite {pre.get("id", "?")}'
        if kind == 'file':
            check_file(label, pre['path'], pre.get('sha256'), pre.get('executable', False))
        elif kind == 'python':
            if sys.version_info[:3] < version_tuple(pre.get('min', '3.9')):
                failures.append(f'{label}: Python {pre.get("min")} or newer needed, this is '
                                f'{".".join(map(str, sys.version_info[:3]))}')
        elif kind == 'command':
            argv = [ctx.expand(a) for a in pre['argv']]
            try:
                r = subprocess.run(argv, capture_output=True, text=True, timeout=pre.get('timeout', 30))
                text = r.stdout + r.stderr
                if r.returncode != 0:
                    failures.append(f'{label}: {Path(argv[0]).name} exited with {r.returncode}')
                elif pre.get('match') and not re.search(pre['match'], text):
                    failures.append(f'{label}: {Path(argv[0]).name} does not report the expected version '
                                    f'(pattern {pre["match"]!r})')
            except (OSError, subprocess.TimeoutExpired) as e:
                failures.append(f'{label}: cannot run {Path(argv[0]).name} ({type(e).__name__})')
        else:
            failures.append(f'{label}: unknown kind {kind!r}')
    need = profile.get('min_free_bytes', 0)
    if need:
        probe = ctx.work
        while not probe.exists():
            probe = probe.parent
        free = shutil.disk_usage(probe).free
        if free < need:
            failures.append(f'disk space: {free / 1e9:.1f} GB free at {probe}, {need / 1e9:.1f} GB needed')
    return failures


# ------------------------------------------------------------------------------------------------------- stages

def stage_definition(stage: dict) -> dict:
    return {k: v for k, v in stage.items() if k not in ('description', 'note')}


def stage_key_parts(stage: dict, ctx: Context, previous_key: str, supply: dict) -> dict:
    """Everything that decides whether a stage's old outputs are still valid. Raises if an input is missing."""
    parts = {'engine': ENGINE_KEY_VERSION, 'definition': hashlib.sha256(canonical(stage_definition(stage))).hexdigest(),
             'game': ctx.game['executable_sha256'], 'previous': previous_key, 'tools': {'builder-engine': hashlib.sha256(__loader__.get_data(__file__)).hexdigest(), 'host-support': hashlib.sha256(host_support.__loader__.get_data(host_support.__file__)).hexdigest()}, 'inputs': {}}
    for raw in tool_paths(stage):
        parts['tools'][raw] = sha256_path(Path(ctx.expand(raw)))['sha256']
    for name, raw in sorted(stage.get('inputs', {}).items()):
        parts['inputs'][name] = sha256_path(Path(ctx.expand(raw)))['sha256']
    if stage['adapter'] == 'disc-file':
        disc = ctx.discs[stage.get('disc', 'play')]
        if stage['path'] == ctx.game['executable'] and stage.get('disc', 'play') == 'play':
            parts['inputs']['disc:' + stage['path']] = ctx.game['executable_sha256']
        else:
            parts['inputs']['disc:' + stage['path']] = disc_hash(disc, stage['path'])[0]
    if stage['adapter'] == 'supplied':
        parts['inputs']['supplied'] = sha256_path(supplied_source(stage, ctx, supply))['sha256']
    return parts


def supplied_source(stage: dict, ctx: Context, supply: dict) -> Path:
    if supply.get(stage['id']):
        return Path(supply[stage['id']])
    if not stage.get('source'):
        raise BuilderError(f'stage {stage["id"]}: nothing supplied (give --supply {stage["id"]}=PATH)', EXIT_PREREQ)
    return Path(ctx.expand(stage['source']))


def key_of(parts: dict) -> str:
    return hashlib.sha256(canonical(parts)).hexdigest()


def receipt_path(ctx: Context, stage_id: str) -> Path:
    return ctx.work / 'receipts' / f'{stage_id}.json'


def read_receipt(ctx: Context, stage_id: str) -> dict | None:
    try:
        r = json.loads(receipt_path(ctx, stage_id).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    return r if isinstance(r, dict) and r.get('schema') == RECEIPT_SCHEMA else None


def receipt_verdict(receipt: dict | None, parts: dict, outputs: dict[str, Path]) -> str | None:
    """None when the receipt covers this stage (skip); otherwise the reason it has to run."""
    if receipt is None:
        return 'no receipt'
    if receipt.get('key') != key_of(parts):
        old = receipt.get('key_parts', {})
        for group in ('inputs', 'tools'):
            for name, digest in parts[group].items():
                if old.get(group, {}).get(name) != digest:
                    return f'{group[:-1]} {name} changed'
        for field in ('definition', 'game', 'previous', 'engine'):
            if old.get(field) != parts[field]:
                return {'previous': 'an earlier stage changed', 'definition': 'the stage definition changed',
                        'game': 'a different game executable', 'engine': 'the builder changed'}[field]
        return 'key differs'
    recorded = receipt.get('outputs', {})
    for name, path in outputs.items():
        rec = recorded.get(name)
        if rec is None or not path.exists():
            return f'output {name} is missing'
        if sha256_path(path)['sha256'] != rec['sha256']:
            return f'output {name} was modified'
    return None


def clean_outputs(stage: dict, ctx: Context, outputs: dict[str, Path]) -> None:
    """Removes what the stage is about to rebuild, only inside the work directory."""
    targets = [work_target(ctx, stage['id'], c, 'clean target') for c in stage.get('clean', [])]
    targets += [p for p in outputs.values() if p.is_relative_to(ctx.work)]
    for t in targets:
        if t.is_dir():
            shutil.rmtree(t)
        elif t.exists():
            t.unlink()


def file_stamp(path: Path) -> tuple[int, int, int]:
    st = path.stat()
    return st.st_ino, st.st_size, st.st_mtime_ns


def run_stage(stage: dict, ctx: Context, outputs: dict[str, Path], supply: dict, nice: int) -> dict:
    """Runs one adapter. Returns extra facts for the receipt. Raises BuilderError on failure."""
    sid, adapter = stage['id'], stage['adapter']
    extra: dict = {}
    clean_outputs(stage, ctx, outputs)
    for value in stage.get('make_dirs', []):
        work_target(ctx, sid, value, 'folder to make').mkdir(parents=True, exist_ok=True)
    # Outputs outside the work directory ("external_outputs"): the file about to be replaced is kept, and afterwards
    # the command must really have written the file.
    outside = {n: p for n, p in outputs.items() if not p.is_relative_to(ctx.work)}
    stamps: dict[str, tuple[int, int, int]] = {}
    for name, p in outside.items():
        if p.is_file():
            stamps[name] = file_stamp(p)
            kept = ctx.work / 'backup' / sid / f'{name}-{short(sha256_file(p))}'
            if not kept.exists():
                kept.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(p, kept)
            extra.setdefault('previous_files', {})[name] = str(kept)
            print(f'  {sid:<16} kept        the {name} file it replaces: {kept}')
    if adapter != 'command':            # a command creates its own output directories (some tools demand a new one)
        for p in outputs.values():
            p.parent.mkdir(parents=True, exist_ok=True)
    if adapter == 'disc-file':
        (name, dest), = outputs.items()
        disc_copy(ctx.discs[stage.get('disc', 'play')], stage['path'], dest)
    elif adapter == 'supplied':
        (name, dest), = outputs.items()
        src = supplied_source(stage, ctx, supply)
        if src.is_dir():
            shutil.copytree(src, dest)
        else:
            shutil.copyfile(src, dest)
    else:
        argv = [ctx.expand(a) for a in stage['argv']]
        argv, priority_options = host_support.low_priority(argv, nice)
        env = {k: v for k, v in os.environ.items() if not k.endswith('_MUTANT')}   # this project's test switches
        env['PYTHONHASHSEED'] = '0'
        log = ctx.work / 'logs' / f'{sid}.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        cwd = Path(ctx.expand(stage['cwd'])) if stage.get('cwd') else ctx.repo
        with open(log, 'wb') as out:
            try:
                r = subprocess.run(argv, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **priority_options)
            except OSError as e:
                raise BuilderError(f'stage {sid}: cannot start {Path(argv[0]).name}: {e}')
        extra['log'] = str(log)
        if r.returncode != 0:
            raise BuilderError(f'stage {sid}: command exited with {r.returncode}; log: {log}')
    for name, p in outputs.items():
        if not p.exists():
            raise BuilderError(f'stage {sid}: output {name} was not produced ({p})')
    for name, p in outside.items():
        if not p.is_file() or stamps.get(name) == file_stamp(p):
            raise BuilderError(f'stage {sid}: output {name} ({p}) is outside the work directory and the command did not '
                               f'write it as a file: an older file there is not accepted as built by this run')
    return extra


def provenance_of(stage: dict) -> str:
    return {'disc-file': 'disc', 'supplied': 'supplied', 'command': 'built'}[stage['adapter']]


def run_pipeline(profile: dict, ctx: Context, *, dry_run: bool, allow_heavy: bool, supply: dict,
                 stop_after: str | None, nice: int) -> tuple[list[dict], int]:
    """Walks the stages in order. Returns (per-stage report rows, exit code)."""
    rows: list[dict] = []
    previous_key = 'start'
    upstream_runs: str | None = None          # dry run: the first earlier stage that would run
    for stage in profile['stages']:
        sid = stage['id']
        outputs = ctx.outputs[sid]
        row = {'stage': sid, 'adapter': stage['adapter'], 'heavy': bool(stage.get('heavy')),
               'provenance': provenance_of(stage)}
        rows.append(row)
        if upstream_runs:
            row.update(action='would-run', reason=f'earlier stage {upstream_runs} runs')
            print(f'  {sid:<16} would run   (earlier stage {upstream_runs} runs)' + ('  [heavy]' if row['heavy'] else ''))
            continue
        try:
            parts = stage_key_parts(stage, ctx, previous_key, supply)
        except BuilderError as e:
            row.update(action='failed', reason=str(e))
            print(f'  {sid:<16} FAILED      {e}')
            return rows, EXIT_STAGE
        key = key_of(parts)
        reason = receipt_verdict(read_receipt(ctx, sid), parts, outputs)
        row['key'] = key
        if reason is None:
            row.update(action='skipped', reason='receipt matches')
            print(f'  {sid:<16} skipped     (receipt {short(key)}: inputs, tools and outputs unchanged)')
        elif dry_run:
            row.update(action='would-run', reason=reason)
            upstream_runs = sid
            print(f'  {sid:<16} would run   ({reason})' + ('  [heavy]' if row['heavy'] else ''))
        elif row['heavy'] and not allow_heavy:
            row.update(action='stopped-before-heavy', reason=reason)
            print(f'  {sid:<16} NOT RUN     heavy stage ({reason}); rerun with --allow-heavy on the build machine')
            return rows, EXIT_HEAVY
        else:
            t0 = time.monotonic()
            try:
                extra = run_stage(stage, ctx, outputs, supply, nice)
                recorded = {}
                for name, p in outputs.items():
                    recorded[name] = dict(sha256_path(p), path=str(p))
                    want = stage.get('expect', {}).get(name)
                    if want and recorded[name]['sha256'] != ctx.expand(want):
                        raise BuilderError(f'stage {sid}: output {name} has SHA-256 {short(recorded[name]["sha256"])}, '
                                           f'the profile expects {short(ctx.expand(want))}')
            except BuilderError as e:
                row.update(action='failed', reason=str(e))
                print(f'  {sid:<16} FAILED      {e}')
                return rows, e.code
            seconds = round(time.monotonic() - t0, 2)
            receipt = dict(schema=RECEIPT_SCHEMA, stage=sid, key=key, key_parts=parts, outputs=recorded,
                           seconds=seconds, provenance=row['provenance'], **extra)
            write_atomic(receipt_path(ctx, sid), json.dumps(receipt, indent=1, sort_keys=True).encode() + b'\n')
            row.update(action='ran', reason=reason, seconds=seconds)
            print(f'  {sid:<16} ran         ({reason}; {seconds} s; receipt {short(key)})')
        previous_key = key
        if stop_after == sid:
            row['stopped_after'] = True
            print(f'  stopped after {sid} as requested')
            return rows, EXIT_OK
    return rows, EXIT_OK


def plan_output(profile: dict, ctx: Context) -> tuple[str, str, str]:
    """Validate the named file delivered by the final stage before any stage runs."""
    spec = profile.get('output')
    if not isinstance(spec, dict):
        raise BuilderError('profile: output must name a stage file', EXIT_USAGE)
    name = ctx.expand(str(spec.get('name', '')))
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,120}', name) or name.endswith('.part'):
        raise BuilderError(f'profile: bad output name {name!r}', EXIT_USAGE)
    stage, out = spec.get('stage'), spec.get('output')
    if stage not in ctx.outputs or out not in ctx.outputs[stage]:
        raise BuilderError(f'profile: the output names no output of a stage ({stage}:{out})', EXIT_USAGE)
    return name, stage, out


# ---------------------------------------------------------------------------------------------------------- CLI

def parse_supply(items: list[str]) -> dict:
    out = {}
    for item in items:
        if '=' not in item:
            raise BuilderError(f'--supply wants STAGE=PATH, got {item!r}', EXIT_USAGE)
        stage, path = item.split('=', 1)
        out[stage] = path
    return out


def cmd_build(a) -> int:
    profile = load_profile(a.profile)
    work = a.work.resolve()
    out_dir = (a.out or work / 'out').resolve()
    check_private_path(work, 'work directory')
    check_private_path(out_dir, 'output directory')
    if a.report:
        check_private_path(a.report, 'report file')
    check_work_dir(work)
    stage_ids = [s['id'] for s in profile['stages']]
    if a.stop_after and a.stop_after not in stage_ids:
        raise BuilderError(f'--stop-after {a.stop_after}: the profile has no such stage (it has: '
                           f'{", ".join(stage_ids)})', EXIT_USAGE)
    ctx = Context(profile, work, a.repo, a.data_root)
    supply = parse_supply(a.supply)
    report: dict = {'profile': profile.get('id', ''), 'dry_run': a.dry_run, 'work': str(work)}
    started = False                     # True once the work directory is in use

    def finish(code: int) -> int:
        report['exit_code'] = code
        target = a.report or (work / 'last-run.json' if started else None)
        if target:
            write_atomic(target, json.dumps(report, indent=1, sort_keys=True).encode() + b'\n')
        return code

    print(f'Profile {profile.get("id", "?")} (flavour {profile.get("flavour", "-")}); work directory {work}'
          + ('; DRY RUN: no stage runs and nothing is written but the --report file (the version commands of the '
             'prerequisites do run)' if a.dry_run else ''))
    # 1. identify
    ctx.discs['play'] = open_disc(a.play_disc)
    if a.install_disc:
        ctx.discs['install'] = open_disc(a.install_disc)
    ctx.game = identify(profile, ctx.discs['play'])
    report['game'] = {k: ctx.game[k] for k in ('id', 'executable', 'executable_sha256', 'executable_size') if k in ctx.game}
    print(f'Game: {ctx.game.get("label", ctx.game.get("id", "?"))} ({ctx.discs["play"].kind} input, '
          f'{ctx.game["executable"]} {short(ctx.game["executable_sha256"])}, {ctx.game["executable_size"]} bytes)')
    if 'install' not in ctx.discs:
        print('Install disc: not given (stages that require it will refuse the plan).')
    # 2. plan: every path and variable of the profile, before anything runs
    plan_paths(profile, ctx)
    output = plan_output(profile, ctx)
    output_path = out_dir / output[0]
    # 3. prerequisites (and the other disc files), all before any stage
    failures = check_disc_files(ctx.game, ctx.discs, a.hash_disc_files) + check_prerequisites(profile, ctx, supply)
    for s in supply:
        if s not in [x['id'] for x in profile['stages'] if x['adapter'] == 'supplied']:
            failures.append(f'--supply {s}: no stage of the "supplied" kind has this name')
    report['prerequisite_failures'] = failures
    if failures:
        print(f'Prerequisites: {len(failures)} problem(s)')
        for f in failures:
            print(f'  MISSING  {f}')
        if not a.dry_run:
            print('Stopped before any stage ran.')
            return finish(EXIT_PREREQ)
    else:
        print('Prerequisites: all present.')
    # 4. stages
    print('Stages:')
    if not a.dry_run:
        work.mkdir(parents=True, exist_ok=True)
        if not (work / WORK_MARKER).exists():
            write_atomic(work / WORK_MARKER, b'Work directory of tools/own_disc/disc_builder.py: the builder removes and '
                                             b'rewrites folders in here.\n')
        started = True
    rows, code = run_pipeline(profile, ctx, dry_run=a.dry_run, allow_heavy=a.allow_heavy, supply=supply,
                              stop_after=a.stop_after, nice=a.nice)
    report['stages'] = rows
    if a.dry_run:
        would = [r['stage'] for r in rows if r['action'] == 'would-run']
        print(f'  {"output":<16} would be written')
        print(f'Dry run: {len(would)} stage(s) would run ({", ".join(would) or "none"}), '
              f'{sum(r["action"] == "skipped" for r in rows)} covered by receipts.')
        return finish(EXIT_PREREQ if failures else code)
    if code != EXIT_OK or any(r.get('stopped_after') for r in rows):
        return finish(code)
    source = ctx.outputs[output[1]][output[2]]
    if not source.is_file():
        print(f'  {"output":<16} FAILED      the stage {output[1]} did not write {source.name}')
        return finish(EXIT_STAGE)
    check_private_path(output_path, 'the output')
    output_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = output_path.with_name(output_path.name + '.part')
    shutil.copyfile(source, tmp)
    os.replace(tmp, output_path)
    digest = sha256_file(output_path)
    report['output'] = {'path': str(output_path), 'sha256': digest, 'size': output_path.stat().st_size}
    print(f'  {"output":<16} written     {output_path}')
    print(f'Done. Output {output_path.name}: sha256 {digest}')
    return finish(EXIT_OK)


def cmd_identify(a) -> int:
    profile = load_profile(a.profile)
    disc = open_disc(a.play_disc)
    game = identify(profile, disc)
    print(f'{game.get("label", game.get("id", "?"))}: {game["executable"]} {short(game["executable_sha256"])}, '
          f'{game["executable_size"]} bytes ({disc.kind} input)')
    return EXIT_OK


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split('\n\n', 1)[1])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build', help='identify, check prerequisites, run the stages, publish the output file')
    b.add_argument('--profile', type=Path, required=True)
    b.add_argument('--play-disc', type=Path, required=True)
    b.add_argument('--install-disc', type=Path)
    b.add_argument('--work', type=Path, required=True)
    b.add_argument('--out', type=Path, help='where the output file goes (default WORK/out)')
    b.add_argument('--dry-run', action='store_true')
    b.add_argument('--allow-heavy', action='store_true')
    b.add_argument('--stop-after')
    b.add_argument('--supply', action='append', default=[], metavar='STAGE=PATH')
    b.add_argument('--hash-disc-files', action='store_true', help='also hash the other listed disc files (slow)')
    b.add_argument('--report', type=Path)
    b.add_argument('--repo', type=Path)
    b.add_argument('--data-root', type=Path)
    b.add_argument('--nice', type=int, default=10)
    b.set_defaults(fn=cmd_build)
    i = sub.add_parser('identify', help='only identify the game on the play disc')
    i.add_argument('--profile', type=Path, required=True)
    i.add_argument('--play-disc', type=Path, required=True)
    i.set_defaults(fn=cmd_identify)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except BuilderError as e:
        print(f'ERROR: {e}')
        return e.code


if __name__ == '__main__':
    raise SystemExit(main())
