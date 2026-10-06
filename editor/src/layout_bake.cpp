/* Command-line baking of authored firmware layouts. */
#include "layout_document.h"

#include <fstream>
#include <iostream>

int
main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: editor_layout_bake <json> <header>\n";
        return 2;
    }
    std::string error;
    const auto document = LayoutDocument::load(argv[1], error);
    if (!document) {
        std::cerr << error << '\n';
        return 1;
    }
    const std::string baked = bake_header(*document, argv[2]);
    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    output << baked;
    if (!output) {
        std::cerr << "could not write " << argv[2] << '\n';
        return 1;
    }
    return 0;
}
