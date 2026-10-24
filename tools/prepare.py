"""Compile owned HLSL and transform explicitly supplied, pinned native shaders."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sss_patch

ROOT = Path(__file__).resolve().parent.parent
INPUTS = {
    'd63e707761558424.cso': (11680, '5dc4063eab5a84e6aa834f8912e623fc31d4d59fc50296363f8812007107270d'),
    '9ee7cea57f70d741.cso': (56388, sss_patch.NATIVE_SHA256),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dxc', required=True, type=Path, help='DXC 1.8.2505.1 bin/x64')
    parser.add_argument('--native', required=True, type=Path, help='Private native shader inputs')
    args = parser.parse_args()
    dxc = args.dxc.resolve() / 'dxc.exe'
    compiler = args.dxc.resolve() / 'dxcompiler.dll'
    native = args.native.resolve()
    out = ROOT / 'build'
    out.mkdir(exist_ok=True)
    require(hashlib.sha256(dxc.read_bytes()).hexdigest() ==
            'ce11bb02f6027055e0b9f8c062d13b14f373c876a16557a4fa744314117d31e7',
            'Use the x64 DXC 1.8.2505.1 executable')
    require(hashlib.sha256(compiler.read_bytes()).hexdigest() ==
            '5888d3e590f5bfd8743484e7f31313996734adec5741506271a5c6368cbe8ba9',
            'Use the x64 DXC 1.8.2505.1 compiler package')
    for name, (size, digest) in INPUTS.items():
        data = (native / name).read_bytes()
        require((size is None or len(data) == size) and hashlib.sha256(data).hexdigest() == digest,
                'Unsupported native shader: ' + name)

    def dump(path):
        return subprocess.check_output([str(dxc), '-dumpbin', str(path)]).decode().replace('\r\n', '\n')

    def assemble(text, leaf):
        source, target = out / (leaf + '.ll'), out / (leaf + '.cso')
        source.write_text(text, encoding='utf-8')
        subprocess.run([str(out / 'assemble.exe'), str(compiler), str(source), str(target)], check=True)
        return target

    results = {}

    def header(path, shaders, extra=''):
        parts = ['// Generated locally; see tools/prepare.py for source and native-input provenance.\n#pragma once\n']
        for symbol, target, hashed in shaders:
            data = target.read_bytes()
            parts.append('inline constexpr unsigned char ' + symbol + '[]={' + ','.join(map(str, data)) + '};\n')
            if hashed:
                parts.append(f'inline constexpr unsigned long long {symbol}Hash=0x{sss_patch.fnv(data):016x}ull;\n')
            results[symbol] = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(''.join(parts) + extra, encoding='utf-8')

    def compile_hlsl(source, leaf, profile='cs_6_0', entry='main', flags=()):
        target = out / (leaf + '.cso')
        subprocess.run([str(dxc), '-T', profile, '-E', entry, '-O3', *flags,
                        '-Fo', str(target), str(ROOT / 'shaders' / source)], check=True)
        return target

    spatial = dump(native / 'd63e707761558424.cso')
    for handle, register in [(1, 1), (2, 0)]:
        require(f'%{handle} = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 {register}, i32 {register}, i1 false)' in spatial
                and f'call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %{handle},' in spatial,
                'Native spatial output contract changed')
    header(out / 'rr_sparse.generated.h', [
        ('RRSpatialPassthrough', compile_hlsl('rr_spatial_passthrough.hlsl', 'rr_spatial_passthrough'), True)])
    header(out / 'rr_vrrt_native.generated.h', [
        ('RRVrrtProbability', compile_hlsl('rr_vrrt_probability.hlsl', 'rr_vrrt_probability', flags=('-Ges',)), False),
        ('RRVrrtScale', compile_hlsl('rr_sparse_scale_vrrt.hlsl', 'rr_sparse_scale_vrrt', flags=('-Ges',)), False),
        ('RRVrrtScaleFloat', compile_hlsl('rr_sparse_scale_vrrt.hlsl', 'rr_sparse_scale_vrrt_float', flags=('-Ges', '-D', 'RR_PACKED_FLOAT=1')), False)])

    original = dump(native / '9ee7cea57f70d741.cso')
    patched = sss_patch.make_mrt(original)
    sss = assemble(patched, 'rr_sss_mrt')
    sss_patch.validate_roundtrip(original, patched, dump(sss))
    header(out / 'sss-owned-shader/rr_sss.generated.h', [('RRSSSMrt', sss, True)],
           f'inline constexpr unsigned long long RRSSSNativeHash=0x{sss_patch.NATIVE_FNV:016x}ull;\n'
           'inline constexpr unsigned RRSSSMrtSampleBudgetScale=1;\n')
    header(out / 'sss-owned-shader/rr_sss_deinterleave.generated.h', [
        ('RRSSSDeinterleave', compile_hlsl('rr_sss_deinterleave.hlsl', 'rr_sss_deinterleave'), False)])
    header(out / 'sss-owned-shader/rr_sss_stencil.generated.h', [
        ('RRSSSStencilVS', compile_hlsl('rr_sss_stencil.hlsl', 'rr_sss_stencil_vs', 'vs_6_0', 'vs'), False),
        ('RRSSSStencilPS', compile_hlsl('rr_sss_stencil.hlsl', 'rr_sss_stencil_ps', 'ps_6_0', 'ps'), False)])
    for source, entry, symbol, leaf in [
        ('rr_convert_guides.hlsl', 'main', 'RRGuideShader', 'rr_convert_guides'),
        ('rr_live_gate.hlsl', 'BeginFrame', 'RRLiveBegin', 'rr_live_begin'),
        ('rr_live_gate.hlsl', 'Validate', 'RRLiveValidate', 'rr_live_validate'),
        ('rr_live_gate.hlsl', 'Display', 'RRLiveDisplay', 'rr_live_display')]:
        subprocess.run(['fxc', '/nologo', '/T', 'cs_5_0', '/E', entry,
                        '/Fh', str(out / (leaf + '.generated.h')), '/Vn', symbol,
                        str(ROOT / 'shaders' / source)], check=True)
    (out / 'shader-build.json').write_text(json.dumps({'nativeInputs': INPUTS, 'outputs': results}, indent=2) + '\n')
    print('Shader preparation passed; native SSS instructions and resource bindings verified.')


if __name__ == '__main__':
    main()

