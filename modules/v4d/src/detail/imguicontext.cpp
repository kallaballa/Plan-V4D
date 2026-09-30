// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include <algorithm>
#include <vector>
#include "opencv2/v4d/v4d.hpp"
#include "opencv2/v4d/detail/imguicontext.hpp"

#if defined(OPENCV_V4D_USE_ES3)
#   define IMGUI_IMPL_OPENGL_ES3
#endif

#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

namespace cv {
namespace v4d {
namespace detail {
ImGuiContextImpl::ImGuiContextImpl(cv::Ptr<FrameBufferContext> fbContext) :
        mainFbContext_(fbContext) {
	IMGUI_CHECKVERSION();
	// ImGui::CreateContext() makes the new context current, which would break a
	// window that is already set up, so the previous current context is restored
	// when this one is initialized.
	ImGuiContext* prevCtx = ImGui::GetCurrentContext();
	ctx_ = ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
//	io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
//	io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
//	io.BackendUsingLegacyNavInputArray = false;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(mainFbContext_->getGLFWWindow(), false);
//	ImGui_ImplGlfw_SetCallbacksChainForAllWindows(true);
#if !defined(OPENCV_V4D_USE_ES3)
    ImGui_ImplOpenGL3_Init("#version 330");
#else
    ImGui_ImplOpenGL3_Init("#version 300 es");
#endif
    // Creating the context made it current; hand the current context back to
    // whoever had it, so that a second window does not steal it.
    ImGui::SetCurrentContext(prevCtx);
}

ImGuiContextImpl::~ImGuiContextImpl() {
    if(ImGui::GetCurrentContext() == ctx_)
        ImGui::SetCurrentContext(nullptr);
    ImGui::DestroyContext(ctx_);
    ctx_ = nullptr;
}

ImGuiContext* ImGuiContextImpl::getContext() {
	return context_;
}

void ImGuiContextImpl::setContext(ImGuiContext* ctx) {
	context_ = ctx;
}

void ImGuiContextImpl::makeCurrent() {
    if(ctx_ && ImGui::GetCurrentContext() != ctx_)
        ImGui::SetCurrentContext(ctx_);
}

bool ImGuiContextImpl::forwardKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if(!ctx_)
        return false;
    makeCurrent();
    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
    return ImGui::GetCurrentContext() ? ImGui::GetIO().WantCaptureKeyboard : false;
}

bool ImGuiContextImpl::forwardMouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    if(!ctx_)
        return false;
    makeCurrent();
    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
    return ImGui::GetCurrentContext() ? ImGui::GetIO().WantCaptureMouse : false;
}

bool ImGuiContextImpl::forwardScrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    if(!ctx_)
        return false;
    makeCurrent();
    ImGui_ImplGlfw_ScrollCallback(window, xoffset, yoffset);
    return ImGui::GetCurrentContext() ? ImGui::GetIO().WantCaptureMouse : false;
}

bool ImGuiContextImpl::forwardCursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    if(!ctx_)
        return false;
    makeCurrent();
    ImGui_ImplGlfw_CursorPosCallback(window, xpos, ypos);
    return ImGui::GetCurrentContext() ? ImGui::GetIO().WantCaptureMouse : false;
}

bool ImGuiContextImpl::forwardCharCallback(GLFWwindow* window, unsigned int codepoint) {
    if(!ctx_)
        return false;
    makeCurrent();
    ImGui_ImplGlfw_CharCallback(window, codepoint);
    return ImGui::GetCurrentContext() ? ImGui::GetIO().WantCaptureKeyboard : false;
}

void ImGuiContextImpl::setTransaction(cv::Ptr<Transaction> tx) {
    renderCallback_ = tx;
}

