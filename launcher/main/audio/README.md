# audio

Sound on the board's codec: blocking mono PCM output and microphone capture, tone synthesis, and the tone-detection maths that checks the speaker can be heard. Callers serialize audio work and keep it off the frame loop.
