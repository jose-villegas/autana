#!/usr/bin/env python3
"""SCRATCH (5eu5): the test's hit and bounce digests under one ISA configuration (argv[1])."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d import isa  # noqa: E402

CONFIGS = {"sse42": ("x86-64-v2", "+sse4.2", 4, b"max_isa=sse4.2"),
           "avx2": ("x86-64-v3", "+avx2,+fma", 8, b"max_isa=avx2"),
           "avx512": ("x86-64-v4", "+avx512f,+avx512vl,+avx512dq,+avx512bw,+avx2,+fma", 16, b"max_isa=avx512")}
isa.JIT_CPU, isa.JIT_FEATURES, isa.JIT_LANES, isa.EMBREE_CAP = CONFIGS[sys.argv[1]]
from tests import test_r3d_isa as t  # noqa: E402
print(sys.argv[1], t.PINNED, isa.jit_target(), "hits", t.digest(*t.soup_hits())[:16], "embree", isa.embree_config(),
      "bounce", t.digest(t.corridor_bounce())[:16], flush=True)
