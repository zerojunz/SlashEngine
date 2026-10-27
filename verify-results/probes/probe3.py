# Read-only probe: the demo's light units, and what Unreal 5.8.4's Python reaches of NS_JumpPad.
import json, os, re, traceback
import unreal
OUTDIR = '/Users/daniellee/SlashRunner/unreal-demo/probe'
out = {}

def safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return 'ERROR ' + traceback.format_exc().strip().splitlines()[-1]

def props(obj, names):
    return {n: safe(lambda n=n: repr(obj.get_editor_property(n))[:600]) for n in names}

def attrs(obj):
    found = {}
    for n in dir(obj):
        if n.startswith('_'):
            continue
        v = safe(lambda n=n: getattr(obj, n))
        if callable(v):
            found[n] = 'method'
        else:
            found[n] = repr(v)[:300]
    return found

# 1. Lights
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels.load_level('/Game/Demo/DemoLevel')
lights = {}
for a in actors.get_all_level_actors():
    label = a.get_actor_label()
    if label in ('Point Light', 'Spot Light', 'Sun', 'Sky Light'):
        comp = a.get_component_by_class(unreal.LightComponentBase)
        lights[label] = props(comp, ['intensity', 'intensity_units', 'attenuation_radius',
                                     'source_radius', 'light_color', 'use_temperature',
                                     'temperature', 'inverse_squared_falloff', 'outer_cone_angle'])
out['lights'] = lights

# 2. Niagara
out['niagara_names'] = sorted(n for n in dir(unreal) if 'Niagara' in n)
system = unreal.load_asset('/Game/LevelPrototyping/Interactable/JumpPad/Assets/NS_JumpPad')
out['system_attrs'] = attrs(system)
out['system_props'] = props(system, [
    'emitter_handles', 'system_spawn_script', 'system_update_script', 'exposed_parameters',
    'user_parameters', 'effect_type', 'warmup_time', 'fixed_bounds', 'determinism',
    'random_seed', 'editor_data', 'scalability_overrides'])
handles = safe(lambda: system.get_editor_property('emitter_handles'))
out['handle_count'] = len(handles) if isinstance(handles, (list, unreal.Array)) else str(handles)
if isinstance(handles, (list, unreal.Array)) and len(handles):
    h = handles[0]
    out['handle0_attrs'] = attrs(h)
    out['handle0_props'] = props(h, ['id', 'name', 'is_enabled', 'instance',
                                     'versioned_instance', 'emitter'])
    emitter = None
    for name in ('instance', 'versioned_instance', 'emitter'):
        v = safe(lambda name=name: h.get_editor_property(name))
        if isinstance(v, unreal.NiagaraEmitter):
            emitter = v
            break
        if v is not None and not isinstance(v, str) and hasattr(v, 'get_editor_property'):
            e = safe(lambda v=v: v.get_editor_property('emitter'))
            if isinstance(e, unreal.NiagaraEmitter):
                emitter = e
                break
    if emitter is not None:
        out['emitter_attrs'] = attrs(emitter)
        out['emitter_props'] = props(emitter, [
            'renderer_properties', 'sim_target', 'local_space', 'spawn_script_props',
            'update_script_props', 'emitter_spawn_script_props', 'emitter_update_script_props',
            'event_handler_script_props', 'simulation_stages', 'version_data', 'emitter_data'])
for fn in ('get_emitter_handles', 'get_emitters'):
    if hasattr(system, fn):
        out['call_' + fn] = safe(lambda fn=fn: repr(getattr(system, fn)())[:600])
out['function_library'] = sorted(n for n in dir(unreal.NiagaraFunctionLibrary) if not n.startswith('_'))
out['clipboard_utils'] = sorted(n for n in dir(unreal.NiagaraClipboardEditorScriptingUtilities)
                                if not n.startswith('_'))

# 3. Text export
def export(obj, exporter_cls, path):
    task = unreal.AssetExportTask()
    task.set_editor_property('object', obj)
    task.set_editor_property('filename', path)
    task.set_editor_property('automated', True)
    task.set_editor_property('prompt', False)
    task.set_editor_property('replace_identical', True)
    if exporter_cls is not None:
        task.set_editor_property('exporter', exporter_cls())
    ok = unreal.Exporter.run_asset_export_task(task)
    info = {'ok': bool(ok), 'exists': os.path.exists(path)}
    if info['exists']:
        text = open(path, encoding='utf-8', errors='replace').read()
        info['bytes'] = len(text)
        for word in ('Module', 'RapidIteration', 'SpawnRate', 'AddVelocity', 'InitializeParticle',
                     'RendererProperties', 'SimTarget', 'NiagaraScript', 'Begin Object'):
            info['count_' + word] = len(re.findall(word, text))
    errors = safe(lambda: [str(e) for e in task.get_editor_property('errors')])
    info['errors'] = errors
    return info
out['export_t3d'] = safe(lambda: export(system, unreal.ObjectExporterT3D,
                                        os.path.join(OUTDIR, 'NS_JumpPad.t3d')))
out['export_copy'] = safe(lambda: export(system, getattr(unreal, 'ObjectExporterCOPY', None),
                                         os.path.join(OUTDIR, 'NS_JumpPad.copy')))
out['export_default'] = safe(lambda: export(system, None, os.path.join(OUTDIR, 'NS_JumpPad.txt')))
json.dump(out, open(os.path.join(OUTDIR, 'probe3.json'), 'w'), indent=1, default=str)
