"""
Creates the editor-side assets the Lambda Engine runtime expects:
  * /LambdaSource/Materials/M_LambdaBase   - master material with a "BaseTexture" parameter (plugin content)
  * /LambdaSource/Materials/M_LambdaDecal  - deferred decal master for bullet impacts
  * /LambdaSource/Materials/M_LambdaSprite - unlit additive master for effect sprites
  * /LambdaSource/Materials/M_LambdaSpriteNoZ - the same, depth test off, for Source's "$ignorez" sprites
  * /Game/LambdaEngine/Maps/LambdaEntry    - empty startup level

Run via Tools/CreateAssets.bat (UnrealEditor-Cmd -run=pythonscript) or from the editor's Python console.
"""
import unreal

MATERIAL_PATH = '/LambdaSource/Materials'
MATERIAL_NAME = 'M_LambdaBase'
DECAL_NAME = 'M_LambdaDecal'
SPRITE_NAME = 'M_LambdaSprite'
SPRITE_NOZ_NAME = 'M_LambdaSpriteNoZ'
LEVEL_PATH = '/Game/LambdaEngine/Maps/LambdaEntry'


def log(msg):
    unreal.log('[LambdaEngine] ' + msg)


def connect(from_expr, from_output, to_expr, to_input):
    """connect_material_expressions returns False - and does nothing - when a pin name does not match, and a
    material with a dangling input fails to compile at load time and silently renders as the engine default.
    Refusing to continue here turns that into an error at generation time, where it is visible."""
    mel = unreal.MaterialEditingLibrary
    if not mel.connect_material_expressions(from_expr, from_output, to_expr, to_input):
        raise RuntimeError(f'could not connect {from_expr.get_name()}.{from_output!r} -> {to_expr.get_name()}.{to_input!r}')


def connect_any(from_expr, from_output, to_expr, to_inputs):
    """Connects to the first of several candidate pin names that exists; raises if none does."""
    mel = unreal.MaterialEditingLibrary
    for name in to_inputs:
        if mel.connect_material_expressions(from_expr, from_output, to_expr, name):
            return name
    raise RuntimeError(f'none of {to_inputs} is an input of {to_expr.get_name()}')


def connect_property(from_expr, from_output, prop):
    mel = unreal.MaterialEditingLibrary
    if not mel.connect_material_property(from_expr, from_output, prop):
        raise RuntimeError(f'could not connect {from_expr.get_name()}.{from_output!r} -> material property {prop}')


