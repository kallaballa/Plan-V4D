// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The Shadertoy data model: passes, their channel inputs and the shader that
// owns them.
//
// Kept apart from shadertoy_project.hpp so that shadertoy_renderer.hpp (and
// with it the offline editor) can consume a shader without dragging the file
// format in with it. The shapes are the ones /api/v1/shaders/<id> returns, so a
// project file written here is what the site itself would hand out.
#ifndef MODULES_V4D_SAMPLES_SHADERTOY_MODEL_HPP_
#define MODULES_V4D_SAMPLES_SHADERTOY_MODEL_HPP_

#pragma once

#include <string>
#include <vector>

namespace shadertoy {

/// The official site. Shader::url() points at it deliberately, even when the
/// data came from a mirror: a mirror serves the API, but the link the user is
/// invited to click should open on shadertoy.com.
inline constexpr const char *kOfficialBaseUrl = "https://www.shadertoy.com";

// ---------------------------------------------------------------------------
// The API's data model
// ---------------------------------------------------------------------------

struct PassInput {
  int id = 0;
  int channel = 0;
  std::string ctype;  // "texture", "buffer", "keyboard", "cubemap", "sound"
  std::string src;    // preset path, pass name or absolute url
  std::string filter = "linear";  // "linear", "nearest", "mipmap"
  std::string wrap = "clamp";     // "clamp", "repeat", "mirror"
  bool vflip = true;
  bool srgb = false;
};

struct PassOutput {
  int id = 0;
  int channel = 0;
};

struct Pass {
  std::string name;
  std::string type;  // "image", "buffer" or "common"
  std::string code;
  std::vector<PassInput> inputs;
  std::vector<PassOutput> outputs;
};

struct Shader {
  std::string id;
  std::string name;
  std::string author;
  std::string username;
  std::string description;
  long long likes = 0;
  long long views = 0;
  std::vector<std::string> tags;
  std::vector<Pass> passes;

  /// The canonical public page of the shader.
  std::string url() const { return std::string(kOfficialBaseUrl) + "/view/" + id; }
  /// The pass whose output is the visible image, or the first pass if the
  /// shader has no explicit "image" pass.
  int imagePass() const {
    for (size_t i = 0; i < passes.size(); ++i)
      if (passes[i].type == "image")
        return int(i);
    return passes.empty() ? -1 : 0;
  }
};

struct Summary {
  std::string id;
  std::string name;
  std::string username;
  long long likes = 0;
  long long views = 0;
};

/// One texture the shader needs, as raw encoded bytes plus the pass input that
/// asked for it (for sampler state and the vertical flip).
struct TextureRef {
  std::string src;
  std::string filter = "linear";
  std::string wrap = "clamp";
  bool vflip = true;
};

}  // namespace shadertoy

#endif  // MODULES_V4D_SAMPLES_SHADERTOY_MODEL_HPP_
