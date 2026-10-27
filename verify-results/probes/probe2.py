# Read-only probe: where Unreal 5.8's Python exposes get_system_summary.
import json, traceback
import unreal
out = {}
out['context_dir'] = [n for n in dir(unreal.NiagaraExternalEditContext) if not n.startswith('_')]
hits = []
for name in dir(unreal):
    obj = getattr(unreal, name, None)
    try:
        if any(m in dir(obj) for m in ('get_system_summary', 'get_emitter_data')):
            hits.append(name)
    except Exception:
        pass
out['classes_with_get_system_summary'] = hits
for h in hits:
    out['dir_' + h] = [n for n in dir(getattr(unreal, h)) if n.startswith('get_') or n.startswith('find')][:60]
try:
    system = unreal.load_asset('/Game/LevelPrototyping/Interactable/JumpPad/Assets/NS_JumpPad')
    if hits:
        cls = getattr(unreal, hits[0])
        try:
            r = cls.get_system_summary(system)
        except TypeError as e:
            out['call_typeerror'] = str(e)
            r = cls.get_system_summary(system, unreal.NiagaraExternalEditContext())
        out['summary'] = str(r)[:1500]
except Exception:
    out['call_error'] = traceback.format_exc()[-1500:]
json.dump(out, open('/Users/daniellee/SlashRunner/unreal-demo/probe/probe2.json', 'w'), indent=1)
