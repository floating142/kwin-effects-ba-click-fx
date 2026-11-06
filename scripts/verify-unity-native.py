#!/usr/bin/env python3
"""Opt-in comparison with the local, hash-verified UnityPlayer native oracles.

Requires the kde-shittim-lockscreen-dev reference checkout, a matching local
UnityPlayer.dll and a C++ compiler/Qt6Core. No downloads and no CI dependency.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import hashlib

ROOT = Path(__file__).resolve().parents[1]


def capture_spans(args, spans):
    import pefile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP
    raw = args.unity_player.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '3f696bcf12b9bc6a4efe26193bdc7f209aee48d9a717e415c773cc5de155b5b3'
    functions = {}
    pe = pefile.PE(data=raw, fast_load=True)
    disassembler = Cs(CS_ARCH_X86, CS_MODE_64)
    disassembler.detail = True
    for name, begin, end in spans:
        code = pe.get_data(begin, end - begin)
        record = dict(rva=hex(begin), rawOffset=pe.get_offset_from_rva(begin), bytes=code.hex(),
                      sha256=hashlib.sha256(code).hexdigest(), calls=[], constants=[], branches=[])
        instructions = list(disassembler.disasm(code, begin))
        assert instructions[-1].mnemonic == 'ret' and instructions[-1].address + instructions[-1].size == end
        for instruction in instructions:
            if instruction.mnemonic == 'call' and instruction.operands[0].type == X86_OP_IMM:
                record['calls'].append(dict(offset=instruction.address - begin, size=instruction.size,
                                            targetRva=hex(instruction.operands[0].imm)))
            if instruction.mnemonic.startswith('j') and instruction.operands[0].type == X86_OP_IMM:
                target = instruction.operands[0].imm
                if not begin <= target < end:
                    assert instruction.imm_size == 4
                    record['branches'].append(dict(displacementOffset=instruction.address-begin+instruction.imm_offset,
                        relativeTo=instruction.address-begin+instruction.size,targetRva=hex(target)))
            for operand in instruction.operands:
                if operand.type == X86_OP_MEM and operand.mem.base == X86_REG_RIP:
                    target = instruction.address + instruction.size + operand.mem.disp
                    record['constants'].append(dict(displacementOffset=instruction.address - begin + instruction.disp_offset,
                        relativeTo=instruction.address - begin + instruction.size,
                        targetRva=hex(target), bytes=pe.get_data(target, operand.size).hex()))
        functions[name] = record
    return dict(sha256=digest, functions=functions)


def verify_rotation(args, temp, flags):
    data = capture_spans(args, [('rotation', 0xb05fd0, 0xb06246), ('sampleTwoCurves', 0xad5b60, 0xad5c24)])
    fixtures = args.reference_root / 'fixtures'
    curve = json.loads((fixtures / 'particle-curve-reproduction/curve-native.json').read_text())
    initial = json.loads((fixtures / 'particle-native-reproduction/particle-native.json').read_text())
    data['functions'].update({key: curve['functions'][key] for key in ('evaluateCurve', 'cacheSegment')})
    data['functions']['seedParticleLanes'] = initial['functions']['seedParticleLanes']
    proof = temp / 'rotation-proof.json'
    proof.write_text(json.dumps(data))
    snapshot = temp / 'rotation-snapshot.json'
    executable = temp / 'rotation'
    subprocess.run(['c++', '-std=c++20', '-O2', '-Wno-volatile', '-ffp-contract=off',
        '-I' + str(ROOT / 'src'), str(ROOT / 'tests/native/rotation_oracle.cpp'), '-o', str(executable),
        f'-DBA_ROTATION_PROOF="{proof}"', f'-DBA_TOUCH_SOURCE="{ROOT / "tests/fixtures/unity-touch.json"}"',
        f'-DSHITTIM_UNITY_PLAYER="{args.unity_player.resolve()}"', f'-DBA_NATIVE_SNAPSHOT="{snapshot}"', *flags], check=True)
    subprocess.run([str(executable)], check=True)
    if args.write_snapshot:
        (ROOT / 'tests/fixtures/unity-rotation-native.json').write_text(json.dumps(json.loads(snapshot.read_text()), indent=2) + '\n')


def verify_trail_points(args, temp, flags):
    data = capture_spans(args, [('points', 0xde6160, 0xde6620), ('prune', 0xde7330, 0xde7401)])
    proof = temp / 'trail-points-proof.json'
    proof.write_text(json.dumps(data))
    snapshot = temp / 'trail-points-snapshot.json'
    executable = temp / 'trail-points'
    subprocess.run(['c++', '-std=c++20', '-O2', '-Wno-volatile', '-ffp-contract=off',
        '-I' + str(ROOT / 'src'), str(ROOT / 'tests/native/trail_points_oracle.cpp'), '-o', str(executable),
        f'-DBA_TRAIL_PROOF="{proof}"', f'-DSHITTIM_UNITY_PLAYER="{args.unity_player.resolve()}"',
        f'-DBA_NATIVE_SNAPSHOT="{snapshot}"', *flags], check=True)
    subprocess.run([str(executable)], check=True)
    if args.write_snapshot:
        (ROOT / 'tests/fixtures/unity-trail-points-native.json').write_text(json.dumps(json.loads(snapshot.read_text()), indent=2) + '\n')


def verify_birth(args, temp):
    # Reuse only the external hash-pinned loader. Compare the complete native
    # dispatcher against production emitTriBurst, rather than isolated kernels.
    source = (args.reference_root / 'tests/particle-birth-native-test.cpp').read_text()
    loader = source[:source.index('int main(')]
    diagnostic = '''
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
struct NativeSpan {std::uintptr_t begin,end,rva;};
inline std::vector<NativeSpan> nativeSpans;
inline void nativeFault(int,siginfo_t *,void *raw) {
    auto *ctx=static_cast<ucontext_t *>(raw);
    const auto pc=std::uintptr_t(ctx->uc_mcontext.gregs[REG_RIP]);
    for(const auto &span:nativeSpans)if(pc>=span.begin && pc<span.end)
        dprintf(2,"Birth oracle fault at original RVA %lx\\n",static_cast<unsigned long>(span.rva+pc-span.begin));
    dprintf(2,"Fault registers RAX=%llx RCX=%llx R15=%llx\\n",(unsigned long long)ctx->uc_mcontext.gregs[REG_RAX],(unsigned long long)ctx->uc_mcontext.gregs[REG_RCX],(unsigned long long)ctx->uc_mcontext.gregs[REG_R15]);
    const auto ret=*reinterpret_cast<const std::uintptr_t *>(ctx->uc_mcontext.gregs[REG_RSP]);
    for(const auto &span:nativeSpans)if(ret>=span.begin && ret<=span.end)
        dprintf(2,"Unsupported birth oracle call from original RVA %lx\\n",static_cast<unsigned long>(span.rva+ret-span.begin-5));
    _exit(1);
}
'''
    loader = '#include <vector>\n#include <cstdint>\n#include <cstdio>\n' + diagnostic + loader
    loader = loader.replace('std::memcpy(memory+cursor,raw.constData(),raw.size());cursor=',
        'nativeSpans.push_back({reinterpret_cast<std::uintptr_t>(memory+cursor),reinterpret_cast<std::uintptr_t>(memory+cursor)+std::uintptr_t(raw.size()),std::uintptr_t(f["rva"].toString().toULongLong(nullptr,16))});std::memcpy(memory+cursor,raw.constData(),raw.size());cursor=')
    loader = loader.replace('memory[0]=0x0f;memory[1]=0x0b;',
        'struct sigaction action{};action.sa_sigaction=nativeFault;action.sa_flags=SA_SIGINFO;sigaction(SIGILL,&action,nullptr);sigaction(SIGSEGV,&action,nullptr);memory[0]=0x0f;memory[1]=0x0b;')
    proof_data = json.loads((args.reference_root / 'fixtures/particle-emission-reproduction/birth-native.json').read_text())
    additions = '\n'.join('            {"' + name + '","' + proof_data['functions'][name]['sha256'] + '"},'
                          for name in ('step', 'integrate', 'postStep', 'advanceShapePhase'))
    loader = loader.replace('const std::map<QString,QByteArray> hashes{',
                            'const std::map<QString,QByteArray> hashes{\n' + additions)
    loader = loader.replace('using Seed=', 'using Step=void(__attribute__((ms_abi)) *)(void *,void *,std::uint64_t,std::uint64_t *,float *,void *);\n    using Seed=')
    loader = loader.replace('Frame frame=nullptr;', 'Frame frame=nullptr;Step step=nullptr;')
    loader = loader.replace('seed=reinterpret_cast<Seed>', 'step=reinterpret_cast<Step>(entries["integrate"]);seed=reinterpret_cast<Seed>')
    (temp / 'birth_native_loader.h').write_text(loader)
    transform_source = (args.reference_root / 'tests/particle-transform-native-test.cpp').read_text()
    start = transform_source.index('template<typename T> void put(')
    end = transform_source.index('#endif', start)
    (temp / 'transform_native_loader.h').write_text(
        'namespace transform_reference {\n' + transform_source[start:end] + '\n}\n')
    executable = temp / 'birth'
    snapshot = temp / 'birth-snapshot.json'
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'Qt6Gui'], text=True))
    subprocess.run(['c++', '-std=c++20', '-O2', '-Wno-volatile', '-ffp-contract=off',
        '-I' + str(temp), '-I' + str(ROOT / 'src'),
        '-I' + str(args.reference_root.parent / 'kde-shittim-lockscreen/src'),
        str(ROOT / 'tests/native/birth_oracle.cpp'),
        *[str(ROOT / 'src' / source) for source in ('particles/clickinstance.cpp',
            'particles/subsystems.cpp', 'particles/curveutils.cpp', 'render/meshprofiles.cpp')],
        '-o', str(executable),
        f'-DSHITTIM_TRANSFORM_PROOF="{args.reference_root / "fixtures/particle-transform-reproduction/transform-native.json"}"',
        f'-DSHITTIM_BIRTH_PROOF="{args.reference_root / "fixtures/particle-emission-reproduction/birth-native.json"}"',
        f'-DSHITTIM_UNITY_PLAYER="{args.unity_player.resolve()}"',
        f'-DBA_NATIVE_SNAPSHOT="{snapshot}"', *flags], check=True)
    subprocess.run([str(executable)], check=True)
    if args.write_snapshot:
        (ROOT / 'tests/fixtures/unity-birth-native.json').write_text(json.dumps(json.loads(snapshot.read_text()), indent=2) + '\n')


def verify_distance(args, temp, flags):
    data = capture_spans(args, [('distance', 0xafb120, 0xafb2cc)])
    reference = json.loads((args.reference_root / 'fixtures/particle-emission-reproduction/emission-native.json').read_text())
    data['functions']['sampleCurve'] = reference['functions']['sampleCurve']
    proof = temp / 'distance-proof.json'
    proof.write_text(json.dumps(data))
    output = temp / 'distance'
    subprocess.run(['c++', '-std=c++20', '-O2', '-Wno-volatile', '-ffp-contract=off',
        '-I' + str(ROOT / 'src'), str(ROOT / 'tests/native/distance_oracle.cpp'), '-o', str(output),
        f'-DBA_DISTANCE_PROOF="{proof}"', f'-DSHITTIM_UNITY_PLAYER="{args.unity_player.resolve()}"', *flags], check=True)
    subprocess.run([str(output)], check=True)


def verify_trail_mesh(args, temp, flags):
    ranges = [('line',0x394c90,0x395412),('normals',0x399ed0,0x39a7bd),('tangent',0x39b440,0x39b6a7),('cap',0x392200,0x39260b),('corner',0x392cb0,0x393574),('round',0x392610,0x392cab),('vertex',0x394340,0x394489),('capNormal',0x391c70,0x3921f7),('cornerNormal',0x3916b0,0x391c62),('roundNormal',0x391180,0x3916a8),('gradient',0xf79a0,0xf7d4c),('gradientFixed',0xf7730,0xf7996)]
    data = capture_spans(args, ranges)
    reference = json.loads((args.reference_root / 'fixtures/particle-curve-reproduction/curve-native.json').read_text())
    data['functions'].update({k:reference['functions'][k] for k in ('evaluateCurve','cacheSegment')})
    proof = temp / 'trail-mesh-proof.json'
    proof.write_text(json.dumps(data))
    snapshot = temp / 'trail-mesh-snapshot.json'
    output = temp / 'trail-mesh'
    subprocess.run(['c++','-std=c++20','-O2','-Wno-volatile','-ffp-contract=off',
        '-I'+str(ROOT/'src'),str(ROOT/'tests/native/trail_mesh_oracle.cpp'),'-o',str(output),
        f'-DBA_TRAIL_MESH_PROOF="{proof}"',f'-DSHITTIM_UNITY_PLAYER="{args.unity_player.resolve()}"',
        f'-DBA_TOUCH_SOURCE="{ROOT / "tests/fixtures/unity-touch.json"}"',f'-DBA_NATIVE_SNAPSHOT="{snapshot}"',*flags],check=True)
    subprocess.run([str(output)],check=True)
    if args.write_snapshot:
        (ROOT/'tests/fixtures/unity-trail-mesh-native.json').write_text(json.dumps(json.loads(snapshot.read_text()),indent=2)+'\n')


def verify_billboard(args,temp,flags):
    source=json.loads((args.reference_root/'fixtures/particle-camera-reproduction/camera-native.json').read_text())
    data=dict(sha256=hashlib.sha256(args.unity_player.read_bytes()).hexdigest(),functions={})
    assert data['sha256']=='3f696bcf12b9bc6a4efe26193bdc7f209aee48d9a717e415c773cc5de155b5b3'
    for name in ('billboardLimitPacking','quadZClamp'):
        record=source['functions'][name]
        record['returnFromFragment']=True
        data['functions'][name]=record
    proof=temp/'billboard.json';proof.write_text(json.dumps(data))
    output=temp/'billboard'
    subprocess.run(['c++','-std=c++20','-O2','-Wno-volatile','-ffp-contract=off',
        '-I'+str(ROOT/'src'),str(ROOT/'tests/native/billboard_oracle.cpp'),'-o',str(output),
        f'-DBA_BILLBOARD_PROOF="{proof}"',f'-DSHITTIM_UNITY_PLAYER="{args.unity_player.resolve()}"',*flags],check=True)
    subprocess.run([str(output)],check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference-root', required=True, type=Path,
                        help='kde-shittim-lockscreen-dev directory')
    parser.add_argument('--unity-player', required=True, type=Path)
    parser.add_argument('--write-snapshot', action='store_true',
                        help='Refresh RGBA8 expectations from the original native update')
    parser.add_argument('--only', nargs='+', help='Run selected kernels only (e.g. motion retirement rotation)')
    args = parser.parse_args()
    raw = json.loads((ROOT / 'tests/fixtures/unity-touch.json').read_text())
    cases = {
        'initial': ('particle-native-reproduction/particle-native.json', 'PARTICLE', 'initial'),
        'gradient': ('particle-gradient-reproduction/gradient-native.json', 'GRADIENT', 'gradient'),
        'uv': ('particle-uv-reproduction/uv-native.json', 'UV', 'uv'),
        'circle': ('particle-circle-reproduction/circle-native.json', 'CIRCLE', 'circle'),
        'curve': ('particle-curve-reproduction/curve-native.json', 'CURVE', 'curve'),
        'emission': ('particle-emission-reproduction/emission-native.json', 'EMISSION', 'emission'),
        'speed': ('particle-speed-reproduction/speed-native.json', 'SPEED', 'initial'),
        'motion': ('particle-motion-reproduction/motion-native.json', 'MOTION', 'motion'),
        'retirement': ('particle-retirement-reproduction/retirement-native.json', 'RETIREMENT', 'retirement'),
    }
    unknown = set(args.only or []) - set(cases) - {'rotation', 'trail_points', 'birth', 'distance', 'trail_mesh', 'billboard'}
    if unknown:
        parser.error('Unknown kernels: ' + ', '.join(sorted(unknown)))
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'Qt6Core'], text=True))
    with tempfile.TemporaryDirectory(prefix='ba-fx-unity-native-') as temp:
        temp = Path(temp)
        for name, (proof_name, define, header) in cases.items():
            if args.only and name not in args.only:
                continue
            proof = json.loads((args.reference_root / 'fixtures' / proof_name).read_text())
            source = (args.reference_root / 'tests' / f'particle-{name}-native-test.cpp').read_text()
            lines = [line for line in source.splitlines() if not line.startswith('#include "')]
            source = f'#include "unity/{header}.h"\n' + '\n'.join(lines)
            source = source.replace('shittimParticle::', 'baclickfx::unity::')
            source = source.replace('ShittimUnityRandom', 'baclickfx::unity::Random')
            if name == 'gradient':
                proof['systems'] = [dict(path=key, enabled=True,
                    gradient=value['ColorModule']['gradient']['maxGradient'],
                    startColor=value['InitialModule']['startColor']['maxColor'])
                    for key, value in raw['particles'].items()]
                source = source.replace('systems!=244', 'systems!=4')
                source = source.replace('int systems=0,samples=0,updates=0,paddedCountOverlaps=0;',
                                        'QJsonArray snapshot; int systems=0,samples=0,updates=0,paddedCountOverlaps=0;')
                source = source.replace('for(size_t ch=0;ch<4;++ch)start[l][ch]=std::uint8_t((samples*37+int(ch)*91+lane*17)&255);',
                    'for(size_t ch=0;ch<4;++ch)start[l][ch]=baclickfx::unity::colorByte(float(row["startColor"].toObject()[QString("rgba").mid(int(ch),1)].toDouble()));')
                source = source.replace('++updates;', '''
                if(i<64 || i>=1024) {
                    QJsonArray bytes; for(auto byte:updated[l])bytes.append(int(byte));
                    snapshot.append(QJsonObject{{"system",row["path"]},{"age",double(time)}, {"rgba",bytes}});
                }
                ++updates;''')
                source = source.replace('if(systems!=4||products!=65536)return 1;', '''
                if(systems!=4||products!=65536)return 1;
                QFile output(BA_NATIVE_SNAPSHOT);if(!output.open(QIODevice::WriteOnly))return 1;
                output.write(QJsonDocument(QJsonObject{{"unityPlayerSha256",data["sha256"]},
                    {"samples",snapshot}}).toJson(QJsonDocument::Compact));''')
            elif name == 'uv':
                proof['systems'] = [dict(uv=value['UVModule']) for value in raw['particles'].values()
                                    if value['UVModule']['enabled']]
                source = source.replace('systems!=82', 'systems!=2')
            elif name == 'circle':
                proof['systems'] = [dict(path=key, shape=value['ShapeModule'])
                                    for key, value in raw['particles'].items() if value['ShapeModule']['enabled']]
                source = source.replace('#include <QGuiApplication>', '#include <QCoreApplication>')
                begin = source.index('class ShittimExitFxTest {')
                end = source.index('int main(', begin)
                source = source[:begin] + source[end:]
                source = source.replace('QGuiApplication app', 'QCoreApplication app')
                source = source.replace('if(repeat==0 && !source.isEmpty() && !ShittimExitFxTest::sourceBirths(source,output))return false;', '')
                source = source.replace('if(systems!=113)', 'if(systems!=2)')
                source = source.replace('Actual source QQuickItem: 2260 births at five supplied viewports, exact raw positions and direction-times-sampled-speed; controlled batches/seed, not PlayerLoop',
                                        'FX_Touch shape modules; controlled batches/seed, not PlayerLoop')
            elif name == 'curve':
                proof['systems'] = []
                for value in raw['particles'].values():
                    for module, field in (('SizeModule', 'curve'), ('RotationModule', 'curve'),
                                          ('CustomDataModule', 'vector0_0')):
                        if not value[module]['enabled']:
                            continue
                        curve = value[module][field]
                        for which in ('minCurve', 'maxCurve'):
                            if curve[which]['m_Curve']:
                                proof['systems'].append(dict(curve=curve[which], scalar=curve['scalar']))
                source = source.replace('t,value,inSlope', 't,v,inSlope').replace('.value', '.v')
                source = source.replace('systems!=602', f'systems!={len(proof["systems"])}')
            elif name == 'emission':
                proof['systems'] = [dict(emission=value['EmissionModule'],duration=value['lengthInSec'])
                                    for value in raw['particles'].values()]
                # Time-based amount has its own state updates; distance is checked separately.
                source = source.replace('systems!=254', 'systems!=4')
            elif name == 'speed':
                proof['systems'] = [dict(speed=value['InitialModule']['startSpeed'])
                                    for value in raw['particles'].values()]
                source = source.replace('systems!=254', 'systems!=4')
            elif name == 'motion':
                source = '#include "unity/initial.h"\n#include "unity/timing.h"\n' + source
                proof['systems'] = [dict(gravity=value['InitialModule']['gravityModifier'],
                                         lifetime=value['InitialModule']['startLifetime'])
                                    for value in raw['particles'].values()]
                source = source.replace('systems!=254', 'systems!=4')
                source = source.replace('if(prewarms!=15)', 'if(prewarms!=0)')
                source = source.replace('int localGravityValues=0;', 'QJsonArray snapshot; int localGravityValues=0;')
                source = source.replace('std::uint64_t end=std::uint64_t(count);n.step', '''
            const bool record = systems==0 && modifier==0 && mode==0 && count==4 && (tick==0 || tick==ticks-1);
            std::array<std::array<float,7>,4> previous{};
            if(record)for(int lane=0;lane<4;++lane)for(int field=0;field<7;++field)
                previous[size_t(lane)][size_t(field)]=field<6?output[size_t(field)][size_t(lane)]:output[0x3c0/0x20][size_t(lane)];
            std::uint64_t end=std::uint64_t(count);n.step''')
                source = source.replace('++samples;', '''
                if(record && i<4) {
                    QJsonArray before,after,module;
                    for(int field=0;field<7;++field) {
                        before.append(double(previous[size_t(i)][size_t(field)]));
                        after.append(double(field<6?output[size_t(field)][size_t(i)]:output[0x3c0/0x20][size_t(i)]));
                    }
                    for(float value:moduleVelocity)module.append(double(value));
                    snapshot.append(QJsonObject{{"before",before},{"after",after},{"dt",double(dt)},
                        {"inverseLifetime",double(inverse[size_t(i)])},{"moduleVelocity",module}});
                }
                ++samples;''')
                source = source.replace('int selections=0;', '''
                QFile destination(BA_NATIVE_SNAPSHOT);if(!destination.open(QIODevice::WriteOnly))return 1;
                destination.write(QJsonDocument(QJsonObject{{"unityPlayerSha256",data["sha256"]},
                    {"samples",snapshot}}).toJson(QJsonDocument::Compact));
                int selections=0;''')
            elif name == 'retirement':
                source = '#include "unity/motion.h"\n' + source
                source = source.replace('for(int count:{0,1,2,3,4,5,7,8,9,15,16,17,63,64,65,127,128,129})',
                    'for(float tick:{0.f,.0001f,1.f/60})for(int count:{0,1,2,3,4,5,7,8,9,15,16,17,63,64,65,127,128,129})')
                source = source.replace('output[0x3c0/0x20][size_t(i)]=ages[size_t(i)];output[0x3e0/0x20][size_t(i)]=1.f;',
                    'output[0x3c0/0x20][size_t(i)]=ages[size_t(i)];output[0x3e0/0x20][size_t(i)]=1.f; ages[size_t(i)]=baclickfx::unity::advancePercentAge(ages[size_t(i)],tick,1.f);')
                source = source.replace('alignas(16) float durations[4]{};', 'alignas(16) float durations[4]{tick,tick,tick,tick};')
            proof_path = temp / f'{name}.json'
            proof_path.write_text(json.dumps(proof))
            source_path = temp / f'{name}.cpp'
            source_path.write_text(source)
            output = temp / name
            snapshot = temp / 'gradient-snapshot.json'
            command = ['c++', '-std=c++20', '-O2', '-Wno-volatile', '-ffp-contract=off',
                       '-I' + str(ROOT / 'src'), str(source_path), '-o', str(output),
                       f'-DSHITTIM_{define}_PROOF="{proof_path}"',
                       f'-DSHITTIM_UNITY_PLAYER="{args.unity_player.resolve()}"',
                       f'-DBA_NATIVE_SNAPSHOT="{snapshot}"', *flags]
            subprocess.run(command, check=True)
            subprocess.run([str(output)], check=True)
            if name == 'gradient' and args.write_snapshot:
                (ROOT / 'tests/fixtures/unity-gradient-native.json').write_text(json.dumps(json.loads(snapshot.read_text()), indent=2) + '\n')
            if name == 'motion' and args.write_snapshot:
                (ROOT / 'tests/fixtures/unity-motion-native.json').write_text(json.dumps(json.loads(snapshot.read_text()), indent=2) + '\n')
        if not args.only or 'billboard' in args.only:
            verify_billboard(args,temp,flags)
        if not args.only or 'trail_mesh' in args.only:
            verify_trail_mesh(args, temp, flags)
        if not args.only or 'distance' in args.only:
            verify_distance(args, temp, flags)
        if not args.only or 'birth' in args.only:
            verify_birth(args, temp)
        if not args.only or 'trail_points' in args.only:
            verify_trail_points(args, temp, flags)
        if not args.only or 'rotation' in args.only:
            verify_rotation(args, temp, flags)


if __name__ == '__main__':
    main()
