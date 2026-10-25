"""Compile project-owned HLSL; no native game shader inputs are required."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
def fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value



def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dxc', required=True, type=Path, help='DXC 1.8.2505.1 bin/x64')
    args = parser.parse_args()
    dxc = args.dxc.resolve() / 'dxc.exe'
    compiler = args.dxc.resolve() / 'dxcompiler.dll'
    out = ROOT / 'build'
    out.mkdir(exist_ok=True)
    require(hashlib.sha256(dxc.read_bytes()).hexdigest() ==
            'ce11bb02f6027055e0b9f8c062d13b14f373c876a16557a4fa744314117d31e7',
            'Use the x64 DXC 1.8.2505.1 executable')
    require(hashlib.sha256(compiler.read_bytes()).hexdigest() ==
            '5888d3e590f5bfd8743484e7f31313996734adec5741506271a5c6368cbe8ba9',
            'Use the x64 DXC 1.8.2505.1 compiler package')
    results = {}

    def header(path, shaders, extra=''):
        parts = ['// Generated locally; see tools/prepare.py for source and native-input provenance.\n#pragma once\n']
        for symbol, target, hashed in shaders:
            data = target.read_bytes()
            parts.append('inline constexpr unsigned char ' + symbol + '[]={' + ','.join(map(str, data)) + '};\n')
            if hashed:
                parts.append(f'inline constexpr unsigned long long {symbol}Hash=0x{fnv(data):016x}ull;\n')
            results[symbol] = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(''.join(parts) + extra, encoding='utf-8')

    def compile_hlsl(source, leaf, profile='cs_6_0', entry='main', flags=()):
        target = out / (leaf + '.cso')
        subprocess.run([str(dxc), '-T', profile, '-E', entry, '-O3', *flags,
                        '-Fo', str(target), str(ROOT / 'shaders' / source)], check=True)
        return target

    header(out / 'rr_sparse.generated.h', [
        ('RRSpatialPassthrough', compile_hlsl('rr_spatial_passthrough.hlsl', 'rr_spatial_passthrough'), True)])
    header(out / 'rr_vrrt_native.generated.h', [
        ('RRVrrtProbability', compile_hlsl('rr_vrrt_probability.hlsl', 'rr_vrrt_probability', flags=('-Ges',)), False),
        ('RRVrrtScale', compile_hlsl('rr_sparse_scale_vrrt.hlsl', 'rr_sparse_scale_vrrt', flags=('-Ges',)), False),
        ('RRVrrtScaleFloat', compile_hlsl('rr_sparse_scale_vrrt.hlsl', 'rr_sparse_scale_vrrt_float', flags=('-Ges', '-D', 'RR_PACKED_FLOAT=1')), False),
        # Lighting source variants (src/rr_lighting_source.h): lobe mask, fill, cap ratio,
        # 4th-half source, SSS guide mask.
        *[(f'RRVrrtScale{name}{suffix}',
           compile_hlsl('rr_sparse_scale_vrrt.hlsl', f'rr_sparse_scale_vrrt{leaf}_{name.lower()}',
                        flags=('-Ges', '-D', f'RR_NATIVE_LOBE={mask}', '-D', f'RR_NATIVE_FILL={fill}',
                               '-D', f'RR_CAP_RATIO={cap}', '-D', f'RR_W_SOURCE={w}',
                               '-D', f'RR_SSS_MASK={sss}', *extra)), False)
          for name, mask, fill, cap, w, sss in (('Native1', 1, 0, 0, 0, 0), ('Native2', 2, 0, 0, 0, 0),
                                                ('Fill2', 2, 1, 0, 0, 0), ('Fill3', 3, 1, 0, 0, 0),
                                                ('Cap8', 0, 0, 8, 0, 0), ('Cap4', 0, 0, 4, 0, 0),
                                                ('WFill', 0, 0, 0, 1, 0), ('WNative', 0, 0, 0, 2, 0),
                                                ('SssNative2', 2, 0, 0, 0, 1), ('SssFill2', 2, 1, 0, 0, 1),
                                                ('SssNative3', 3, 0, 0, 0, 1), ('SssFill3', 3, 1, 0, 0, 1))
          for suffix, leaf, extra in (('', '', ()), ('Float', '_float', ('-D', 'RR_PACKED_FLOAT=1')))]])

    header(out / 'sss-owned-shader/rr_sss_deinterleave.generated.h', [
        ('RRSSSDeinterleave', compile_hlsl('rr_sss_deinterleave.hlsl', 'rr_sss_deinterleave'), False)])
    header(out / 'sss-owned-shader/rr_sss_stencil.generated.h', [
        ('RRSSSStencilVS', compile_hlsl('rr_sss_stencil.hlsl', 'rr_sss_stencil_vs', 'vs_6_0', 'vs'), False),
        ('RRSSSStencilPS', compile_hlsl('rr_sss_stencil.hlsl', 'rr_sss_stencil_ps', 'ps_6_0', 'ps'), False)])
    # Running water test masks (src/rr_water.h).
    header(out / 'rr_water_masks.generated.h', [
        ('RRWaterMasks', compile_hlsl('rr_water_masks.hlsl', 'rr_water_masks'), False)])
    for source, entry, symbol, leaf in [
        ('rr_convert_guides.hlsl', 'main', 'RRGuideShader', 'rr_convert_guides'),
        ('rr_live_gate.hlsl', 'BeginFrame', 'RRLiveBegin', 'rr_live_begin'),
        ('rr_live_gate.hlsl', 'Validate', 'RRLiveValidate', 'rr_live_validate'),
        ('rr_live_gate.hlsl', 'Display', 'RRLiveDisplay', 'rr_live_display'),
        ('rr_sky_edge_mask.hlsl', 'main', 'RRSkyEdgeMask', 'rr_sky_edge_mask'),
        ('rr_sky_depth_floor.hlsl', 'main', 'RRSkyDepthFloor', 'rr_sky_depth_floor'),
        ('rr_color_scale.hlsl', 'main', 'RRColorScale', 'rr_color_scale')]:
        subprocess.run(['fxc', '/nologo', '/T', 'cs_5_0', '/E', entry,
                        '/Fh', str(out / (leaf + '.generated.h')), '/Vn', symbol,
                        str(ROOT / 'shaders' / source)], check=True)
    # Guide test variants (src/rr_guide_test.h).
    for test in range(1, 10):
        subprocess.run(['fxc', '/nologo', '/T', 'cs_5_0', '/E', 'main', '/D', f'RR_GUIDE_TEST={test}',
                        '/Fh', str(out / f'rr_convert_guides_test{test}.generated.h'),
                        '/Vn', f'RRGuideShaderTest{test}',
                        str(ROOT / 'shaders' / 'rr_convert_guides.hlsl')], check=True)
    (out / 'shader-build.json').write_text(json.dumps({'outputs': results}, indent=2) + '\n')
    print('Project-owned shaders compiled; runtime guide preparation uses the game-supplied shader.')


if __name__ == '__main__':
    main()

