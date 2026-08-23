"""Prints what a generated master material actually ended up with: its blend/shading settings, its expressions,
and which material properties have something connected. Used to check the output of create_assets.py without
opening the editor UI.

Run via Tools/CreateAssets.bat's sibling invocation:
  UnrealEditor-Cmd <project> -run=pythonscript -script="Tools/dump_material.py"
"""
import unreal

PATHS = [
    '/LambdaSource/Materials/M_LambdaBase',
    '/LambdaSource/Materials/M_LambdaSprite',
    '/LambdaSource/Materials/M_LambdaSpriteNoZ',
    '/LambdaSource/Materials/M_LambdaDecal',
]

PROPERTIES = [
    ('BaseColor', unreal.MaterialProperty.MP_BASE_COLOR),
    ('Emissive', unreal.MaterialProperty.MP_EMISSIVE_COLOR),
    ('Opacity', unreal.MaterialProperty.MP_OPACITY),
    ('Normal', unreal.MaterialProperty.MP_NORMAL),
    ('Roughness', unreal.MaterialProperty.MP_ROUGHNESS),
]


def log(msg):
    unreal.log('[LambdaEngine] ' + msg)


registry = unreal.AssetRegistryHelpers.get_asset_registry()
registry.scan_paths_synchronous(['/LambdaSource/Materials'], force_rescan=True)
registry.wait_for_completion()

mel = unreal.MaterialEditingLibrary

for path in PATHS:
    if not unreal.EditorAssetLibrary.does_asset_exist(path):
        log(f'{path}: MISSING')
        continue
    material = unreal.load_asset(path)
    log(f'--- {path} ---')
    for name in ('material_domain', 'blend_mode', 'shading_model', 'two_sided', 'disable_depth_test'):
        try:
            log(f'    {name} = {material.get_editor_property(name)}')
        except Exception as e:  # noqa: BLE001
            log(f'    {name} = <unreadable: {e}>')

    for label, prop in PROPERTIES:
        try:
            connected = mel.get_material_property_input_node(material, prop)
        except Exception as e:  # noqa: BLE001
            connected = f'<error {e}>'
        log(f'    {label:<10} <- {connected}')

    exprs = mel.get_material_selected_nodes(material) if False else None
    try:
        log(f'    expressions: {len(material.get_editor_property("expression_collection").expressions)}')
    except Exception as e:  # noqa: BLE001
        log(f'    expressions: <unreadable: {e}>')

log('done')
