// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "imgui.h"
#include <opencv2/plan/util.hpp>
#include "opencv2/v4d/detail/timetrackerwidget.hpp"

using std::string;
using std::vector;

namespace {

// The columns of the table, in declaration order. The enum value doubles as the
// sort key of a column, so it has to stay in sync with the TableSetupColumn()
// calls below.
enum Column {
	Name = 0,
	AvgPerCall,
	AvgPerFrame,
	Share,
	Last,
	Peak,
	Calls,
	ColumnCount
};

// What one row of the table shows, in milliseconds - except for the share, which
// is a fraction of the visible per-frame time, and for the call count.
struct Row {
	string name_;
	double avgPerCall_ = 0.0;
	double avgPerFrame_ = 0.0;
	double share_ = 0.0;
	double last_ = 0.0;
	double peak_ = 0.0;
	long calls_ = 0;
};

const char* const kColumnTooltips[ColumnCount] = {
	"The tracked section. Click to sort by name, shift-click to add a column to the sort.",
	"Average duration of a single call since the last reset - what this section costs on average.",
	"Average duration of a single call in the frame being displayed - what it costs right now.",
	"Share of the per-frame time of all visible sections. Sections that dominate a frame are tinted.",
	"Duration of the most recent call, in milliseconds.",
	"Slowest single call since the last reset, i.e. the worst jitter of this section.",
	"How often this section has been called since the last reset.",
};

// Case-insensitive substring test for the filter field.
bool containsIgnoreCase(const string& haystack, const char* needle) {
	const size_t needleLength = strlen(needle);
	if(needleLength == 0)
		return true;
	auto it = std::search(haystack.begin(), haystack.end(), needle, needle + needleLength,
		[](char lhs, char rhs) {
			return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
		});
	return it != haystack.end();
}

int compare(double lhs, double rhs) {
	return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

// Orders two rows by one column. A negative result means `lhs` comes first.
int compareRows(const Row& lhs, const Row& rhs, int column) {
	switch(column) {
		case Name: return lhs.name_.compare(rhs.name_);
		case AvgPerCall: return compare(lhs.avgPerCall_, rhs.avgPerCall_);
		case AvgPerFrame: return compare(lhs.avgPerFrame_, rhs.avgPerFrame_);
		case Share: return compare(lhs.share_, rhs.share_);
		case Last: return compare(lhs.last_, rhs.last_);
		case Peak: return compare(lhs.peak_, rhs.peak_);
		case Calls: return compare(static_cast<double>(lhs.calls_), static_cast<double>(rhs.calls_));
		default: return 0;
	}
}

// Sorts by every sort spec in turn, the first spec being the primary key. The
// comparator is a strict weak ordering - it reports ties instead of guessing a
// direction - so sections that compare equal keep the order of the snapshot,
// which is alphabetical.
void sortRows(vector<Row>& rows, const ImGuiTableSortSpecs* specs) {
	if(specs == nullptr || specs->SpecsCount <= 0)
		return;

	std::stable_sort(rows.begin(), rows.end(), [specs](const Row& lhs, const Row& rhs) {
		for(int i = 0; i < specs->SpecsCount; ++i) {
			const ImGuiTableColumnSortSpecs& spec = specs->Specs[i];
			const int cmp = compareRows(lhs, rhs, spec.ColumnIndex);
			if(cmp != 0)
				return spec.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0 : cmp > 0;
		}
		return false;
	});
}

// Text at the right edge of its cell, so that the numbers line up like a column
// of figures instead of being ragged on the left.
void textRight(const char* text) {
	const ImVec2 size = ImGui::CalcTextSize(text);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - size.x);
	ImGui::TextUnformatted(text);
}

void textRightMs(double milliseconds) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%.3f", milliseconds);
	textRight(buf);
}

void textRightCount(long count) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%ld", count);
	textRight(buf);
}

}

TimeTrackerWidget::TimeTrackerWidget() :
		tableId_(string("TimeTracker") + std::to_string(reinterpret_cast<uintptr_t>(this))) {
}

