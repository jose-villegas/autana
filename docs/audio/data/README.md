# Microphone test material

`speech/` holds six clean read-speech references for microphone comparisons
(three female, three male speakers, 5-7 s each, 16 kHz FLAC) with their
transcripts in `speech/transcripts.txt`.

They are unmodified utterances from LibriSpeech dev-clean (OpenSLR SLR12,
<https://www.openslr.org/12>), (c) 2014 Vassil Panayotov, licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Corpus: V. Panayotov,
G. Chen, D. Povey and S. Khudanpur, "LibriSpeech: an ASR corpus based on public
domain audio books", ICASSP 2015.

Generate deterministic synthetic material with:

```sh
python launcher/tools/audio/mic/generate_noise.py pink --seconds 10 --seed 1 -o docs/audio/data/pink.wav
python launcher/tools/audio/mic/generate_noise.py babble --seconds 10 --seed 1 -o docs/audio/data/babble.wav
```

Babble is speech-shaped noise with independent amplitude modulation; it contains
no recorded voices. Store generated WAV files through Git LFS when committing them.

Recorded takes have a sibling `take_metrics.json`, keyed by WAV filename. Each
entry stores the audio SHA-256 and table metrics. Regenerate it with
`python launcher/tools/audio/audio_doc_tables.py --write` and real WAVs present.

Table checks recompute metrics from real audio. With LFS pointers they require
a matching content hash in the metrics file, so CI needs no audio download.
