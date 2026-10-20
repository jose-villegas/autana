# Tilt

`input/motion/tilt.c` filters the gravity direction with elapsed-time
smoothing. Time constants and magnitude thresholds live in
`input/motion/tilt.h`. The direction estimator that also uses the gyro is
[Gravity.md](Gravity.md); the shell reads tilt today and gravity is not in
`input_t`.

| Rule | Why |
|---|---|
| Use a time constant, not a fixed fraction per frame | frame-rate changes must not change the response |
| Gyro rotation adapts the response: still readings smooth more, turning readings less | steady when held, quick when turned |
| A magnitude far from one g holds the previous direction | the sample is contaminated by linear acceleration |
| A magnitude near one g is not proof of pure gravity; gyro activity does not bypass the gate | rotation also adds linear acceleration |

Shake strength is the accelerometer magnitude's departure from one g, not
angular velocity. Smooth it independently of direction-filter priming: a
shaking sample may be rejected for direction and still need a shake update.

A rotation test must preserve vector magnitude; linear interpolation between
perpendicular unit vectors is not a constant-magnitude rotation. A consumer
that quantizes direction into coarse steps needs its own treatment of that
quantization, because smoothing the input cannot restore discarded angular
resolution.
