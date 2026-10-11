#include "samples.hpp"
#include <opencv2/v4d/v4d.hpp>

// Dear ImGui's docking is an "internal" API but the canonical one for building
// a default layout programmatically (DockBuilderXXX). V4D vendors the docking
// branch, so the dockspace and the builder are available.
#include "imgui.h"
#include "imgui_internal.h"

using namespace cv;
using namespace cv::v4d;

// A docking layout manager built on Dear ImGui's native docking, which V4D's
// vendored ImGui provides (IMGUI_HAS_DOCK). Every window below is a real,
// dockable ImGui window: drag a tab onto another window's edge to split, onto
// its middle to merge into tabs, or out to float it. The layout is built once
// by the DockBuilder so the demo opens as a docked split view rather than a
// flat tab bar, and ImGui persists it (imgui.ini) across runs.
class DockDemoPlan : public V4DPlan {
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    // Static: the plan machinery only accepts global-storage (shared) variables
    // in RWS(); an instance member would fail checkShared().
    static struct Params {
        float sliderValue = 0.5f;
        int counter = 0;
        bool checkA = true;
        bool checkB = false;
        char textInput[256] = "Hello, docking!";
        const char *items[3] = {"Tab A", "Tab B", "Tab C"};
        // Set from the menu bar; the next frame rebuilds the default layout.
        bool resetLayout = false;
    } params_;

    // The window titles, in one place, because both the DockBuilder and the
    // Begin() calls below have to agree on them exactly.
    static constexpr const char *kSettings = "Settings";
    static constexpr const char *kPreview = "Preview";
    static constexpr const char *kProperties = "Properties";
    static constexpr const char *kConsole = "Console";
    static constexpr const char *kTimeline = "Timeline";

    // Lays the docked windows out: Settings/Properties stacked on the left,
    // Console/Timeline along the bottom, Preview filling the rest. Called
    // before the dockspace is submitted, which is where DockBuilder expects to
    // run.
    static void buildDefaultLayout(ImGuiID dockspace_id, const ImVec2 &size) {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, size);

        ImGuiID center = dockspace_id;
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.25f,
                                                   nullptr, &center);
        ImGuiID bottom =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr,
                                        &center);

        ImGui::DockBuilderDockWindow(kSettings, left);
        ImGui::DockBuilderDockWindow(kProperties, left);
        ImGui::DockBuilderDockWindow(kConsole, bottom);
        ImGui::DockBuilderDockWindow(kTimeline, bottom);
        ImGui::DockBuilderDockWindow(kPreview, center);
        ImGui::DockBuilderFinish(dockspace_id);
    }

  public:
    void gui() override {
        imgui(
            [](Params &p) {
                using namespace ImGui;

                if (BeginMainMenuBar()) {
                    if (BeginMenu("Layout")) {
                        if (MenuItem("Reset docking layout")) p.resetLayout = true;
                        EndMenu();
                    }
                    Text("  drag a tab to dock, or to a window edge to split");
                    EndMainMenuBar();
                }

                ImGuiViewport *vp = GetMainViewport();
                const ImGuiID dockspace_id = ImHashStr("V4DDockSpace");

                static bool layoutBuilt = false;
                if (p.resetLayout || !layoutBuilt) {
                    p.resetLayout = false;
                    layoutBuilt = true;
                    buildDefaultLayout(dockspace_id, vp->WorkSize);
                }
                DockSpaceOverViewport(dockspace_id, vp, ImGuiDockNodeFlags_None);

                if (Begin(kSettings)) {
                    SliderFloat("Value", &p.sliderValue, 0.0f, 1.0f);
                    Checkbox("Enable A", &p.checkA);
                    Checkbox("Enable B", &p.checkB);
                    InputText("Text", p.textInput, IM_ARRAYSIZE(p.textInput));
                }
                End();

                if (Begin(kPreview)) {
                    Text("Preview window");
                    Text("Counter: %d", p.counter);
                    Separator();
                    Text("Dock me anywhere.");
                }
                End();

                if (Begin(kProperties)) {
                    Text("Properties window");
                    Separator();
                    for (int i = 0; i < 3; ++i) {
                        if (TreeNode(p.items[i])) {
                            Text("Item %d properties", i);
                            TreePop();
                        }
                    }
                }
                End();

                if (Begin(kConsole)) {
                    Text("Console output:");
                    Separator();
                    for (int i = 0; i < 20; ++i)
                        Text("[%d] Log message %d", i, i * 42);
                }
                End();

                if (Begin(kTimeline)) {
                    static float timeline = 0.0f;
                    SliderFloat("Time", &timeline, 0.0f, 100.0f);
                    for (int i = 0; i < 5; ++i) {
                        Text("Track %d", i);
                        SameLine();
                        Button("Play");
                    }
                }
                End();
            },
            RWS(params_));
    }

    void infer() override {
        // A plain dark background under the GUI.
        nvg(
            [](const cv::Size &sz) {
                using namespace cv::v4d::nvg;
                clearScreen();
                fillColor(Scalar(30, 30, 30, 255));
                beginPath();
                rect(0, 0, sz.width, sz.height);
                fill();
            },
            size_);
    }
};

DockDemoPlan::Params DockDemoPlan::params_;

V4D_DEMO_MAIN(int argc, char **argv) {
    cv::Rect viewport(0, 0, 1280, 720);
    Ptr<V4D> runtime = V4D::init(viewport, "Dock Demo",
                                 AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    V4DPlan::run<DockDemoPlan>(0);
}
