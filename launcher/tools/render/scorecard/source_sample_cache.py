"""Share deterministic source-part surface samples between scorecard revisions."""
from contextlib import closing, contextmanager
import json
import sqlite3

import numpy as np

CACHE_WAIT_SECONDS = 60
SAMPLE_CACHE_FORMAT = 1


@contextmanager
def source_sample_cache(folder, source_oid):
    """Yield a sampler keyed by source content, part and the incoming RNG state."""
    if folder is None:
        yield None
        return
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"{source_oid}-{np.__version__}-{SAMPLE_CACHE_FORMAT}.sqlite"
    with closing(sqlite3.connect(path, timeout=CACHE_WAIT_SECONDS)) as db:
        # A compare owns this scratch database and removes it when it exits.
        db.execute("PRAGMA synchronous = OFF")
        db.execute("""CREATE TABLE IF NOT EXISTS samples (
            part BLOB, before TEXT, points BLOB, after TEXT, PRIMARY KEY (part, before))""")

        hits, misses = 0, 0

        def sample(part, rng, generate):
            nonlocal hits, misses
            before = json.dumps(rng.bit_generator.state, sort_keys=True)
            def lookup():
                return db.execute("SELECT points, after FROM samples WHERE part = ? AND before = ?",
                                  (part, before)).fetchone()

            hit = lookup()
            if hit is None:
                with db:
                    db.execute("BEGIN IMMEDIATE")
                    hit = lookup()
                    if hit is None:
                        points = np.asarray(generate(), dtype=np.float64)
                        after = json.dumps(rng.bit_generator.state)
                        db.execute("INSERT INTO samples VALUES (?, ?, ?, ?)", (part, before, points.tobytes(), after))
                        misses += 1
                        return points
            raw, after = hit
            hits += 1
            rng.bit_generator.state = json.loads(after)
            return np.frombuffer(raw, dtype=np.float64).reshape(-1, 3)

        yield sample
        print(f"scorecard source samples: {source_oid} | {hits} reused | {misses} generated", flush=True)
