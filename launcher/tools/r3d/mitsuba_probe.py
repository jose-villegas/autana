#!/usr/bin/env python3
"""Whether Mitsuba's CUDA variant traces on this machine: one small render, timed.

    sh launcher/tools/r3d/gpu_python.sh launcher/tools/r3d/mitsuba_probe.py [--variant VARIANT]

Prints the Mitsuba and Dr.Jit versions, the variants the build has, and either the time and mean of a
Cornell box render or the error that stopped it, then exits 0 either way: it answers a question, it does
not gate anything. Installs nothing.
"""

import argparse
import sys
import time

# The render: Mitsuba's own Cornell box, small enough to finish quickly.
SIZE = 128
SAMPLES_PER_PIXEL = 64


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--variant", default="cuda_ad_rgb")
    args = parser.parse_args(argv)
    import drjit as dr
    import mitsuba as mi

    print(f"mitsuba {mi.__version__}, drjit {dr.__version__}, variants {', '.join(mi.variants())}")
    try:
        mi.set_variant(args.variant)
        scene_dict = mi.cornell_box()
        scene_dict["sensor"]["film"]["width"] = scene_dict["sensor"]["film"]["height"] = SIZE
        scene = mi.load_dict(scene_dict)
        start = time.perf_counter()
        image = mi.render(scene, spp=SAMPLES_PER_PIXEL)
        dr.eval(image)
        mean = float(dr.mean(image, axis=None).array[0])
        print(f"{args.variant}: rendered {SIZE}x{SIZE} at {SAMPLES_PER_PIXEL} spp in "
              f"{time.perf_counter() - start:.2f} s, mean {mean:.4f}")
    except Exception as error:  # the answer is the error itself
        print(f"{args.variant}: cannot render: {type(error).__name__}: {error}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
