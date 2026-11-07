"""Host-only portability primitives. No game data, shell, device or download code."""
from __future__ import annotations
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for data in iter(lambda: source.read(1 << 20), b''):
            digest.update(data)
    return digest.hexdigest()


def package_root(source: str) -> Path:
    return Path(os.environ.get('BUILDER_PACKAGE_ROOT', Path(source).resolve().parents[2])).resolve()


NDK_HOST_TAGS = {'darwin': 'darwin-x86_64', 'win32': 'windows-x86_64', 'linux': 'linux-x86_64'}


def ndk_host_tag(platform: str = '') -> str:
    """The name of this host's folder inside the Android NDK (toolchains/llvm/prebuilt/<tag>, shader-tools/<tag>)."""
    platform = platform or sys.platform
    tag = NDK_HOST_TAGS.get('linux' if platform.startswith('linux') else platform)
    if tag is None:
        raise ValueError(f'no NDK host for {platform}')
    return tag


def python_command(script, *args) -> list[str]:
    return [os.environ.get('BUILDER_PYTHON', sys.executable), '-X', 'utf8', str(script), *map(str, args)]


def low_priority(argv: list[str], amount: int = 10) -> tuple[list[str], dict]:
    if sys.platform == 'win32':
        return list(argv), {'creationflags': 0x00004000} if amount else {}  # BELOW_NORMAL_PRIORITY_CLASS
    nice = shutil.which('nice') if amount else None
    return ([nice, '-n', str(amount), *argv] if nice else list(argv)), {}


# Windows priority classes on the scale of nice: below normal is the class low_priority() starts tools in.
WINDOWS_CLASS_NICE = {0x00000040: 19, 0x00004000: 10, 0x00000020: 0, 0x00008000: -5, 0x00000080: -10, 0x00000100: -20}


def nice_of_priority_class(value: int) -> int:
    if value not in WINDOWS_CLASS_NICE:
        raise ValueError('unknown process priority class')
    return WINDOWS_CLASS_NICE[value]


def _windows_priority_class() -> int:
    import ctypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.GetPriorityClass.argtypes = [ctypes.c_void_p]
    kernel.GetPriorityClass.restype = ctypes.c_uint32
    value = kernel.GetPriorityClass(kernel.GetCurrentProcess())
    if not value:
        raise OSError('the process priority class could not be read')
    return value


def effective_nice() -> int:
    """This process's priority as a nice value: the system's own, or on Windows its priority class on that scale."""
    if sys.platform == 'win32':
        return nice_of_priority_class(_windows_priority_class())
    return os.getpriority(os.PRIO_PROCESS, 0)


_SEEK_LOCK = threading.Lock()


def pread_by_seek(fd: int, size: int, offset: int) -> bytes:
    """A positioned read where the system has none (Windows): the descriptor is moved and read under one lock, so
    two threads cannot read at each other's place. Up to size bytes; fewer only at the end of the file."""
    with _SEEK_LOCK:
        os.lseek(fd, offset, os.SEEK_SET)
        chunks, left = [], size
        while left > 0:
            chunk = os.read(fd, left)
            if not chunk:
                break
            chunks.append(chunk)
            left -= len(chunk)
        return b''.join(chunks)


pread = os.pread if hasattr(os, 'pread') else pread_by_seek


def clone_or_copy(src: Path, dst: Path) -> None:
    if src.resolve() == dst.resolve():
        raise ValueError('source and destination are the same file')
    if sys.platform == 'darwin':
        try:
            if dst.exists():
                dst.unlink()
            if subprocess.run(['/bin/cp', '-c', '-p', str(src), str(dst)], capture_output=True).returncode == 0:
                return
        except OSError:
            pass
    shutil.copy2(src, dst)


def dep_paths(text: str) -> list[str]:
    """LLVM/GCC make dependencies: drive colons, escaped whitespace/# and $$.

    Consume the first rule only (later -MP phony rules are not dependencies).
    Keep native backslash separators unless they escape a make special character.
    """
    text = text.replace('\\\r\n', ' ').replace('\\\n', ' ')
    line = next((s for s in text.splitlines() if s.strip()), '')
    match = re.search(r':(?:\s|$)', line)
    if not match:
        raise ValueError('dependency file has no target separator; rebuild with -MD -MF')
    words, word, i = [], [], match.end()
    while i < len(line):
        c = line[i]
        if c == '\\' and i + 1 < len(line) and line[i + 1] in ' \t#:\\':
            word.append(line[i + 1]); i += 2; continue
        if c == '$' and i + 1 < len(line) and line[i + 1] == '$':
            word.append('$'); i += 2; continue
        if c == '#':
            break
        if c.isspace():
            if word:
                words.append(''.join(word)); word = []
        else:
            word.append(c)
        i += 1
    if word:
        words.append(''.join(word))
    if not words:
        raise ValueError('dependency file has no inputs; rebuild with -MD -MF')
    return words


def relative_or_absolute(target: Path, base: Path) -> str:
    try:
        return os.path.relpath(target, base)
    except ValueError:  # Windows paths on different drives
        return str(target.resolve())


def private_output(path: Path) -> None:
    path = path.resolve()
    for parent in (path, *path.parents):
        if (parent / '.git').exists():
            if not path.is_relative_to(parent / 'local-analysis'):
                raise ValueError('output is in a checkout: choose its local-analysis directory')
            break
