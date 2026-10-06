/* Authored layout validation, persistence and history tests. */
#include "layout_document.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "editor/runtime.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace {

using testing::Contains;
using testing::HasSubstr;

std::string
read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << stream.rdbuf();
    return bytes.str();
}

class TemporaryLayout {
  public:
    TemporaryLayout() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("layout-document-" + std::to_string(suffix) + ".json");
        std::filesystem::copy_file(EDITOR_TEST_LAYOUT_PATH, path_, std::filesystem::copy_options::overwrite_existing);
    }

    ~TemporaryLayout() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path&
    path() const {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

LayoutDocument
load_layout() {
    std::string error;
    std::optional<LayoutDocument> document = LayoutDocument::load(EDITOR_TEST_LAYOUT_PATH, error);
    EXPECT_TRUE(document.has_value()) << error;
    return std::move(document.value());
}

LayoutRect&
rect_of(LayoutDocument& document, LayoutOrientation orientation, const char* id) {
    return document.layout(orientation).rects[document.element_index(id).value()];
}

TEST(LayoutDocument, LoadsScreenElementsAndBothOrientations) {
    const LayoutDocument document = load_layout();

    EXPECT_TRUE(document.validate().empty());
    EXPECT_EQ(document.screen(), "control_center");
    EXPECT_EQ(document.title(), "Control Center");
    EXPECT_EQ(document.layout(LayoutOrientation::Landscape).canvas_width, editor_runtime_panel_height());
    EXPECT_EQ(document.layout(LayoutOrientation::Portrait).canvas_height, editor_runtime_panel_height());
    ASSERT_EQ(document.elements().size(), document.layout(LayoutOrientation::Portrait).rects.size());
    EXPECT_EQ(document.elements().front().id, "wifi");
    EXPECT_EQ(document.elements().front().label, "Wi-Fi");
    EXPECT_FALSE(document.element_index("absent").has_value());
}

TEST(LayoutDocument, SerializesTheCheckedInSourceByteForByte) {
    const LayoutDocument document = load_layout();

    EXPECT_EQ(document.serialize(), read_bytes(EDITOR_TEST_LAYOUT_PATH));
}

TEST(LayoutDocument, RejectsPanelOverlap) {
    LayoutDocument document = load_layout();
    rect_of(document, LayoutOrientation::Landscape, "bluetooth") =
        rect_of(document, LayoutOrientation::Landscape, "wifi");

    EXPECT_THAT(document.validate(), Contains("landscape: wifi overlaps bluetooth"));
}

TEST(LayoutDocument, RejectsUndersizedInteractiveTarget) {
    LayoutDocument document = load_layout();
    rect_of(document, LayoutOrientation::Portrait, "volume").height = editor_runtime_tap_min() - 1;

    EXPECT_THAT(document.validate(), Contains("portrait: volume is smaller than "
                                              + std::to_string(editor_runtime_tap_min()) + "px tap target"));
}

TEST(LayoutDocument, AllowsUndersizedPassiveElement) {
    LayoutDocument document = load_layout();

    EXPECT_LT(rect_of(document, LayoutOrientation::Portrait, "notifications_header").height, editor_runtime_tap_min());
    EXPECT_TRUE(document.validate().empty());
}

TEST(LayoutDocument, RejectsOutOfBoundsElement) {
    LayoutDocument document = load_layout();
    rect_of(document, LayoutOrientation::Portrait, "notifications_header").x = -1;

    EXPECT_THAT(document.validate(), Contains("portrait: notifications_header leaves the canvas"));
}

TEST(LayoutDocument, ReportsMissingSource) {
    std::string error;
    EXPECT_FALSE(LayoutDocument::load("missing-layout.json", error));
    EXPECT_THAT(error, HasSubstr("could not open"));
}

TEST(LayoutDocument, RoundTripsEditedGeometry) {
    TemporaryLayout source;
    std::string error;
    std::optional<LayoutDocument> loaded = LayoutDocument::load(source.path(), error);
    ASSERT_TRUE(loaded.has_value()) << error;
    LayoutDocument document = std::move(*loaded);
    rect_of(document, LayoutOrientation::Landscape, "wifi").x++;
    document.mark_dirty();

    ASSERT_TRUE(document.dirty());
    ASSERT_TRUE(document.save(error)) << error;
    EXPECT_FALSE(document.dirty());
    EXPECT_TRUE(error.empty());
    loaded = LayoutDocument::load(source.path(), error);
    ASSERT_TRUE(loaded.has_value()) << error;
    EXPECT_EQ(rect_of(*loaded, LayoutOrientation::Landscape, "wifi"),
              rect_of(document, LayoutOrientation::Landscape, "wifi"));
    EXPECT_EQ(read_bytes(source.path()), document.serialize());
}

TEST(LayoutDocument, RefusesToSaveInvalidGeometry) {
    LayoutDocument document = load_layout();
    document.layout(LayoutOrientation::Portrait).canvas_height = 1;

    std::string error;
    EXPECT_FALSE(document.save(error));
    EXPECT_THAT(error, HasSubstr("canvas must be " + std::to_string(editor_runtime_panel_width()) + " x "
                                 + std::to_string(editor_runtime_panel_height())));
}

TEST(LayoutDocument, RejectsSharedBadDocuments) {
    std::size_t count = 0;
    for (const auto& fixture : std::filesystem::directory_iterator(EDITOR_TEST_FIXTURE_DIR)) {
        if (fixture.path().extension() != ".json") {
            continue;
        }
        SCOPED_TRACE(fixture.path().filename().string());
        const auto source = nlohmann::json::parse(read_bytes(fixture.path()));
        TemporaryLayout document;
        {
            std::ofstream stream(document.path(), std::ios::binary | std::ios::trunc);
            stream << source.at("document").dump();
        }
        std::string error;
        EXPECT_FALSE(LayoutDocument::load(document.path(), error));
        EXPECT_THAT(error, HasSubstr(source.at("error").get<std::string>()));
        count++;
    }
    EXPECT_GT(count, 0u);
}

TEST(LayoutDocument, RefusesWrongRectCount) {
    LayoutDocument document = load_layout();
    document.layout(LayoutOrientation::Portrait).rects.pop_back();
    EXPECT_THAT(document.validate(), Contains("portrait: rects must contain exactly the declared element ids"));
}

TEST(LayoutDocument, BakesEditedGeometryAndRefusesInvalidGeometry) {
    LayoutDocument document = load_layout();
    rect_of(document, LayoutOrientation::Portrait, "wifi").x++;
    const std::filesystem::path header_path = "custom folder/output.h";
    const std::string baked = bake_header(document, header_path);
    EXPECT_THAT(baked, HasSubstr("[CONTROL_CENTER_ELEMENT_WIFI] = {17,"));
    EXPECT_THAT(baked, HasSubstr(document.path().generic_string()));
    EXPECT_THAT(baked, HasSubstr("\"custom folder/output.h\""));
    document.layout(LayoutOrientation::Portrait).rects.pop_back();
    EXPECT_THROW(bake_header(document, header_path), std::runtime_error);
}

TEST(LayoutDocument, HistoryTracksGeometryAndSavedRevision) {
    LayoutDocument document = load_layout();
    LayoutEditHistory history(document);
    rect_of(document, LayoutOrientation::Portrait, "brightness").x--;
    history.commit(document);

    EXPECT_TRUE(document.dirty());
    ASSERT_TRUE(history.undo(document));
    EXPECT_FALSE(document.dirty());
    ASSERT_TRUE(history.redo(document));
    history.mark_saved(document);
    EXPECT_FALSE(document.dirty());
}

} // namespace
