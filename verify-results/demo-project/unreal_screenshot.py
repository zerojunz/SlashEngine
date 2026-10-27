# Opens /Game/Demo/DemoLevel in the editor and takes 1280x720 shots through "Demo Camera" at a few
# times (shaders compile in the background), then quits.
# Run: UnrealEditor DemoThird.uproject -ExecutePythonScript=<this>
import time
import unreal
OUT = '/Users/daniellee/SlashRunner/unreal-demo/unreal-DemoLevel-%ds.png'
TIMES = [180, 420, 720]
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels.load_level('/Game/Demo/DemoLevel')
state = {'start': time.time(), 'next': 0}
def tick(dt):
    now = time.time() - state['start']
    if state['next'] < len(TIMES) and now > TIMES[state['next']]:
        cam = next(a for a in actors.get_all_level_actors() if a.get_actor_label() == 'Demo Camera')
        levels.pilot_level_actor(cam)
        unreal.AutomationLibrary.take_high_res_screenshot(1280, 720, OUT % TIMES[state['next']], camera=cam)
        unreal.log_warning('[demo] screenshot requested at %d s' % now)
        state['next'] += 1
    elif state['next'] == len(TIMES) and now > TIMES[-1] + 30:
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.SystemLibrary.quit_editor()
handle = unreal.register_slate_post_tick_callback(tick)
