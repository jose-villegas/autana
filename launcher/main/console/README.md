# console

The serial command line a development build listens on: the reader task, the table of verbs it dispatches to, the lines that read and set tunables, and the frame slots that hand a verb's request to the frame loop.

It sits just above `core/` in [the layers](../../../docs/firmware/Layers.md): a module with verbs includes it, never the other way round. A verb lives in a `console/` folder beside the module it drives, which a development build compiles and a release build leaves out.
