#include "layout_document.h"

#include <cctype>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::ordered_json;

void
require_exact_keys(const Json& value, std::initializer_list<const char*> keys, const std::string& context) {
    if (!value.is_object() || value.size() != keys.size()) {
        throw std::runtime_error(context + " has unexpected fields");
    }
    for (const char* key : keys) {
        if (!value.contains(key)) {
            throw std::runtime_error(context + " is missing " + key);
        }
    }
}

bool
is_identifier(const std::string& text) {
    if (text.empty() || !std::islower(static_cast<unsigned char>(text.front()))) {
        return false;
    }
    for (char character : text) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (!std::islower(value) && !std::isdigit(value) && character != '_') {
            return false;
        }
    }
    return true;
}

std::string
read_nonempty_string(const Json& value, const std::string& context) {
    if (!value.is_string() || value.get<std::string>().empty()) {
        throw std::runtime_error(context + " must be a non-empty string");
    }
    return value.get<std::string>();
}

std::vector<LayoutElement>
read_elements(const Json& value) {
    if (!value.is_array() || value.empty()) {
        throw std::runtime_error("elements must be a non-empty list");
    }
    std::vector<LayoutElement> elements;
    for (const Json& source : value) {
        require_exact_keys(source, {"id", "label", "interactive"}, "element");
        LayoutElement element = {
            read_nonempty_string(source.at("id"), "element id"),
            read_nonempty_string(source.at("label"), "element label"),
            false,
        };
        if (!is_identifier(element.id)) {
            throw std::runtime_error("element id " + element.id + " is not a lower_snake_case identifier");
        }
        if (!source.at("interactive").is_boolean()) {
            throw std::runtime_error(element.id + ".interactive must be true or false");
        }
        element.interactive = source.at("interactive").get<bool>();
        for (const LayoutElement& earlier : elements) {
            if (earlier.id == element.id) {
                throw std::runtime_error("element id " + element.id + " is repeated");
            }
        }
        elements.push_back(std::move(element));
    }
    return elements;
}

LayoutRect
read_rect(const Json& value) {
    if (!value.is_array() || value.size() != 4) {
        throw std::runtime_error("rectangle must be [x, y, width, height]");
    }
    for (const Json& component : value) {
        if (!component.is_number_integer()) {
            throw std::runtime_error("rectangle components must be integers");
        }
    }
    return {value[0].get<int>(), value[1].get<int>(), value[2].get<int>(), value[3].get<int>()};
}

ScreenLayout
read_layout(const Json& value, const std::vector<LayoutElement>& elements) {
    require_exact_keys(value, {"canvas", "rects"}, "orientation");
    const Json& canvas = value.at("canvas");
    if (!canvas.is_array() || canvas.size() != 2 || !canvas[0].is_number_integer() || !canvas[1].is_number_integer()) {
        throw std::runtime_error("canvas must be [width, height]");
    }

    ScreenLayout layout = {canvas[0].get<int>(), canvas[1].get<int>(), {}};
    const Json& rects = value.at("rects");
    if (!rects.is_object() || rects.size() != elements.size()) {
        throw std::runtime_error("rects must contain exactly the declared element ids");
    }
    for (const LayoutElement& element : elements) {
        if (!rects.contains(element.id)) {
            throw std::runtime_error("rects is missing " + element.id);
        }
        layout.rects.push_back(read_rect(rects.at(element.id)));
    }
    return layout;
}

bool
overlaps(const LayoutRect& first, const LayoutRect& second) {
    return first.x < second.x + second.width && second.x < first.x + first.width && first.y < second.y + second.height
           && second.y < first.y + first.height;
}

void
validate_layout(const ScreenLayout& layout, const std::vector<LayoutElement>& elements, LayoutOrientation orientation,
                std::vector<std::string>& problems) {
    const std::string prefix = std::string(layout_orientation_id(orientation)) + ": ";
    for (std::size_t index = 0; index < elements.size(); index++) {
        const LayoutRect& rect = layout.rects[index];
        if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 || rect.x + rect.width > layout.canvas_width
            || rect.y + rect.height > layout.canvas_height) {
            problems.push_back(prefix + elements[index].id + " leaves the canvas");
        }
        if (elements[index].interactive
            && (rect.width < LayoutDocument::min_tap_target || rect.height < LayoutDocument::min_tap_target)) {
            problems.push_back(prefix + elements[index].id + " is smaller than 44px");
        }
    }
    for (std::size_t first = 0; first < elements.size(); first++) {
        for (std::size_t second = first + 1; second < elements.size(); second++) {
            if (overlaps(layout.rects[first], layout.rects[second])) {
                problems.push_back(prefix + elements[first].id + " overlaps " + elements[second].id);
            }
        }
    }
}

std::string
quoted(const std::string& text) {
    return Json(text).dump();
}

void
write_layout(std::ostringstream& out, const char* name, const ScreenLayout& layout,
             const std::vector<LayoutElement>& elements, bool last) {
    out << "    \"" << name << "\": {\n";
    out << "      \"canvas\": [" << layout.canvas_width << ", " << layout.canvas_height << "],\n";
    out << "      \"rects\": {\n";
    for (std::size_t index = 0; index < elements.size(); index++) {
        const LayoutRect& rect = layout.rects[index];
        out << "        " << quoted(elements[index].id) << ": [" << rect.x << ", " << rect.y << ", " << rect.width
            << ", " << rect.height << "]" << (index + 1 < elements.size() ? "," : "") << "\n";
    }
    out << "      }\n";
    out << "    }" << (last ? "" : ",") << "\n";
}

} // namespace

