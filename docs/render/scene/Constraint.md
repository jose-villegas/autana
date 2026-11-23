# Scene constraints

![The owner follows an earlier target pose and faces a target sampled at the current time.](figures/constraint.svg)

A constraint writes the owner's local transform. It starts from the owner's
timeline keys and uses the authored local for unkeyed fields. Weight blends
the two poses while preserving the owner's scale.

Target sampling includes every ancestor. Unkeyed target fields use their current locals.

| Input | Sample |
|---|---|
| Follow | Subtract `lag_ms` from `t`, then wrap in a looping clip or clamp at zero. Apply the offset in the [follow frame](../../kinematics/Rig.md#follow-position). |
| Look-at | Sample at `t` and transform `look_offset` through the target's world transform. Compute the [look rotation](../../kinematics/Rig.md#look-rotation) from the follow position. |
| Unset | Use the owner's position for follow or its rotation for look-at. |

A changed target starts a smooth hand-off with the `handoff_ms` sampled
at that key. Repeated targets keep the active hand-off. Before the first
key the scene uses the row's default target.

During an interrupted hand-off, the outgoing pose is the preceding
hand-off evaluated at the current time. A completed hand-off supplies
its target directly. Seek and playback yield the same pose at the same time.

Constraints apply after the timeline samples and when a target path is
placed. Without a timeline they apply at open. If a parent scale axis
is effectively zero, the owner keeps its input local for that frame.

See [Scene Files](Scene-Files.md#constraint-and-hinge) for authoring.
