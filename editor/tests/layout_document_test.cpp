#include "layout_document.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

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

    nlohmann::ordered_json
    read() const {
        std::ifstream stream(path_);
        return nlohmann::ordered_json::parse(stream);
    }

    void
    write(const nlohmann::ordered_json& source) const {
        std::ofstream stream(path_, std::ios::trunc);
        stream << source.dump(2) << '\n';
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

std::string
load_error(const TemporaryLayout& source) {
    std::string error;
    EXPECT_FALSE(LayoutDocument::load(source.path(), error));
    return error;
}

TEST(LayoutDocument, LoadsScreenElementsAndBothOrientations) {
    const LayoutDocument document = load_layout();

    EXPECT_TRUE(document.validate().empty());
    EXPECT_EQ(document.screen(), "control_center");
    EXPECT_EQ(document.title(), "Control Center");
    EXPECT_EQ(document.layout(LayoutOrientation::Landscape).canvas_width, 448);
    EXPECT_EQ(document.layout(LayoutOrientation::Portrait).canvas_height, 448);
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
    rect_of(document, LayoutOrientation::Portrait, "volume").height = 55;

    EXPECT_THAT(document.validate(), Contains("portrait: volume is smaller than 44px"));
}

TEST(LayoutDocument, AllowsUndersizedPassiveElement) {
    LayoutDocument document = load_layout();

    EXPECT_LT(rect_of(document, LayoutOrientation::Portrait, "notifications_header").height, 56);
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
    EXPECT_THAT(error, HasSubstr("canvas must be 368 x 448"));
}

TEST(LayoutDocument, RejectsUnexpectedFieldsAndOldSchema) {
    TemporaryLayout source;
    nlohmann::ordered_json json = source.read();
    json["unexpected"] = true;
    source.write(json);
    EXPECT_THAT(load_error(source), HasSubstr("unexpected fields"));

    json.erase("unexpected");
    json["schema_version"] = 1;
    source.write(json);
    EXPECT_THAT(load_error(source), HasSubstr("schema_version"));
}

TEST(LayoutDocument, RejectsMalformedElements) {
    TemporaryLayout source;
    const nlohmann::ordered_json original = source.read();

    nlohmann::ordered_json json = original;
    json["elements"][0]["id"] = "Wi-Fi";
    source.write(json);
    EXPECT_THAT(load_error(source), HasSubstr("lower_snake_case"));

    json = original;
    json["elements"][1]["id"] = "wifi";
    source.write(json);
    EXPECT_THAT(load_error(source), HasSubstr("repeated"));

    json = original;
    json["elements"][0]["interactive"] = "yes";
    source.write(json);
    EXPECT_THAT(load_error(source), HasSubstr("true or false"));

    json = original;
    json["elements"] = nlohmann::ordered_json::array();
    source.write(json);
    EXPECT_THAT(load_error(source), HasSubstr("non-empty list"));

    json = original;
    json["screen"] = "Control Center";
    source.write(json);
    EXPECT_THAT(load_error(source), HasSubstr("screen must be"));
}

TEST(LayoutDocument, RejectsMismatchedAndMalformedRectangles) {
    TemporaryLayout source;
    const nlohmann::ordered_json original = source.read();

    nlohmann::ordered_json json = original;
    json["orientations"]["portrait"]["rects"].erase("wifi");
    json["orientations"]["portrait"]["rects"]["stray"] = {1, 2, 3, 4};
    source.write(json);
    EXPECT_THAT(load_error(source), HasSubstr("rects is missing wifi"));

    json = original;
    json["orientations"]["portrait"]["rects"]["wifi"] = {1, 2, 3};
    source.write(json);
    EXPECT_THAT(load_error(source), HasSubstr("rectangle must be"));

    json["orientations"]["portrait"]["rects"]["wifi"] = {1, 2, "wide", 4};
    source.write(json);
    EXPECT_THAT(load_error(source), HasSubstr("components must be integers"));

    json = original;
    json["orientations"]["landscape"]["canvas"] = {448};
    source.write(json);
    EXPECT_THAT(load_error(source), HasSubstr("canvas must be"));
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
