# Microphone test material

Microphone comparisons expect a clean mono Harvard-sentence reference WAV with
an accompanying source URL and a licence permitting redistribution. Speech
recordings are supplied separately; no speech audio is bundled here.

Generate deterministic synthetic material with:

```sh
python launcher/tools/audio/mic/generate_noise.py pink --seconds 10 --seed 1 -o docs/audio/data/pink.wav
python launcher/tools/audio/mic/generate_noise.py babble --seconds 10 --seed 1 -o docs/audio/data/babble.wav
```

Babble is speech-shaped noise with independent amplitude modulation; it contains
no recorded voices. Store generated WAV files through Git LFS when committing them.
