/* Command-line baking of authored firmware layouts. */
#include "layout_document.h"

#include <fstream>
#include <iostream>
#include <sstream>

int
main(int argc, char** argv) {
    if (argc != 3 && !(argc == 4 && std::string(argv[3]) == "--check")) {
        std::cerr << "usage: editor_layout_bake <json> <header> [--check]\n";
        return 2;
    }
    std::string error;
    const auto document = LayoutDocument::load(argv[1], error);
    if (!document) {
        std::cerr << error << '\n';
        return 1;
    }
    const std::string baked = bake_header(*document);
    if (argc == 4) {
        std::ifstream input(argv[2], std::ios::binary);
        std::ostringstream bytes;
        bytes << input.rdbuf();
        if (!input || bytes.str() != baked) {
            std::cerr << argv[2] << " differs from the baked layout\n";
            return 1;
        }
    } else {
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
        output << baked;
        if (!output) {
            std::cerr << "could not write " << argv[2] << '\n';
            return 1;
        }
    }
    return 0;
}
