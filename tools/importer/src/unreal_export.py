# Runs inside the Unreal Editor (UnrealEditor-Cmd -run=pythonscript) when SlashEngine imports an
# Unreal project, and writes what the importer reads into the folder SLASH_UNREAL_EXPORT names:
#
#   export.json        the project's levels (each actor with its components), the meshes and
#                      sounds they use, its Blueprints, Enhanced Input mappings and what couldn't
#                      be read, in Unreal's units and axes (centimeters, Z up, left-handed)
#   meshes/<path>.glb  each static and skeletal mesh a level uses, written by Epic's glTF
#                      exporter
#   materials/<path>.glb  each material a component puts over its mesh's own, on the exporter's
#                      preview mesh
#   animations/<path>.glb  each animation sequence a skeletal mesh component or an Animation
#                      Blueprint plays, with its skeletal mesh
#   sounds/<path>.wav  each sound wave an ambient sound or audio component plays
#   niagara_systems    in export.json, each Niagara system a level's components play: its emitters,
#                      their modules' inputs as Unreal writes them as text, and their renderers,
#                      read through Unreal 5.8's NiagaraExternalEditUtilities (earlier editors
#                      give only the system's own settings)
#   landscapes/<level>/<actor>.r16  each landscape's heights, its streaming proxies' too, as
#                      Unreal's .r16 heightmaps are: 16-bit and little-endian, a row per vertex
#                      along its y, 32768 at its own height and 128 a unit of its space
#
# The importer can leave a request.json there first: its "properties" name, by class path, the
# properties of the project's C++ classes to read of each actor and component of those classes
# (or of Blueprints derived from them), which levels set.
#
# It only reads the project: nothing is saved, and the editor quits when it is done. Everything
# it can't read becomes an entry of "problems" rather than an error, so one odd asset can't stop
# an import. Written for Unreal Engine 5.0 and later; calls that moved between versions are tried
# both ways.

import json
import os
import traceback

import unreal

FORMAT = 1
OUT = os.environ.get('SLASH_UNREAL_EXPORT', '')

problems = []


def problem(what, where, why=''):
    problems.append({'what': what, 'where': where, 'why': why})


def call(default, function, *args):
    try:
        return function(*args)
    except Exception:  # an API missing in this version, or an object without the property
        return default


def prop(obj, name, default=None):
    try:
        return obj.get_editor_property(name)
    except Exception:
        return default


def enum_name(value):
    name = getattr(value, 'name', None)
    if name is None:
        name = str(value).split('.')[-1].split(':')[0].strip('<> ')
    return name.lower()


def vector(v):
    return [float(v.x), float(v.y), float(v.z)]


def color(c):
    # An FColor (0 to 255, sRGB) or an FLinearColor (linear, as SkyLight and fog keep theirs).
    return [float(c.r), float(c.g), float(c.b)]


def transform(location, rotator, scale):
    q = rotator.quaternion()
    return {'location': vector(location), 'rotation': [float(q.x), float(q.y), float(q.z),
                                                       float(q.w)], 'scale': vector(scale)}


def world_transform(t):
    q = t.rotation
    return {'location': vector(t.translation),
            'rotation': [float(q.x), float(q.y), float(q.z), float(q.w)],
            'scale': vector(t.scale3d)}


def package_of(obj):
    # "/Game/Props/SM_Crate" for /Game/Props/SM_Crate.SM_Crate.
    return obj.get_path_name().split('.')[0] if obj is not None else ''


def file_for(folder, package, extension):
    return folder + '/' + package.lstrip('/') + extension


def subsystem(name):
    cls = getattr(unreal, name, None)
    return call(None, unreal.get_editor_subsystem, cls) if cls is not None else None


# --- Meshes and sounds ---

meshes = {}
materials = {}
animations = {}
sounds = {}


