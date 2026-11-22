# Work checkpoints

A journaled session keeps checkpoints of its work in `.slash/work/checkpoints/`: the files it
wrote, the world and a frame, after each `scene.save`, each `test.run` with no failures and each
`work.checkpoint` ([watching agents](../../docs/reference/watching-agents.md#checkpoints)). A
person sees how the agent got where it is, compares two points and goes back to one.

In [main.cpp](main.cpp) an agent's session saves a scene with a crate, moves the crate and marks
a checkpoint, then adds a lamp and saves it in a scene of its own. A reviewer's session lists the
checkpoints with `work.checkpoints` and compares the first with the last with `work.compare`.
Putting the files back is refused while a session runs; once none does, the files go back to the
first checkpoint, as `slash-cli work restore 1` does, the lamp's scene removed, and back again
from the checkpoint the restore kept first. The journal runs on a fixed clock, so
[expected.txt](expected.txt) repeats.

From the command line:

```bash
slash-cli work checkpoints            # the timeline
slash-cli work compare 1 4            # what changed from checkpoint 1 to 4
slash-cli work restore 1              # the files back as they were at checkpoint 1
slash-cli work video --out work.ivf   # the frames as a video
```
