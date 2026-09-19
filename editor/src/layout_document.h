#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/edit_history.h"
#include "core/layout.h"

struct LayoutElement {
    std::string id;
    std::string label;
    bool interactive;
};

// An authored <screen>_layout.json. The file declares its own screen and
// elements, so every authored screen is this one type; launcher/tools/
// gen_ui_layout.py applies the same rules when it bakes the header.
class LayoutDocument {
  public:
    static constexpr int schema_version = 2;
    static constexpr int min_tap_target = 44;

    static std::optional<LayoutDocument> load(const std::filesystem::path& path, std::string& error);

    bool save(std::string& error);
    std::string serialize() const;
    std::vector<std::string> validate() const;

    const std::string& screen() const;
    const std::string& title() const;
    const std::vector<LayoutElement>& elements() const;
    std::optional<std::size_t> element_index(const std::string& id) const;

    ScreenLayout& layout(LayoutOrientation orientation);
    const ScreenLayout& layout(LayoutOrientation orientation) const;
    const std::filesystem::path& path() const;

    bool dirty() const;
    void mark_dirty();
    void set_dirty(bool dirty);

  private:
    std::filesystem::path path_;
    std::string screen_;
    std::string title_;
    std::vector<LayoutElement> elements_;
    ScreenLayout portrait_;
    ScreenLayout landscape_;
    bool dirty_ = false;
};

struct LayoutGeometryEqual {
    bool operator()(const LayoutDocument& first, const LayoutDocument& second) const;
};

using LayoutEditHistory = EditHistory<LayoutDocument, LayoutGeometryEqual>;
