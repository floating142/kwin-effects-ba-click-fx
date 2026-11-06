#!/usr/bin/env python3
"""Generate the FX_Touch parameter tables from a reviewable source snapshot.

Normal --check needs only Python. --extract reads the original resources.assets
with UnityPy; it never packages game binaries or generated machine code.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SNAPSHOT = ROOT / "tests/fixtures/unity-touch.json"
NAMES = {15: "Ring", 16: "MeshTri", 17: "Ring4", 18: "Ring3"}
PARTICLE_ASSETS = (
    ("Ring", "ring", "11", "FX_MAT_Touch_Cross2", "Circle_01"),
    ("Ring3", "ring3", "7", "FX_MAT_Touch_Tri 2", "Triangle_02_1"),
    ("Ring4", "ring4", "8", "FX_MAT_Touch_Tri 2", "Triangle_02_1"),
    ("MeshTri", "meshTri", "10", "FX_MAT_Touch_Tri3", "Grad_Ring3"),
)


def additive_shader_audit(shader):
    """Record source Forward DXBC semantics/hashes, without storing game bytecode."""
    import struct
    from UnityPy.helpers import CompressionHelper
    blob = bytes(shader.compressedBlob)
    start, size = shader.offsets[0][0], shader.compressedLengths[0][0]
    raw = CompressionHelper.decompress_lz4(blob[start:start+size], shader.decompressedLengths[0][0])
    programs = []
    cursor = raw.find(b'DXBC')
    while cursor >= 0:
        size, count = struct.unpack_from('<II', raw, cursor+24)
        code = raw[cursor:cursor+size]
        chunks = {}
        for i in range(count):
            offset = struct.unpack_from('<I', code, 32+i*4)[0]
            length = struct.unpack_from('<I', code, offset+4)[0]
            chunks[code[offset:offset+4]] = code[offset+8:offset+8+length]
        words = struct.unpack('<'+'I'*(len(chunks[b'SHDR'])//4), chunks[b'SHDR'])
        def signatures(name):
            data = chunks[name]
            result = {}
            for i in range(struct.unpack_from('<I', data)[0]):
                offset, index, system, component, register = struct.unpack_from('<IIIII', data, 8+i*24)
                semantic = data[offset:data.index(0, offset)].decode()
                result[(semantic, index)] = register
            return result
        instructions = []
        at = 2
        while at < len(words):
            token = words[at]
            length, opcode = token >> 24 & 127, token & 2047
            assert length
            if opcode < 88:
                instructions.append((opcode, words[at+1:at+length]))
            at += length
        programs.append(dict(sha256=hashlib.sha256(code).hexdigest(), version=words[0],
            inputs=signatures(b'ISGN'), outputs=signatures(b'OSGN'), instructions=instructions))
        cursor = raw.find(b'DXBC', cursor+size)
    vertex, pixel = programs[:2]
    assert vertex['version'] == 0x10040 and pixel['version'] == 0x40
    color_input, color_output = vertex['inputs'][('COLOR', 0)], vertex['outputs'][('COLOR', 0)]
    writes = []
    for opcode, operands in vertex['instructions']:
        if len(operands) >= 2 and (operands[0] >> 12 & 255) == 2 and operands[1] == color_output:
            writes.append((opcode, operands))
    assert len(writes) == 1
    opcode, operands = writes[0]
    assert opcode == 54 and len(operands) == 4
    assert operands[0] >> 4 & 15 == 15 and operands[2] >> 12 & 255 == 1
    assert operands[3] == color_input and operands[2] >> 4 & 255 == 0xe4
    assert set(op for op, _ in pixel['instructions']) <= {0,50,54,55,56,57,62,69}
    return dict(shader=shader.m_ParsedForm.m_Name, forwardVertexSha256=vertex['sha256'],
                forwardPixelSha256=pixel['sha256'], vertexColorPassThrough=True,
                forwardPixelOpcodes=sorted(set(op for op, _ in pixel['instructions'])))


def extract_player(path):
    """Resolve the actual PC Resources FX_Touch hierarchy by object references."""
    import UnityPy
    env = UnityPy.load(str(path))
    objects = {obj.path_id: obj for obj in env.objects}
    roots = [obj for obj in objects.values() if obj.type.name == 'GameObject'
             and obj.read_typetree()['m_Name'] == 'FX_Touch']
    assert len(roots) == 1
    result = {'sources': {path.name: hashlib.sha256(path.read_bytes()).hexdigest()},
              'particles': {}, 'transforms': {}, 'materials': {}, 'textures': {}, 'renderers': {}, 'meshes': {}}
    resource = path.with_suffix(path.suffix + '.resS')
    if resource.exists():
        result['sources'][resource.name] = hashlib.sha256(resource.read_bytes()).hexdigest()
    managers = path.parent / 'globalgamemanagers'
    manager_objects = list(UnityPy.load(str(managers)).objects)
    time_managers = [obj.read_typetree() for obj in manager_objects
                     if obj.type.name == 'TimeManager']
    assert len(time_managers) == 1
    result['timing'] = time_managers[0]
    tag_managers = [obj.read_typetree() for obj in manager_objects if obj.type.name == 'TagManager']
    assert len(tag_managers) == 1
    layer_names = tag_managers[0]['layers']
    result['gameObjectLayers'] = {}
    players = [obj.read_typetree() for obj in manager_objects if obj.type.name == 'PlayerSettings']
    assert len(players) == 1
    result['playerColor'] = {key: players[0][key] for key in
                             ('m_ActiveColorSpace', 'useHDRDisplay', 'm_ColorGamuts')}
    result['sources'][managers.name] = hashlib.sha256(managers.read_bytes()).hexdigest()
    names = {'ring': ('Ring', '11'), 'MeshTri': ('MeshTri', '10'),
             'Ring (3)': ('Ring3', '7'), 'Ring (4)': ('Ring4', '8'),
             'FX_Touch': ('Root', '12'), 'Trail': ('Trail', '9')}
    mesh_source = None

    def walk(game_object):
        nonlocal mesh_source
        game = game_object.read_typetree()
        assert game['m_Name'] in names
        name, transform_id = names[game['m_Name']]
        layer = game['m_Layer']
        result['gameObjectLayers'][name] = {'id': layer, 'name': layer_names[layer]}
        components = {objects[c['component']['m_PathID']].type.name: objects[c['component']['m_PathID']]
                      for c in game['m_Component']}
        transform = components['Transform'].read_typetree()
        result['transforms'][transform_id] = {key: transform[key] for key in
            ('m_LocalScale', 'm_LocalRotation', 'm_LocalPosition')}
        if name == 'Root':
            raw = components['ParticleSystem'].read_typetree()
            result['rootSystem'] = {key:raw[key] for key in
                ('lengthInSec','simulationSpeed','looping','prewarm','useUnscaledTime','playOnAwake')}
            result['rootSystem']['emissionEnabled'] = raw['EmissionModule']['enabled']
            script = components['MonoBehaviour'].get_raw_data()
            assert len(script) == 44
            file_id, trail_id = struct.unpack_from('<iq', script, 32)
            assert file_id == 0
            result['fxTouchTrailPathId'] = trail_id
        if name in NAMES.values():
            raw = components['ParticleSystem'].read_typetree()
            selected = {key: raw[key] for key in ('scalingMode', 'moveWithTransform', 'autoRandomSeed',
                'randomSeed', 'InitialModule', 'SizeModule', 'ColorModule', 'ShapeModule',
                'UVModule', 'RotationModule', 'CustomDataModule', 'EmissionModule',
                'prewarm', 'looping', 'lengthInSec', 'simulationSpeed', 'ringBufferMode', 'useUnscaledTime',
                'startDelay', 'stopAction', 'cullingMode', 'emitterVelocityMode', 'playOnAwake')}
            selected['enabledModules'] = [key for key, value in raw.items()
                                          if isinstance(value, dict) and value.get('enabled')]
            result['particles'][name] = selected
            renderer = components['ParticleSystemRenderer'].read_typetree()
            if name == 'MeshTri':
                assert renderer['m_RenderMode'] == 4
                assert all(renderer[key]['m_PathID'] == 0 for key in ('m_Mesh1', 'm_Mesh2', 'm_Mesh3'))
                mesh_ref = renderer['m_Mesh']
                assert mesh_ref['m_FileID'] == 0
                mesh_source = objects[mesh_ref['m_PathID']].read()
                assert mesh_source.m_Name == 'Cylinder002' and len(mesh_source.m_SubMeshes) == 1
            else:
                assert renderer['m_RenderMode'] == 0
            result['renderers'][name] = {key: renderer[key] for key in
                ('m_ApplyActiveColorSpace', 'm_RenderMode', 'm_MinParticleSize', 'm_MaxParticleSize',
                 'm_RenderAlignment', 'm_SortingOrder', 'm_SortingLayerID', 'm_SortingLayer',
                 'm_SortingFudge', 'm_SortMode', 'm_RendererPriority', 'm_Materials')}
        if name == 'Trail':
            raw = components['TrailRenderer'].read_typetree()
            result['trail'] = {key: raw[key] for key in ('m_Time', 'm_MinVertexDistance', 'm_Parameters', 'm_Autodestruct', 'm_Emitting')}
            script = components['MonoBehaviour'].get_raw_data()
            assert len(script) == 36
            result['trail']['killUnderTimeScale'] = struct.unpack_from('<f', script, 32)[0]
            assert components['TrailRenderer'].path_id == result['fxTouchTrailPathId']
            result['trail']['renderer'] = {key:raw[key] for key in
                ('m_Materials','m_SortingLayerID','m_SortingLayer','m_SortingOrder','m_RendererPriority')}
        for child in transform['m_Children']:
            child_transform = objects[child['m_PathID']].read_typetree()
            walk(objects[child_transform['m_GameObject']['m_PathID']])

    walk(roots[0])
    material_bindings = [(result['renderers'][name], material, texture)
                         for name, _, _, material, texture in PARTICLE_ASSETS]
    material_bindings.append((result['trail']['renderer'], 'FX_MAT_TouchFXTrail', 'Trail_03'))
    for renderer, material_name, texture_name in material_bindings:
        # Slot zero is the main renderer material. Particle trail material is in slot one.
        material_ref = renderer['m_Materials'][0]
        assert material_ref['m_FileID'] == 0
        material = objects[material_ref['m_PathID']].read_typetree()
        assert material['m_Name'] == material_name
        textures = [props['m_Texture'] for _, props in material['m_SavedProperties']['m_TexEnvs']
                    if props['m_Texture']['m_PathID']]
        assert len(textures) == 1 and textures[0]['m_FileID'] == 0
        texture = objects[textures[0]['m_PathID']]
        assert texture.type.name == 'Texture2D' and texture.peek_name() == 'FX_TEX_' + texture_name

    from UnityPy.helpers.MeshHelper import MeshHandler
    assert mesh_source is not None
    handler = MeshHandler(mesh_source)
    handler.process()
    result['meshes'][mesh_source.m_Name] = {'vertices': handler.m_Vertices,
        'uv': handler.m_UV0, 'triangles': [list(face) for group in handler.get_triangles() for face in group]}

    for obj in objects.values():
        if obj.type.name == 'Material':
            raw = obj.read_typetree()
            if raw['m_Name'].startswith('FX_MAT_Touch'):
                shader = objects[raw['m_Shader']['m_PathID']].read_typetree()['m_ParsedForm']
                forward_blends = [
                    {key: value['val'] for key, value in p['m_State']['rtBlend0'].items()}
                    for sub in shader['m_SubShaders'] for p in sub['m_Passes']
                    if p['m_State']['m_Name'] in ('Forward', 'Universal Forward')
                ]
                assert forward_blends and all(b == forward_blends[0] for b in forward_blends)
                result['materials'][raw['m_Name']] = {'queue': raw['m_CustomRenderQueue'],
                    'colors': dict(raw['m_SavedProperties']['m_Colors']),
                    'floats': dict(raw['m_SavedProperties']['m_Floats']),
                    'textures': dict(raw['m_SavedProperties']['m_TexEnvs']),
                    'forwardBlend': forward_blends[0]}
        elif obj.type.name == 'Texture2D':
            texture = obj.read()
            if (ROOT / 'assets' / (texture.m_Name + '.png')).exists():
                rgba = texture.image.convert('RGBA')
                result['textures'][texture.m_Name + '.png'] = {
                    'width': texture.m_Width, 'height': texture.m_Height,
                    'format': texture.m_TextureFormat, 'colorSpace': texture.m_ColorSpace,
                    'mipCount': texture.m_MipCount,
                    'sampling': obj.read_typetree()['m_TextureSettings'],
                    'rgbaSha256': hashlib.sha256(rgba.tobytes()).hexdigest()}
    trail_material = objects[result['trail']['renderer']['m_Materials'][0]['m_PathID']].read_typetree()
    result['additiveShader'] = additive_shader_audit(objects[trail_material['m_Shader']['m_PathID']].read())
    # The actual TouchEffectCreater camera, rather than a camera from the rebuilt scene.
    scripts = {obj.path_id:obj.read_typetree()['m_ClassName']
               for obj in UnityPy.load(str(path.parent/'globalgamemanagers.assets')).objects
               if obj.type.name == 'MonoScript'}
    level = path.parent/'level1'
    scene = {obj.path_id:obj for obj in UnityPy.load(str(level)).objects}
    creators = [obj for obj in scene.values() if obj.type.name == 'MonoBehaviour'
                and scripts.get(obj.read_typetree(check_read=False)['m_Script']['m_PathID']) == 'TouchEffectCreater']
    assert len(creators) == 1
    file_id, camera_id = struct.unpack_from('<iq', creators[0].get_raw_data(), 32)
    assert file_id == 0 and scene[camera_id].type.name == 'Camera'
    camera = scene[camera_id].read_typetree()
    result['camera'] = {key:camera[key] for key in ('orthographic','orthographic size','m_NormalizedViewPortRect')}
    result['camera']['pathId'] = camera_id
    result['camera']['cullingMask'] = camera['m_CullingMask']['m_Bits']
    result['sources'][level.name] = hashlib.sha256(level.read_bytes()).hexdigest()
    managers_assets = path.parent/'globalgamemanagers.assets'
    shared = {obj.path_id: obj for obj in UnityPy.load(str(managers_assets)).objects}
    bloom_assets = [obj for obj in shared.values() if obj.type.name == 'MonoBehaviour'
                    and b'UIRenderBloomSettings' in obj.get_raw_data()]
    assert len(bloom_assets) == 1
    raw = bloom_assets[0].get_raw_data()
    at = (raw.index(b'UIRenderBloomSettings') + len(b'UIRenderBloomSettings') + 3) & ~3
    shader_file, shader_id = struct.unpack_from('<iq', raw, at)
    assert shader_file == 0
    assert shared[shader_id].read().m_ParsedForm.m_Name == 'Hidden/MXFinalBloom'
    enable, intensity, threshold, knee, clamp, diffusion, anamorphic, *color = struct.unpack_from('<Iffffif4f', raw, at+12)
    result['bloom'] = dict(enabled=bool(enable), intensity=intensity, threshold=threshold,
                           softKnee=knee, clamp=clamp, diffusion=diffusion,
                           anamorphicRatio=anamorphic, color=color, shaderPathId=shader_id)
    result['sources'][managers_assets.name] = hashlib.sha256(managers_assets.read_bytes()).hexdigest()
    return result


def number(value):
    return repr(float(value))


def generate(data):
    curves = ['// SPDX-License-Identifier: GPL-3.0-or-later',
              '// Generated by scripts/import-unity-touch.py; do not edit.',
              '#pragma once', '#include "curveutils.h"',
              'namespace baclickfx::curves {']
    params = ['// SPDX-License-Identifier: GPL-3.0-or-later',
              '// Generated by scripts/import-unity-touch.py; do not edit.',
              '#pragma once', '#include "subsystems.h"',
              'namespace baclickfx {', 'inline void configureTouchSubsystems(SubsystemMap &m) {',
              '    using namespace curves;']

    def scalar(name, raw):
        curves.append(f'inline constexpr ScalarStop k{name}[] = {{')
        for key in raw["m_Curve"]:
            assert key["weightedMode"] == 0, "Weighted curves need a separate native implementation"
            curves.append('    {' + ', '.join(number(key[k]) for k in
                          ("time", "value", "inSlope", "outSlope")) + '},')
        curves.append('};')

    def gradient(name, raw):
        assert raw["m_Mode"] == 0
        curves.append(f'inline constexpr ColorStop k{name}Color[] = {{')
        for i in range(raw["m_NumColorKeys"]):
            rgb = ', '.join(number(raw[f'key{i}'][c]) for c in "rgb")
            curves.append(f'    {{{raw[f"ctime{i}"]}.0 / 65535.0, {{{rgb}}}}},')
        curves.append('};')
        curves.append(f'inline constexpr ScalarStop k{name}Alpha[] = {{')
        for i in range(raw["m_NumAlphaKeys"]):
            curves.append(f'    {{{raw[f"atime{i}"]}.0 / 65535.0, {number(raw[f"key{i}"]["a"])}}},')
        curves.append('};')

    def assign(field, value):
        params.append(f'        s.{field} = {value};')

    assert data['camera']['orthographic'] and data['camera']['orthographic size'] == 1
    assert data['playerColor'] == dict(m_ActiveColorSpace=1, useHDRDisplay=False, m_ColorGamuts=[0])
    root = data['rootSystem']
    assert not root['emissionEnabled'] and not root['looping'] and not root['prewarm'] and not root['playOnAwake']
    assert root['simulationSpeed'] == root['lengthInSec'] == 1 and root['useUnscaledTime']
    materials = data["materials"]
    assert data['additiveShader']['vertexColorPassThrough']
    # These states affect visible seams and softness even when decoded pixels match.
    for name, texture in data['textures'].items():
        sampling = texture['sampling']
        wrap = 0 if name in ('FX_TEX_Circle_01.png', 'FX_TEX_Trail_03.png') else 1
        assert texture['mipCount'] == 1 and texture['colorSpace'] == 1
        assert sampling['m_FilterMode'] == 1 and sampling['m_Aniso'] == 1
        assert sampling['m_MipBias'] == 0 and sampling['m_WrapU'] == sampling['m_WrapV'] == wrap
    for name, material in materials.items():
        # Unity BlendOp.Add = 0, One = 1, SrcAlpha = 5, OneMinusSrcAlpha = 10.
        src = 5 if name == 'FX_MAT_Touch_Tri3' else 1
        dst = 10 if name == 'FX_MAT_Touch_Cross2' else 1
        assert material['forwardBlend'] == dict(srcBlend=src, destBlend=dst, srcBlendAlpha=1,
            destBlendAlpha=dst, blendOp=0, blendOpAlpha=0, colMask=15)
        for texture in material['textures'].values():
            assert texture['m_Scale'] == dict(x=1, y=1) and texture['m_Offset'] == dict(x=0, y=0)
        assert material['floats'].get('_Custom_Data_Offset_Use', 0) == 0
    bloom = data['bloom']
    assert bloom['enabled'] and bloom['intensity'] == 1.7000000476837158
    assert bloom['threshold'] == 1 and bloom['softKnee'] == 0 and bloom['clamp'] == 65472
    assert bloom['diffusion'] == 7 and bloom['anamorphicRatio'] == 0 and bloom['color'] == [1]*4
    # UIRenderPass draws UIAlwaysVisible before Bloom, then draws LetterBox separately.
    assert data['gameObjectLayers'] == {
        **{name: {'id': 13, 'name': 'UIAlwaysVisible'} for name in ('Root', 'Ring', 'MeshTri', 'Trail')},
        **{name: {'id': 8, 'name': 'LetterBox'} for name in ('Ring3', 'Ring4')},
    }
    assert all(data['camera']['cullingMask'] & (1 << layer) for layer in (8, 13))
    for renderer in [*data['renderers'].values(), data['trail']['renderer']]:
        assert renderer['m_SortingLayerID'] == renderer['m_SortingLayer'] == renderer['m_SortingOrder'] == 0
        assert renderer['m_RendererPriority'] == 0
        assert renderer.get('m_SortMode', 0) == 0 and renderer.get('m_SortingFudge', 0) == 0
    for name, member, transform, material, texture in PARTICLE_ASSETS:
        raw = data["particles"][name]
        enabled = set(raw.get('enabledModules', [k for k, v in raw.items()
                                                 if isinstance(v, dict) and v.get('enabled')]))
        assert enabled <= {"InitialModule", "SizeModule", "ColorModule", "ShapeModule",
                           "UVModule", "RotationModule", "CustomDataModule", "EmissionModule"}
        assert raw["scalingMode"] == 1 and raw["moveWithTransform"] == 1
        assert raw['startDelay']['minMaxState'] == 0 and raw['startDelay']['scalar'] == 0
        assert raw['stopAction'] == 0 and raw['cullingMode'] == 3
        assert raw['emitterVelocityMode'] == 1 and not raw['playOnAwake']
        assert raw['autoRandomSeed'] and raw['randomSeed'] == 0
        assert not raw['prewarm'] and raw['ringBufferMode'] == 0 and raw['useUnscaledTime']
        assert raw['simulationSpeed'] == raw['lengthInSec'] == 1
        assert raw['looping'] == (name == 'Ring4')
        initial = raw["InitialModule"]
        assert initial['gravityModifier']['minMaxState'] == 0 and initial['gravityModifier']['scalar'] == 0
        assert not initial["size3D"] and not initial["rotation3D"]
        assert initial['randomizeRotationDirection'] == 0
        assert initial["startColor"]["minMaxState"] == 0
        scalar(name + "Size", raw["SizeModule"]["curve"]["maxCurve"])
        gradient(name, raw["ColorModule"]["gradient"]["maxGradient"])
        params.extend(['    {', f'        auto &s = m.{member};'])
        assign("textureAsset", f'"FX_TEX_{texture}.png"')
        color = next(iter(materials[material]["colors"].values()))
        assert color['r'] == color['g'] == color['b'] and color['a'] == 1
        assign("hdrColor", 'Rgb{' + ', '.join(number(color[c]) for c in "rgb") + '}')
        assign("renderQueue", str(materials[material]["queue"]))
        assign("minParticleSize", number(data['renderers'][name]['m_MinParticleSize']) + 'f')
        assign("maxParticleSize", number(data['renderers'][name]['m_MaxParticleSize']) + 'f')
        start = initial["startColor"]["maxColor"]
        assert start['a'] == 1
        assign("startColor", 'Rgb{' + ', '.join(number(start[c]) for c in "rgb") + '}')
        for field, source in (("startLifetime", "startLifetime"), ("startSize", "startSize"),
                              ("startSpeed", "startSpeed"), ("startRotation", "startRotation")):
            value = initial[source]
            assert value["minMaxState"] in (0, 3)
            for suffix, key in (("Mode", "minMaxState"), ("Scalar", "scalar"), ("MinScalar", "minScalar")):
                assign(field + suffix, str(value[key]))
        for field, source in (("lifetimeSec", "startLifetime"), ("size", "startSize"), ("speed", "startSpeed")):
            assign(field, number(initial[source]["scalar"]))
        scale = data["transforms"][transform]["m_LocalScale"]
        assert scale["x"] == scale["y"] == scale["z"]
        assert data['transforms'][transform]['m_LocalRotation'] == dict(x=0, y=0, z=0, w=1)
        assert data['transforms'][transform]['m_LocalPosition'] == dict(x=0, y=0, z=0)
        assign("localScale", number(scale["x"]))
        assign("maxParticles", str(initial["maxNumParticles"]))
        emission = raw["EmissionModule"]
        assert emission['rateOverTime']['scalar'] == 0
        assert emission['rateOverDistance']['minMaxState'] == 0
        for burst in emission['m_Bursts']:
            assert burst['time'] == 0 and burst['cycleCount'] == 1 and burst['probability'] == 1
            assert burst['countCurve']['minMaxState'] == 0
        assign("rateOverDistance", number(emission["rateOverDistance"]["scalar"]))
        assign("clickBurst", str(sum(int(b["countCurve"]["scalar"]) for b in emission["m_Bursts"])))
        assign("sizeCurve", f'ScalarCurve(k{name}Size)')
        assign("sizeCurveMultiplier", number(raw["SizeModule"]["curve"]["scalar"]))
        assign("colorStops", f'ColorCurve(k{name}Color)')
        assign("alphaStops", f'ScalarCurve(k{name}Alpha)')
        if raw["ShapeModule"]["enabled"]:
            shape = raw["ShapeModule"]
            assert shape["type"] == 10 and shape["radiusThickness"] == 0
            assert shape['randomPositionAmount'] == 0 and not shape['alignToDirection']
            assert shape['arc']['spread'] == 0
            assert shape["randomDirectionAmount"] == shape["sphericalDirectionAmount"] == 1
            assert shape['radius']['value'] == 1 and shape['radius']['mode'] == 0
            assert shape['arc']['value'] == 360 and shape['arc']['mode'] == 0
            assert shape['m_Scale']['x'] == shape['m_Scale']['y'] and shape['m_Scale']['z'] == 1
            assert shape['m_Position'] == shape['m_Rotation'] == dict(x=0, y=0, z=0)
            assign("shapeEnabled", "true")
            assign("shapeScale", number(shape["m_Scale"]["x"]))
        if raw["UVModule"]["enabled"]:
            uv = raw["UVModule"]
            assert uv["mode"] == uv["timeMode"] == uv["animationType"] == 0
            assert uv['cycles'] == 1 and uv['startFrame']['minMaxState'] in (0, 3)
            assert uv['frameOverTime']['minMaxState'] in (0, 3)
            assign("uvEnabled", "true")
            for field, key in (("uvTilesX", "tilesX"), ("uvTilesY", "tilesY"), ("uvCycles", "cycles")):
                assign(field, str(uv[key]))
            for prefix, key in (("uvStartFrame", "startFrame"), ("uvFrameOverTime", "frameOverTime")):
                for suffix, source in (("Mode", "minMaxState"), ("Scalar", "scalar"), ("MinScalar", "minScalar")):
                    assign(prefix + suffix, str(uv[key][source]))
        if raw["RotationModule"]["enabled"]:
            rotation = raw["RotationModule"]["curve"]
            assert rotation["minMaxState"] == 2
            scalar(name + "RotationMin", rotation["minCurve"])
            scalar(name + "RotationMax", rotation["maxCurve"])
            assign("rotationEnabled", "true")
            assign("rotationScalar", number(rotation["scalar"]))
            assign("rotationCurveMin", f'ScalarCurve(k{name}RotationMin)')
            assign("rotationCurveMax", f'ScalarCurve(k{name}RotationMax)')
        if raw["CustomDataModule"]["enabled"]:
            assert raw['CustomDataModule']['vector0_0']['scalar'] == 1
            scalar(name + "CustomData0", raw["CustomDataModule"]["vector0_0"]["maxCurve"])
            assign("customData0Curve", f'ScalarCurve(k{name}CustomData0)')
        params.append('    }')
    trail = data["trail"]
    assert not trail["m_Autodestruct"] and trail["m_Emitting"]
    line = trail["m_Parameters"]
    assert line['alignment'] == 1 and not line['generateLightingData'] and line['textureMode'] == 0
    assert len(line['widthCurve']['m_Curve']) == 1
    assert line['widthCurve']['m_Curve'][0]['value'] == 1
    gradient("Trail", trail["m_Parameters"]["colorGradient"])
    scalar("TrailWidth", trail["m_Parameters"]["widthCurve"])
    params.extend(['    {', '        auto &s = m.trail;'])
    for field, value in (("lifetimeSec", trail["m_Time"]), ("minVertexDistance", trail["m_MinVertexDistance"]),
                         *[(k, trail["m_Parameters"][k]) for k in
                           ("widthMultiplier", "numCornerVertices", "numCapVertices", "textureMode")]):
        assign(field, str(value))
    color = next(iter(materials["FX_MAT_TouchFXTrail"]["colors"].values()))
    assign("hdrColor", 'Rgb{' + ', '.join(number(color[c]) for c in "rgb") + '}')
    assign("renderQueue", str(materials["FX_MAT_TouchFXTrail"]["queue"]))
    assign("textureAsset", '"FX_TEX_Trail_03.png"')
    assign("killUnderTimeScale", number(trail["killUnderTimeScale"]))
    assign("textureScale", '1.0')
    assign("widthCurve", 'ScalarCurve(kTrailWidth)')
    assign("colorKeys", 'ColorCurve(kTrailColor)')
    assign("alphaKeys", 'ScalarCurve(kTrailAlpha)')
    params.extend(['    }', '}', '}'])
    curves.append('}')
    timing = ['// SPDX-License-Identifier: GPL-3.0-or-later',
              '// Generated from the original TimeManager by scripts/import-unity-touch.py.',
              '#pragma once', 'namespace baclickfx::unity {',
              f'inline constexpr float sourceFixedTimestep = {number(data["timing"]["Fixed Timestep"])}f;',
              f'inline constexpr float sourceMaximumAllowedTimestep = {number(data["timing"]["Maximum Allowed Timestep"])}f;',
              f'inline constexpr float sourceMaximumParticleTimestep = {number(data["timing"]["Maximum Particle Timestep"])}f;',
              '}']
    return {ROOT / 'src/particles/curvedata.h': '\n'.join(curves) + '\n',
            ROOT / 'src/particles/touchparams.h': '\n'.join(params) + '\n',
            ROOT / 'src/unity/timing.h': '\n'.join(timing) + '\n'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extract', type=Path, help='Original PC resources.assets (requires UnityPy)')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if args.extract:
        assert not args.check
        data = extract_player(args.extract)
        SNAPSHOT.parent.mkdir(parents=True, exist_ok=True)
        SNAPSHOT.write_text(json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True) + '\n')
    data = json.loads(SNAPSHOT.read_text())
    for path, content in generate(data).items():
        if args.check:
            if not path.exists() or path.read_text() != content:
                raise SystemExit(f'Stale Unity table: {path.relative_to(ROOT)}')
        else:
            path.write_text(content)
    print('Unity parameter tables match the source snapshot.')


if __name__ == '__main__':
    main()
