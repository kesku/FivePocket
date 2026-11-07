"""Read-only whole-request clear requirements inventory; never native admission."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

from xenos_preflight_batch import BatchLimits, _require_private_input, _read_private_input


def clear_requirements(meta, payloads):
    if not isinstance(meta, dict) or meta.get('version') != 1:
        raise ValueError('clear capture version1 required')
    for key in ('stack', 'device', 'flags', 'rectangle', 'color', 'stencil', 'link', 'depth_bits'):
        value = meta.get(key)
        if type(value) is not int or not 0 <= value < (1 << (64 if key == 'depth_bits' else 32)):
            raise ValueError('invalid captured argument: ' + key)
    issues, unavailable, surfaces = [], [], []
    def read(name, size, needed=True):
        data = payloads.get(name)
        if data is None or not len(data):
            if needed: unavailable.append(name)
            return None
        if len(data) != size:
            issues.append('malformed ' + name)
            return None
        return data
    state = read('device-state.bin', 13864)
    rect_bytes, color_bytes = read('clear-rectangle.bin', 16), read('clear-color.bin', 16)
    rectangle = list(struct.unpack('>4i', rect_bytes)) if rect_bytes else None
    color = list(struct.unpack('>4f', color_bytes)) if color_bytes else None
    # Keep the JSON report strict even for captured NaN/Inf input.
    reported_color = [f if math.isfinite(f) else str(f) for f in color] if color else None
    if color and any(not math.isfinite(f) or not 0 <= f <= 1 for f in color):
        issues.append('clear color outside finite color8 profile')
    flags = meta['flags']
    if not flags & 1 or flags & ~0x3F: issues.append('unsupported color8 clear flags')
    effective, binned, clipped = None, None, None
    depth = struct.unpack('>d', meta['depth_bits'].to_bytes(8, 'big'))[0]
    depth_encoding = None
    if state:
        word = lambda at: struct.unpack_from('>I', state, at)[0]
        objects, caches = [word(12816+4*i) for i in range(5)], [word(13104+4*i) for i in range(5)]
        effective = flags if objects[4] else flags & ~0x30
        binned = bool(state[11068] & 0x10 or state[11068] & 0x20 and
                      all(not obj or obj == cache for obj, cache in zip(objects, caches)))
        if binned: issues.append('binned clear requires tile-cache execution')
        if effective & 0x30 and effective & 0x30 != 0x30:
            issues.append('single-aspect clear requires preservation of the other aspect')
        if any(flags & (1 << i) and objects[i] for i in range(1, 4)):
            issues.append('multiple selected color targets')
        for i, obj in enumerate(objects):
            header = read(f'surface-{i}-resource.bin', 40, bool(obj))
            record = dict(slot=i, object=obj, tile_cache=caches[i], header_available=header is not None,
                          header_reader='graphics' if obj >= 0x20000000 else 'heap')
            if header and obj:
                info, color_info = struct.unpack_from('>2I', header, 24)
                dimensions = struct.unpack_from('>I', header, 36)[0]
                record.update(width=(dimensions >> 18)+1, height=((dimensions >> 3) & 32767)+1,
                              pitch=info & 16383, msaa=(info >> 16) & 3, base_tile=color_info & 2047,
                              format=(color_info >> 16) & (1 if i == 4 else 15))
                mode = record['msaa']
                record['sample_count'] = 1 << mode if mode < 3 else None
                if i == 0:
                    if info != word(10368) or color_info != word(10372): issues.append('color header/shadow mismatch')
                    if mode > 2 or info & 0xC000 or color_info & ~0x107FF:
                        issues.append('unsupported sample/color-format/exponent fields')
                    pitch, width, height = record['pitch'], record['width'], record['height']
                    if not pitch or pitch % 80 or pitch < width or width > 1280 or height > 720:
                        issues.append('unsupported color8 pitch/extent')
                    tiles = ((pitch*(2 if mode == 2 else 1)+79)//80)*((height*(2 if mode else 1)+15)//16)
                    record['tile_count'] = tiles
                    if not tiles or record['base_tile']+tiles > 2048: issues.append('color EDRAM wraps')
                if i == 4 and effective & 0x30:
                    color_surface = surfaces[0]
                    if mode > 2 or info & 0xC000 or color_info & ~0x107FF or record['format'] != 1:
                        issues.append('unsupported depth sample/format fields')
                    if info != word(10368) or color_info != word(10376): issues.append('depth header/shadow mismatch')
                    if (record['width'],record['height'],record['sample_count']) != tuple(
                            color_surface.get(k) for k in ('width','height','sample_count')):
                        issues.append('color/depth extent or sample mismatch')
                    pitch = record['pitch']
                    if not pitch or pitch % 80 or pitch < record['width']: issues.append('unsupported depth pitch')
                    tiles = ((pitch*(2 if mode == 2 else 1)+79)//80)*((record['height']*(2 if mode else 1)+15)//16)
                    record['tile_count'] = tiles
                    if not tiles or record['base_tile']+tiles > 2048: issues.append('depth EDRAM wraps')
                    if 'tile_count' in color_surface and (record['object'] == color_surface['object'] or
                            record['base_tile'] < color_surface['base_tile']+color_surface['tile_count'] and
                            color_surface['base_tile'] < record['base_tile']+tiles):
                        issues.append('color/depth EDRAM overlap')
            surfaces.append(record)
        if effective & 0x30:
            if math.isfinite(depth) and 0 <= depth <= 1 and struct.unpack('>f',struct.pack('>f',depth))[0] == depth:
                if depth == 0: depth_encoding = 0
                elif depth < 2**-14:
                    scaled = math.ldexp(depth,34)
                    if scaled.is_integer(): depth_encoding = int(scaled)
                else:
                    fraction, exponent = math.frexp(depth)
                    mantissa = math.ldexp(fraction,21)-0x100000
                    if mantissa.is_integer(): depth_encoding = ((exponent+14)<<20)|int(mantissa)
            if depth_encoding is None or meta['stencil'] > 255:
                issues.append('depth/stencil requires unproved rounding or conversion')
        viewport = struct.unpack_from('>4f', state, 13024)
        if any(not math.isfinite(f) or not -8192 <= f <= 8192 for f in viewport):
            issues.append('viewport conversion unsupported')
        elif rectangle:
            v = list(map(int, viewport)); v[2] += v[0]; v[3] += v[1]
            clipped = [max(rectangle[i], v[i]) if i < 2 else min(rectangle[i], v[i]) for i in range(4)]
            if word(12288):
                scissor = struct.unpack_from('>4i', state, 13052)
                clipped = [max(clipped[i], scissor[i]) if i < 2 else min(clipped[i], scissor[i]) for i in range(4)]
            extent = surfaces[0]
            if 'width' in extent and clipped != [0, 0, extent['width'], extent['height']]:
                issues.append('partial/empty clear requires preservation of existing samples')
        if not objects[0]: issues.append('no color0 target')
    return dict(classification='read-only requirements inventory, NOT native admission',
                issues=issues, unavailable=unavailable, effective_flags=effective, binned=binned,
                surfaces=surfaces, rectangle=rectangle, clipped_rectangle=clipped, color=reported_color,
                depth=depth if math.isfinite(depth) else str(depth), stencil=meta['stencil'],
                exact_depth_encoding=depth_encoding,
                captured_issue_mask=meta.get('issues'),
                unverified=['bound allocation lifetime and header ownership',
                            'overlapping EDRAM views and preservation of unaffected contents',
                            'actual GPU completion; later draw shader, gamma blending and resolve contracts',
                            'D32 stores exact expanded20e4 clear values, not general Xbox depth-test equivalence',
                            'backchain links are captured evidence, not an independently verified call graph',
                            'gameplay'],
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
    meta = json.loads(read('clear.json'))
    names = ['device-state.bin', 'clear-rectangle.bin', 'clear-color.bin'] + [f'surface-{i}-resource.bin' for i in range(5)]
    report = clear_requirements(meta, {name: read(name, True) for name in names})
    report['source_sha256'] = {name: hashlib.sha256((root/name).read_bytes()).hexdigest() for name in (
        'tools/rpf7_inventory/clear_capture_preflight.py', 'runtime/host/gta5_color8_clear_plan.h',
        'runtime/host/gta5_color8_clear.h', 'runtime/host/gta5_clear_capture.h',
        'runtime/host/xenos_float24_clear.h',
        'runtime/host/native_color8_clear_target.h')}
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == '__main__': main()