std::optional<LayoutDocument>
LayoutDocument::load(const std::filesystem::path& path, std::string& error) {
    try {
        std::ifstream stream(path);
        if (!stream) {
            throw std::runtime_error("could not open " + path.string());
        }
        Json source;
        stream >> source;
        require_exact_keys(source, {"schema_version", "screen", "title", "elements", "orientations"}, "document");
        if (source.at("schema_version") != schema_version) {
            throw std::runtime_error("unsupported layout schema_version");
        }

        LayoutDocument document;
        document.path_ = path;
        document.screen_ = read_nonempty_string(source.at("screen"), "screen");
        if (!is_identifier(document.screen_)) {
            throw std::runtime_error("screen must be a lower_snake_case identifier");
        }
        document.title_ = read_nonempty_string(source.at("title"), "title");
        document.elements_ = read_elements(source.at("elements"));

        const Json& orientations = source.at("orientations");
        require_exact_keys(orientations, {"portrait", "landscape"}, "orientations");
        document.portrait_ = read_layout(orientations.at("portrait"), document.elements_);
        document.landscape_ = read_layout(orientations.at("landscape"), document.elements_);
        const std::vector<std::string> problems = document.validate();
        if (!problems.empty()) {
            throw std::runtime_error(problems.front());
        }
        error.clear();
        return document;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

// Written by hand rather than dumped: one rect per line is what keeps an
// edit's diff reviewable, and a generic pretty-printer spreads each rect
// over six lines.
std::string
LayoutDocument::serialize() const {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema_version\": " << schema_version << ",\n";
    out << "  \"screen\": " << quoted(screen_) << ",\n";
    out << "  \"title\": " << quoted(title_) << ",\n";
    out << "  \"elements\": [\n";
    for (std::size_t index = 0; index < elements_.size(); index++) {
        const LayoutElement& element = elements_[index];
        out << "    {\"id\": " << quoted(element.id) << ", \"label\": " << quoted(element.label)
            << ", \"interactive\": " << (element.interactive ? "true" : "false") << "}"
            << (index + 1 < elements_.size() ? "," : "") << "\n";
    }
    out << "  ],\n";
    out << "  \"orientations\": {\n";
    write_layout(out, "portrait", portrait_, elements_, false);
    write_layout(out, "landscape", landscape_, elements_, true);
    out << "  }\n";
    out << "}\n";
    return out.str();
}

bool
LayoutDocument::save(std::string& error) {
    const std::vector<std::string> problems = validate();
    if (!problems.empty()) {
        error = problems.front();
        return false;
    }
    std::ofstream stream(path_, std::ios::trunc | std::ios::binary);
    if (!stream) {
        error = "could not write " + path_.string();
        return false;
    }
    stream << serialize();
    if (!stream) {
        error = "failed while writing " + path_.string();
        return false;
    }
    dirty_ = false;
    error.clear();
    return true;
}

std::vector<std::string>
LayoutDocument::validate() const {
    std::vector<std::string> problems;
    if (portrait_.canvas_width != 368 || portrait_.canvas_height != 448) {
        problems.emplace_back("portrait: canvas must be 368 x 448");
    }
    if (landscape_.canvas_width != 448 || landscape_.canvas_height != 368) {
        problems.emplace_back("landscape: canvas must be 448 x 368");
    }
    validate_layout(portrait_, elements_, LayoutOrientation::Portrait, problems);
    validate_layout(landscape_, elements_, LayoutOrientation::Landscape, problems);
    return problems;
}

const std::string&
LayoutDocument::screen() const {
    return screen_;
}

const std::string&
LayoutDocument::title() const {
    return title_;
}

const std::vector<LayoutElement>&
LayoutDocument::elements() const {
    return elements_;
}

std::optional<std::size_t>
LayoutDocument::element_index(const std::string& id) const {
    for (std::size_t index = 0; index < elements_.size(); index++) {
        if (elements_[index].id == id) {
            return index;
        }
    }
    return std::nullopt;
}

ScreenLayout&
LayoutDocument::layout(LayoutOrientation orientation) {
    return orientation == LayoutOrientation::Landscape ? landscape_ : portrait_;
}

const ScreenLayout&
LayoutDocument::layout(LayoutOrientation orientation) const {
    return orientation == LayoutOrientation::Landscape ? landscape_ : portrait_;
}

const std::filesystem::path&
LayoutDocument::path() const {
    return path_;
}

bool
LayoutDocument::dirty() const {
    return dirty_;
}

void
LayoutDocument::mark_dirty() {
    dirty_ = true;
}

void
LayoutDocument::set_dirty(bool dirty) {
    dirty_ = dirty;
}

bool
LayoutGeometryEqual::operator()(const LayoutDocument& first, const LayoutDocument& second) const {
    for (LayoutOrientation orientation : {LayoutOrientation::Landscape, LayoutOrientation::Portrait}) {
        const ScreenLayout& a = first.layout(orientation);
        const ScreenLayout& b = second.layout(orientation);
        if (a.canvas_width != b.canvas_width || a.canvas_height != b.canvas_height || a.rects != b.rects) {
            return false;
        }
    }
    return true;
}
