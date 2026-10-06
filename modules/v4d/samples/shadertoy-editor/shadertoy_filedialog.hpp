// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// A file selection dialog for the Shadertoy editor.
//
// V4D ships no file dialog of its own and the editor needs three of them -
// open a project, save a project, pick a texture - so this header is the one
// reusable piece. It is a plain floating ImGui window: directory listing with
// directories first, an editable path bar, extension filters, keyboard
// navigation, and a filename field in save mode. It deliberately knows nothing
// about what the caller does with the chosen path; it reports one through
// draw() and forgets.
#ifndef MODULES_V4D_SAMPLES_SHADERTOY_FILEDIALOG_HPP_
#define MODULES_V4D_SAMPLES_SHADERTOY_FILEDIALOG_HPP_

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "imgui.h"
// SetKeyOwner(): the dialog's arrow/enter keys must not ALSO drive ImGui's
// keyboard nav, which V4D switches on (a press would otherwise move the nav
// focus and activate a button underneath the dialog's own handling).
#include "imgui_internal.h"

namespace shadertoy {

/// One modal file browser. Reuse the same instance for repeated opens: it
/// remembers the directory it was last left in, which is what a user expects
/// from an Open dialog.
class FileDialog {
public:
  enum class Mode { Open, Save };

  /// What happened when draw() returned true. `chosen` distinguishes "picked a
  /// file" from "cancelled"; `path` is the picked file, only when chosen.
  struct Result {
    bool chosen = false;
    std::string path;
  };

  /// Opens the dialog. `startDir` is where the listing begins (the current
  /// working directory when empty); `extensions` (with the dot, lowercase)
  /// filters the listed files - an empty list shows everything. In Save mode
  /// `initialName` pre-fills the filename field.
  void open(Mode mode, std::string title, const std::string &startDir,
          std::vector<std::string> extensions, std::string initialName = "") {
    mode_ = mode;
    title_ = std::move(title);
    extensions_ = std::move(extensions);
    for (std::string &ext : extensions_)
      ext = lower(ext);
    std::error_code ec;
    std::filesystem::path dir =
        startDir.empty() ? std::filesystem::current_path(ec)
                         : std::filesystem::path(startDir);
    if (ec || !std::filesystem::is_directory(dir, ec))
      dir = std::filesystem::current_path(ec);
    dir_ = dir.lexically_normal().string();
    std::snprintf(dirBuf_, sizeof(dirBuf_), "%s", dir_.c_str());
    std::snprintf(nameBuf_, sizeof(nameBuf_), "%s", initialName.c_str());
    selected_ = -1;
    error_.clear();
    showAll_ = extensions_.empty();
    open_ = true;
    needOpenPopup_ = true;
    refresh();
  }

  bool isOpen() const { return open_; }
  const std::string &currentDir() const { return dir_; }

