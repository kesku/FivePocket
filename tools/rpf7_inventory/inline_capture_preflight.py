"""Read-only, multi-failure requirements inventory for an owned inline capture.

This is NOT a native draw admission check or proof of shader semantics. It lists
all encoded instructions even when the dataflow analyzer rejects an earlier one.
Register comparisons use the current native source tables, fingerprinted below.
Unknown control flow, dataflow and runtime ownership remain separate obligations.
No source patch, shader installation, texture sampling or device operation occurs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import struct
from collections import Counter

from raw_shader_analysis import RawShaderError
from vertex_arithmetic_dataflow import schedule, analyze_vertex_dataflow, vertex_supported_opcodes
from pixel_sample_dataflow import analyze_pixel_sample_dataflow, pixel_supported_opcodes, pixel_instruction_budget
from inline_shader_translation import translate_inline_pair


# The geometry profiles are translator capabilities that no profile name here
# could previously reach, so a capture needing one was reported as "unsupported
# opcode" against a profile it was never going to be analysed under. Naming them
# does NOT widen any admission set: this module is a read-only inventory and the
# translator's own mutual-exclusion rules still reject invalid combinations.
GEOMETRY_PROFILES = ('geometry', 'geometry-predicated', 'geometry-gradient', 'geometry-fixed16', 'geometry-lod')
PROFILES = ('legacy', 'extended', 'loading', 'scalar-fan', 'cube', 'explicit-lod', 'predicated-lod', 'composite', 'screen-space', 'post-stencil', 'screen-strip', 'world-strip', *GEOMETRY_PROFILES)


def profile_options(profile):
    if profile not in PROFILES:
        raise ValueError('unknown explicit analysis profile')
    geometry = profile in GEOMETRY_PROFILES
    return dict(extended_math=profile != 'legacy',
                loading_math=profile in ('loading', 'scalar-fan', 'cube', 'explicit-lod', 'predicated-lod', 'composite', 'screen-space', 'post-stencil', 'screen-strip', 'world-strip') or geometry,
                cube_profile=profile == 'cube', explicit_lod_profile=profile in ('explicit-lod','predicated-lod'),
                predicate_profile=profile == 'predicated-lod',
                geometry_profile=geometry,
                geometry_lod_profile=profile=='geometry-lod',
                vertex_predicate_profile=profile=='geometry-lod',
                geometry_predicate_profile=profile in ('geometry-predicated', 'geometry-fixed16'),
                geometry_gradient_depth_profile=profile in ('geometry-gradient', 'geometry-fixed16'),
                geometry_fixed16_effects_profile=profile == 'geometry-fixed16',
                composite_profile=profile in ('composite', 'screen-space'),post_stencil_profile=profile=='post-stencil')


def shader_requirements(data, stage, profile='legacy'):
    if stage not in ('vertex', 'pixel'):
        raise ValueError('explicit shader stage required')
    options = profile_options(profile)
    vertex_options = dict(dot4=True, scalar_constants=options['extended_math'], loading_math=options['loading_math'],
                          packed_geometry=options['geometry_profile'] and profile!='world-strip',
                          predicate_profile=options['vertex_predicate_profile'],
                          tonemap=options['post_stencil_profile'] or profile=='screen-strip',scalar_gt_zero=profile=='world-strip')
    # Reuse only the pixel arithmetic/effect analyzer. A world strip is NOT
    # native indexed geometry and does not inherit its draw/resource admission.
    pixel_options={**options,**(dict(geometry_profile=True,geometry_predicate_profile=True)
                               if profile=='world-strip' else {})}
    pixel_options.pop('vertex_predicate_profile')
    result = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data), 'profile': profile,
              'instructions': [], 'exports': [], 'issues': []}
    try:
        words, controls = schedule(data)
    except ValueError as error:
        result['issues'].append({'kind': 'control_flow_unknown', 'detail': str(error)})
        return result
    result['controls'] = controls
    exports = set()
    pixel_keys = ('extended_math', 'loading_math', 'cube_profile', 'predicate_profile',
                  'geometry_profile', 'geometry_predicate_profile', 'geometry_fixed16_effects_profile',
                  'composite_profile', 'post_stencil_profile', 'geometry_lod_profile')
    allowed_vectors, allowed_scalars = (vertex_supported_opcodes(**vertex_options) if stage == 'vertex' else
        pixel_supported_opcodes(ui_operations=True, **{k: pixel_options[k] for k in pixel_keys}))
    for control in controls:
        if control[0] != 'exec':
            continue
        for address in range(control[1], control[1] + control[2]):
            budget=256 if stage=='vertex' else pixel_instruction_budget(predicate_profile=options['predicate_profile'])
            if len(result['instructions']) == budget:
                result['issues'].append({'kind': 'instruction_budget'})
                return result
            a, b, c = words[address * 3:address * 3 + 3]
            row = {'address': address}
            if control[3] & (1 << ((address - control[1]) * 2)):
                row.update(kind='fetch', opcode=a & 31)
                if stage == 'vertex':
                    row.update(format=(b >> 16) & 63, stride_words=c & 255)
                    row['inline_format_profiles'] = {
                        'abi4': row['format'] in (6,25,37,38),
                        'abi5': row['format'] in (6,25,36,37,38,57)}
                    selected_format=row['inline_format_profiles']['abi5'] or (
                        vertex_options['packed_geometry'] and row['format'] in (7,31,32))
                    if not selected_format:
                        result['issues'].append({'kind': 'inline_vertex_format', 'address': address,
                                                 'format': row['format']})
                else:
                    row.update(slot=(a >> 20) & 31, source=(a >> 5) & 63,
                               destination=(a >> 12) & 63,
                               coordinate_components=[(a >> shift) & 3 for shift in (26, 28)])
                    if row['opcode'] == 24:
                        row.update(kind='set_lod')
                        row.pop('slot')
                        row.pop('destination')
                        if not (options['explicit_lod_profile'] or options['geometry_lod_profile']):
                            result['issues'].append({'kind': 'set_lod_profile', 'address': address})
                    elif row['opcode'] != 1:
                        result['issues'].append({'kind': 'fetch_opcode', 'address': address, 'opcode': row['opcode']})
                # Encoding inventory, NOT successful fetch decoding or ownership.
            else:
                row.update(kind='alu', vector=(c >> 24) & 31, scalar=a >> 26,
                           vector_mask=(a >> 16) & 15, scalar_mask=(a >> 20) & 15,
                           export=bool(a & (1 << 15)), vector_destination=a & 63,
                           scalar_destination=(a >> 8) & 63,
                           modifier_a=a & ((1 << 6) | (1 << 7) | (3 << 24)),
                           modifier_b=b >> 24)
                row.update(predicated=bool(b & (1 << 28)), predicate_condition=bool(b & (1 << 27)))
                instruction_predicate=(options['vertex_predicate_profile'] if stage=='vertex' else
                    options['predicate_profile'] or pixel_options['geometry_predicate_profile'])
                if row['predicated'] and not instruction_predicate:
                    result['issues'].append({'kind': 'instruction_predication', 'address': address,
                                             'condition': row['predicate_condition']})
                if row['predicated'] and instruction_predicate and row['scalar']==28:
                    result['issues'].append({'kind': 'predicated_predicate_mutation', 'address': address})
                if b & (7 << 29):
                    result['issues'].append({'kind': 'relative_addressing_modifier', 'address': address})
                if row['export']:
                    exports.add(row['vector_destination'])
                for field, allowed in (('vector', allowed_vectors), ('scalar', allowed_scalars)):
                    if row[field] not in allowed:
                        result['issues'].append({'kind': 'analyzer_opcode', 'address': address,
                                                 'unit': field, 'opcode': row[field]})
            result['instructions'].append(row)
    result['exports'] = sorted(exports)
    try:
        proof = (analyze_vertex_dataflow(data, **vertex_options) if stage == 'vertex' else
                 analyze_pixel_sample_dataflow(data, ui_operations=True, **pixel_options))
        result['dataflow'] = {'accepted': True, 'exports': proof['exports'],
                              'fetches': proof['fetches']}
    except ValueError as error:
        result['dataflow'] = {'accepted': False, 'first_error': str(error)}
    return result


def selected_profile_requirements(draw, payloads, profile):
    options = profile_options(profile)
    vertex = shader_requirements(payloads['vertex-shader.bin'], 'vertex', profile)
    pixel = shader_requirements(payloads['pixel-shader.bin'], 'pixel', profile)
    indexed = bool(draw.get('index_count'))
    primitive = draw.get('primitive')
    translation = dict(accepted=False, native_admission=False)
    try:
        if profile=='scalar-fan' and (indexed or primitive!=5):
            raise ValueError('scalar fan requires nonindexed primitive5')
        if profile in ('screen-strip','world-strip') and (indexed or primitive!=6):
            raise ValueError('screen strip requires nonindexed primitive6')
        if primitive not in ((6,) if profile in ('screen-strip','world-strip') else (4,) if indexed else (4,5,8,13)):
            raise ValueError('unsupported selected topology')
        _, _, proof = translate_inline_pair(payloads['vertex-shader.bin'], payloads['pixel-shader.bin'],
            indexed=indexed, extended_profile=options['extended_math'], loading_profile=options['loading_math'],
            cube_profile=options['cube_profile'], explicit_lod_profile=options['explicit_lod_profile'],
            predicate_profile=options['predicate_profile'],
            geometry_profile=options['geometry_profile'] and profile!='world-strip',
            geometry_lod_profile=options['geometry_lod_profile'],
            vertex_predicate_profile=options['vertex_predicate_profile'],
            geometry_predicate_profile=options['geometry_predicate_profile'] and profile!='world-strip',
            geometry_gradient_depth_profile=options['geometry_gradient_depth_profile'],
            geometry_fixed16_effects_profile=options['geometry_fixed16_effects_profile'],
            composite_profile=options['composite_profile'],
            screen_space_profile=profile == 'screen-space',
            post_stencil_profile=options['post_stencil_profile'],
            screen_strip_profile=profile=='screen-strip',
            world_strip_profile=profile=='world-strip',
            scalar_fan_profile=profile=='scalar-fan',
            direct_triangles=not indexed and primitive == 4,
            triangle_fan=not indexed and primitive == 5,
            rectangle_list=not indexed and primitive == 8)
        translation.update(accepted=True, abi=proof['abi_version'])
        if profile == 'screen-space':
            # Interface capability only. The native SQ/context registers,
            # target ownership and connected consumers still need live proof.
            translation.update(screen_space_parameter_register=proof['screen_space_parameter_register'],
                               screen_space_native_state_verified=False)
    except ValueError as error:
        translation['first_error'] = str(error)
    return dict(profile=profile, vertex=vertex, pixel=pixel, translation=translation)


def compact_summary(result):
    selected = result['selected_profile']
    stages = {}
    for stage in ('vertex', 'pixel'):
        proof = selected[stage]
        stages[stage] = dict(bytes=proof['bytes'], sha256=proof['sha256'],
            instructions=len(proof['instructions']),
            instruction_kinds=dict(Counter(r['kind'] for r in proof['instructions'])),
            issues=dict(Counter(r['kind'] for r in proof['issues'])),
            unsupported_opcodes=sorted({(r['unit'], r['opcode']) for r in proof['issues'] if r['kind']=='analyzer_opcode'}),
            dataflow=proof.get('dataflow', {'accepted': False}))
        stages[stage]['dataflow'] = {k:v for k,v in stages[stage]['dataflow'].items() if k in ('accepted','first_error')}
    return dict(classification=result['classification'], profile=selected['profile'],
                translation=selected['translation'], stages=stages, native_admission=False)


def literal_register_requirements(source):
    """Only exact literal pairs; dynamic linkage/viewport are separate below."""
    pairs = re.findall(r'\{\{(0x[0-9A-Fa-f]+),(0x[0-9A-Fa-f]+|[0-9]+)\}\}', source)
    result = {}
    for register, value in pairs:
        key, expected = int(register, 0), int(value, 0)
        if key in result and result[key] != expected:
            raise ValueError('conflicting literal register requirements')
        result[key] = expected
    return result


def draw_register(state, scissor, index):
    if 0x2080 <= index <= 0x2082:
        return scissor[index - 0x2080] if isinstance(scissor, list) and len(scissor) == 3 else None
    blocks = ((0x2000,16,10368), (0x2100,21,10444), (0x2180,5,10528),
              (0x2200,12,10548), (0x2280,21,10596), (0x2300,38,10680),
              (0x2380,8,10832), (0x4900,40,10112))
    for first, count, offset in blocks:
        if first <= index < first + count:
            at = offset + (index - first) * 4
            return struct.unpack_from('>I', state, at)[0] if at + 4 <= len(state) else None
    return None


def compare_state(state, scissor, literal_requirements):
    # These registers are dynamically interpreted by nativeTranslatedViewport/
    # Scissor, not compared against the original full-screen literal values.
    dynamic = {0x2081, 0x2082, 0x210F, 0x2110, 0x2111, 0x2112, 0x2302}
    rows = []
    for register, expected in sorted(literal_requirements.items()):
        if register in dynamic:
            continue
        actual = draw_register(state, scissor, register)
        if actual != expected:
            rows.append({'register': hex(register), 'expected': expected, 'actual': actual})
    return rows


def capture_requirements(draw, payloads, native_sources, profile='legacy'):
    vertex = shader_requirements(payloads['vertex-shader.bin'], 'vertex')
    pixel = shader_requirements(payloads['pixel-shader.bin'], 'pixel')
    issues = []
    indexed = bool(draw.get('index_count'))
    if draw.get('primitive') != (4 if indexed else 13):
        issues.append({'kind': 'topology', 'primitive': draw.get('primitive'), 'indexed': indexed})
    count, stride = draw.get('vertex_count'), draw.get('stride')
    if (type(count) is not int or type(stride) is not int or count <= 0 or stride <= 0 or
            count * stride != len(payloads['vertices.bin'])):
        issues.append({'kind': 'vertex_extent'})
    if not indexed and type(count) is int and count % 4:
        issues.append({'kind': 'quad_expansion_extent', 'vertex_count': count})
    samples = [r for r in pixel['instructions'] if r['kind'] == 'fetch']
    slots = [r['slot'] for r in samples]
    if len(samples) > 2:
        issues.append({'kind': 'sample_instruction_count', 'actual': len(samples), 'supported_max': 2})
    if len(set(slots)) != len(slots):
        issues.append({'kind': 'repeated_texture_slot', 'slots': slots})
    available = {t.get('slot') for t in draw.get('textures', [])}
    missing = sorted(set(slots) - available)
    if missing:
        issues.append({'kind': 'missing_captured_texture_slots', 'slots': missing})
    textured = bool(samples)
    layouts = ({0,1,62}, {0,1,2,62}, {0,1,2,3,62}) if textured else ({0,62},)
    if set(vertex['exports']) not in layouts:
        issues.append({'kind': 'interpolator_layout', 'exports': vertex['exports'], 'textured': textured})
    try:
        translate_inline_pair(payloads['vertex-shader.bin'], payloads['pixel-shader.bin'], indexed=indexed)
        pair = {'accepted': True}
    except ValueError as error:
        pair = {'accepted': False, 'first_error': str(error)}
    # Keep legacy requirements explicit after adding the separate ABI5 runtime
    # path. Successful shader translation still proves neither native state,
    # target ownership, actual rectangle coverage nor installed program identity.
    try:
        _, _, proof = translate_inline_pair(payloads['vertex-shader.bin'], payloads['pixel-shader.bin'],
            indexed=indexed, extended_profile=True,
            direct_triangles=not indexed and draw.get('primitive') == 4)
        extended = {'accepted': True, 'abi': proof['abi_version'],
                    'direct_triangles': proof['direct_triangles'],
                    'affine_position_xy': proof['affine_position_xy']}
    except ValueError as error:
        extended = {'accepted': False, 'abi': 5, 'first_error': str(error)}
    try:
        _, _, proof = translate_inline_pair(payloads['vertex-shader.bin'], payloads['pixel-shader.bin'],
            indexed=indexed, extended_profile=True, loading_profile=True,
            direct_triangles=not indexed and draw.get('primitive') == 4,
            triangle_fan=not indexed and draw.get('primitive') == 5)
        loading = {'accepted': True, 'abi': proof['abi_version'],
                   'triangle_fan': proof['triangle_fan'], 'native_admission': False,
                   'hardware_transcendentals_verified': False}
    except ValueError as error:
        loading = {'accepted': False, 'abi': 6, 'first_error': str(error), 'native_admission': False}
    required = {}
    for source in native_sources.values():
        required.update(literal_register_requirements(source))
    state = payloads['device-state.bin']
    mismatches = compare_state(state, draw.get('scissor'), required)
    return {'classification': 'requirements inventory, not an admission certificate',
            'selected_profile': selected_profile_requirements(draw, payloads, profile),
            'legacy_requirements_abi': 4, 'extended_shader_translation': extended,
            'loading_shader_translation': loading,
            'vertex': vertex, 'pixel': pixel, 'draw_issues': issues,
            'inline_pair': pair, 'literal_state_mismatches': mismatches,
            'submitted_texture_evidence': submitted_texture_requirements(draw, slots),
            'state_for_further_interpretation': {hex(r): draw_register(state, draw.get('scissor'), r)
                 for r in (0x2000,0x2001,0x2180,0x2181,0x2182,0x2201,0x210F,0x2110,0x2111,0x2112,0x2302)},
            'unverified': ['dynamic viewport/scissor and shader linkage',
                           'texture format/filter/LOD and allocation ownership',
                           'render target identity/clear/resolve lifetime',
                           'hardware arithmetic and actual GPU execution',
                           'ABI5 native state/coverage/ownership and installed shader identity'],
            'source_sha256': {name: hashlib.sha256(s.encode()).hexdigest() for name,s in native_sources.items()},
            'payload_sha256': {name: hashlib.sha256(data).hexdigest() for name,data in payloads.items()}}


def submitted_texture_requirements(draw, required_slots):
    """Metadata evidence only. Never replace unavailable submissions with shadow.

    Fields match XenosTextureDescription::decode, not planXenosTexture2D. A
    decoded descriptor says nothing about allocation ownership or mip residency.
    """
    result = {'available': False, 'slots': [], 'issues': []}
    submitted = draw.get('submitted_textures')
    if submitted is None:
        result['issues'].append({'kind': 'submitted_descriptors_not_captured'})
        return result
    if (draw.get('submitted_texture_source') != 'driver-upload' or
            not isinstance(submitted, list) or len(submitted) != 32):
        result['issues'].append({'kind': 'invalid_submitted_descriptor_snapshot'})
        return result
    captured = {t.get('slot'): t.get('fetch') for t in draw.get('textures', [])}
    result['available'] = True
    for slot, words in enumerate(submitted):
        row = {'slot': slot, 'required': slot in required_slots, 'submitted': words is not None}
        result['slots'].append(row)
        if words is None:
            if row['required']:
                result['issues'].append({'kind': 'required_submission_unavailable', 'slot': slot})
            continue
        if (not isinstance(words, list) or len(words) != 6 or
                any(type(w) is not int or not 0 <= w <= 0xFFFFFFFF for w in words)):
            row['valid_words'] = False
            result['issues'].append({'kind': 'invalid_submitted_descriptor_words', 'slot': slot})
            continue
        row.update(valid_words=True, fetch=words, fetch_type=words[0] & 3)
        if slot in captured and captured[slot] != words:
            result['issues'].append({'kind': 'captured_submission_mismatch', 'slot': slot})
        if words[0] & 3 != 2:
            if row['required']:
                result['issues'].append({'kind': 'required_nontexture_submission', 'slot': slot})
            continue
        row.update(format=words[1] & 63, endian=(words[1] >> 6) & 3,
            base_address=words[1] & 0xFFFFF000, mip_address=words[5] & 0xFFFFF000,
            mip_min=(words[4] >> 2) & 15, mip_max=(words[4] >> 6) & 15,
            packed_mips=bool(words[5] & 2048), dimension=(words[5] >> 9) & 3,
            pitch_texels=((words[0] >> 22) & 511) * 32, tiled=bool(words[0] >> 31),
            ownership_verified=False, sampling_verified=False)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--profile', choices=PROFILES, default='legacy')
    parser.add_argument('--summary', action='store_true', help='compact selected-profile findings only')
    parser.add_argument('--output', type=Path, help='save full report exclusively under local-analysis')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    capture = args.capture.resolve()
    if not capture.is_relative_to(root / 'local-analysis'):
        parser.error('private capture must be under ignored local-analysis')
    def read(name, limit):
        path = capture / name
        info = path.stat()
        if (path.is_symlink() or not stat.S_ISREG(info.st_mode) or info.st_size > limit or
                getattr(info, 'st_flags', 0) & 0x40000000):
            raise ValueError('only bounded resident regular capture files supported')
        return path.read_bytes()
    draw = json.loads(read('draw.json', 65536))
    if draw.get('version') not in (1, 2):
        raise ValueError('capture version1 or2 required')
    payloads = {name: read(name, limit) for name,limit in (
        ('vertex-shader.bin',4096), ('pixel-shader.bin',4096), ('device-state.bin',16384),
        ('vertices.bin',256*1024), ('indices.bin',256*1024),
        ('vertex-constants.bin',4096), ('pixel-constants.bin',4096))}
    sources = {name: (root/'runtime/host'/name).read_text()
               for name in ('native_translated_draw.h','native_color_viewport.h','gta5_draw_state.h',
                            'native_offscreen_draw_plan.h','native_affine_position.h')}
    for name, count in (('native_translated_draw.h',11), ('native_color_viewport.h',28)):
        if len(literal_register_requirements(sources[name])) != count:
            raise ValueError('native literal requirement grammar changed; review auditor before use')
    result = capture_requirements(draw, payloads, sources, args.profile)
    result['capture'] = str(capture)
    result['audit_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    result['analysis_source_sha256'] = {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
        for name in ('raw_shader_analysis.py','vertex_arithmetic_dataflow.py','pixel_sample_dataflow.py',
                     'inline_shader_translation.py','affine_position.py','scalar_constant.py','scalar_mul_const.py','unorm8_vertex_fetch.py',
                     'loading_shader_math.py')}
    if args.output:
        output = (root / args.output).absolute()
        if output != output.resolve() or not output.is_relative_to(root / 'local-analysis'):
            raise ValueError('report must use an explicit private path without symlinks')
        with output.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(result, stream, indent=2)
            stream.write('\n')
    print(json.dumps(compact_summary(result) if args.summary else result, indent=2))


if __name__ == '__main__':
    main()
