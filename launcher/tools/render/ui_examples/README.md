# Render/UI examples

One minimal face example per control, each shown whole on its page in
`docs/ui/`. `<name>.c` defines `<name>_example_t`, the values the example
keeps between frames, and `bool <name>_example(<name>_example_t*)`, which
draws with face verbs alone and returns what its control returned. The
examples scene wraps each in `ui_begin()`, `ui_screen()` and `ui_end()`,
starts its state zeroed, and fails to build when either name is missing.

`screens/<name>.c` is the same for a whole screen, drawn at rest only.

The folder is the list: [ui_examples_render_host.sh](../scenes/ui_examples_render_host.sh)
renders every file in it, and [ui_examples_doc.py](../ui_examples_doc.py)
puts each into the document that owns its `ui-<name>-example` block.
