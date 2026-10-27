# Opens /Game/Demo/DemoLevel in the editor, waits for shaders, takes a 1280x720 shot through
# "Demo Camera", then quits. Run: UnrealEditor DemoThird.uproject -ExecutePythonScript=<this>
import time
import unreal
OUT = '/Users/daniellee/SlashRunner/unreal-demo/unreal-DemoLevel.png'
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels.load_level('/Game/Demo/DemoLevel')
state = {'start': time.time(), 'shot': None}
def tick(dt):
    now = time.time() - state['start']
    if state['shot'] is None and now > 90:
        cam = next(a for a in actors.get_all_level_actors() if a.get_actor_label() == 'Demo Camera')
        levels.pilot_level_actor(cam)
        unreal.AutomationLibrary.take_high_res_screenshot(1280, 720, OUT, camera=cam)
        state['shot'] = now
        unreal.log_warning('[demo] screenshot requested')
    elif state['shot'] is not None and now > state['shot'] + 20:
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.SystemLibrary.quit_editor()
handle = unreal.register_slate_post_tick_callback(tick)