def scan_assets():
    """The asset registry has not necessarily scanned plugin content yet when this runs headless, and an
    unscanned asset reports as missing - which then fails to create because the package is really there."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([MATERIAL_PATH, '/Game/LambdaEngine'], force_rescan=True)
    registry.wait_for_completion()


def ensure_master_material():
    full_path = f'{MATERIAL_PATH}/{MATERIAL_NAME}'
    if unreal.EditorAssetLibrary.does_asset_exist(full_path):
        log(f'{full_path} already exists')
        return unreal.load_asset(full_path)

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(MATERIAL_NAME, MATERIAL_PATH, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError(f'Could not create {full_path}')

    mel = unreal.MaterialEditingLibrary

    tex_param = mel.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -500, 0)
    tex_param.set_editor_property('parameter_name', 'BaseTexture')
    default_tex = unreal.load_asset('/Engine/EngineResources/DefaultTexture')
    if default_tex:
        tex_param.set_editor_property('texture', default_tex)
    connect_property(tex_param, 'RGB', unreal.MaterialProperty.MP_BASE_COLOR)

    roughness = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -500, 320)
    roughness.set_editor_property('parameter_name', 'Roughness')
    roughness.set_editor_property('default_value', 0.9)
    connect_property(roughness, '', unreal.MaterialProperty.MP_ROUGHNESS)

    specular = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -500, 480)
    specular.set_editor_property('parameter_name', 'Specular')
    specular.set_editor_property('default_value', 0.2)
    connect_property(specular, '', unreal.MaterialProperty.MP_SPECULAR)

    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(full_path, only_if_is_dirty=False)
    log(f'created {full_path}')
    return material


def ensure_entry_level():
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PATH):
        log(f'{LEVEL_PATH} already exists')
        return True

    try:
        les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if les is not None and les.new_level(LEVEL_PATH):
            les.save_current_level()
            log(f'created {LEVEL_PATH} (LevelEditorSubsystem)')
            return True
    except Exception as e:  # noqa: BLE001
        log(f'LevelEditorSubsystem path failed: {e}')

    try:
        world = unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
        if world is not None and unreal.EditorLoadingAndSavingUtils.save_map(world, LEVEL_PATH):
            log(f'created {LEVEL_PATH} (EditorLoadingAndSavingUtils)')
            return True
    except Exception as e:  # noqa: BLE001
        log(f'EditorLoadingAndSavingUtils path failed: {e}')

    log(f'FAILED to create {LEVEL_PATH}')
    return False


def ensure_decal_material():
    # Deferred decal master for Source bullet-impact decals, with parallax occlusion mapping.
    #
    # HL2's impact decals are tiles of a shared atlas (a "Subrect" VMT), so the colour needs a UV rectangle
    # parameter to select the tile: UVRect = (offsetU, offsetV, scaleU, scaleV). The atlas is still what decides
    # how a hole in concrete differs from one in metal; what is new is the shape.
    #
    # Source's decals are flat. The runtime cuts each decal's tile out of the atlas and turns it into a height
    # tile (HeightMap: white = undisturbed surface, dark = deep), and UE's standard ParallaxOcclusionMapping
    # function ray-marches that height field per pixel, so the hole has real apparent depth that shifts and
    # self-occludes as you move past it. The same height tile is tapped to build the surface normal, so the
    # crater rim catches light. DecalDepth scales both; 0 gives a flat Source-style decal.
    #
    # Two kinds of atlas exist. decals_mod2x is DecalModulate, Source's mod2x blend, which
    # decalmodulate_dx9.cpp runs entirely in gamma space: the framebuffer is multiplied by saturate(2 * src) and
    # raw 128 is neutral. We blend black in linear space, so that gamma factor is raised to 2.2 first. decals_lit
    # is an ordinary translucent decal whose alpha carries the shape. The "Modulate" switch selects between them.
    # DecalBlendMode is not settable from Python, so both live under the default translucent decal blend.
    full_path = f'{MATERIAL_PATH}/{DECAL_NAME}'
    if unreal.EditorAssetLibrary.does_asset_exist(full_path):
        log(f'{full_path} already exists')
        return unreal.load_asset(full_path)

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(DECAL_NAME, MATERIAL_PATH, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError(f'Could not create {full_path}')

    material.set_editor_property('material_domain', unreal.MaterialDomain.MD_DEFERRED_DECAL)
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)

    mel = unreal.MaterialEditingLibrary

    def expr(cls, x, y):
        return mel.create_material_expression(material, cls, x, y)

    def scalar(name, value, x, y):
        e = expr(unreal.MaterialExpressionScalarParameter, x, y)
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('default_value', value)
        return e

    # ---- parameters ----
    texcoord = expr(unreal.MaterialExpressionTextureCoordinate, -2200, 0)
    depth = scalar('DecalDepth', 1.0, -2200, 200)
    normal_strength = scalar('NormalStrength', 3.0, -2200, 300)
    texel = scalar('HeightTexelSize', 1.0 / 64.0, -2200, 400)
    modulate = scalar('Modulate', 1.0, -2200, 500)

    uv_rect = expr(unreal.MaterialExpressionVectorParameter, -2200, 650)
    uv_rect.set_editor_property('parameter_name', 'UVRect')
    uv_rect.set_editor_property('default_value', unreal.LinearColor(0.0, 0.0, 1.0, 1.0))

    height_obj = expr(unreal.MaterialExpressionTextureObjectParameter, -2200, 850)
    height_obj.set_editor_property('parameter_name', 'HeightMap')
    default_tex = unreal.load_asset('/Engine/EngineResources/DefaultTexture')
    if default_tex:
        height_obj.set_editor_property('texture', default_tex)

    # ---- parallax occlusion mapping over the height tile ----
    # HeightRatio is a fraction of the tile's width; 0.06 on a 6.4-unit bullet hole is a ~0.4-unit-deep crater.
    # ReferencePlane 1 puts white (the undisturbed surface) exactly on the decal plane, so only the hole sinks.
    pom = expr(unreal.MaterialExpressionMaterialFunctionCall, -1800, 200)
    pom_fn = unreal.load_asset('/Engine/Functions/Engine_MaterialFunctions01/Texturing/ParallaxOcclusionMapping')
    if pom_fn is None:
        raise RuntimeError('ParallaxOcclusionMapping material function not found')
    pom.set_editor_property('material_function', pom_fn)

    # Fade the parallax out as the view goes grazing. POM's ray-march length grows with 1/cos(view angle); with
    # the camera a hand's width from the wall and the hole off to one side, the march runs clear across the tile
    # and smears. The camera vector in the decal's tangent space gives cos(angle) directly as its Z.
    cam_ts = expr(unreal.MaterialExpressionTransform, -2200, 100)
    cam_ts.set_editor_property('transform_source_type', unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD)
    cam_ts.set_editor_property('transform_type', unreal.MaterialVectorCoordTransform.TRANSFORM_TANGENT)
    cam_world = expr(unreal.MaterialExpressionCameraVectorWS, -2400, 100)
    connect(cam_world, '', cam_ts, '')
    cam_z = expr(unreal.MaterialExpressionComponentMask, -2040, 100)
    cam_z.set_editor_property('r', False); cam_z.set_editor_property('g', False)
    cam_z.set_editor_property('b', True); cam_z.set_editor_property('a', False)
    connect(cam_ts, '', cam_z, '')
    cam_abs = expr(unreal.MaterialExpressionAbs, -1940, 100)
    connect(cam_z, '', cam_abs, '')
    # full strength above ~30 degrees of elevation, nothing below ~9: (|z| - 0.15) / 0.35, saturated
    fade_sub = expr(unreal.MaterialExpressionSubtract, -1860, 100)
    connect(cam_abs, '', fade_sub, 'A')
    fade_sub.set_editor_property('const_b', 0.15)
    fade_div = expr(unreal.MaterialExpressionDivide, -1780, 100)
    connect(fade_sub, '', fade_div, 'A')
    fade_div.set_editor_property('const_b', 0.35)
    grazing_fade = expr(unreal.MaterialExpressionSaturate, -1700, 100)
    connect(fade_div, '', grazing_fade, '')

    depth_eff = expr(unreal.MaterialExpressionMultiply, -2000, 200)
    connect(depth, '', depth_eff, 'A')
    connect(grazing_fade, '', depth_eff, 'B')
    height_ratio = expr(unreal.MaterialExpressionMultiply, -1900, 200)
    connect(depth_eff, '', height_ratio, 'A')
    height_ratio.set_editor_property('const_b', 0.06)

    min_steps = expr(unreal.MaterialExpressionConstant, -2000, 280)
    min_steps.set_editor_property('r', 8.0)
    max_steps = expr(unreal.MaterialExpressionConstant, -2000, 340)
    max_steps.set_editor_property('r', 32.0)
    channel = expr(unreal.MaterialExpressionConstant4Vector, -2000, 420)
    channel.set_editor_property('constant', unreal.LinearColor(1.0, 0.0, 0.0, 0.0))
    ref_plane = expr(unreal.MaterialExpressionConstant, -2000, 520)
    ref_plane.set_editor_property('r', 1.0)

    connect(height_obj, '', pom, 'Heightmap Texture')
    connect(height_ratio, '', pom, 'Height Ratio')
    connect(min_steps, '', pom, 'Min Steps')
    connect(max_steps, '', pom, 'Max Steps')
    connect(channel, '', pom, 'Heightmap Channel')
    connect(ref_plane, '', pom, 'Reference Plane')
    uv_pin = connect_any(texcoord, '', pom, ['UVs', 'UV', 'Coordinates', 'Coordinate', 'TexCoord'])
    log(f'POM UV pin is {uv_pin!r}')

    # A ray that marches outside the tile has left the decal: rather than clamping (which stretches the edge
    # texels into streaks) it is made transparent below, through in_tile. The clamp only keeps the samples sane.
    raw_puv = pom
    puv = expr(unreal.MaterialExpressionClamp, -1500, 200)
    puv.set_editor_property('min_default', 0.0)
    puv.set_editor_property('max_default', 1.0)
    connect(raw_puv, 'Parallax UVs', puv, '')

    # in_tile = 1 when 0 <= u,v <= 1, else 0: built as step functions on each axis.
    puv_u = expr(unreal.MaterialExpressionComponentMask, -1500, 40)
    puv_u.set_editor_property('r', True); puv_u.set_editor_property('g', False)
    puv_u.set_editor_property('b', False); puv_u.set_editor_property('a', False)
    connect(raw_puv, 'Parallax UVs', puv_u, '')
    puv_v = expr(unreal.MaterialExpressionComponentMask, -1500, -40)
    puv_v.set_editor_property('r', False); puv_v.set_editor_property('g', True)
    puv_v.set_editor_property('b', False); puv_v.set_editor_property('a', False)
    connect(raw_puv, 'Parallax UVs', puv_v, '')

    def inside01(v, x, y):
        # step(0, v) * step(v, 1)
        lo = expr(unreal.MaterialExpressionStep, x, y)
        connect(v, '', lo, 'X')            # X >= Y -> 1 ; Y default (const) 0
        lo.set_editor_property('const_y', 0.0)
        hi = expr(unreal.MaterialExpressionStep, x, y + 60)
        connect(v, '', hi, 'Y')            # X(1) >= v -> 1
        hi.set_editor_property('const_x', 1.0)
        both = expr(unreal.MaterialExpressionMultiply, x + 120, y + 30)
        connect(lo, '', both, 'A')
        connect(hi, '', both, 'B')
        return both

    in_u = inside01(puv_u, -1380, 40)
    in_v = inside01(puv_v, -1380, -80)
    in_tile = expr(unreal.MaterialExpressionMultiply, -1180, 0)
    connect(in_u, '', in_tile, 'A')
    connect(in_v, '', in_tile, 'B')

    # ---- height at the parallax UV, and its two neighbours for the normal ----
    def height_at(uv, x, y):
        tex = expr(unreal.MaterialExpressionTextureSample, x, y)
        connect_any(height_obj, '', tex, ['Tex', 'TextureObject', 'Texture'])
        connect(uv, '', tex, 'UVs')
        return tex    # use output 'R'

    zero = expr(unreal.MaterialExpressionConstant, -1500, 560)
    zero.set_editor_property('r', 0.0)
    du = expr(unreal.MaterialExpressionAppendVector, -1340, 520)
    connect(texel, '', du, 'A')
    connect(zero, '', du, 'B')
    dv = expr(unreal.MaterialExpressionAppendVector, -1340, 640)
    connect(zero, '', dv, 'A')
    connect(texel, '', dv, 'B')
    uv_du = expr(unreal.MaterialExpressionAdd, -1180, 520)
    connect(puv, '', uv_du, 'A')
    connect(du, '', uv_du, 'B')
    uv_dv = expr(unreal.MaterialExpressionAdd, -1180, 640)
    connect(puv, '', uv_dv, 'A')
    connect(dv, '', uv_dv, 'B')

    h_c = height_at(puv, -1000, 200)
    h_du = height_at(uv_du, -1000, 520)
    h_dv = height_at(uv_dv, -1000, 640)

    # A dent: the surface drops where the height tile darkens, so the normal tilts towards the higher side -
    # (h_centre - h_neighbour), scaled by NormalStrength * DecalDepth. At the default texel size, 3.0 makes a
    # crater about 6% of the tile wide deep, which matches the parallax HeightRatio above.
    normal_scale = expr(unreal.MaterialExpressionMultiply, -1000, 900)
    connect(normal_strength, '', normal_scale, 'A')
    connect(depth, '', normal_scale, 'B')

    slope_u = expr(unreal.MaterialExpressionSubtract, -760, 520)
    connect(h_c, 'R', slope_u, 'A')
    connect(h_du, 'R', slope_u, 'B')
    slope_v = expr(unreal.MaterialExpressionSubtract, -760, 640)
    connect(h_c, 'R', slope_v, 'A')
    connect(h_dv, 'R', slope_v, 'B')
    su = expr(unreal.MaterialExpressionMultiply, -620, 520)
    connect(slope_u, '', su, 'A')
    connect(normal_scale, '', su, 'B')
    sv = expr(unreal.MaterialExpressionMultiply, -620, 640)
    connect(slope_v, '', sv, 'A')
    connect(normal_scale, '', sv, 'B')
    xy = expr(unreal.MaterialExpressionAppendVector, -480, 580)
    connect(su, '', xy, 'A')
    connect(sv, '', xy, 'B')
    one = expr(unreal.MaterialExpressionConstant, -620, 760)
    one.set_editor_property('r', 1.0)
    xyz = expr(unreal.MaterialExpressionAppendVector, -360, 600)
    connect(xy, '', xyz, 'A')
    connect(one, '', xyz, 'B')
    normal = expr(unreal.MaterialExpressionNormalize, -240, 600)
    connect(xyz, '', normal, '')
    connect_property(normal, '', unreal.MaterialProperty.MP_NORMAL)

    # ---- colour from the atlas tile at the parallax UV ----
    uv_scale = expr(unreal.MaterialExpressionAppendVector, -1340, 1000)
    connect(uv_rect, 'B', uv_scale, 'A')
    connect(uv_rect, 'A', uv_scale, 'B')
    uv_offset = expr(unreal.MaterialExpressionAppendVector, -1340, 1120)
    connect(uv_rect, 'R', uv_offset, 'A')
    connect(uv_rect, 'G', uv_offset, 'B')
    atlas_mul = expr(unreal.MaterialExpressionMultiply, -1180, 1000)
    connect(puv, '', atlas_mul, 'A')
    connect(uv_scale, '', atlas_mul, 'B')
    atlas_uv = expr(unreal.MaterialExpressionAdd, -1040, 1000)
    connect(atlas_mul, '', atlas_uv, 'A')
    connect(uv_offset, '', atlas_uv, 'B')

    tex_param = expr(unreal.MaterialExpressionTextureSampleParameter2D, -880, 1000)
    tex_param.set_editor_property('parameter_name', 'BaseTexture')
    if default_tex:
        tex_param.set_editor_property('texture', default_tex)
    connect(atlas_uv, '', tex_param, 'UVs')

    black = expr(unreal.MaterialExpressionConstant3Vector, -620, 960)
    black.set_editor_property('constant', unreal.LinearColor(0.0, 0.0, 0.0, 1.0))
    colour = expr(unreal.MaterialExpressionLinearInterpolate, -360, 1000)
    connect(tex_param, 'RGB', colour, 'A')
    connect(black, '', colour, 'B')
    connect(modulate, '', colour, 'Alpha')
    connect_property(colour, '', unreal.MaterialProperty.MP_BASE_COLOR)

    # ---- opacity ----
    # The height tile already is saturate(2 * luma) for a mod2x decal (white = neutral), i.e. mod2x's gamma-space
    # factor; raising it to 2.2 and inverting gives the linear-space opacity for blending black. A translucent
    # decal's opacity is its own alpha.
    k_linear = expr(unreal.MaterialExpressionPower, -760, 1200)
    connect(h_c, 'R', k_linear, 'Base')
    k_linear.set_editor_property('const_exponent', 2.2)
    opacity_mod = expr(unreal.MaterialExpressionOneMinus, -620, 1200)
    connect(k_linear, '', opacity_mod, '')
    opacity_sel = expr(unreal.MaterialExpressionLinearInterpolate, -360, 1200)
    connect(tex_param, 'A', opacity_sel, 'A')
    connect(opacity_mod, '', opacity_sel, 'B')
    connect(modulate, '', opacity_sel, 'Alpha')
    opacity = expr(unreal.MaterialExpressionMultiply, -220, 1200)
    connect(opacity_sel, '', opacity, 'A')
    connect(in_tile, '', opacity, 'B')
    connect_property(opacity, '', unreal.MaterialProperty.MP_OPACITY)

    # A bullet hole is a rough pit, not a polished one.
    roughness = scalar('Roughness', 0.95, -360, 1380)
    connect_property(roughness, '', unreal.MaterialProperty.MP_ROUGHNESS)

    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(full_path, only_if_is_dirty=False)
    log(f'created {full_path}')
    return material


def build_sprite_material(name, ignore_z):
    # Unlit additive master for Source's UnlitGeneric "$additive 1" effect sprites.
    #
    # Source's flash VMTs also set "$vertexcolor 1", and the first-person ones set "$ignorez 1" so the flash draws
    # over the view model instead of being occluded by the gun and hand it is attached to. ignore_z produces the
    # second variant; picking between them is driven by the VMT at load time.
    full_path = f'{MATERIAL_PATH}/{name}'
    if unreal.EditorAssetLibrary.does_asset_exist(full_path):
        log(f'{full_path} already exists')
        return unreal.load_asset(full_path)

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(name, MATERIAL_PATH, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError(f'Could not create {full_path}')

    material.set_editor_property('material_domain', unreal.MaterialDomain.MD_SURFACE)
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_ADDITIVE)
    material.set_editor_property('two_sided', True)
    if ignore_z:
        try:
            material.set_editor_property('disable_depth_test', True)
        except Exception as e:  # noqa: BLE001
            log(f'could not disable depth test on {name}: {e}')

    mel = unreal.MaterialEditingLibrary

    tex_param = mel.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -700, 0)
    tex_param.set_editor_property('parameter_name', 'BaseTexture')
    default_tex = unreal.load_asset('/Engine/EngineResources/DefaultTexture')
    if default_tex:
        tex_param.set_editor_property('texture', default_tex)

    # "$vertexcolor 1": Source tints each particle through its vertex colour. A MaterialExpressionVertexColor
    # cannot be wired up from Python - all five of its outputs are named "", so neither the RGB output nor a
    # ComponentMask can be addressed by name and the multiply silently collapses to black. A Tint parameter set
    # on the material instance carries the same colour, one value per effect instead of one per particle.
    tint = mel.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -700, 300)
    tint.set_editor_property('parameter_name', 'Tint')
    tint.set_editor_property('default_value', unreal.LinearColor(1.0, 1.0, 1.0, 1.0))

    tinted = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -420, 60)
    connect(tex_param, 'RGB', tinted, 'A')
    connect(tint, 'RGB', tinted, 'B')

    brightness = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -700, 480)
    brightness.set_editor_property('parameter_name', 'Brightness')
    brightness.set_editor_property('default_value', 1.0)

    emissive = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -220, 60)
    connect(tinted, '', emissive, 'A')
    connect(brightness, '', emissive, 'B')
    connect_property(emissive, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # Additive blending still reads opacity; the flash textures carry their shape in RGB.
    one = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -220, 380)
    one.set_editor_property('r', 1.0)
    connect_property(one, '', unreal.MaterialProperty.MP_OPACITY)

    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(full_path, only_if_is_dirty=False)
    log(f'created {full_path}')
    return material


def ensure_sprite_materials():
    build_sprite_material(SPRITE_NAME, ignore_z=False)
    build_sprite_material(SPRITE_NOZ_NAME, ignore_z=True)


scan_assets()
ensure_master_material()
ensure_decal_material()
ensure_sprite_materials()
ensure_entry_level()
log('done')
