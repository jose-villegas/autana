/* Command-line access to the editor's authored layout rules. */
#include "layout_document.h"

#include <iostream>

int
main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: editor_layout_validate document.json\n";
        return 2;
    }
    std::string error;
    if (!LayoutDocument::load(argv[1], error)) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}
