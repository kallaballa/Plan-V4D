// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The text of one pass: a buffer with a caret, a selection and an undo history.
//
// Split out from the editor widget (which draws it and reads keys) because the
// two want to be reasoned about separately: this half is plain strings and
// offsets and has no ImGui in it at all.
//
// Positions are byte offsets into text(); the editor only ever asks for a
// line/column pair to show in the status bar or to mark an error, so the two
// views never have to agree about what a column is. GLSL is ASCII in practice,
// and a caret move steps a whole code point so a multi-byte character cannot
// end up split in half.
//
// The one thing borrowed from the tokenizer is which bracket pairs with which;
// everything else here is plain strings and offsets.
#ifndef MODULES_V4D_SAMPLES_SHADERTOY_TEXT_HPP_
#define MODULES_V4D_SAMPLES_SHADERTOY_TEXT_HPP_

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shadertoy_syntax.hpp"

namespace shadertoy {
namespace text {

/// A caret position, remembered as both a byte offset and a line/column pair.
/// Keeping both means "go to line 12" does not have to walk the buffer, and a
/// click does not have to walk it either.
struct Position {
  std::size_t offset = 0;
  int line = 0;
  int column = 0;
};

/// One step of the undo history: the text as it was, plus where the caret was.
/// Snapshot based rather than delta based on purpose - a GLSL pass is a few
/// kilobytes, a diff would be more code than the feature deserves, and a
/// snapshot cannot be half applied.
struct Snapshot {
  std::string text;
  Position anchor;
  Position head;
};

/// An edit the undo history should remember. The editor fills this in when the
/// user types, pastes, deletes or auto-indents, so that a whole gesture (a
/// word, a line, a pasted block) becomes one undo step and not one per
/// keystroke.
struct Edit {
  std::string text;       // the buffer content after the edit
  Position anchor;        // caret after the edit
  Position head;
};

class Buffer {
public:
  explicit Buffer(std::string initial = std::string()) {
    text_ = std::move(initial);
    rebuildLines();
    clampCaret();
    history_.push_back(snapshot());
  }

  // ---- content -----------------------------------------------------------

  const std::string &text() const { return text_; }

  /// Replaces the whole text, dropping the history: this is what loading a
  /// project does, and undoing back into the previous file would not make sense.
  void reset(std::string text) {
    text_ = std::move(text);
    generation_++;
    rebuildLines();
    clampCaret();
    history_.clear();
    redo_.clear();
    history_.push_back(snapshot());
    lastGroup_ = {};
  }

  /// Bumped by every committed change, so a consumer can tell "the text moved"
  /// from "the same text was set again".
  unsigned long generation() const { return generation_; }

  int lineCount() const {
    return int(lineStarts_.size());
  }

  /// The [begin, end) byte range of line `line` (0-based), without the newline.
  std::pair<std::size_t, std::size_t> lineRange(int line) const {
    line = std::clamp(line, 0, lineCount() - 1);
    const std::size_t begin = lineStarts_[size_t(line)];
    const std::size_t end = (line + 1 < lineCount()) ? lineStarts_[size_t(line) + 1] - 1
                                                      : text_.size();
    return {begin, std::max(end, begin)};
  }

  std::string line(int lineIndex) const {
    const auto [begin, end] = lineRange(lineIndex);
    return text_.substr(begin, end - begin);
  }

  Position positionAt(int line, int column) const {
    line = std::clamp(line, 0, lineCount() - 1);
    const auto [begin, end] = lineRange(line);
    Position p;
    p.line = line;
    p.column = std::max(column, 0);
    p.offset = std::min(begin + std::size_t(p.column), end);
    return p;
  }

  Position offsetToPosition(std::size_t offset) const {
    offset = std::min(offset, text_.size());
    const auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    Position p;
    p.line = std::max(int(it - lineStarts_.begin()) - 1, 0);
    p.offset = offset;
    p.column = int(offset - lineStarts_[size_t(p.line)]);
    return p;
  }

  /// The line and column a mouse at (x, y) pixels landed on, given the font
  /// metrics the editor drew with. `columnAt` turns a pixel x into a byte
  /// offset inside the line.
  Position hitTest(int line, int column) const { return positionAt(line, column); }

  // ---- selection ---------------------------------------------------------

  const Position &anchor() const { return anchor_; }
  const Position &head() const { return head_; }

  /// The selection as an ordered byte range. An empty one collapses to the
  /// caret, so it is always safe to splice it.
  std::pair<std::size_t, std::size_t> selection() const {
    return {std::min(anchor_.offset, head_.offset),
            std::max(anchor_.offset, head_.offset)};
  }

  bool hasSelection() const { return anchor_.offset != head_.offset; }

  std::string selectedText() const {
    const auto [a, b] = selection();
    return text_.substr(a, b - a);
  }

  void setCaret(std::size_t offset) {
    head_ = offsetToPosition(offset);
    anchor_ = head_;
  }

  void setSelection(std::size_t anchorOffset, std::size_t headOffset) {
    anchor_ = offsetToPosition(anchorOffset);
    head_ = offsetToPosition(headOffset);
  }

