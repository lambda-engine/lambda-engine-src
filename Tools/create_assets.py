"""
Creates the editor-side assets the Lambda Engine runtime expects:
  * /LambdaSource/Materials/M_LambdaBase   - master material with a "BaseTexture" parameter (plugin content)
  * /LambdaSource/Materials/M_LambdaDecal  - deferred decal master for bullet impacts
  * /LambdaSource/Materials/M_LambdaSprite - unlit additive master for muzzle flashes
  * /Game/LambdaEngine/Maps/LambdaEntry    - empty startup level

Run via Tools/CreateAssets.bat (UnrealEditor-Cmd -run=pythonscript) or from the editor's Python console.
"""
import unreal

MATERIAL_PATH = '/LambdaSource/Materials'
MATERIAL_NAME = 'M_LambdaBase'
DECAL_NAME = 'M_LambdaDecal'
SPRITE_NAME = 'M_LambdaSprite'
LEVEL_PATH = '/Game/LambdaEngine/Maps/LambdaEntry'


def log(msg):
    unreal.log('[LambdaEngine] ' + msg)


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
    mel.connect_material_property(tex_param, 'RGB', unreal.MaterialProperty.MP_BASE_COLOR)

    roughness = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -500, 320)
    roughness.set_editor_property('parameter_name', 'Roughness')
    roughness.set_editor_property('default_value', 0.9)
    mel.connect_material_property(roughness, '', unreal.MaterialProperty.MP_ROUGHNESS)

    specular = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -500, 480)
    specular.set_editor_property('parameter_name', 'Specular')
    specular.set_editor_property('default_value', 0.2)
    mel.connect_material_property(specular, '', unreal.MaterialProperty.MP_SPECULAR)

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
    # and because the decal contributes no colour of its own the surface keeps its own hue. mod2x can also
    # brighten (src > 0.5); that half has no translucent equivalent and is clamped away, which for scorch marks
    # is the half that does not matter.
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

    # UV = TexCoord * UVRect.zw + UVRect.xy - selects one tile of the decal atlas.
    texcoord = mel.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -1400, 0)
    uv_rect = mel.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -1400, 200)
    uv_rect.set_editor_property('parameter_name', 'UVRect')
    uv_rect.set_editor_property('default_value', unreal.LinearColor(0.0, 0.0, 1.0, 1.0))

    scale = mel.create_material_expression(material, unreal.MaterialExpressionAppendVector, -1160, 220)
    mel.connect_material_expressions(uv_rect, 'B', scale, 'A')
    mel.connect_material_expressions(uv_rect, 'A', scale, 'B')
    offset = mel.create_material_expression(material, unreal.MaterialExpressionAppendVector, -1160, 360)
    mel.connect_material_expressions(uv_rect, 'R', offset, 'A')
    mel.connect_material_expressions(uv_rect, 'G', offset, 'B')

    mul_uv = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -960, 60)
    mel.connect_material_expressions(texcoord, '', mul_uv, 'A')
    mel.connect_material_expressions(scale, '', mul_uv, 'B')
    add_uv = mel.create_material_expression(material, unreal.MaterialExpressionAdd, -800, 60)
    mel.connect_material_expressions(mul_uv, '', add_uv, 'A')
    mel.connect_material_expressions(offset, '', add_uv, 'B')

    tex_param = mel.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -600, 0)
    tex_param.set_editor_property('parameter_name', 'BaseTexture')
    default_tex = unreal.load_asset('/Engine/EngineResources/DefaultTexture')
    if default_tex:
        tex_param.set_editor_property('texture', default_tex)
    mel.connect_material_expressions(add_uv, '', tex_param, 'UVs')

    modulate = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -600, 620)
    modulate.set_editor_property('parameter_name', 'Modulate')
    modulate.set_editor_property('default_value', 1.0)

    # ---- colour: the surface's own colour survives, so a modulated decal only darkens ----
    black = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -340, -120)
    black.set_editor_property('constant', unreal.LinearColor(0.0, 0.0, 0.0, 1.0))

    colour = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -120, -60)
    mel.connect_material_expressions(tex_param, 'RGB', colour, 'A')
    mel.connect_material_expressions(black, '', colour, 'B')
    mel.connect_material_expressions(modulate, '', colour, 'Alpha')
    mel.connect_material_property(colour, '', unreal.MaterialProperty.MP_BASE_COLOR)

    # ---- opacity: 1 - saturate(2 * luma(src)) reproduces mod2x's darkening curve ----
    luma = mel.create_material_expression(material, unreal.MaterialExpressionDesaturation, -440, 280)
    mel.connect_material_expressions(tex_param, 'RGB', luma, '')

    times_two = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -320, 280)
    mel.connect_material_expressions(luma, '', times_two, 'A')
    times_two.set_editor_property('const_b', 2.0)

    clamped = mel.create_material_expression(material, unreal.MaterialExpressionSaturate, -220, 280)
    mel.connect_material_expressions(times_two, '', clamped, '')

    opacity_mod = mel.create_material_expression(material, unreal.MaterialExpressionOneMinus, -140, 280)
    mel.connect_material_expressions(clamped, '', opacity_mod, '')

    opacity = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -40, 420)
    mel.connect_material_expressions(tex_param, 'A', opacity, 'A')
    mel.connect_material_expressions(opacity_mod, '', opacity, 'B')
    mel.connect_material_expressions(modulate, '', opacity, 'Alpha')
    mel.connect_material_property(opacity, '', unreal.MaterialProperty.MP_OPACITY)

    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(full_path, only_if_is_dirty=False)
    log(f'created {full_path}')
    return material


def ensure_sprite_material():
    # Unlit additive master for Source's UnlitGeneric "$additive 1" effect sprites (muzzle flashes).
    full_path = f'{MATERIAL_PATH}/{SPRITE_NAME}'
    if unreal.EditorAssetLibrary.does_asset_exist(full_path):
        log(f'{full_path} already exists')
        return unreal.load_asset(full_path)

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(SPRITE_NAME, MATERIAL_PATH, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError(f'Could not create {full_path}')

    material.set_editor_property('material_domain', unreal.MaterialDomain.MD_SURFACE)
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_ADDITIVE)
    material.set_editor_property('two_sided', True)

    mel = unreal.MaterialEditingLibrary

    tex_param = mel.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -500, 0)
    tex_param.set_editor_property('parameter_name', 'BaseTexture')
    default_tex = unreal.load_asset('/Engine/EngineResources/DefaultTexture')
    if default_tex:
        tex_param.set_editor_property('texture', default_tex)

    brightness = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -500, 300)
    brightness.set_editor_property('parameter_name', 'Brightness')
    brightness.set_editor_property('default_value', 1.0)

    emissive = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -220, 0)
    mel.connect_material_expressions(tex_param, 'RGB', emissive, 'A')
    mel.connect_material_expressions(brightness, '', emissive, 'B')
    mel.connect_material_property(emissive, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # Additive blending still reads opacity; the flash textures carry their shape in RGB.
    one = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -220, 300)
    one.set_editor_property('r', 1.0)
    mel.connect_material_property(one, '', unreal.MaterialProperty.MP_OPACITY)

    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(full_path, only_if_is_dirty=False)
    log(f'created {full_path}')
    return material


scan_assets()
ensure_master_material()
ensure_decal_material()
ensure_sprite_material()
ensure_entry_level()
log('done')
