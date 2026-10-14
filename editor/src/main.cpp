/* Desktop editing of authored UI layouts. */
#define SDL_MAIN_HANDLED

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include "core/dockspace.h"
#include "editor/runtime.h"
#include "layout_document.h"

namespace {

struct Canvas {
    const char* name;
    LayoutOrientation orientation;
    int width;
    int height;

    Canvas(const char* canvas_name, LayoutOrientation canvas_orientation, int canvas_width, int canvas_height)
        : name(canvas_name), orientation(canvas_orientation), width(canvas_width), height(canvas_height) {}
};

enum class DragMode {
    None,
    Move,
    Resize,
};

struct CanvasInteraction {
    DragMode mode = DragMode::None;
    LayoutOrientation orientation = LayoutOrientation::Landscape;
    std::size_t element_index = 0;
    LayoutRect original = {};
    ImVec2 pointer_origin = {};
};

bool
contains(const LayoutRect& rect, float x, float y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

std::optional<std::size_t>
hit_test(const ScreenLayout& layout, float x, float y) {
    for (std::size_t index = layout.rects.size(); index > 0; index--) {
        if (contains(layout.rects[index - 1], x, y)) {
            return index - 1;
        }
    }
    return std::nullopt;
}

// Documents own the editable geometry for each project screen.
struct SystemScreen {
    const char* title;
    std::optional<LayoutDocument> document;
    std::optional<LayoutEditHistory> history;
    std::size_t selected = 0;
};

std::filesystem::path
baked_header_path(const LayoutDocument& document) {
    return document.path().parent_path() / (document.screen() + "_layout_generated.h");
}

bool
bake_layout(const LayoutDocument& document, std::string& error) {
    try {
        const std::string baked = bake_header(document, baked_header_path(document));
        std::ofstream output(baked_header_path(document), std::ios::binary | std::ios::trunc);
        output << baked;
        if (!output) {
            throw std::runtime_error("could not write baked header");
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

void constrain_rect(LayoutRect& rect, const ScreenLayout& layout);

// An empty canvas has no authored geometry to select.
bool
draw_canvas(Canvas& canvas, ScreenLayout* editable, std::size_t& selected, LayoutOrientation& active_orientation,
            CanvasInteraction& interaction, float available_width) {
    ImGui::TextUnformatted(canvas.name);
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d", canvas.width, canvas.height);

    const float scale = std::min(1.0f, available_width / static_cast<float>(canvas.width));
    const ImVec2 size(canvas.width * scale, canvas.height * scale);
    const ImVec2 image_position = ImGui::GetCursorScreenPos();
    const std::string canvas_id = std::string("##canvas-") + layout_orientation_id(canvas.orientation);
    ImGui::InvisibleButton(canvas_id.c_str(), size, ImGuiButtonFlags_MouseButtonLeft);
    ImGui::GetWindowDrawList()->AddRectFilled(
        image_position, ImVec2(image_position.x + size.x, image_position.y + size.y), IM_COL32_BLACK);

    if (!editable) {
        return false;
    }
    ScreenLayout& layout = *editable;

    bool changed = false;
    const ImVec2 pointer = ImGui::GetIO().MousePos;
    const float local_x = (pointer.x - image_position.x) / scale;
    const float local_y = (pointer.y - image_position.y) / scale;
    LayoutRect& selected_rect = layout.rects[selected];
    const ImVec2 selected_maximum(image_position.x + (selected_rect.x + selected_rect.width) * scale,
                                  image_position.y + (selected_rect.y + selected_rect.height) * scale);
    const bool over_resize_handle = pointer.x >= selected_maximum.x - 12.0f && pointer.x <= selected_maximum.x
                                    && pointer.y >= selected_maximum.y - 12.0f && pointer.y <= selected_maximum.y;

    if (ImGui::IsItemHovered()) {
        if (over_resize_handle) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        } else if (contains(selected_rect, local_x, local_y)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const std::optional<std::size_t> hit = hit_test(layout, local_x, local_y);
        if (hit) {
            const bool resize = *hit == selected && over_resize_handle;
            selected = *hit;
            active_orientation = canvas.orientation;
            interaction.mode = resize ? DragMode::Resize : DragMode::Move;
            interaction.orientation = canvas.orientation;
            interaction.element_index = *hit;
            interaction.original = layout.rects[*hit];
            interaction.pointer_origin = pointer;
        }
    }

    if (interaction.mode != DragMode::None && interaction.orientation == canvas.orientation) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            interaction.mode = DragMode::None;
        } else {
            LayoutRect next = interaction.original;
            const int delta_x = static_cast<int>(std::lround((pointer.x - interaction.pointer_origin.x) / scale));
            const int delta_y = static_cast<int>(std::lround((pointer.y - interaction.pointer_origin.y) / scale));
            if (interaction.mode == DragMode::Move) {
                next.x = std::clamp<int>(next.x + delta_x, 0, layout.canvas_width - next.width);
                next.y = std::clamp<int>(next.y + delta_y, 0, layout.canvas_height - next.height);
            } else {
                next.width = std::clamp<int>(next.width + delta_x, 1, layout.canvas_width);
                next.height = std::clamp<int>(next.height + delta_y, 1, layout.canvas_height);
            }
            constrain_rect(next, layout);
            LayoutRect& target = layout.rects[interaction.element_index];
            if (target != next) {
                target = next;
                changed = true;
            }
        }
    }

    const LayoutRect& rect = layout.rects[selected];
    const ImVec2 minimum(image_position.x + rect.x * scale, image_position.y + rect.y * scale);
    const ImVec2 maximum(minimum.x + rect.width * scale, minimum.y + rect.height * scale);
    ImGui::GetWindowDrawList()->AddRect(minimum, maximum, IM_COL32(91, 229, 235, 255), 2.0f, 0, 2.0f);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(maximum.x - 8.0f, maximum.y - 8.0f), maximum,
                                              IM_COL32(91, 229, 235, 255));
    return changed;
}

void
constrain_rect(LayoutRect& rect, const ScreenLayout& layout) {
    rect.width = std::clamp<int>(rect.width, 1, layout.canvas_width);
    rect.height = std::clamp<int>(rect.height, 1, layout.canvas_height);
    rect.x = std::clamp<int>(rect.x, 0, layout.canvas_width - rect.width);
    rect.y = std::clamp<int>(rect.y, 0, layout.canvas_height - rect.height);
}

struct RectEditResult {
    bool changed;
    bool committed;
};

RectEditResult
draw_rect_editor(LayoutRect& rect, const ScreenLayout& layout) {
    bool changed = false;
    bool committed = false;
    changed |= ImGui::DragScalar("X", ImGuiDataType_S16, &rect.x, 1.0f);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::DragScalar("Y", ImGuiDataType_S16, &rect.y, 1.0f);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::DragScalar("Width", ImGuiDataType_S16, &rect.width, 1.0f);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::DragScalar("Height", ImGuiDataType_S16, &rect.height, 1.0f);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    if (changed) {
        constrain_rect(rect, layout);
    }
    return {changed, committed};
}

struct EditorState {
    std::vector<SystemScreen> screens;
    std::size_t active = 0;
    LayoutOrientation orientation = LayoutOrientation::Landscape;
    CanvasInteraction interaction;
    std::string notice;
};

struct MenuRequests {
    bool save = false;
    bool bake = false;
    bool undo = false;
    bool redo = false;
    bool reset_workspace = false;
};

MenuRequests
draw_menu_and_shortcuts(const SystemScreen& screen, bool valid) {
    const bool dirty = screen.document && screen.document->dirty();
    const bool can_undo = screen.history && screen.history->can_undo();
    const bool can_redo = screen.history && screen.history->can_redo();
    MenuRequests requests;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            requests.save = ImGui::MenuItem("Save", "Ctrl+S", false, dirty && valid);
            ImGui::Separator();
            requests.bake =
                ImGui::MenuItem("Bake layout", nullptr, false, screen.document.has_value() && !dirty && valid);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            requests.undo = ImGui::MenuItem("Undo", "Ctrl+Z", false, can_undo);
            requests.redo = ImGui::MenuItem("Redo", "Ctrl+Y", false, can_redo);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            requests.reset_workspace = ImGui::MenuItem("Reset workspace");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && dirty && valid) {
        requests.save = true;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        requests.redo |= io.KeyShift && can_redo;
        requests.undo |= !io.KeyShift && can_undo;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && can_redo) {
        requests.redo = true;
    }
    return requests;
}

void
draw_hierarchy(EditorState& state) {
    ImGui::SetNextWindowSize(ImVec2(220, 540), ImGuiCond_FirstUseEver);
    ImGui::Begin("Hierarchy");
    ImGui::TextDisabled("Screens");
    for (std::size_t index = 0; index < state.screens.size(); index++) {
        if (ImGui::Selectable(state.screens[index].title, state.active == index)) {
            state.active = index;
            state.interaction.mode = DragMode::None;
        }
    }
    ImGui::Separator();

    SystemScreen& screen = state.screens[state.active];
    if (!screen.document) {
        ImGui::TextDisabled("No authored layout");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%s", screen.document->path().filename().string().c_str());
    if (ImGui::TreeNodeEx(screen.title, ImGuiTreeNodeFlags_DefaultOpen)) {
        const std::vector<LayoutElement>& elements = screen.document->elements();
        for (std::size_t index = 0; index < elements.size(); index++) {
            if (ImGui::Selectable(elements[index].label.c_str(), screen.selected == index)) {
                screen.selected = index;
            }
        }
        ImGui::TreePop();
    }
    ImGui::End();
}

bool
draw_canvases(EditorState& state, Canvas& landscape, Canvas& portrait) {
    SystemScreen& screen = state.screens[state.active];
    bool changed = false;

    ImGui::SetNextWindowSize(ImVec2(950, 620), ImGuiCond_FirstUseEver);
    ImGui::Begin("Canvas");
    ImGui::TextDisabled(screen.document ? "Click to select, drag to move, or drag the cyan corner to resize."
                                        : "Canvas only: this screen has no authored layout.");
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    ImGui::BeginChild("Landscape", ImVec2(half, 0), ImGuiChildFlags_Borders);
    changed |= draw_canvas(landscape, screen.document ? &screen.document->layout(landscape.orientation) : nullptr,
                           screen.selected, state.orientation, state.interaction, ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Portrait", ImVec2(0, 0), ImGuiChildFlags_Borders);
    changed |= draw_canvas(portrait, screen.document ? &screen.document->layout(portrait.orientation) : nullptr,
                           screen.selected, state.orientation, state.interaction, ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::End();
    return changed;
}

bool
draw_inspector(EditorState& state, MenuRequests& requests) {
    SystemScreen& screen = state.screens[state.active];
    bool changed = false;

    ImGui::SetNextWindowSize(ImVec2(300, 540), ImGuiCond_FirstUseEver);
    ImGui::Begin("Inspector");
    if (!screen.document) {
        ImGui::TextDisabled("Nothing to inspect.");
        ImGui::End();
        return false;
    }
    LayoutDocument& document = *screen.document;
    const LayoutElement& element = document.elements()[screen.selected];
    ImGui::TextUnformatted(element.label.c_str());
    ImGui::TextDisabled("%s%s", element.id.c_str(), element.interactive ? "  (tap target)" : "");

    ImGui::SeparatorText("Orientation");
    if (ImGui::RadioButton("Landscape", state.orientation == LayoutOrientation::Landscape)) {
        state.orientation = LayoutOrientation::Landscape;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Portrait", state.orientation == LayoutOrientation::Portrait)) {
        state.orientation = LayoutOrientation::Portrait;
    }

    ScreenLayout& layout = document.layout(state.orientation);
    ImGui::TextDisabled("Canvas %d x %d", layout.canvas_width, layout.canvas_height);
    ImGui::SeparatorText("Rectangle");
    const RectEditResult rect_edit = draw_rect_editor(layout.rects[screen.selected], layout);
    if (rect_edit.changed) {
        document.mark_dirty();
        changed = true;
    }
    if (rect_edit.committed) {
        screen.history->commit(document);
    }

    const bool valid = document.validate().empty();
    ImGui::BeginDisabled(!document.dirty() || !valid);
    requests.save |= ImGui::Button("Save source");
    ImGui::EndDisabled();
    if (document.dirty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "Unsaved");
    }
    ImGui::BeginDisabled(document.dirty() || !valid);
    requests.bake |= ImGui::Button("Bake layout");
    ImGui::EndDisabled();
    ImGui::End();
    return changed;
}

void
draw_problems(const EditorState& state) {
    const SystemScreen& screen = state.screens[state.active];
    ImGui::SetNextWindowSize(ImVec2(950, 160), ImGuiCond_FirstUseEver);
    ImGui::Begin("Problems");
    if (screen.document) {
        const std::vector<std::string> problems = screen.document->validate();
        if (problems.empty()) {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.82f, 1.0f), "Layout valid in both orientations.");
        }
        for (const std::string& problem : problems) {
            ImGui::BulletText("%s", problem.c_str());
        }
    }
    if (!state.notice.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", state.notice.c_str());
    }
    ImGui::TextDisabled("Save updates authored JSON; Bake explicitly regenerates project geometry.");
    ImGui::End();
}

void
save_and_bake(EditorState& state, const MenuRequests& requests) {
    SystemScreen& screen = state.screens[state.active];
    if (!screen.document) {
        return;
    }
    std::string error;
    if (requests.save) {
        if (screen.document->save(error)) {
            screen.history->mark_saved(*screen.document);
            state.notice = "Saved " + screen.document->path().string();
        } else {
            state.notice = "Save failed: " + error;
        }
    }
    if (requests.bake) {
        state.notice = bake_layout(*screen.document, error)
                           ? "Baked " + baked_header_path(*screen.document).filename().string()
                           : "Bake failed: " + error;
    }
}

void
draw_editor(EditorState& state, Canvas& landscape, Canvas& portrait) {
    const std::size_t screen_before = state.active;
    SystemScreen* screen = &state.screens[state.active];
    const bool valid = !screen->document || screen->document->validate().empty();
    MenuRequests requests = draw_menu_and_shortcuts(*screen, valid);

    if (requests.undo || requests.redo) {
        state.interaction.mode = DragMode::None;
        if (requests.undo ? screen->history->undo(*screen->document) : screen->history->redo(*screen->document)) {
            state.notice = requests.undo ? "Undo" : "Redo";
        }
    }

    draw_editor_dockspace(requests.reset_workspace);
    draw_hierarchy(state);
    if (state.active != screen_before) {
        screen = &state.screens[state.active];
        requests = {};
    }

    const bool was_canvas_editing = state.interaction.mode != DragMode::None;
    if (draw_canvases(state, landscape, portrait)) {
        screen->document->mark_dirty();
        state.notice.clear();
    }
    if (was_canvas_editing && state.interaction.mode == DragMode::None) {
        screen->history->commit(*screen->document);
    }

    draw_inspector(state, requests);
    save_and_bake(state, requests);
    draw_problems(state);
}

} // namespace

int
main() {
    EditorState state;
    state.screens.push_back({"Layout", std::nullopt, std::nullopt});

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Autana Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1440, 900,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* renderer =
        window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (window && !renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!window || !renderer) {
        std::fprintf(stderr, "SDL window creation failed: %s\n", SDL_GetError());
        if (window) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // One workspace file wherever the editor is launched from; ImGui keeps
    // the pointer, so the string outlives the context.
    const std::string workspace_file = (std::filesystem::path(EDITOR_PROJECT_ROOT) / "editor" / "imgui.ini").string();
    io.IniFilename = workspace_file.c_str();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    Canvas landscape("Landscape", LayoutOrientation::Landscape, editor_runtime_panel_height(),
                     editor_runtime_panel_width());
    Canvas portrait("Portrait", LayoutOrientation::Portrait, editor_runtime_panel_width(),
                    editor_runtime_panel_height());
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT
                || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE
                    && event.window.windowID == SDL_GetWindowID(window))) {
                running = false;
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        draw_editor(state, landscape, portrait);
        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, 18, 18, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
