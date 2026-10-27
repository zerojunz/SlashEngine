# Adds the jump pad's Niagara system to /Game/Demo/DemoLevel (spawn_actor_from_object gives nothing in a commandlet).
import json, traceback
import unreal
REPORT = '/Users/daniellee/SlashRunner/unreal-demo/add_niagara.json'
result = {}
try:
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not levels.load_level('/Game/Demo/DemoLevel'):
        raise RuntimeError('load_level failed')
    if any(a.get_actor_label() == 'Jump Pad FX' for a in actors.get_all_level_actors()):
        raise RuntimeError('Jump Pad FX is there already')
    system = unreal.load_asset('/Game/LevelPrototyping/Interactable/JumpPad/Assets/NS_JumpPad')
    actor = actors.spawn_actor_from_class(unreal.NiagaraActor, unreal.Vector(-200, 250, 0))
    actor.set_actor_label('Jump Pad FX')
    comp = actor.get_component_by_class(unreal.NiagaraComponent)
    comp.set_asset(system)
    result['asset'] = str(comp.get_asset())
    result['saved'] = bool(levels.save_current_level())
except Exception:
    result['error'] = traceback.format_exc()
json.dump(result, open(REPORT, 'w'), indent=1)
