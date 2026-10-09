"""One instruction set for every ray Mitsuba traces, so a bake's bytes do not depend on the CPU it ran on.

Two parts of Mitsuba's CPU backend choose their code by the host: Dr.Jit compiles its LLVM kernels for the host CPU
(16 lanes and AVX-512 approximations where it has them), and Embree picks its intersection kernels by the host's
instruction set. Either one moves a hit distance or a bounced radiance by an ulp between an AVX-512 host and an
AVX2 one, an ulp moves a rounded colour, and the simplifier then keeps different triangles. `pin()` compiles every
kernel for x86-64-v3 (AVX2 and FMA) at 8 lanes and caps Embree at AVX2, the same code on every x86-64 CPU from
2015 on. It must run before `import mitsuba`; `mitsuba_reference.import_mitsuba` calls it.

The pin needs Linux on x86-64 with AVX2 and FMA, a C compiler (CC, else cc, gcc or clang) and Dr.Jit as
requirements.txt pins it; elsewhere it logs that bakes there may differ from other hosts' and leaves the defaults.
"""
import ctypes
import importlib.util
import os
import pathlib
import platform
import sys

from native.shared_library import build_shared, find_compiler

from .. import log

HERE = pathlib.Path(__file__).resolve().parent
CACHE = HERE.parent / ".cache"
JIT_CPU = "x86-64-v3"
JIT_FEATURES = "+avx2,+fma"
JIT_LANES = 8
EMBREE_CAP = b"max_isa=avx2"
# Dr.Jit's C++ entry points in libdrjit-core, which its Python module does not wrap.
SET_TARGET = "_Z19jit_llvm_set_targetPKcS0_j"
TARGET_CPU = "_Z19jit_llvm_target_cpuv"
TARGET_FEATURES = "_Z24jit_llvm_target_featuresv"
VECTOR_WIDTH = "_Z21jit_llvm_vector_widthv"
# Embree's device constructor as Mitsuba's build names it, which embree_cap.c defines over.
NEW_DEVICE = "_ZN7mitsuba12rtcNewDeviceEPKc"

_state = None


def _jit_core():
    import drjit

    core = ctypes.CDLL(os.path.join(os.path.dirname(drjit.__file__), "libdrjit-core.so"))
    for name, restype in ((TARGET_CPU, ctypes.c_char_p), (TARGET_FEATURES, ctypes.c_char_p),
                          (VECTOR_WIDTH, ctypes.c_uint32)):
        getattr(core, name).restype = restype
    return core


def jit_target():
    """(cpu, lanes) Dr.Jit's LLVM backend compiles for."""
    core = _jit_core()
    return getattr(core, TARGET_CPU)().decode(), getattr(core, VECTOR_WIDTH)()


def embree_config():
    """The configuration of the last Embree device created through the cap, None when the pin is off."""
    return _state.embree_cap_last_config().decode() if _state else None


def pin():
    """Pins Dr.Jit's LLVM target and caps Embree, once per process. Returns whether the pin is on."""
    global _state
    if _state is not None:
        return bool(_state)
    _state = False
    if sys.platform != "linux" or platform.machine() != "x86_64":
        log(f"ray tracing on {sys.platform} {platform.machine()} is not pinned: bakes here may differ from other hosts'")
        return False
    if "mitsuba" in sys.modules:
        raise RuntimeError("r3d.isa.pin() must run before mitsuba is imported")
    core = _jit_core()
    features = (getattr(core, TARGET_FEATURES)() or b"").decode().split(",")
    if not {"+avx2", "+fma"} <= set(features):
        log("this CPU has no AVX2 and FMA: ray tracing is not pinned, and bakes here may differ from other hosts'")
        return False
    mitsuba_dir = pathlib.Path(importlib.util.find_spec("mitsuba").origin).parent
    shim = build_shared("embree_cap", CACHE, (HERE / "embree_cap.c",), find_compiler("CC", ("cc", "gcc", "clang"), "C"),
                        log=log, mode=ctypes.RTLD_GLOBAL)
    shim.embree_cap_last_config.restype = ctypes.c_char_p
    embree = ctypes.CDLL(str(mitsuba_dir / "libembree3.so"))
    shim.embree_cap_bind(ctypes.cast(getattr(embree, NEW_DEVICE), ctypes.c_void_p), EMBREE_CAP)
    _state = shim
    getattr(core, SET_TARGET)(JIT_CPU.encode(), JIT_FEATURES.encode(), ctypes.c_uint32(JIT_LANES))
    return True
