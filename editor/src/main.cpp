#define SDL_MAIN_HANDLED

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include "core/dockspace.h"
#include "core/rgb565_texture.h"
#include "editor/runtime.h"
#include "layout_document.h"

namespace {

struct Preview {
    const char* name;
    LayoutOrientation orientation;
    Rgb565Texture surface;

    Preview(const char* preview_name, LayoutOrientation preview_orientation, int width, int height)
        : name(preview_name), orientation(preview_orientation), surface(width, height) {}
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

// A screen the firmware draws. One with a document is edited here; one
// without is previewed through the same renderer and nothing more.
struct SystemScreen {
    const char* title;
    editor_screen_t runtime_screen;
    std::optional<LayoutDocument> document;
    std::optional<LayoutEditHistory> history;
    std::size_t selected = 0;
};

bool
render_preview(Preview& preview, const SystemScreen& screen) {
    std::vector<editor_rect_t> rects;
    editor_layout_t authored = {};
    if (screen.document) {
        const ScreenLayout& layout = screen.document->layout(preview.orientation);
        for (const LayoutRect& rect : layout.rects) {
            rects.push_back({rect.x, rect.y, rect.width, rect.height});
        }
        authored = {layout.canvas_width, layout.canvas_height, static_cast<int>(rects.size()), rects.data()};
    }
    std::string error;
    return editor_runtime_render(screen.runtime_screen, screen.document ? &authored : nullptr, preview.surface.pixels(),
                                 preview.surface.width(), preview.surface.height())
           && preview.surface.upload(error);
}

std::filesystem::path
baked_header_path(const LayoutDocument& document) {
    return document.path().parent_path() / (document.screen() + "_layout_generated.h");
}

std::string
shell_argument(const std::filesystem::path& path) {
    const std::string value = path.string();
#ifdef _WIN32
    if (value.find('"') != std::string::npos || value.find('%') != std::string::npos) {
        return {};
    }
    return '"' + value + '"';
#else
    std::string quoted = "'";
    for (char character : value) {
        quoted += character == '\'' ? "'\\''" : std::string(1, character);
    }
    return quoted + "'";
#endif
}

bool
bake_layout(const LayoutDocument& document, bool check_only, std::string& error) {
#ifndef EDITOR_PYTHON_EXECUTABLE
    (void)document;
    (void)check_only;
    error = "Python was not available when the editor was configured";
    return false;
#else
    const std::filesystem::path generator =
        std::filesystem::path(EDITOR_PROJECT_ROOT) / "launcher" / "tools" / "gen_ui_layout.py";
    const std::filesystem::path output = baked_header_path(document);
    const std::string python_argument = shell_argument(EDITOR_PYTHON_EXECUTABLE);
    const std::string generator_argument = shell_argument(generator);
    const std::string source_argument = shell_argument(document.path());
    const std::string output_argument = shell_argument(output);
    if (python_argument.empty() || generator_argument.empty() || source_argument.empty() || output_argument.empty()) {
        error = "A bake path contains unsupported shell characters";
        return false;
    }

    // gen_ui_layout.py stays the only writer of firmware geometry; the
    // editor launches it and never reimplements it.
    std::string command = python_argument + " " + generator_argument + " " + source_argument + " " + output_argument;
    if (check_only) {
        command += " --check";
    }
#ifdef _WIN32
    command = '"' + command + '"';
#endif
    if (std::system(command.c_str()) != 0) {
        error = "Layout generator failed";
        return false;
    }
    error.clear();
    return true;
#endif
}

bool
create_preview(SDL_Renderer* renderer, Preview& preview, const SystemScreen& screen) {
    std::string error;
    if (!preview.surface.create(renderer, error)) {
        return false;
    }
    return render_preview(preview, screen);
}

void constrain_rect(LayoutRect& rect, const ScreenLayout& layout);

// `editable` is null for a screen with no authored layout: the image is
// shown and nothing on it can be selected.
bool
draw_preview(Preview& preview, ScreenLayout* editable, std::size_t& selected, LayoutOrientation& active_orientation,
             CanvasInteraction& interaction, float available_width) {
    ImGui::TextUnformatted(preview.name);
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d", preview.surface.width(), preview.surface.height());

    const float scale = std::min(1.0f, available_width / static_cast<float>(preview.surface.width()));
    const ImVec2 size(preview.surface.width() * scale, preview.surface.height() * scale);
    const ImVec2 image_position = ImGui::GetCursorScreenPos();
    const std::string canvas_id = std::string("##canvas-") + layout_orientation_id(preview.orientation);
    ImGui::InvisibleButton(canvas_id.c_str(), size, ImGuiButtonFlags_MouseButtonLeft);
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(preview.surface.native_handle()), image_position,
                                         ImVec2(image_position.x + size.x, image_position.y + size.y));

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
            active_orientation = preview.orientation;
            interaction.mode = resize ? DragMode::Resize : DragMode::Move;
            interaction.orientation = preview.orientation;
            interaction.element_index = *hit;
            interaction.original = layout.rects[*hit];
            interaction.pointer_origin = pointer;
        }
    }

    if (interaction.mode != DragMode::None && interaction.orientation == preview.orientation) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            interaction.mode = DragMode::None;
        } else {
            LayoutRect next = interaction.original;
            const int delta_x = static_cast<int>(std::lround((pointer.x - interaction.pointer_origin.x) / scale));
            const int delta_y = static_cast<int>(std::lround((pointer.y - interaction.pointer_origin.y) / scale));
            if (interaction.mode == DragMode::Move) {
                next.x += delta_x;
                next.y += delta_y;
            } else {
                next.width += delta_x;
                next.height += delta_y;
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
    rect.width = std::clamp(rect.width, 1, layout.canvas_width);
    rect.height = std::clamp(rect.height, 1, layout.canvas_height);
    rect.x = std::clamp(rect.x, 0, layout.canvas_width - rect.width);
    rect.y = std::clamp(rect.y, 0, layout.canvas_height - rect.height);
}

struct RectEditResult {
    bool changed;
    bool committed;
};

RectEditResult
draw_rect_editor(LayoutRect& rect, const ScreenLayout& layout) {
    bool changed = false;
    bool committed = false;
    changed |= ImGui::DragInt("X", &rect.x, 1.0f);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::DragInt("Y", &rect.y, 1.0f);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::DragInt("Width", &rect.width, 1.0f);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::DragInt("Height", &rect.height, 1.0f);
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
                ImGui::MenuItem("Bake firmware layout", nullptr, false, screen.document.has_value() && !dirty && valid);
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
    ImGui::TextDisabled("System screens");
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
draw_previews(EditorState& state, Preview& landscape, Preview& portrait) {
    SystemScreen& screen = state.screens[state.active];
    bool changed = false;

    ImGui::SetNextWindowSize(ImVec2(950, 620), ImGuiCond_FirstUseEver);
    ImGui::Begin("Preview");
    ImGui::TextDisabled(screen.document ? "Click to select, drag to move, or drag the cyan corner to resize."
                                        : "Preview only: this screen has no authored layout.");
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    ImGui::BeginChild("Landscape", ImVec2(half, 0), ImGuiChildFlags_Borders);
    changed |= draw_preview(landscape, screen.document ? &screen.document->layout(landscape.orientation) : nullptr,
                            screen.selected, state.orientation, state.interaction, ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Portrait", ImVec2(0, 0), ImGuiChildFlags_Borders);
    changed |= draw_preview(portrait, screen.document ? &screen.document->layout(portrait.orientation) : nullptr,
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
    requests.bake |= ImGui::Button("Bake firmware layout");
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
    ImGui::TextDisabled("Save updates authored JSON; Bake explicitly regenerates firmware geometry.");
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
        state.notice = bake_layout(*screen.document, false, error)
                           ? "Baked " + baked_header_path(*screen.document).filename().string()
                           : "Bake failed: " + error;
    }
}

// Returns true when the previews no longer show the active screen's state.
bool
draw_editor(EditorState& state, Preview& landscape, Preview& portrait) {
    const std::size_t screen_before = state.active;
    SystemScreen* screen = &state.screens[state.active];
    const bool valid = !screen->document || screen->document->validate().empty();
    MenuRequests requests = draw_menu_and_shortcuts(*screen, valid);
    bool preview_changed = false;

    if (requests.undo || requests.redo) {
        state.interaction.mode = DragMode::None;
        if (requests.undo ? screen->history->undo(*screen->document) : screen->history->redo(*screen->document)) {
            preview_changed = true;
            state.notice = requests.undo ? "Undo" : "Redo";
        }
    }

    draw_editor_dockspace(requests.reset_workspace);
    draw_hierarchy(state);
    if (state.active != screen_before) {
        screen = &state.screens[state.active];
        requests = {};
        preview_changed = true;
    }

    const bool was_canvas_editing = state.interaction.mode != DragMode::None;
    if (draw_previews(state, landscape, portrait)) {
        screen->document->mark_dirty();
        preview_changed = true;
        state.notice.clear();
    }
    if (was_canvas_editing && state.interaction.mode == DragMode::None) {
        screen->history->commit(*screen->document);
    }

    preview_changed |= draw_inspector(state, requests);
    save_and_bake(state, requests);
    draw_problems(state);
    return preview_changed;
}

std::optional<EditorState>
load_system_screens(std::string& error) {
    const std::filesystem::path ui_directory = std::filesystem::path(EDITOR_PROJECT_ROOT) / "launcher" / "main" / "ui";
    std::optional<LayoutDocument> control_center =
        LayoutDocument::load(ui_directory / "control_center_layout.json", error);
    if (!control_center) {
        return std::nullopt;
    }
    if (static_cast<int>(control_center->elements().size())
        != editor_runtime_element_count(EDITOR_SCREEN_CONTROL_CENTER)) {
        error = "control_center_layout.json and the baked header disagree on the element count; rebake";
        return std::nullopt;
    }

    EditorState state;
    state.screens.push_back({"Launcher", EDITOR_SCREEN_LAUNCHER, std::nullopt, std::nullopt});
    state.screens.push_back({"Control Center", EDITOR_SCREEN_CONTROL_CENTER, std::nullopt, std::nullopt});
    SystemScreen& authored = state.screens.back();
    authored.history.emplace(*control_center);
    authored.document = std::move(control_center);
    state.active = state.screens.size() - 1;
    return state;
}

int
check_bakes(const EditorState& state) {
    for (const SystemScreen& screen : state.screens) {
        std::string error;
        if (screen.document && !bake_layout(*screen.document, true, error)) {
            std::fprintf(stderr, "%s bake check failed: %s\n", screen.title, error.c_str());
            return 1;
        }
    }
    return 0;
}

} // namespace

int
main(int argument_count, char** arguments) {
    std::string load_error;
    std::optional<EditorState> loaded = load_system_screens(load_error);
    if (!loaded) {
        std::fprintf(stderr, "System screens failed to load: %s\n", load_error.c_str());
        return 1;
    }
    EditorState state = std::move(*loaded);

    if (argument_count == 2 && std::string(arguments[1]) == "--check-bake") {
        return check_bakes(state);
    }

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

    if (!editor_runtime_init()) {
        std::fprintf(stderr, "Firmware runtime initialization failed\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
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

    Preview landscape("Landscape", LayoutOrientation::Landscape, 448, 368);
    Preview portrait("Portrait", LayoutOrientation::Portrait, 368, 448);
    if (!create_preview(renderer, landscape, state.screens[state.active])
        || !create_preview(renderer, portrait, state.screens[state.active])) {
        std::fprintf(stderr, "Preview texture creation failed: %s\n", SDL_GetError());
        portrait.surface.reset();
        landscape.surface.reset();
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

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
        if (draw_editor(state, landscape, portrait)) {
            const SystemScreen& screen = state.screens[state.active];
            if (!render_preview(landscape, screen) || !render_preview(portrait, screen)) {
                state.notice = std::string("Preview update failed: ") + SDL_GetError();
            }
        }
        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, 18, 18, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    portrait.surface.reset();
    landscape.surface.reset();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
