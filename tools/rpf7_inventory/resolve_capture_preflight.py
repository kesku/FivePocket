"""Read-only multi-requirement inventory for a stopped resolve (not admission).

Run against ignored local-analysis captures. Lists missing evidence separately
from known unsupported operations; never guesses v1's missing pointed arguments.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

from xenos_preflight_batch import BatchLimits, _require_private_input, _read_private_input


def resolve_requirements(meta, payloads):
    if not isinstance(meta, dict) or meta.get('version') not in (1, 2):
        raise ValueError('resolve capture version1 or2 required')
    args = meta.get('arguments')
    if not isinstance(args, list) or len(args) != 10 or any(
            type(a) is not int or not 0 <= a <= 0xFFFFFFFF for a in args):
        raise ValueError('ten captured DWORD arguments required')
    device, flags, rect, destination, point, mip, slice_, color, lr, source = args
    issues, unavailable, unverified = [], [], []
    def read(name, size, needed=True):
        b = payloads.get(name)
        if b is None or len(b) == 0:
            if needed: unavailable.append(name)
            return None
        if len(b) not in (size if isinstance(size, tuple) else (size,)):
            issues.append('malformed ' + name)
            return None
        return b
    if flags & ~0x100:
        issues.append('unsupported resolve flags/source/depth-clear: ' + hex(flags & ~0x100))
    if not destination or destination & 3 or destination > 0xFFFFFFFF - 51:
        issues.append('destination object alignment/extent')
    if slice_: issues.append('nonzero slice')
    for name in ('stack-92.bin', 'stack-100.bin'):
        b = read(name, 4)
        if b and any(b): issues.append('nonzero ' + name)
    plan = meta.get('source_plan', {})
    extent = [plan.get(k) for k in ('width', 'height')]
    known_extent = all(type(v) is int and 0 < v <= limit for v, limit in zip(extent, (1280, 720)))
    if not known_extent: unverified.append('source full written extent')
    rectangle = None
    if rect:
        if rect & 3 or rect > 0xFFFFFFFF - 15: issues.append('source rectangle pointer alignment/extent')
        b = read('source-rectangle.bin', 16)
        if b:
            rectangle = list(struct.unpack('>4i', b))
            if rectangle[:2] != [0, 0] or min(rectangle[2:]) <= 0 or (
                    known_extent and rectangle[2:] != extent):
                issues.append('partial or out-of-range source rectangle')
    if point:
        if point & 3 or point > 0xFFFFFFFF - 7: issues.append('destination point pointer alignment/extent')
        b = read('destination-point.bin', 8)
        if b and any(b): issues.append('nonzero destination offset')
    if flags & 0x100:
        if not color: issues.append('default clear constant requires original-image evidence')
        elif color & 15 or color > 0xFFFFFFFF - 15: issues.append('clear color pointer alignment/extent')
        b = read('clear-color.bin', 16)
        if b and any(not math.isfinite(f) or not 0 <= f <= 1 for f in struct.unpack('>4f', b)):
            issues.append('clear color outside finite UNORM profile')
    header = read('destination-resource.bin', 52)
    texture = None
    if header:
        w = struct.unpack_from('>6I', header, 28)
        texture = dict(width=(w[2] & 8191)+1, height=((w[2] >> 13) & 8191)+1,
                       pitch=((w[0] >> 22) & 511)*32, format=w[1] & 63,
                       endian=(w[1] >> 6) & 3, tiled=bool(w[0] >> 31),
                       mip_min=(w[4] >> 2) & 15, mip_max=(w[4] >> 6) & 15,
                       packed_mips=bool(w[5] & 2048), selected_mip=mip,
                       base_address=w[1] & 0xFFFFF000, mip_address=w[5] & 0xFFFFF000,
                       swizzle=[(w[3] >> (1+3*c)) & 7 for c in range(4)])
        if w[0] & 3 != 2 or (w[5] >> 9) & 3 != 1 or w[1] & 1024:
            issues.append('destination is not a single 2D texture')
        if texture['format'] != 6 or not texture['tiled']: issues.append('destination is not tiled RGBA8')
        if texture['pitch'] < texture['width'] or not w[1] & 0xFFFFF000:
            issues.append('destination pitch/base storage')
        if w[0] & 0x3FC or w[3] & (1 | (63 << 13)):
            issues.append('destination sign/exponent/integer conversion')
        if texture['mip_min'] or mip > texture['mip_max']:
            issues.append('requested mip outside supported declared range')
        if texture['packed_mips']:
            issues.append('packed mip storage requires a separate address proof')
        if bool(texture['mip_max']) != bool(texture['mip_address']):
            issues.append('mip range/address disagreement')
        s = texture['swizzle']
        if s[:3] not in ([0, 1, 2], [2, 1, 0]) or s[3] not in (3, 5):
            issues.append('destination channel conversion')
        texture['selected_extent'] = [max(texture['width'] >> mip,1),max(texture['height'] >> mip,1)] if mip <= 15 else None
        if known_extent and mip <= texture['mip_max'] and texture['selected_extent'] != extent:
            issues.append('source/destination extent mismatch')
    surface = read('source-resource.bin', (32,40))
    source_surface = None
    if surface:
        info, color_info = struct.unpack_from('>2I', surface, 24)
        source_surface = dict(pitch=info & 0x3FFF, base_tile=color_info & 0x7FF,
                              format=(color_info >> 16) & 15, msaa=(info >> 16) & 3)
        if info & 0x3C000 or color_info & ~0x7FF:
            issues.append('source sample/format/exponent profile')
        pitch = source_surface['pitch']
        if not pitch or pitch % 80 or (known_extent and pitch < extent[0]):
            issues.append('source pitch profile')
        if known_extent and pitch // 80 * ((extent[1]+15)//16) > 2048-source_surface['base_tile']:
            issues.append('source EDRAM wraps')
    unverified += ['source identity, binding, allocation lifetime and EDRAM alias ownership',
                   'actual completed GPU pixels and clear-after-copy side effects',
                   'destination allocation extent, padding and compare-before-publication',
                   'selected mip address/stride, other-level preservation and later mip initialization',
                   'later game operations and gameplay']
    return dict(classification='read-only requirements inventory, NOT native admission',
                capture_version=meta['version'], issues=issues, unavailable=unavailable,
                unverified=unverified, header_reader='graphics' if destination >= 0x20000000 else 'heap',
                clear_after_copy=bool(flags & 0x100),
                depth_value_used=bool(flags & 0x200), source_rectangle=rectangle,
                source_surface=source_surface, destination=texture,
                payload_sha256={k: hashlib.sha256(v).hexdigest() for k, v in payloads.items() if v is not None})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    limits = BatchLimits(max_file_bytes=65536)
    def read(name, optional=False):
        path = args.capture / name
        if optional and not path.exists() and not path.is_symlink(): return None
        return _read_private_input(_require_private_input(path, root, limits))
    meta = json.loads(read('resolve.json'))
    names = ('destination-resource.bin', 'source-resource.bin', 'source-rectangle.bin',
             'destination-point.bin', 'clear-color.bin', 'stack-92.bin', 'stack-100.bin')
    result = resolve_requirements(meta, {n: read(n, True) for n in names})
    result['source_sha256'] = {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in (
        'tools/rpf7_inventory/resolve_capture_preflight.py',
        'runtime/host/native_offscreen_resolve_request.h',
        'runtime/host/native_offscreen_resolve.h',
        'runtime/host/native_offscreen_draw_target.h',
        'runtime/host/xenos_texture_2d.cpp')}
    print(json.dumps(result, indent=2))


if __name__ == '__main__': main()