int ImGuiContextImpl::execute(const cv::Rect& vp, std::function<void()> fn) {
	CV_UNUSED(fn);
	CV_UNUSED(vp);
	if (ctx_ && GlobalState::get<bool>(GlobalState::Keys::SHOW_GUI)) {
	    // The GUI of this window is drawn from its display thread, so it must be
	    // the current context even if another plan's context is more recent.
	    makeCurrent();
	    ImGui_ImplOpenGL3_NewFrame();
	    ImGui_ImplGlfw_NewFrame();
	    ImGui::NewFrame();

	    bool open_ptr[1] = { true };
		static ImGuiWindowFlags window_flags = 0;
//            window_flags |= ImGuiWindowFlags_NoBackground;
		window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
		window_flags |= ImGuiWindowFlags_NoMove;
		window_flags |= ImGuiWindowFlags_NoScrollWithMouse;
		window_flags |= ImGuiWindowFlags_AlwaysAutoResize;
		window_flags |= ImGuiWindowFlags_NoSavedSettings;
		window_flags |= ImGuiWindowFlags_NoFocusOnAppearing;
		window_flags |= ImGuiWindowFlags_NoNav;
		window_flags |= ImGuiWindowFlags_NoDecoration;
		window_flags |= ImGuiWindowFlags_NoInputs;
                if(GlobalState::get<bool>(GlobalState::Keys::SHOW_FRAME_TIME)) {
			static ImVec2 pos(0, 0);
			ImGui::SetNextWindowPos(pos, ImGuiCond_Once);
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));
			ImGui::Begin("Display", open_ptr, window_flags);
			double fps = GlobalState::get<double>(GlobalState::Keys::FPS);
			size_t workers = GlobalState::get<size_t>(GlobalState::Keys::WORKERS_READY);
			ImGui::Text("%.4f ms/frame (%.1f FPS), workers: %ld", (1000.0f / fps), fps, workers);
			ImGui::End();
		        ImGui::PopStyleColor(1);
		}
		if(GlobalState::get<bool>(GlobalState::Keys::TIME_TRACKER)) {
			struct TableEntry {
				string name;
				double totalAvg;
				double iterAvg;
				long count;
				long last;
			};

			std::vector<TableEntry> entries;
			{
				std::unique_lock lock(TimeTracker::getInstance()->getMap());
				for (const auto& pair : TimeTracker::getInstance()->getMap()) {
					const TimeInfo& ti = pair.second;
					entries.push_back({pair.first,
						ti.totalCnt_ > 0 ? (ti.totalTime_ / 1000.0) / ti.totalCnt_ : 0.0,
						ti.iterCnt_ > 0 ? (ti.iterTime_ / 1000.0) / ti.iterCnt_ : 0.0,
						ti.totalCnt_,
						ti.last_ / 1000.0});
				}
			}

			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));
			ImGui::Begin("Time Tracking", open_ptr,
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse
				| ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
				| ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
				| ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs);

			ImGuiTableFlags table_flags =
				ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg
				| ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable
				| ImGuiTableFlags_SizingFixedFit;

			if (ImGui::BeginTable("time_tracking_table", 5, table_flags)) {
				ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
				ImGui::TableSetupColumn("Avg Total (ms)", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, 0.0f, 1);
				ImGui::TableSetupColumn("Avg Frame (ms)", ImGuiTableColumnFlags_WidthFixed, 0.0f, 2);
				ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 0.0f, 3);
				ImGui::TableSetupColumn("Last (ms)", ImGuiTableColumnFlags_WidthFixed, 0.0f, 4);
				ImGui::TableHeadersRow();

				if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
					if (sort_specs->SpecsDirty && !entries.empty()) {
						sort_specs->SpecsDirty = false;
						for (int s = 0; s < sort_specs->SpecsCount; s++) {
							const ImGuiTableColumnSortSpecs* col_sort = &sort_specs->Specs[s];
							auto comparator = [col_sort](const TableEntry& a, const TableEntry& b) {
								double va = 0.0, vb = 0.0;
								switch (col_sort->ColumnUserID) {
									case 1: va = a.totalAvg; vb = b.totalAvg; break;
									case 2: va = a.iterAvg;  vb = b.iterAvg;  break;
									case 3: va = (double)a.count; vb = (double)b.count; break;
									case 4: va = (double)a.last;  vb = (double)b.last;  break;
									default: va = (double)a.last; vb = (double)b.last; break;
								}
								if (va < vb) return col_sort->SortDirection == ImGuiSortDirection_Ascending;
								if (va > vb) return col_sort->SortDirection == ImGuiSortDirection_Descending;
								return col_sort->SortDirection == ImGuiSortDirection_Ascending;
							};
							std::stable_sort(entries.begin(), entries.end(), comparator);
						}
					}
				}

				for (const auto& e : entries) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(e.name.c_str());
					ImGui::TableNextColumn();
					ImGui::Text("%.4f", e.totalAvg);
					ImGui::TableNextColumn();
					ImGui::Text("%.4f", e.iterAvg);
					ImGui::TableNextColumn();
					ImGui::Text("%ld", e.count);
					ImGui::TableNextColumn();
					ImGui::Text("%.4f", (double)e.last);
				}
				ImGui::EndTable();
			}

			ImGui::End();
			ImGui::PopStyleColor(1);
		}
	    if (renderCallback_)
	        renderCallback_->perform();

	    ImGui::Render();
	    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	}
	return 1;
}
}
}
}