bool TimeTrackerWidget::draw(TimeTracker& tracker) {
	if(!tracker.isEnabled())
		return true;

	// The workers keep writing into the tracker while the widget is drawn, so
	// the numbers come from a snapshot instead of from the live map.
	const vector<std::pair<string, TimeInfo>> entries = tracker.snapshot();

	vector<Row> rows;
	rows.reserve(entries.size());
	double perFrameTotal = 0.0;
	long callsTotal = 0;
	for(const auto& entry : entries) {
		if(!containsIgnoreCase(entry.first, filter_))
			continue;
		const TimeInfo& info = entry.second;
		Row row;
		row.name_ = entry.first;
		row.avgPerCall_ = info.avgTotal() / 1000.0;
		row.avgPerFrame_ = info.avgIter() / 1000.0;
		row.last_ = static_cast<double>(info.last_) / 1000.0;
		row.peak_ = static_cast<double>(info.max_) / 1000.0;
		row.calls_ = info.totalCnt_;
		rows.push_back(std::move(row));
	}
	for(const auto& row : rows) {
		perFrameTotal += row.avgPerFrame_;
		callsTotal += row.calls_;
	}
	for(auto& row : rows)
		row.share_ = perFrameTotal > 0.0 ? row.avgPerFrame_ / perFrameTotal : 0.0;

	ImGui::SetNextWindowPos(ImVec2(24.0f, 60.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(680.0f, 320.0f), ImGuiCond_FirstUseEver);
	bool open = true;
	const bool shown = ImGui::Begin("Time Tracking", &open,
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus);
	if(shown) {
		const float resetWidth = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::SetNextItemWidth(std::max(80.0f,
				ImGui::GetContentRegionAvail().x - resetWidth - ImGui::GetStyle().ItemSpacing.x));
		ImGui::InputTextWithHint("##filter", "filter", filter_, sizeof(filter_));
		ImGui::SameLine();
		if(ImGui::Button("Reset"))
			tracker.reset();

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 2.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 2.0f));

		const ImGuiTableFlags tableFlags = ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti
				| ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable
				| ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
		if(ImGui::BeginTable(tableId_.c_str(), ColumnCount, tableFlags)) {
			ImGui::TableSetupScrollFreeze(1, 1);
			ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthStretch, 1.0f, Name);
			ImGui::TableSetupColumn("Avg/call (ms)", ImGuiTableColumnFlags_WidthFixed, 100.0f, AvgPerCall);
			ImGui::TableSetupColumn("Avg/frame (ms)",
					ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort
					| ImGuiTableColumnFlags_PreferSortDescending, 110.0f, AvgPerFrame);
			ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, 120.0f, Share);
			ImGui::TableSetupColumn("Last (ms)", ImGuiTableColumnFlags_WidthFixed, 90.0f, Last);
			ImGui::TableSetupColumn("Peak (ms)", ImGuiTableColumnFlags_WidthFixed, 90.0f, Peak);
			ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 90.0f, Calls);
			ImGui::TableHeadersRow();
			for(int column = 0; column < ColumnCount; ++column) {
				ImGui::TableSetColumnIndex(column);
				if(ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
					ImGui::BeginTooltip();
					ImGui::TextUnformatted(kColumnTooltips[column]);
					ImGui::EndTooltip();
				}
			}

			// Sorted on every frame, not only when the sort specs change: the
			// numbers themselves change every frame, so the order has to be
			// rebuilt along with them.
			sortRows(rows, ImGui::TableGetSortSpecs());

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(rows.size()));
			while(clipper.Step()) {
				for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
					const Row& row = rows[i];
					ImGui::TableNextRow();
					// Sections that eat a quarter of a frame (or a tenth of it)
					// are the ones worth looking at, so they are tinted.
					if(row.share_ >= 0.25)
						ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(220, 70, 70, 60));
					else if(row.share_ >= 0.10)
						ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(230, 180, 60, 45));

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(row.name_.c_str());
					ImGui::TableNextColumn();
					textRightMs(row.avgPerCall_);
					ImGui::TableNextColumn();
					textRightMs(row.avgPerFrame_);
					ImGui::TableNextColumn();
					char overlay[16];
					snprintf(overlay, sizeof(overlay), "%.0f%%", row.share_ * 100.0);
					ImGui::ProgressBar(static_cast<float>(row.share_), ImVec2(-1.0f, 0.0f), overlay);
					ImGui::TableNextColumn();
					textRightMs(row.last_);
					ImGui::TableNextColumn();
					textRightMs(row.peak_);
					ImGui::TableNextColumn();
					textRightCount(row.calls_);
				}
			}
			ImGui::EndTable();

			ImGui::PopStyleVar(2);
			ImGui::Separator();

			const double fps = cv::plan::GlobalState::get<double>(cv::plan::GlobalState::Keys::FPS);
			const double frameTime = fps > 0.0 ? 1000.0 / fps : 0.0;
			char status[256];
			snprintf(status, sizeof(status),
					"%d of %d sections  ·  %.3f ms/frame  ·  %ld calls",
					static_cast<int>(rows.size()), static_cast<int>(entries.size()),
					perFrameTotal, callsTotal);
			ImGui::TextDisabled("%s", status);
			if(frameTime > 0.0) {
				char budget[96];
				snprintf(budget, sizeof(budget), "  ·  %.1f%% of the %.2f ms frame",
						perFrameTotal / frameTime * 100.0, frameTime);
				ImGui::SameLine();
				ImGui::TextDisabled("%s", budget);
			}
		} else {
			ImGui::PopStyleVar(2);
		}
	}
	ImGui::End();
	return open;
}