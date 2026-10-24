"""Pinned native SSS DXIL transformation; original game shader is supplied separately."""
import difflib
import hashlib
import re

NATIVE_SHA256 = "3632196273fba431d92b8ae659cb9dac09444c43c6244205ba4b4fefab02e591"
NATIVE_FNV = 0x9ee7cea57f70d741


def require(condition, reason):
    if not condition:
        raise ValueError(reason)

def fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value

def verify_source(raw):
    require(hashlib.sha256(raw).hexdigest() == NATIVE_SHA256 and fnv(raw) == NATIVE_FNV,
            'Unsupported native SSS shader identity')

def make_mrt(text):
    text=text.replace('\r\n','\n')
    require('%sss.' not in text, 'Shader already patched')
    require(text.count('%608 = and i32 %607, 16384') == 1, 'Native enable-word contract changed')
    require(text.count('!52 = !{!53}') == 1, 'Native output signature changed')
    require(text.count('  ret void') == 1 and '@dx.op.discard' not in text, 'Native return/coverage contract changed')
    lines = []
    for number in range(611,624):
        found = re.findall(r'^  %'+str(number)+r' = .+$',text,re.M)
        require(len(found) == 1, 'Missing/ambiguous guide SSA '+str(number))
        lines.append(found[0])
    require(lines[-1] == '  %623 = fmul fast float %622, 2.500000e-01', 'Signed luminance scaling changed')
    # The exact CSO is verified before disassembly. Reuse its scalar operations
    # and operands; add no samples, loads, stores to native UAVs, or branch edits.
    duplicate = '\n'.join(re.sub(r'%(61[1-9]|62[0-3])\b',r'%sss.\1',line) for line in lines)
    first = '  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %3584)'
    require(text.count(first) == 1, 'Original target0 write changed')
    patched = text.replace(first, duplicate+'\n'+first)
    patched = patched.replace('  ret void',
        '  call void @dx.op.storeOutput.f32(i32 5, i32 1, i32 0, i8 0, float %sss.623)\n  ret void')
    next_id = max(map(int,re.findall(r'^!(\d+) = ',text,re.M)))+1
    patched = patched.replace('!52 = !{!53}',f'!52 = !{{!53, !{next_id}}}')
    patched += (f'\n!{next_id} = !{{i32 1, !"SV_Target", i8 9, i8 16, !{next_id+1}, i8 0, i32 1, i8 1, i32 1, i8 0, !{next_id+2}}}\n'
                f'!{next_id+1} = !{{i32 1}}\n!{next_id+2} = !{{i32 3, i32 1}}\n')
    return patched

def body(text):
    found = re.search(r'define void @ps_main\(\).*?\{\n(.*?)\n\}',text,re.S)
    require(found, 'PS body missing')
    result=[]
    for line in found[1].splitlines():
        line=line.split(';')[0].rstrip()
        # LLVM metadata IDs may be renumbered by assembler; they are compared
        # separately by the source diff. Arithmetic, flags and operands remain.
        line=re.sub(r', ![\w.]+ !\d+','',line)
        if line.strip():result.append(line)
    return result

def resource_table(text):
    match=re.search(r'; Resource Bindings:\n(.*?)\n; ViewId state:',text,re.S)
    require(match, 'Resource binding table missing')
    return match[1]

def validate_roundtrip(native, patched, roundtrip):
    require(resource_table(native)==resource_table(roundtrip), 'Assembled resource bindings changed')
    # The assembler can renumber SSA IDs. The scalar insertion is at the end,
    # so all original SSA instructions and target0 stores must retain identity.
    original=body(native);result=body(roundtrip)
    changes=[(tag,a,b,c,d) for tag,a,b,c,d in difflib.SequenceMatcher(None,original,result,autojunk=False).get_opcodes() if tag!='equal']
    require(all(tag=='insert' for tag,*_ in changes), 'Assembler changed original PS instructions')
    inserted=[line for _,_,_,c,d in changes for line in result[c:d]]
    require(len(inserted)==14, 'Expected13 scalar instructions plus1 new output store')
    stores=[line for line in inserted if '@dx.op.storeOutput.f32' in line]
    require(len(stores)==1 and 'i32 5, i32 1, i32 0, i8 0,' in stores[0], 'Added store is not scalar target1')
    def normalize_added(lines):
        names=[line.strip().split(' = ')[0] for line in lines if ' = ' in line]
        rename={name:f'%extra{i}' for i,name in enumerate(names)}
        return [re.sub(r'%[A-Za-z0-9._]+',lambda m:rename.get(m[0],m[0]),line) for line in lines]
    expected_added=[line for line in body(patched) if '%sss.' in line]
    require(normalize_added(inserted)==normalize_added(expected_added), 'Assembled scalar guide math changed')
    require(len(re.findall(r'call void @dx.op.storeOutput.f32',roundtrip))==5, 'Unexpected output stores')
    signature=re.search(r'; Output signature:\n(.*?)\n; shader hash:',roundtrip,re.S)
    require(signature, 'Assembled output signature absent')
    rows=[line for line in signature[1].splitlines() if line.startswith('; SV_Target')]
    require(len(rows)==2 and re.search(r'SV_Target\s+1\s+x\s+1\s+TARGET\s+float\s+x',rows[1]), 'Assembled target1 signature is not float scalar at index1')
    native_signature=re.search(r'; Output signature:\n(.*?)\n; shader hash:',native,re.S)
    native_rows=[line for line in native_signature[1].splitlines() if line.startswith('; SV_Target')]
    require(native_rows==rows[:1], 'Original target0 signature changed')
    source_native=body(native);source_patch=body(patched)
    require([line for line in source_patch if '%sss.' not in line]==source_native, 'Source original body changed')
    return {'originalInstructionsUnchanged':True,'originalTarget0SignatureUnchanged':True,'resourceBindingsIdentical':True,'addedMathMatchesNativeGuide':True,
            'addedScalarInstructions':13,'addedTarget1Stores':1,'outputSignature':rows}
