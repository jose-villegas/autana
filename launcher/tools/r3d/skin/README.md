# Skin frame comparison

`skin_frame.py MODEL.glb --poses CLIP:MS ... --vertices I,J,... --out DIR`
builds a scratch pack with the entry writers and runs `skin_host.c`
through the scene entity APIs. It uses the compiler selected by `track_host`,
the glTF reference skinner and the preview and comparison renderers.
It requires the mesh baker's dependencies and Pillow; it runs no board code.

`--scale`, `--size` and `--view` select quantization and the common camera.
The camera frames the bind pose once. Output includes a reference sheet,
two-versus-four influence sheet, requested vertex crops and measurements JSON.
C positions are checked against the packed-weight reference before writing
sheets. The full glTF reference uses source positions and weights, so those
panels also expose quantization and discarded-influence error. Colours are
held at bind values for every render.
