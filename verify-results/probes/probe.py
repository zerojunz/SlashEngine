# Read-only probe of Unreal 5.8's Python API for the SlashEngine import check.
import json, traceback
import unreal
out = {}
def t(name, fn):
    try: out[name] = fn()
    except Exception: out[name] = 'ERROR ' + traceback.format_exc().splitlines()[-1]
t('GIsEditor_is_editor', lambda: unreal.SystemLibrary.is_editor() if hasattr(unreal.SystemLibrary,'is_editor') else 'n/a')
t('niagara_names', lambda: sorted(n for n in dir(unreal) if 'niagara' in n.lower() and ('ext' in n.lower() or 'edit' in n.lower() or 'util' in n.lower() or 'librar' in n.lower())))
t('niagara_all_count', lambda: len([n for n in dir(unreal) if 'niagara' in n.lower()]))
cube = unreal.load_asset('/Engine/BasicShapes/Cube')
sms = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
t('sms_simple', lambda: sms.get_simple_collision_count(cube))
t('sms_convex', lambda: sms.get_convex_collision_count(cube))
t('lib_simple', lambda: unreal.EditorStaticMeshLibrary.get_simple_collision_count(cube))
def bodysetup():
    bs = cube.get_editor_property('body_setup')
    agg = bs.get_editor_property('agg_geom')
    return {k: len(agg.get_editor_property(k)) for k in ('box_elems','sphere_elems','sphyl_elems','convex_elems')}
t('body_setup_agg', bodysetup)
t('body_setup_props', lambda: [n for n in dir(cube.get_editor_property('body_setup')) if not n.startswith('_')][:80])
imc = unreal.load_asset('/Game/Input/IMC_Default')
t('imc_mappings', lambda: str(imc.get_editor_property('mappings'))[:300])
t('imc_default_key_mappings', lambda: str(imc.get_editor_property('default_key_mappings'))[:300])
t('imc_dir', lambda: [n for n in dir(imc) if not n.startswith('_') and 'map' in n.lower()])
def dkm():
    d = imc.get_editor_property('default_key_mappings')
    return [n for n in dir(d) if not n.startswith('_')]
t('dkm_dir', dkm)
t('imc_get_mappings', lambda: str(imc.get_mappings())[:300] if hasattr(imc,'get_mappings') else 'n/a')
json.dump(out, open('/Users/daniellee/SlashRunner/unreal-demo/probe/probe.json','w'), indent=1)
