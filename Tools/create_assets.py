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
    # Deferred decal master for Source bullet-impact decals.
    #
    # HL2's impact decals are tiles of a shared atlas (a "Subrect" VMT), so the material needs a UV rectangle
    # parameter to select the tile: UVRect = (offsetU, offsetV, scaleU, scaleV).
    #
    # Two kinds of atlas exist. decals_mod2x is a DecalModulate material - Source's mod2x blend, dst = 2*dst*src,
    # where mid-grey leaves the surface alone and darker darkens it. decals_lit is an ordinary translucent decal
    # whose alpha carries the shape. UE's DecalBlendMode is not settable from Python (the property is protected),
    # so both are expressed under the default translucent decal blend, selected by a "Modulate" switch:
    #
    #   modulate: colour = black, opacity = 1 - saturate(2 * luma(src))
    #   lit:      colour = src.rgb, opacity = src.a
    #
    # Blending black over the surface with that opacity gives dst * saturate(2*src) - mod2x's darkening exactly,
    # and because the decal contributes no colour of its own the surface keeps its own hue.
    #
    # Source's decals are flat. To make a bullet hole read as an actual dent, the same darkness doubles as a
    # height field (dark = deep) and drives two things Source never had: a parallax offset on the UVs, so the
    # hole shifts against the surface as you move past it, and a surface normal, so the crater rim catches light
    # - which is what makes a muzzle flash sweep across it convincingly. DecalDepth scales both; set it to 0 for
    # flat Source-style decals.
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

    # ---- tile selection: UV = TexCoord * UVRect.zw + UVRect.xy ----
    texcoord = expr(unreal.MaterialExpressionTextureCoordinate, -2000, 0)
    uv_rect = expr(unreal.MaterialExpressionVectorParameter, -2000, 200)
    uv_rect.set_editor_property('parameter_name', 'UVRect')
    uv_rect.set_editor_property('default_value', unreal.LinearColor(0.0, 0.0, 1.0, 1.0))

    uv_scale = expr(unreal.MaterialExpressionAppendVector, -1780, 200)
    connect(uv_rect, 'B', uv_scale, 'A')
    connect(uv_rect, 'A', uv_scale, 'B')
    uv_offset = expr(unreal.MaterialExpressionAppendVector, -1780, 340)
    connect(uv_rect, 'R', uv_offset, 'A')
    connect(uv_rect, 'G', uv_offset, 'B')

    depth = expr(unreal.MaterialExpressionScalarParameter, -2000, 460)
    depth.set_editor_property('parameter_name', 'DecalDepth')
    depth.set_editor_property('default_value', 1.0)

    def tile_uv(base_uv, x, y):
        """base_uv (0..1 within the tile) -> atlas UV."""
        mul = expr(unreal.MaterialExpressionMultiply, x, y)
        connect(base_uv, '', mul, 'A')
        connect(uv_scale, '', mul, 'B')
        add = expr(unreal.MaterialExpressionAdd, x + 160, y)
        connect(mul, '', add, 'A')
        connect(uv_offset, '', add, 'B')
        return add

    def sample(uv, x, y):
        tex = expr(unreal.MaterialExpressionTextureSampleParameter2D, x, y)
        tex.set_editor_property('parameter_name', 'BaseTexture')
        default_tex = unreal.load_asset('/Engine/EngineResources/DefaultTexture')
        if default_tex:
            tex.set_editor_property('texture', default_tex)
        connect(uv, '', tex, 'UVs')
        return tex

    def height_of(tex, x, y):
        """height = 1 - saturate(2 * luma(src)): 1 at the black centre of the hole, 0 on untouched surface."""
        luma = expr(unreal.MaterialExpressionDesaturation, x, y)
        connect(tex, 'RGB', luma, '')
        two_x = expr(unreal.MaterialExpressionMultiply, x + 120, y)
        connect(luma, '', two_x, 'A')
        two_x.set_editor_property('const_b', 2.0)
        sat = expr(unreal.MaterialExpressionSaturate, x + 240, y)
        connect(two_x, '', sat, '')
        inv = expr(unreal.MaterialExpressionOneMinus, x + 340, y)
        connect(sat, '', inv, '')
        return inv

    # ---- parallax: sample the height once, then offset the UVs by the view vector ----
    height_uv = tile_uv(texcoord, -1600, 0)
    height_tex = sample(height_uv, -1400, 0)
    height_probe = height_of(height_tex, -1180, 0)

    # BumpOffset's HeightRatio is a fraction of the UV range; a few percent is the whole effect, and anything
    # like a full tile of shift pulls the neighbouring decals out of the atlas. It is negative because the height
    # field measures depth *into* the surface, the opposite of BumpOffset's raised-towards-the-viewer convention,
    # and the reference plane is 0 so untouched surface (height 0) is not displaced at all.
    parallax_scale = expr(unreal.MaterialExpressionMultiply, -1800, 620)
    connect(depth, '', parallax_scale, 'A')
    parallax_scale.set_editor_property('const_b', -0.04)

    bump = expr(unreal.MaterialExpressionBumpOffset, -1600, 620)
    bump.set_editor_property('reference_plane', 0.0)
    connect(texcoord, '', bump, 'Coordinate')
    connect(height_probe, '', bump, 'Height')
    connect(parallax_scale, '', bump, 'HeightRatioInput')

    # Keep the displaced coordinate inside this tile: the atlas neighbour is a different decal.
    bumped_uv = expr(unreal.MaterialExpressionClamp, -1440, 620)
    bumped_uv.set_editor_property('min_default', 0.02)
    bumped_uv.set_editor_property('max_default', 0.98)
    connect(bump, '', bumped_uv, '')

    main_uv = tile_uv(bumped_uv, -1300, 620)
    tex_param = sample(main_uv, -1000, 620)

    # ---- normal from the height field: two neighbouring taps give its slope ----
    texel = expr(unreal.MaterialExpressionScalarParameter, -2000, 900)
    texel.set_editor_property('parameter_name', 'HeightTexelSize')
    texel.set_editor_property('default_value', 0.02)

    du = expr(unreal.MaterialExpressionAppendVector, -1840, 900)
    zero_u = expr(unreal.MaterialExpressionConstant, -2000, 1000)
    zero_u.set_editor_property('r', 0.0)
    connect(texel, '', du, 'A')
    connect(zero_u, '', du, 'B')

    dv = expr(unreal.MaterialExpressionAppendVector, -1840, 1080)
    connect(zero_u, '', dv, 'A')
    connect(texel, '', dv, 'B')

    uv_du = expr(unreal.MaterialExpressionAdd, -1660, 900)
    connect(bumped_uv, '', uv_du, 'A')
    connect(du, '', uv_du, 'B')
    uv_dv = expr(unreal.MaterialExpressionAdd, -1660, 1080)
    connect(bumped_uv, '', uv_dv, 'A')
    connect(dv, '', uv_dv, 'B')

    h_centre = height_of(tex_param, -760, 900)
    h_du = height_of(sample(tile_uv(uv_du, -1480, 1240), -1180, 1240), -940, 1240)
    h_dv = height_of(sample(tile_uv(uv_dv, -1480, 1440), -1180, 1440), -940, 1440)

    # A dent: the surface falls away where the height field rises, so the normal tilts towards the rising side -
    # (h_neighbour - h_centre), not the other way round, or the hole shades as a bump.
    slope_u = expr(unreal.MaterialExpressionSubtract, -520, 1240)
    connect(h_du, '', slope_u, 'A')
    connect(h_centre, '', slope_u, 'B')
    slope_v = expr(unreal.MaterialExpressionSubtract, -520, 1440)
    connect(h_dv, '', slope_v, 'A')
    connect(h_centre, '', slope_v, 'B')

    # The slope is a height difference over one HeightTexelSize of UV, so it needs dividing by that to become a
    # gradient, then multiplying by how deep the dent is as a fraction of the tile. NormalStrength folds both in:
    # at the default texel size, 3.0 is a dent about 6% of the tile wide deep, which on a 6.4-unit bullet hole is
    # a believable 0.4 units. DecalDepth scales it along with the parallax.
    normal_strength = expr(unreal.MaterialExpressionScalarParameter, -2000, 1200)
    normal_strength.set_editor_property('parameter_name', 'NormalStrength')
    normal_strength.set_editor_property('default_value', 3.0)
    normal_scale = expr(unreal.MaterialExpressionMultiply, -1800, 1200)
    connect(normal_strength, '', normal_scale, 'A')
    connect(depth, '', normal_scale, 'B')

    slope_u_scaled = expr(unreal.MaterialExpressionMultiply, -400, 1240)
    connect(slope_u, '', slope_u_scaled, 'A')
    connect(normal_scale, '', slope_u_scaled, 'B')
    slope_v_scaled = expr(unreal.MaterialExpressionMultiply, -400, 1440)
    connect(slope_v, '', slope_v_scaled, 'A')
    connect(normal_scale, '', slope_v_scaled, 'B')

    xy = expr(unreal.MaterialExpressionAppendVector, -280, 1300)
    connect(slope_u_scaled, '', xy, 'A')
    connect(slope_v_scaled, '', xy, 'B')

    one_z = expr(unreal.MaterialExpressionConstant, -400, 1560)
    one_z.set_editor_property('r', 1.0)
    xyz = expr(unreal.MaterialExpressionAppendVector, -180, 1360)
    connect(xy, '', xyz, 'A')
    connect(one_z, '', xyz, 'B')

    normal = expr(unreal.MaterialExpressionNormalize, -60, 1360)
    connect(xyz, '', normal, '')
    connect_property(normal, '', unreal.MaterialProperty.MP_NORMAL)

    modulate = expr(unreal.MaterialExpressionScalarParameter, -760, 700)
    modulate.set_editor_property('parameter_name', 'Modulate')
    modulate.set_editor_property('default_value', 1.0)

    # ---- colour: the surface keeps its own hue, a modulated decal only darkens it ----
    black = expr(unreal.MaterialExpressionConstant3Vector, -520, 480)
    black.set_editor_property('constant', unreal.LinearColor(0.0, 0.0, 0.0, 1.0))

    colour = expr(unreal.MaterialExpressionLinearInterpolate, -260, 520)
    connect(tex_param, 'RGB', colour, 'A')
    connect(black, '', colour, 'B')
    connect(modulate, '', colour, 'Alpha')
    connect_property(colour, '', unreal.MaterialProperty.MP_BASE_COLOR)

    # ---- opacity ----
    # mod2x multiplies the framebuffer by saturate(2 * src) *in gamma space* (decalmodulate_dx9.cpp turns sRGB read
    # and write off for exactly this). We blend black in linear space, so the same visual result needs that
    # gamma factor raised to 2.2 before it becomes 1 - opacity. The atlas is loaded raw for this material, which
    # is what makes its neutral 128 come out as 0.5 here.
    luma_raw = expr(unreal.MaterialExpressionDesaturation, -760, 800)
    connect(tex_param, 'RGB', luma_raw, '')
    k_gamma_2x = expr(unreal.MaterialExpressionMultiply, -640, 800)
    connect(luma_raw, '', k_gamma_2x, 'A')
    k_gamma_2x.set_editor_property('const_b', 2.0)
    k_gamma = expr(unreal.MaterialExpressionSaturate, -540, 800)
    connect(k_gamma_2x, '', k_gamma, '')
    k_linear = expr(unreal.MaterialExpressionPower, -440, 800)
    connect(k_gamma, '', k_linear, 'Base')
    k_linear.set_editor_property('const_exponent', 2.2)
    opacity_mod = expr(unreal.MaterialExpressionOneMinus, -340, 800)
    connect(k_linear, '', opacity_mod, '')

    opacity = expr(unreal.MaterialExpressionLinearInterpolate, -260, 780)
    connect(tex_param, 'A', opacity, 'A')
    connect(opacity_mod, '', opacity, 'B')
    connect(modulate, '', opacity, 'Alpha')
    connect_property(opacity, '', unreal.MaterialProperty.MP_OPACITY)

    # A bullet hole is a rough pit, not a polished one.
    roughness = expr(unreal.MaterialExpressionScalarParameter, -260, 980)
    roughness.set_editor_property('parameter_name', 'Roughness')
    roughness.set_editor_property('default_value', 0.95)
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
