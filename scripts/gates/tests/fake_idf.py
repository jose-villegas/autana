"""A few files laid out as ESP-IDF and its toolchain lay them out, for the
tests of idf_vocabulary.py and the gates that read it."""
import pathlib
from tree import write

TOOLCHAIN_INCLUDE = "tools/xtensa-esp-elf/esp-0.0/xtensa-esp-elf/xtensa-esp-elf/include"



def fake_idf(root):
    """An ESP-IDF checkout under `root`: a HAL header, a Kconfig, a tool."""
    write(root, "components/hal/esp32s3/include/hal/fake_ll.h",
          "/* ghost_ll_function() is only named in this comment */\n"
          "#define FAKE_FREQ_DEFAULT 20000\n"
          "static inline void fake_ll_cal_clock(int hz) { (void)hz; }\n")
    write(root, "components/fatfs/Kconfig",
          "menu \"FAT\"\n    choice FAKE_LFN\n        config FAKE_LFN_NONE\n            bool\n"
          "    endchoice\nendmenu\n")
    write(root, "components/fatfs/CMakeLists.txt", "idf_component_register(FAKE_WHOLE_ARCHIVE)\n")
    write(root, "tools/idf.py", "print('idf')\n")
    write(root, "tools/cmake/version.cmake", "set(IDF_VERSION_MAJOR 5)\n")
    return pathlib.Path(root)


def fake_toolchain(root):
    """An IDF_TOOLS_PATH under `root` whose toolchain's C library has math.h."""
    write(root, TOOLCHAIN_INCLUDE + "/math.h", "double fake_hypot(double, double);\n")
    return pathlib.Path(root)