  /// Draws the dialog while it is open. Returns true exactly once - when the
  /// user confirmed (result.chosen, result.path) or cancelled.
  bool draw(Result &result) {
    if (!open_)
      return false;
    using namespace ImGui;

    // A modal popup rather than a floating window: it stays above the panel,
    // a click outside flashes instead of raising whatever was underneath, and
    // there is no z-order to manage.
    if (needOpenPopup_) {
      OpenPopup(title_.c_str());
      needOpenPopup_ = false;
    }
    bool done = false;
    bool visible = open_;
    SetNextWindowSize(ImVec2(680.0f, 460.0f), ImGuiCond_Appearing);
    SetNextWindowPos(GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                     ImVec2(0.5f, 0.5f));
    if (!BeginPopupModal(title_.c_str(), &visible,
                         ImGuiWindowFlags_NoScrollbar)) {
      // Not open (yet, or the user closed it): without the popup there is
      // nothing to draw. A close through the window's X is a cancel.
      if (!visible && open_) {
        open_ = false;
        done = true;
      }
      return done;
    }

    // Every way out of the dialog goes through here: the popup has to be
    // closed as well as the flag cleared, or OpenPopup reopens it next frame.
    auto closeDialog = [&] {
      CloseCurrentPopup();
      open_ = false;
      done = true;
    };

    // ---- the path bar ------------------------------------------------------
    if (Button("Up"))
      goUp();
    SameLine();
    if (Button("Home")) {
      const char *home = std::getenv("HOME");
      if (home != nullptr && changeDir(home))
        refresh();
    }
    SameLine();
    PushItemWidth(-1.0f);
    if (InputText("##dir", dirBuf_, sizeof(dirBuf_),
                  ImGuiInputTextFlags_EnterReturnsTrue)) {
      if (!changeDir(dirBuf_))
        error_ = "no such directory: " + std::string(dirBuf_);
    }
    PopItemWidth();

    // ---- the listing -------------------------------------------------------
    const float footer = GetFrameHeightWithSpacing() * (mode_ == Mode::Save ? 3.0f : 2.0f) +
                         GetTextLineHeightWithSpacing();
    if (BeginChild("##listing", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
      if (entries_.empty())
        TextDisabled(showAll_ ? "  (empty directory)" : "  (no matching files)");
      for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry &entry = entries_[i];
        const bool isSel = selected_ == int(i);
        std::string label = entry.isDir ? "[dir]  " + entry.name : entry.name;
        PushID(int(i));
        if (Selectable(label.c_str(), isSel,
                       ImGuiSelectableFlags_AllowDoubleClick)) {
          selected_ = int(i);
          if (entry.isDir) {
            if (mode_ == Mode::Save)
              nameBuf_[0] = '\0';
          } else if (mode_ == Mode::Save) {
            std::snprintf(nameBuf_, sizeof(nameBuf_), "%s",
                          entry.name.c_str());
          }
          if (IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (entry.isDir) {
              changeDir(join(dir_, entry.name));
            } else {
              result.chosen = true;
              result.path = join(dir_, entry.name);
              closeDialog();
            }
          }
        }
        if (!entry.isDir) {
          SameLine();
          SetCursorPosX(std::max(GetCursorPosX(), GetWindowWidth() - 110.0f));
          TextDisabled("%s", humanSize(entry.size).c_str());
        }
        // Keyboard navigation and the click both move the selection; keep the
        // row visible when it moved off screen.
        if (isSel && scrollToSelection_) {
          SetScrollHereY();
          scrollToSelection_ = false;
        }
        PopID();
      }
    }
    EndChild();

    // ---- keyboard navigation ----------------------------------------------
    // Only when no input field owns the keys: an Enter in the path bar is the
    // path bar's, not the listing's. Ownership is claimed for the window, so
    // ImGui's nav (enabled by V4D) cannot ALSO consume them - pressing Down
    // would otherwise move the nav focus while Enter activates a button.
    const bool keysForListing =
        IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !IsAnyItemActive();
    if (keysForListing) {
      const ImGuiID owner = GetCurrentWindow()->ID;
      for (ImGuiKey key : {ImGuiKey_DownArrow, ImGuiKey_UpArrow, ImGuiKey_Enter,
                           ImGuiKey_Backspace, ImGuiKey_Escape})
        SetKeyOwner(key, owner);
    }
    if (keysForListing) {
      if (IsKeyPressed(ImGuiKey_DownArrow))
        moveSelection(1);
      else if (IsKeyPressed(ImGuiKey_UpArrow))
        moveSelection(-1);
      else if (IsKeyPressed(ImGuiKey_Enter) && selected_ >= 0 &&
               size_t(selected_) < entries_.size()) {
        const Entry &entry = entries_[size_t(selected_)];
        if (entry.isDir) {
          changeDir(join(dir_, entry.name));
        } else if (mode_ == Mode::Open) {
          result.chosen = true;
          result.path = join(dir_, entry.name);
          closeDialog();
        } else {
          std::snprintf(nameBuf_, sizeof(nameBuf_), "%s", entry.name.c_str());
        }
      } else if (IsKeyPressed(ImGuiKey_Backspace)) {
        goUp();
      } else if (IsKeyPressed(ImGuiKey_Escape)) {
        closeDialog();
      }
    }

    // ---- the footer ---------------------------------------------------------
    if (!error_.empty()) {
      TextColored(ImVec4(0.94f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());
      SameLine();
      if (SmallButton("clear"))
        error_.clear();
    } else if (mode_ == Mode::Save && !overwriteName_.empty()) {
      TextColored(ImVec4(0.96f, 0.73f, 0.3f, 1.0f), "\"%s\" exists - saving overwrites it",
                  overwriteName_.c_str());
    } else {
      TextDisabled("%s", filterText().c_str());
    }

    if (mode_ == Mode::Save) {
      PushItemWidth(-1.0f);
      if (InputTextWithHint("##name", "file name", nameBuf_, sizeof(nameBuf_),
                            ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (confirmSave(result))
          closeDialog();
      }
      if (IsWindowAppearing())
        SetKeyboardFocusHere();
      PopItemWidth();
      updateOverwriteHint();
    }

    const char *verb = mode_ == Mode::Save ? "Save" : "Open";
    if (Button(verb, ImVec2(120.0f, 0.0f))) {
      if (mode_ == Mode::Save) {
        if (confirmSave(result))
          closeDialog();
      } else if (selected_ >= 0 && size_t(selected_) < entries_.size()) {
        const Entry &entry = entries_[size_t(selected_)];
        if (entry.isDir) {
          changeDir(join(dir_, entry.name));
        } else {
          result.chosen = true;
          result.path = join(dir_, entry.name);
          closeDialog();
        }
      } else {
        error_ = "select a file first";
      }
    }
    SameLine();
    if (Button("Cancel", ImVec2(120.0f, 0.0f)))
      closeDialog();
    if (!showAll_) {
      SameLine();
      if (SmallButton("show all files")) {
        showAll_ = true;
        refresh();
      }
    } else if (!extensions_.empty()) {
      SameLine();
      if (SmallButton("only matching files")) {
        showAll_ = false;
        refresh();
      }
    }
    SameLine();
    if (SmallButton(showHidden_ ? "hide dotfiles" : "show dotfiles")) {
      showHidden_ = !showHidden_;
      refresh();
    }

    EndPopup();
    return done;
  }

private:
  struct Entry {
    std::string name;
    bool isDir = false;
    std::uintmax_t size = 0;
  };

  static std::string lower(std::string s) {
    for (char &c : s)
      c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }

  static std::string join(const std::string &dir, const std::string &name) {
    return (std::filesystem::path(dir) / name).lexically_normal().string();
  }

  bool accepts(const std::filesystem::path &p) const {
    if (showAll_ || extensions_.empty())
      return true;
    const std::string ext = lower(p.extension().string());
    return std::find(extensions_.begin(), extensions_.end(), ext) !=
           extensions_.end();
  }

  void refresh() {
    entries_.clear();
    error_.clear();
    std::error_code ec;
    std::filesystem::directory_iterator it(dir_, ec);
    if (ec) {
      error_ = "cannot list " + dir_;
      return;
    }
    for (const auto &item : it) {
      std::error_code ec2;
      const bool isDir = item.is_directory(ec2);
      if (!isDir && !accepts(item.path()))
        continue;
      const std::string name = item.path().filename().string();
      if (name.empty() || name == "." || name == "..")
        continue;
      if (!showHidden_ && !name.empty() && name[0] == '.')
        continue;
      Entry entry;
      entry.name = name;
      entry.isDir = isDir;
      entry.size = isDir ? 0 : item.file_size(ec2);
      entries_.push_back(std::move(entry));
      if (entries_.size() >= 5000)
        break; // a directory this size would make the listing unusable anyway
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
      if (a.isDir != b.isDir)
        return a.isDir; // directories first
      return lower(a.name) < lower(b.name);
    });
    selected_ = -1;
    scrollToSelection_ = false;
  }

  bool changeDir(const std::string &path) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec))
      return false;
    dir_ = std::filesystem::path(path).lexically_normal().string();
    std::snprintf(dirBuf_, sizeof(dirBuf_), "%s", dir_.c_str());
    refresh();
    return true;
  }

  void goUp() {
    std::filesystem::path p(dir_);
    if (p.has_parent_path())
      changeDir(p.parent_path().string());
  }

  void moveSelection(int delta) {
    if (entries_.empty())
      return;
    int next = selected_ < 0 ? 0 : selected_ + delta;
    selected_ = std::clamp(next, 0, int(entries_.size()) - 1);
    scrollToSelection_ = true;
  }

  bool confirmSave(Result &result) {
    std::string name = nameBuf_;
    // Trim surrounding blanks: a trailing space in a file name is a trap.
    while (!name.empty() && name.front() == ' ')
      name.erase(name.begin());
    while (!name.empty() && name.back() == ' ')
      name.pop_back();
    if (name.empty()) {
      error_ = "give the file a name";
      return false;
    }
    // A name without one of the known extensions gets the first one appended,
    // so "plasma" saves as "plasma.json" rather than a file nothing lists.
    if (!extensions_.empty()) {
      const std::string ext = lower(std::filesystem::path(name).extension().string());
      if (std::find(extensions_.begin(), extensions_.end(), ext) ==
          extensions_.end())
        name += extensions_.front();
    }
    result.chosen = true;
    result.path = join(dir_, name);
    return true;
  }

  void updateOverwriteHint() {
    overwriteName_.clear();
    std::error_code ec;
    const std::string name = nameBuf_;
    if (!name.empty() && std::filesystem::exists(join(dir_, name), ec))
      overwriteName_ = name;
  }

  std::string filterText() const {
    if (showAll_ || extensions_.empty())
      return "showing all files";
    std::string text = "showing ";
    for (size_t i = 0; i < extensions_.size(); ++i)
      text += (i == 0 ? "" : " ") + extensions_[i];
    return text;
  }

  static std::string humanSize(std::uintmax_t size) {
    static const char *kUnits[] = {"B", "K", "M", "G", "T"};
    double value = double(size);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
      value /= 1024.0;
      ++unit;
    }
    char out[32];
    if (unit == 0)
      std::snprintf(out, sizeof(out), "%u %s", unsigned(size), kUnits[unit]);
    else
      std::snprintf(out, sizeof(out), "%.1f %s", value, kUnits[unit]);
    return out;
  }

  Mode mode_ = Mode::Open;
  std::string title_;
  std::vector<std::string> extensions_;
  std::string dir_;
  char dirBuf_[1024] = "";
  char nameBuf_[512] = "";
  std::vector<Entry> entries_;
  int selected_ = -1;
  bool open_ = false;
  bool needOpenPopup_ = false;
  bool showAll_ = true;
  bool showHidden_ = false;
  bool scrollToSelection_ = false;
  std::string error_;
  std::string overwriteName_;
};

} // namespace shadertoy

#endif // MODULES_V4D_SAMPLES_SHADERTOY_FILEDIALOG_HPP_
