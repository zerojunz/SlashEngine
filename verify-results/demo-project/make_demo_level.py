# Builds /Game/Demo/DemoLevel in the DemoThird project for SlashEngine's Unreal import check.
# Run: UnrealEditor-Cmd DemoThird.uproject -run=pythonscript -script=make_demo_level.py
import json
import os
import traceback

import unreal

LEVEL = '/Game/Demo/DemoLevel'
REPORT = '/Users/daniellee/SlashRunner/unreal-demo/make_demo_level.json'
made, skipped = [], {}


def log(text):
    unreal.log_warning('[demo] ' + text)


def item(name):
    def wrap(fn):
        try:
            fn()
            made.append(name)
            log('made ' + name)
        except Exception:
            skipped[name] = traceback.format_exc()
            log('skipped ' + name + ':\n' + skipped[name])
        return fn
    return wrap


actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
assets = unreal.EditorAssetLibrary


def load(path):
    asset = unreal.load_asset(path)
    if asset is None:
        raise RuntimeError('no asset ' + path)
    return asset


def spawn(cls, location, rotation=(0, 0, 0), label=None):
    actor = actors.spawn_actor_from_class(cls, unreal.Vector(*location),
                                          unreal.Rotator(roll=rotation[0], pitch=rotation[1],
                                                         yaw=rotation[2]))
    if actor is None:
        raise RuntimeError('could not spawn ' + str(cls))
    if label:
        actor.set_actor_label(label)
    return actor


def mesh_actor(label, mesh, location, scale, material=None):
    actor = spawn(unreal.StaticMeshActor, location, label=label)
    comp = actor.static_mesh_component
    comp.set_static_mesh(load(mesh))
    actor.set_actor_scale3d(unreal.Vector(*scale))
    if material:
        comp.set_material(0, load(material))
    return actor


def main():
    if assets.does_asset_exist(LEVEL):
        raise RuntimeError(LEVEL + ' exists already; move it aside first')
    if not levels.new_level(LEVEL, False):
        raise RuntimeError('new_level failed for ' + LEVEL)

    @item('floor')
    def _():
        mesh_actor('Floor', '/Engine/BasicShapes/Cube', (0, 0, -5), (30, 30, 0.1),
                   '/Game/LevelPrototyping/Materials/MI_PrototypeGrid_Gray')

    @item('cube')
    def _():
        mesh_actor('Shape Cube', '/Engine/BasicShapes/Cube', (0, -250, 50), (1, 1, 1))

    @item('sphere with override material')
    def _():
        mesh_actor('Shape Sphere', '/Engine/BasicShapes/Sphere', (0, 0, 50), (1, 1, 1),
                   '/Game/LevelPrototyping/Interactable/JumpPad/Assets/Materials/MI_GlowNT')

    @item('cylinder')
    def _():
        mesh_actor('Shape Cylinder', '/Engine/BasicShapes/Cylinder', (0, 250, 50), (1, 1, 2))

    @item('directional light')
    def _():
        light = spawn(unreal.DirectionalLight, (0, 0, 500), (0, -45, 30), 'Sun')
        light.light_component.set_intensity(10.0)
        light.light_component.set_editor_property('atmosphere_sun_light', True)

    @item('point light')
    def _():
        light = spawn(unreal.PointLight, (150, -150, 200), label='Point Light')
        comp = light.point_light_component
        comp.set_intensity(5000.0)
        comp.set_light_color(unreal.LinearColor(1.0, 0.5, 0.2, 1.0))
        comp.set_attenuation_radius(800.0)

    @item('spot light')
    def _():
        light = spawn(unreal.SpotLight, (150, 250, 400), (0, -90, 0), 'Spot Light')
        comp = light.spot_light_component
        comp.set_intensity(20000.0)
        comp.set_outer_cone_angle(30.0)
        comp.set_inner_cone_angle(15.0)
        comp.set_light_color(unreal.LinearColor(0.3, 0.6, 1.0, 1.0))

    @item('sky atmosphere')
    def _():
        spawn(unreal.SkyAtmosphere, (0, 0, 0), label='Sky Atmosphere')

    @item('sky light')
    def _():
        sky = spawn(unreal.SkyLight, (0, 0, 300), label='Sky Light')
        sky.light_component.set_editor_property('real_time_capture', True)

    @item('exponential height fog')
    def _():
        fog = spawn(unreal.ExponentialHeightFog, (0, 0, 0), label='Height Fog')
        fog.component.set_fog_density(0.02)

    @item('camera actor')
    def _():
        cam = spawn(unreal.CameraActor, (-700, 0, 300), (0, -15, 0), 'Demo Camera')
        cam.camera_component.set_field_of_view(90.0)
        cam.set_editor_property('auto_activate_for_player', unreal.AutoReceiveInput.PLAYER0)

    @item('player start')
    def _():
        spawn(unreal.PlayerStart, (-400, 0, 100), label='Player Start')

    @item('trigger box')
    def _():
        box = spawn(unreal.TriggerBox, (300, 0, 100), label='Goal Trigger')
        box.collision_component.set_editor_property('box_extent', unreal.Vector(100, 100, 100))

    @item('ambient sound')
    def _():
        sound = spawn(unreal.AmbientSound, (0, 0, 150), label='Tone')
        sound.audio_component.set_sound(load('/Engine/EngineSounds/1kSineTonePing'))

    @item('niagara system')
    def _():
        system = load('/Game/LevelPrototyping/Interactable/JumpPad/Assets/NS_JumpPad')
        actor = actors.spawn_actor_from_object(system, unreal.Vector(-200, 250, 0))
        if actor is None:
            raise RuntimeError('spawn_actor_from_object gave nothing for NS_JumpPad')
        actor.set_actor_label('Jump Pad FX')

    @item('third person character')
    def _():
        bp = unreal.EditorAssetLibrary.load_blueprint_class(
            '/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter')
        spawn(bp, (-150, -150, 100), (0, 0, 45), 'Mannequin')

    @item('landscape')
    def _():
        # Unreal's Python API has no documented call that gives a landscape its components.
        names = [n for n in dir(unreal.Landscape)
                 if 'import' in n.lower() or 'create' in n.lower() or 'height' in n.lower()]
        log('Landscape API: ' + ', '.join(names))
        raise RuntimeError('no Python call creates landscape components (looked at: %s)' % names)

    saved = levels.save_current_level()
    log('saved %s: %s' % (LEVEL, saved))
    return saved


saved, error = False, ''
try:
    saved = main()
except Exception:
    error = traceback.format_exc()
    unreal.log_error('[demo] failed:\n' + error)
with open(REPORT, 'w') as out:
    json.dump({'level': LEVEL, 'saved': bool(saved), 'made': made, 'skipped': skipped,
               'error': error, 'engine': unreal.SystemLibrary.get_engine_version()}, out, indent=1)
