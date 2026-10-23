# Debug

Intervention only: it suspends or steps the frame loop, or overrides sensors
below the input layer, with a matching resume or release. Hidden from bare
`autana help`; `autana help debug` lists it.

<!-- generated: cli-debug check: python scripts/autana/cli_reference.py --check -->
| Command | What it does |
|---|---|
| `autana debug freeze` | stop the frame loop where it is |
| `autana debug resume` | run it again |
| `autana debug step [N]` | advance N frames while frozen; 1 when omitted |
| `autana debug touch <down\|up> <x> <y>` | one raw touch level at a panel-frame point (a --framebuffer pixel); up hands back to the controller |
| `autana debug imu <ax> <ay> <az>` | raw accelerometer counts; imu quarter <q> turns the screen to display quarter q, imu release hands back to the sensor |
<!-- /generated: cli-debug -->

`freeze`, `resume`, `step`, `touch`, `imu` and `debug framewatch` are accepted
as aliases and print the canonical spelling once.