  /// The word under `offset` - or, on punctuation, the matching pair, so that
  /// double-clicking a bracket selects both halves of it, which is what every
  /// other editor does.
  std::pair<std::size_t, std::size_t> wordAt(std::size_t offset) const {
    if (text_.empty())
      return {0, 0};
    offset = std::min(offset, text_.size() - 1);
    auto isWord = [](char ch) {
      return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
             (ch >= '0' && ch <= '9') || ch == '_';
    };
    const char c = text_[offset];
    if (syntax::closingOf(c) != 0) {
      const char mate = syntax::closingOf(c);
      // Forward first: with the cursor on the opening bracket that is the short
      // way round, and if the pair is unbalanced we select the bracket itself
      // rather than an arbitrary run. The depth is signed because an unbalanced
      // run of closers must not wrap a counter around.
      int depth = 0;
      for (std::size_t i = offset; i < text_.size(); ++i) {
        if (text_[i] == c)
          ++depth;
        else if (text_[i] == mate && --depth == 0)
          return {offset, i + 1};
      }
      depth = 0;
      for (std::size_t i = offset + 1; i-- > 0;) {
        if (text_[i] == mate)
          ++depth;
        else if (text_[i] == c && --depth == 0)
          return {i, offset + 1};
      }
      return {offset, offset + 1};
    }
    if (!isWord(c)) {
      std::size_t begin = offset;
      std::size_t end = offset + 1;
      while (begin > 0 && text_[begin - 1] == c)
        --begin;
      while (end < text_.size() && text_[end] == c)
        ++end;
      return {begin, end};
    }
    std::size_t begin = offset;
    std::size_t end = offset;
    while (begin > 0 && isWord(text_[begin - 1]))
      --begin;
    while (end < text_.size() && isWord(text_[end]))
      ++end;
    return {begin, end};
  }

  // ---- editing -----------------------------------------------------------

  /// Commits `edit` as the new state. The pre-edit text goes onto the undo
  /// stack, unless this edit continues the group that is already there - which
  /// is what makes typing one undo step rather than one per keystroke.
  ///
  /// `breakGroup` forces a new step: a paste, an auto-indent, or the first
  /// keystroke after the user paused.
  ///
  /// The history holds the states *behind* the current one, never the current
  /// one itself, so `history_.size() > 1` is exactly "there is something to undo"
  /// and a commit that changes no text adds nothing.
  void commit(const Edit &edit, bool breakGroup = false) {
    const auto now = std::chrono::steady_clock::now();
    const bool newGroup = breakGroup || history_.empty() ||
                          now - lastGroup_ > std::chrono::milliseconds(700);
    if (newGroup && (history_.empty() || history_.back().text != text_)) {
      history_.push_back(snapshot());
      // A long session must not grow without bound. A pass is small, so
      // dropping the oldest steps is invisible in practice.
      while (history_.size() > kMaxHistory)
        history_.pop_front();
      lastGroup_ = now;
    }
    if (text_ != edit.text) {
      text_ = edit.text;
      generation_++;
    }
    rebuildLines();
    anchor_ = offsetToPosition(edit.anchor.offset);
    head_ = offsetToPosition(edit.head.offset);
    redo_.clear();
  }

  bool canUndo() const { return history_.size() > 1; }
  bool canRedo() const { return !redo_.empty(); }
  std::size_t undoDepth() const { return history_.size() > 1 ? history_.size() - 1 : 0; }
  std::size_t redoDepth() const { return redo_.size(); }

  /// Steps back one entry and returns the text that is now current. The next
  /// call to redo() puts this one back, so the caller only has to write the
  /// returned string into the document.
  std::string undo() {
    if (!canUndo())
      return text_;
    redo_.push_back(snapshot());
    const Snapshot previous = history_.back();
    history_.pop_back();
    text_ = std::move(previous.text);
    generation_++;
    rebuildLines();
    anchor_ = offsetToPosition(previous.anchor.offset);
    head_ = offsetToPosition(previous.head.offset);
    lastGroup_ = std::chrono::steady_clock::now();
    return text_;
  }

  std::string redo() {
    if (redo_.empty())
      return text_;
    history_.push_back(snapshot());
    const Snapshot next = redo_.back();
    redo_.pop_back();
    text_ = std::move(next.text);
    generation_++;
    rebuildLines();
    anchor_ = offsetToPosition(next.anchor.offset);
    head_ = offsetToPosition(next.head.offset);
    lastGroup_ = std::chrono::steady_clock::now();
    return text_;
  }

  void clearHistory() {
    history_.clear();
    redo_.clear();
    history_.push_back(snapshot());
    lastGroup_ = {};
  }

private:
  static constexpr std::size_t kMaxHistory = 256;

  Snapshot snapshot() const { return Snapshot{text_, anchor_, head_}; }

  void clampCaret() {
    anchor_ = offsetToPosition(anchor_.offset);
    head_ = offsetToPosition(head_.offset);
  }

  void rebuildLines() {
    lineStarts_.clear();
    lineStarts_.push_back(0);
    for (std::size_t i = 0; i < text_.size(); ++i)
      if (text_[i] == '\n')
        lineStarts_.push_back(i + 1);
    if (lineStarts_.back() > text_.size())
      lineStarts_.pop_back();
  }

  std::string text_;
  Position anchor_, head_;
  std::vector<std::size_t> lineStarts_;
  std::deque<Snapshot> history_;
  std::deque<Snapshot> redo_;
  std::chrono::steady_clock::time_point lastGroup_{};
  unsigned long generation_ = 0;
};

}  // namespace text
}  // namespace shadertoy

#endif  // MODULES_V4D_SAMPLES_SHADERTOY_TEXT_HPP_