def export_gltf(asset, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    options = unreal.GLTFExportOptions()
    for name, value in (('export_level_sequences', False), ('export_animation_sequences', True),
                        ('export_vertex_colors', True)):
        try:
            options.set_editor_property(name, value)
        except Exception:
            pass
    try:
        result = unreal.GLTFExporter.export_to_gltf(asset, path, options, set())
    except TypeError:
        result = unreal.GLTFExporter.export_to_gltf(asset, path, options, unreal.Set(unreal.Actor))
    ok = result[0] if isinstance(result, tuple) else result
    # Its messages: a material it couldn't bake is written plain, so the import says so.
    messages = result[-1] if isinstance(result, tuple) and len(result) > 1 else None
    for kind in ('errors', 'warnings'):
        for message in (prop(messages, kind, []) or []) if messages is not None else []:
            text = str(message)
            if 'bake' in text.lower():
                problem('material the glTF exporter could not bake', package_of(asset), text)
    return bool(ok) and os.path.isfile(path)


def bounds_of(asset):
    b = call(None, asset.get_bounds)
    if b is None:
        return None
    return {'origin': vector(b.origin), 'extent': vector(b.box_extent)}


def static_collision(mesh, entry):
    # The mesh's body setup holds its simple shapes. In a commandlet (-run=pythonscript) there is
    # no StaticMeshEditorSubsystem and EditorStaticMeshLibrary answers -1, so they come second.
    geometry = prop(prop(mesh, 'body_setup'), 'agg_geom')
    if geometry is not None:
        kinds = ('box_elems', 'sphere_elems', 'sphyl_elems', 'convex_elems',
                 'tapered_capsule_elems')
        counts = {kind: len(prop(geometry, kind, []) or []) for kind in kinds}
        entry['simple_collision'] = sum(counts.values())
        entry['convex_collision'] = counts['convex_elems']
        flag = prop(prop(mesh, 'body_setup'), 'collision_trace_flag')
        if flag is not None:
            entry['collision_complexity'] = enum_name(flag)
        return
    tools = subsystem('StaticMeshEditorSubsystem') or getattr(unreal, 'EditorStaticMeshLibrary',
                                                              None)
    if tools is None:
        return
    simple = call(-1, tools.get_simple_collision_count, mesh)
    if simple is None or simple < 0:
        problem('mesh whose collision Unreal does not report', package_of(mesh))
        return
    entry['simple_collision'] = simple
    entry['convex_collision'] = max(0, call(0, tools.get_convex_collision_count, mesh) or 0)
    flag = call(None, tools.get_collision_complexity, mesh)
    if flag is not None:
        entry['collision_complexity'] = enum_name(flag)


def use_mesh(asset, skeletal):
    package = package_of(asset)
    if not package:
        return ''
    if package in meshes:
        return package
    entry = {'skeletal': skeletal}
    path = file_for(OUT + '/meshes', package, '.glb')
    try:
        if export_gltf(asset, path):
            entry['file'] = file_for('meshes', package, '.glb')
        else:
            problem('mesh the glTF exporter did not write', package)
    except Exception as error:
        problem('mesh the glTF exporter did not write', package, str(error))
    bounds = bounds_of(asset)
    if bounds is not None:
        entry['bounds'] = bounds
    if not skeletal:
        static_collision(asset, entry)
    # Its material slots in order, as components' override_materials name them.
    slots = []
    for slot in prop(asset, 'materials' if skeletal else 'static_materials', []) or []:
        material = prop(slot, 'material_interface')
        slots.append(package_of(material))
    entry['materials'] = slots
    meshes[package] = entry
    return package


def use_material(asset):
    package = package_of(asset)
    if not package or package in materials:
        return package
    entry = {'class': asset.get_class().get_name()}
    path = file_for(OUT + '/materials', package, '.glb')
    try:
        if export_gltf(asset, path):
            entry['file'] = file_for('materials', package, '.glb')
        else:
            problem('material the glTF exporter did not write', package)
    except Exception as error:
        problem('material the glTF exporter did not write', package, str(error))
    materials[package] = entry
    return package


def use_animation(asset):
    package = package_of(asset)
    if not package or package in animations:
        return package
    entry = {'class': asset.get_class().get_name()}
    if not isinstance(asset, unreal.AnimSequence):
        problem('animation that is not a sequence', package,
                entry['class'] + ': only animation sequences export with their keys')
        animations[package] = entry
        return package
    path = file_for(OUT + '/animations', package, '.glb')
    try:
        if export_gltf(asset, path):
            entry['file'] = file_for('animations', package, '.glb')
        else:
            problem('animation the glTF exporter did not write', package)
    except Exception as error:
        problem('animation the glTF exporter did not write', package, str(error))
    length = call(None, asset.get_play_length)
    if length is not None:
        entry['length'] = float(length)
    animations[package] = entry
    return package


def use_sound(asset):
    package = package_of(asset)
    if not package:
        return ''
    if package in sounds:
        return package
    entry = {}
    if not isinstance(asset, unreal.SoundWave):
        problem('sound that is not a sound wave', package,
                asset.get_class().get_name() + ': only sound waves export as files')
        sounds[package] = entry
        return package
    path = file_for(OUT + '/sounds', package, '.wav')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        task = unreal.AssetExportTask()
        task.set_editor_property('object', asset)
        task.set_editor_property('filename', path)
        task.set_editor_property('automated', True)
        task.set_editor_property('prompt', False)
        task.set_editor_property('replace_identical', True)
        task.set_editor_property('exporter', unreal.SoundExporterWAV())
        if unreal.Exporter.run_asset_export_task(task) and os.path.isfile(path):
            entry['file'] = file_for('sounds', package, '.wav')
        else:
            problem('sound the WAV exporter did not write', package)
    except Exception as error:
        problem('sound the WAV exporter did not write', package, str(error))
    entry['looping'] = bool(prop(asset, 'looping', False))
    duration = prop(asset, 'duration')
    if duration is not None:
        entry['duration'] = float(duration)
    sounds[package] = entry
    return package


# --- Niagara ---

niagara_systems = {}
NIAGARA_SCRIPTS = ('emitter_spawn_script', 'emitter_update_script', 'particle_spawn_script',
                   'particle_update_script')


def text_of(value):
    # A struct's value as Unreal writes it as text, (Name=Value,...); nested values are kept.
    exported = call(None, value.export_text) if hasattr(value, 'export_text') else None
    return exported if exported is not None else str(value)


def edit_call(name, *args):
    # One of NiagaraExternalEditUtilities' functions; its result, the last of its outputs when it
    # has several. Its editing context is an argument where Python asks for one.
    utilities = getattr(unreal, 'NiagaraExternalEditUtilities', None)
    function = getattr(utilities, name, None) if utilities is not None else None
    if function is None:
        return None
    tries = [args]
    context = getattr(unreal, 'NiagaraExternalEditContext', None)
    if context is not None:
        tries.append(args + (context(),))
    for arguments in tries:
        try:
            result = function(*arguments)
        except TypeError:
            continue
        except Exception:
            return None
        if isinstance(result, tuple):
            result = result[-1] if result else None
        return result
    return None


def stack_reference(system, emitter='', script='', module='', renderer=-1, inputs=()):
    ref = unreal.NiagaraExt_StackItemReference()
    for key, value in (('system', system), ('emitter_name', emitter), ('script_name', script),
                       ('module_name', module), ('renderer_index', renderer)):
        if value not in ('', -1) or key == 'system':
            call(None, ref.set_editor_property, key, value)
    if inputs:
        call(None, ref.set_editor_property, 'input_name_stack', [unreal.Name(i) for i in inputs])
    return ref


def json_of(data):
    # A Niagara schema's JSON values (PropertyValues), or their text when they don't parse.
    values = prop(data, 'property_values', '') if data is not None else ''
    try:
        return json.loads(str(values))
    except ValueError:
        return {'text': str(values)}


def asset_paths(value, out):
    # The asset paths in a renderer's values: /Game/... and StaticMesh'/Game/...' alike.
    if isinstance(value, dict):
        for item in value.values():
            asset_paths(item, out)
    elif isinstance(value, list):
        for item in value:
            asset_paths(item, out)
    elif isinstance(value, str) and '/' in value:
        path = value.split("'")[1] if value.count("'") >= 2 else value
        if path.startswith('/') and path not in out:
            out.append(path)


def renderer_of(system, emitter, ref):
    cls = prop(ref, 'renderer_class')
    index = prop(ref, 'renderer_index', 0)
    out = {'class': cls.get_name() if cls is not None else ''}
    data = edit_call('get_renderer_data', stack_reference(system, emitter, renderer=index))
    out['data'] = json_of(data)
    meshes_used = []
    paths = []
    asset_paths(out['data'], paths)
    for path in paths:
        asset = call(None, unreal.load_asset, path)
        if isinstance(asset, unreal.StaticMesh):
            meshes_used.append(use_mesh(asset, False))
    if meshes_used:
        out['meshes'] = meshes_used
    return out


def module_of(system, emitter, script, module):
    name = str(prop(module, 'module_name', ''))
    out = {'module': name, 'enabled': bool(prop(module, 'enabled', True))}
    asset = prop(module, 'module_script')
    if asset is not None:
        out['script'] = package_of(asset)
    if prop(module, 'is_set_parameters_module', False):
        out['set_parameters'] = True
    values = edit_call('get_module_input_values',
                       stack_reference(system, emitter, script, name))
    inputs = {}
    for entry in (prop(values, 'inputs', []) or []) if values is not None else []:
        input_name = str(prop(entry, 'name', ''))
        value = prop(entry, 'value')
        found = {'value': text_of(value) if value is not None else ''}
        chain = edit_call('get_dynamic_input_chain',
                          stack_reference(system, emitter, script, name, inputs=(input_name,)))
        if chain is not None and (prop(chain, 'inputs', []) or []):
            found['chain'] = text_of(chain)
        inputs[input_name] = found
    out['inputs'] = inputs
    return out


def emitter_of(system, summary):
    name = str(prop(summary, 'emitter_name', ''))
    out = {'name': name, 'enabled': bool(prop(summary, 'enabled', True)),
           'sim_target': enum_name(prop(summary, 'sim_target', 'cpu_sim'))}
    out['data'] = json_of(edit_call('get_emitter_data', stack_reference(system, name)))
    topology = edit_call('get_emitter_topology', stack_reference(system, name))
    if topology is None:
        problem('Niagara emitter the editor did not describe', package_of(system) + ': ' + name)
        return out
    scripts = {}
    for key in NIAGARA_SCRIPTS:
        stack = prop(topology, key)
        if stack is None:
            continue
        script = str(prop(stack, 'script_name', ''))
        scripts[script] = [module_of(system, name, script, module)
                           for module in (prop(stack, 'modules', []) or [])]
    out['scripts'] = scripts
    out['renderers'] = [renderer_of(system, name, ref)
                        for ref in (prop(topology, 'renderers', []) or [])]
    return out


def use_niagara(system):
    package = package_of(system)
    if not package or package in niagara_systems:
        return package
    out = {'random_seed': int(prop(system, 'random_seed', 0)),
           'determinism': bool(prop(system, 'determinism', False)),
           'warmup_time': float(prop(system, 'warmup_time', 0.0))}
    bounds = prop(system, 'fixed_bounds')
    if isinstance(bounds, unreal.Box) and bool(getattr(bounds, 'is_valid', False)):
        out['fixed_bounds'] = {'min': vector(bounds.min), 'max': vector(bounds.max)}
    niagara_systems[package] = out
    summary = edit_call('get_system_summary', system)
    if summary is None:
        problem('Niagara emitters', package,
                "reading a Niagara system's emitters needs Unreal Engine 5.8 or later")
        return package
    out['user_variables'] = [text_of(v) for v in (prop(summary, 'user_variables', []) or [])]
    emitters = []
    for emitter in prop(summary, 'emitters', []) or []:
        try:
            emitters.append(emitter_of(system, emitter))
        except Exception as error:
            problem('Niagara emitter the export could not read',
                    package + ': ' + str(prop(emitter, 'emitter_name', '')), str(error))
    out['emitters'] = emitters
    return package


def niagara_of(comp):
    out = {'auto_activate': bool(prop(comp, 'auto_activate', True))}
    system = prop(comp, 'asset')
    if system is not None:
        out['system'] = use_niagara(system)
    return out


# --- Components ---

LIGHTS = (('DirectionalLightComponent', 'directional'), ('SpotLightComponent', 'spot'),
          ('RectLightComponent', 'rect'), ('PointLightComponent', 'point'),
          ('SkyLightComponent', 'sky'))


def body_of(comp, out):
    instance = prop(comp, 'body_instance')
    simulate = prop(instance, 'simulate_physics') if instance is not None else None
    if simulate is None:
        simulate = call(False, comp.is_simulating_physics)
    out['simulate'] = bool(simulate)
    out['collision'] = enum_name(call('query_and_physics', comp.get_collision_enabled))
    out['profile'] = str(call('', comp.get_collision_profile_name))
    out['overlaps'] = bool(prop(comp, 'generate_overlap_events', False))
    if instance is not None:
        if prop(instance, 'override_mass', False):
            out['mass'] = float(prop(instance, 'mass_in_kg_override', 0.0))
        out['gravity'] = bool(prop(instance, 'enable_gravity', True))
        out['linear_damping'] = float(prop(instance, 'linear_damping', 0.01))
        out['angular_damping'] = float(prop(instance, 'angular_damping', 0.0))
    if out['simulate'] and 'mass' not in out:
        mass = call(None, comp.get_mass)
        if mass:
            out['mass'] = float(mass)


def light_of(comp, kind):
    light = {'type': kind, 'intensity': float(prop(comp, 'intensity', 0.0)),
             'shadows': bool(prop(comp, 'cast_shadows', True))}
    c = prop(comp, 'light_color')
    if c is not None:
        light['color'] = color(c)
    units = prop(comp, 'intensity_units')
    if units is not None:
        light['units'] = enum_name(units)
    for name in ('attenuation_radius', 'source_radius', 'inner_cone_angle', 'outer_cone_angle',
                 'light_source_angle', 'indirect_lighting_intensity', 'volumetric_scattering_intensity'):
        value = prop(comp, name)
        if value is not None:
            light[name] = float(value)
    if prop(comp, 'use_temperature', False):
        light['temperature'] = float(prop(comp, 'temperature', 6500.0))
    mobility = prop(comp, 'mobility')
    if mobility is not None:
        light['mobility'] = enum_name(mobility)
    return light


# The project's C++ classes whose properties the importer asks for: (class, path, names).
requested = []
MISSING = object()


def read_request():
    path = os.path.join(OUT, 'request.json')
    if not os.path.exists(path):
        return
    with open(path, encoding='utf-8') as request:
        asked = json.load(request)
    for class_path, names in sorted((asked.get('properties') or {}).items()):
        cls = call(None, unreal.load_class, None, class_path)
        if cls is None:
            problem('C++ class the editor did not load', class_path,
                    "build the project's code for the editor; its actors keep the class's "
                    'defaults')
            continue
        requested.append((cls, class_path, names))


def python_name(name):
    # A property's name as Unreal's Python names it (PyGenUtil::PythonizeName): bIsDead is
    # is_dead, MaxHP is max_hp.
    if len(name) > 1 and name[0] == 'b' and name[1].isupper():
        name = name[1:]
    out = ''
    for i, c in enumerate(name):
        if c.isupper() and i > 0 and (name[i - 1].islower() or name[i - 1].isdigit() or
                                      (i + 1 < len(name) and name[i + 1].islower())):
            out += '_'
        out += c.lower()
    return out


def property_value(obj, name):
    for key in (python_name(name), name):
        try:
            return obj.get_editor_property(key)
        except Exception:
            pass
    return MISSING


def value_of(v):
    # A property's value as JSON: actors and components by name, assets by package, classes by
    # path, and the rest as Unreal keeps it.
    if v is None or isinstance(v, (bool, int, float, str)):
        return v
    if isinstance(v, (unreal.Name, unreal.Text)):
        return str(v)
    if isinstance(v, unreal.Vector):
        return vector(v)
    if isinstance(v, unreal.Vector2D):
        return [float(v.x), float(v.y)]
    if isinstance(v, unreal.Rotator):
        return [float(v.pitch), float(v.yaw), float(v.roll)]
    if isinstance(v, unreal.Quat):
        return [float(v.x), float(v.y), float(v.z), float(v.w)]
    if isinstance(v, (unreal.LinearColor, unreal.Color)):
        return [float(v.r), float(v.g), float(v.b), float(v.a)]
    if isinstance(v, unreal.EnumBase):
        return int(v.value)
    if isinstance(v, unreal.Actor):
        return {'actor': v.get_name()}
    if isinstance(v, unreal.ActorComponent):
        owner = call(None, v.get_owner)
        return {'actor': owner.get_name() if owner is not None else '', 'component': v.get_name()}
    if isinstance(v, unreal.Class):
        return {'class': v.get_path_name()}
    if isinstance(v, unreal.Object):
        return {'asset': package_of(v)}
    if isinstance(v, (unreal.Map, dict)):
        return {str(key): value_of(item) for key, item in v.items()}
    if isinstance(v, (unreal.Array, unreal.Set, list, tuple, set)):
        return [value_of(item) for item in v]
    return None


def properties_of(obj, out):
    # The properties levels set of an actor or component of a requested class.
    for cls, class_path, names in requested:
        if not call(False, unreal.MathLibrary.class_is_child_of, obj.get_class(), cls):
            continue
        values = {}
        for name in names:
            value = property_value(obj, name)
            if value is not MISSING:
                values[name] = call(None, value_of, value)
        out['properties'] = {'class': class_path, 'values': values}
        return


def overrides_of(comp, out):
    # The materials the component draws its mesh's slots with in place of the mesh's own; ''
    # keeps a slot's.
    overrides = [use_material(m) if m is not None else ''
                 for m in (prop(comp, 'override_materials', []) or [])]
    if any(overrides):
        out['override_materials'] = overrides


ANIMATION_ASSETS = ('BlendSpace', 'BlendSpace1D', 'AimOffsetBlendSpace', 'AimOffsetBlendSpace1D',
                    'AnimMontage', 'AnimComposite', 'PoseAsset')


def sequences_of(package, registry, depth=0):
    # The animation sequences an asset references: its own, and those of the blend spaces,
    # montages and composites it uses.
    options = unreal.AssetRegistryDependencyOptions()
    out = []
    for dep in call([], registry.get_dependencies, package, options) or []:
        dep = str(dep)
        if not dep.startswith('/') or dep.startswith('/Script/'):
            continue
        for data in call([], registry.get_assets_by_package_name, dep) or []:
            kind = class_name(data)
            if kind == 'AnimSequence':
                out.append(dep)
            elif kind in ANIMATION_ASSETS and depth < 2:
                out.extend(sequences_of(dep, registry, depth + 1))
    return out


def anim_blueprint_of(data, registry, entry):
    # The sequences an Animation Blueprint's graphs play, exported as the meshes' are, so the
    # importer can make them an AnimationTree's states.
    package = str(data.package_name)
    seen = []
    for dep in sequences_of(package, registry):
        if dep in seen:
            continue
        seen.append(dep)
        asset = call(None, unreal.EditorAssetLibrary.load_asset, dep)
        if asset is not None:
            use_animation(asset)
    if seen:
        entry['animations'] = seen
    blueprint = call(None, data.get_asset)
    skeleton = prop(blueprint, 'target_skeleton') if blueprint is not None else None
    if skeleton is not None:
        entry['skeleton'] = package_of(skeleton)


def animation_of(comp):
    mode = enum_name(prop(comp, 'animation_mode', 'animation_blueprint'))
    if 'single' in mode:
        data = prop(comp, 'animation_data')
        anim = prop(data, 'anim_to_play') if data is not None else None
        out = {'mode': 'single_node'}
        if anim is not None:
            out['animation'] = use_animation(anim)
        if data is not None:
            out['looping'] = bool(prop(data, 'saved_looping', True))
            out['playing'] = bool(prop(data, 'saved_playing', True))
            out['position'] = float(prop(data, 'saved_position', 0.0))
            out['play_rate'] = float(prop(data, 'saved_play_rate', 1.0))
        return out
    if 'blueprint' in mode:
        anim_class = prop(comp, 'anim_class')
        out = {'mode': 'blueprint'}
        if anim_class is not None:
            out['blueprint'] = package_of(anim_class)
        return out
    return {'mode': mode}


def component_of(actor, comp):
    out = {'name': comp.get_name(), 'class': comp.get_class().get_name()}
    scene = isinstance(comp, unreal.SceneComponent)
    if scene:
        parent = call(None, comp.get_attach_parent)
        if parent is not None and call(None, parent.get_owner) == actor:
            out['parent'] = parent.get_name()
        out['transform'] = transform(prop(comp, 'relative_location', unreal.Vector()),
                                     prop(comp, 'relative_rotation', unreal.Rotator()),
                                     prop(comp, 'relative_scale3d', unreal.Vector(1, 1, 1)))
        if not call(True, comp.is_visible) or prop(comp, 'hidden_in_game', False):
            out['visible'] = False
        mobility = prop(comp, 'mobility')
        if mobility is not None:
            out['mobility'] = enum_name(mobility)
    if isinstance(comp, unreal.PrimitiveComponent) and not prop(comp, 'cast_shadow', True):
        out['cast_shadow'] = False
    if isinstance(comp, unreal.StaticMeshComponent):
        mesh = prop(comp, 'static_mesh')
        if mesh is not None:
            out['mesh'] = use_mesh(mesh, False)
        overrides_of(comp, out)
        body_of(comp, out)
        if isinstance(comp, unreal.InstancedStaticMeshComponent):
            instances = []
            for i in range(call(0, comp.get_instance_count)):
                t = call(None, comp.get_instance_transform, i, False)
                if isinstance(t, tuple):
                    t = t[-1]
                if t is not None:
                    instances.append(world_transform(t))
            out['instances'] = instances
    elif isinstance(comp, unreal.SkeletalMeshComponent):
        mesh = prop(comp, 'skeletal_mesh_asset') or prop(comp, 'skeletal_mesh')
        if mesh is not None:
            out['mesh'] = use_mesh(mesh, True)
        overrides_of(comp, out)
        body_of(comp, out)
        out['animation'] = animation_of(comp)
    elif isinstance(comp, unreal.BoxComponent):
        out['shape'] = {'type': 'box', 'extent': vector(prop(comp, 'box_extent', unreal.Vector()))}
        body_of(comp, out)
    elif isinstance(comp, unreal.SphereComponent):
        out['shape'] = {'type': 'sphere', 'radius': float(prop(comp, 'sphere_radius', 0.0))}
        body_of(comp, out)
    elif isinstance(comp, unreal.CapsuleComponent):
        out['shape'] = {'type': 'capsule', 'radius': float(prop(comp, 'capsule_radius', 0.0)),
                        'half_height': float(prop(comp, 'capsule_half_height', 0.0))}
        body_of(comp, out)
    elif isinstance(comp, unreal.CameraComponent):
        out['camera'] = {'fov': float(prop(comp, 'field_of_view', 90.0)),
                         'projection': enum_name(prop(comp, 'projection_mode', 'perspective')),
                         'ortho_width': float(prop(comp, 'ortho_width', 512.0)),
                         'aspect_ratio': float(prop(comp, 'aspect_ratio', 1.777778)),
                         'constrain_aspect_ratio': bool(prop(comp, 'constrain_aspect_ratio',
                                                             False))}
    elif isinstance(comp, unreal.AudioComponent):
        sound = prop(comp, 'sound')
        audio = {'volume': float(prop(comp, 'volume_multiplier', 1.0)),
                 'pitch': float(prop(comp, 'pitch_multiplier', 1.0)),
                 'auto_activate': bool(prop(comp, 'auto_activate', True))}
        if sound is not None:
            audio['sound'] = use_sound(sound)
        if prop(comp, 'override_attenuation', False):
            settings = prop(comp, 'attenuation_overrides')
            if settings is not None:
                audio['falloff_distance'] = float(prop(settings, 'falloff_distance', 3600.0))
                shape = prop(settings, 'attenuation_shape_extents')
                if shape is not None:
                    audio['inner_radius'] = float(shape.x)
        out['audio'] = audio
    elif isinstance(comp, unreal.ExponentialHeightFogComponent):
        fog = {'density': float(prop(comp, 'fog_density', 0.02)),
               'height_falloff': float(prop(comp, 'fog_height_falloff', 0.2)),
               'start_distance': float(prop(comp, 'start_distance', 0.0)),
               'volumetric': bool(prop(comp, 'volumetric_fog', False))}
        # A LinearColor is never false, so each is looked at in turn: 5.8's luminance is black
        # by default, and the fog takes its color from the sky atmosphere then.
        for name in ('fog_inscattering_luminance', 'fog_inscattering_color'):
            c = prop(comp, name)
            if c is not None and (c.r > 0 or c.g > 0 or c.b > 0):
                fog['color'] = color(c)
                break
        else:
            fog['color'] = [0.0, 0.0, 0.0]
        scale = prop(comp, 'sky_atmosphere_ambient_contribution_color_scale')
        if scale is not None:
            fog['sky_contribution'] = color(scale)
        out['fog'] = fog
    elif isinstance(comp, unreal.SpringArmComponent):
        out['spring_arm'] = {'length': float(prop(comp, 'target_arm_length', 300.0)),
                             'socket_offset': vector(prop(comp, 'socket_offset', unreal.Vector())),
                             'target_offset': vector(prop(comp, 'target_offset', unreal.Vector()))}
    elif isinstance(comp, getattr(unreal, 'NiagaraComponent', ())):
        out['niagara'] = niagara_of(comp)
    elif isinstance(comp, unreal.SkyAtmosphereComponent):
        out['sky_atmosphere'] = {}
    elif isinstance(comp, unreal.CharacterMovementComponent):
        out['character'] = {'max_walk_speed': float(prop(comp, 'max_walk_speed', 600.0)),
                            'jump_z_velocity': float(prop(comp, 'jump_z_velocity', 420.0)),
                            'walkable_floor_angle': float(prop(comp, 'walkable_floor_angle',
                                                               44.765)),
                            'max_step_height': float(prop(comp, 'max_step_height', 45.0))}
    else:
        for cls, kind in LIGHTS:
            if isinstance(comp, getattr(unreal, cls)):
                out['light'] = light_of(comp, kind)
                break
    properties_of(comp, out)
    return out


# --- Landscapes ---

level_package = ''
level_actors = []


def export_heights(actor, path, width, height):
    # Unreal 5.1 and later export a landscape's heights to a file as its editor's Export does;
    # before, they render into a render target, each height's high and low bytes in red and green.
    os.makedirs(os.path.dirname(path), exist_ok=True)
    size = width * height * 2
    try:
        actor.export_heightmap(path)
    except Exception:
        pass
    if os.path.isfile(path) and os.path.getsize(path) == size:
        return True
    world = call(None, actor.get_world)
    library = unreal.RenderingLibrary
    target = call(None, library.create_render_target2d, world, width, height,
                  unreal.TextureRenderTargetFormat.RTF_RGBA8)
    if target is None or not call(False, actor.landscape_export_heightmap_to_render_target,
                                  target, True, True):
        return False
    pixels = call([], library.read_render_target, world, target, False)
    if len(pixels) != width * height:
        return False
    data = bytearray()
    for c in pixels:
        data += (int(c.r) * 256 + int(c.g)).to_bytes(2, 'little')
    with open(path, 'wb') as f:
        f.write(bytes(data))
    return True


def landscape_of(actor):
    where = level_package + ': ' + actor.get_actor_label()
    guid = prop(actor, 'landscape_guid')
    t = actor.get_actor_transform()
    low = None
    high = None
    # Its vertices in its own space: the bounds of its components and its proxies'.
    for proxy in level_actors:
        if not isinstance(proxy, unreal.LandscapeProxy):
            continue
        if proxy is not actor and (guid is None or prop(proxy, 'landscape_guid') != guid):
            continue
        origin, extent = proxy.get_actor_bounds(False)
        if extent.x <= 0.0 and extent.y <= 0.0:
            continue
        for sx in (-1.0, 1.0):
            for sy in (-1.0, 1.0):
                corner = unreal.Vector(origin.x + sx * extent.x, origin.y + sy * extent.y,
                                       origin.z)
                local = t.inverse_transform_location(corner)
                point = (round(local.x), round(local.y))
                low = point if low is None else (min(low[0], point[0]), min(low[1], point[1]))
                high = point if high is None else (max(high[0], point[0]),
                                                   max(high[1], point[1]))
    if low is None:
        problem('landscape without components', where)
        return None
    width = high[0] - low[0] + 1
    height = high[1] - low[1] + 1
    path = file_for('landscapes', level_package + '/' + actor.get_name(), '.r16')
    if not export_heights(actor, os.path.join(OUT, path), width, height):
        problem('landscape the export could not read', where,
                'its heights are not ' + str(width) + ' by ' + str(height))
        return None
    out = {'heightmap': path, 'size': [width, height], 'min': [low[0], low[1]]}
    material = prop(actor, 'landscape_material')
    if material is not None:
        out['material'] = package_of(material)
    layers = []
    for layer in prop(actor, 'editor_layer_settings', []) or []:
        info = prop(layer, 'layer_info_obj')
        if info is not None:
            layers.append(str(prop(info, 'layer_name', info.get_name())))
    if layers:
        out['layers'] = layers
    body = prop(actor, 'body_instance')
    if body is not None:
        out['collision'] = enum_name(prop(body, 'collision_enabled', 'query_and_physics'))
    if not prop(actor, 'cast_shadow', True):
        out['cast_shadow'] = False
    return out


# --- Actors and levels ---

def actor_of(actor):
    out = {'name': actor.get_name(), 'label': actor.get_actor_label(),
           'class': actor.get_class().get_name(), 'class_path': actor.get_class().get_path_name()}
    parent = call(None, actor.get_attach_parent_actor)
    if parent is not None:
        out['parent'] = parent.get_name()
        root = prop(actor, 'root_component')
        socket = call(None, root.get_attach_parent) if root is not None else None
        if socket is not None:
            out['parent_component'] = socket.get_name()
    tags = [str(t) for t in (prop(actor, 'tags', []) or [])]
    if tags:
        out['tags'] = tags
    if prop(actor, 'hidden', False):
        out['visible'] = False
    if isinstance(actor, unreal.Pawn):
        out['auto_possess'] = enum_name(prop(actor, 'auto_possess_player', 'disabled'))
    root = prop(actor, 'root_component')
    if root is not None:
        out['root'] = root.get_name()
    out['transform'] = world_transform(actor.get_actor_transform())
    if isinstance(actor, unreal.Volume) or isinstance(actor, unreal.Brush):
        origin, extent = actor.get_actor_bounds(False)
        out['bounds'] = {'origin': vector(origin), 'extent': vector(extent)}
    if isinstance(actor, unreal.LandscapeProxy):
        # A landscape's components are its heights' tiles; its proxies' are in its heights.
        if isinstance(actor, unreal.Landscape):
            landscape = landscape_of(actor)
            if landscape is not None:
                out['landscape'] = landscape
        out['components'] = []
        return out
    components = []
    for comp in actor.get_components_by_class(unreal.ActorComponent):
        try:
            components.append(component_of(actor, comp))
        except Exception as error:
            problem('component the export could not read',
                    actor.get_actor_label() + '.' + comp.get_name(), str(error))
    out['components'] = components
    properties_of(actor, out)
    return out


def load_level(package):
    world = call(None, unreal.EditorLoadingAndSavingUtils.load_map, package)
    if world is None:
        level_editor = subsystem('LevelEditorSubsystem')
        if level_editor is not None and call(False, level_editor.load_level, package):
            world = call(None, unreal.EditorLevelLibrary.get_editor_world)
    return world


def load_partitioned(world):
    # A World Partition level keeps its actors in packages of their own; load them all.
    library = getattr(unreal, 'WorldPartitionBlueprintLibrary', None)
    if library is None:
        return
    descs = call(None, library.get_actor_descs)
    if isinstance(descs, tuple):
        descs = descs[-1]
    if descs:
        call(None, library.load_actors, [d.guid for d in descs])


def level_of(package):
    world = load_level(package)
    if world is None:
        problem('level the editor did not open', package)
        return None
    load_partitioned(world)
    actors = call(None, unreal.GameplayStatics.get_all_actors_of_class, world, unreal.Actor)
    if actors is None:
        actors = subsystem('EditorActorSubsystem').get_all_level_actors()
    global level_package, level_actors
    level_package = package
    level_actors = list(actors)
    level = {'path': package, 'actors': []}
    settings = call(None, world.get_world_settings)
    if settings is not None:
        level['kill_z'] = float(prop(settings, 'kill_z', -1048575.0))
        game_mode = prop(settings, 'default_game_mode')
        if game_mode is not None:
            level['game_mode'] = game_mode.get_path_name()
    for actor in actors:
        try:
            level['actors'].append(actor_of(actor))
        except Exception as error:
            problem('actor the export could not read', package + ': ' + actor.get_name(),
                    str(error))
    return level


# --- Assets ---

def class_name(data):
    path = getattr(data, 'asset_class_path', None)
    if path is not None:
        return str(path.asset_name)
    return str(data.asset_class)


def tag(data, name):
    value = call(None, data.get_tag_value, name)
    if isinstance(value, tuple):
        value = value[-1] if value[0] else None
    if not value:
        return ''
    # "/Script/CoreUObject.Class'/Script/Engine.Actor'" -> "/Script/Engine.Actor"
    value = str(value)
    return value.split("'")[1] if "'" in value else value


def mappings_of(context):
    # Unreal 5.8 keeps a context's mappings in default_key_mappings and leaves mappings empty.
    found = prop(context, 'mappings')
    if not found:
        keys = prop(context, 'default_key_mappings')
        found = prop(keys, 'mappings') if keys is not None else None
    out = []
    for mapping in found or []:
        action = prop(mapping, 'action')
        key = prop(mapping, 'key')
        if action is None or key is None:
            continue
        modifiers = []
        for modifier in prop(mapping, 'modifiers', []) or []:
            entry = {'class': modifier.get_class().get_name()}
            order = prop(modifier, 'order')
            if order is not None:
                entry['order'] = enum_name(order)
            for axis in ('x', 'y', 'z'):
                if prop(modifier, axis) is not None and entry['class'].endswith('Negate'):
                    entry[axis] = bool(prop(modifier, axis))
            modifiers.append(entry)
        out.append({'action': package_of(action), 'key': str(prop(key, 'key_name', '')),
                    'modifiers': modifiers})
    return out


def main():
    read_request()
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = registry.get_assets_by_path('/Game', recursive=True)
    result = {'format': FORMAT, 'engine_version': call('', unreal.SystemLibrary.get_engine_version),
              'project': {'name': call('', unreal.SystemLibrary.get_game_name)},
              'levels': [], 'blueprints': [], 'input_actions': {}, 'input_contexts': {}}
    worlds = []
    for data in assets:
        kind = class_name(data)
        package = str(data.package_name)
        if kind == 'World':
            worlds.append(package)
        elif kind in ('Blueprint', 'WidgetBlueprint', 'AnimBlueprint'):
            entry = {'path': package, 'kind': kind, 'parent': tag(data, 'ParentClass'),
                     'native_parent': tag(data, 'NativeParentClass')}
            if kind == 'AnimBlueprint':
                anim_blueprint_of(data, registry, entry)
            result['blueprints'].append(entry)
        elif kind == 'InputAction':
            action = call(None, data.get_asset)
            value = prop(action, 'value_type') if action is not None else None
            result['input_actions'][package] = enum_name(value) if value is not None else 'boolean'
        elif kind == 'InputMappingContext':
            context = call(None, data.get_asset)
            if context is not None:
                result['input_contexts'][package] = mappings_of(context)
    for package in sorted(worlds):
        level = level_of(package)
        if level is not None:
            result['levels'].append(level)
    result['meshes'] = meshes
    result['materials'] = materials
    result['animations'] = animations
    result['sounds'] = sounds
    result['niagara_systems'] = niagara_systems
    result['problems'] = problems
    with open(os.path.join(OUT, 'export.json'), 'w', encoding='utf-8') as out:
        json.dump(result, out, indent=1, sort_keys=True)


if __name__ == '__main__':
    try:
        if not OUT:
            raise RuntimeError('SLASH_UNREAL_EXPORT names no folder')
        main()
    except Exception:
        unreal.log_error('SlashEngine export failed:\n' + traceback.format_exc())
        with open(os.path.join(OUT or '.', 'error.txt'), 'w', encoding='utf-8') as out:
            out.write(traceback.format_exc())
    finally:
        # A commandlet ends with the script; the full editor needs telling.
        call(None, unreal.SystemLibrary.quit_editor)
