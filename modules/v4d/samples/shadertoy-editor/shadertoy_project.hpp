// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// Reading and writing Shadertoy project files, plus local texture loading.
//
// This is the editor's file format story, and it is deliberately the same JSON
// the REST API speaks: a file the user saves here can be dropped onto
// shadertoy.com unchanged. The reader therefore accepts the API's envelope
// ({"Shader": {...}}) as well as a bare {"info": ..., "renderpass": [...]}.
#ifndef MODULES_V4D_SAMPLES_SHADERTOY_PROJECT_HPP_
#define MODULES_V4D_SAMPLES_SHADERTOY_PROJECT_HPP_

#pragma once

#include <fstream>
#include <sstream>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "shadertoy-editor/shadertoy_json.hpp"
#include "shadertoy-editor/shadertoy_model.hpp"

namespace shadertoy {

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

inline bool readFile(const std::string &path, std::string &out,
                     std::string &error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open " + path;
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (in.bad()) {
    error = "cannot read " + path;
    return false;
  }
  out = buffer.str();
  return true;
}

/// Reads a shader out of a document. `passesOut` receives the render passes, and
/// `infoOut` the metadata; both are optional.
inline bool readProject(const std::string &text, Shader &shader,
                        std::string &error) {
  Json doc;
  if (!jsonParse(text, doc, error))
    return false;
  // Accept the API's envelope and a bare shader object. A Shadertoy *export*
  // (what the site's "download .json" gives you) is the bare form.
  const Json *node = doc.get("Shader");
  if (node == nullptr || !node->isObject())
    node = &doc;
  if (const Json *err = node->get("Error")) {
    error = "the document is an API error, not a shader: " + err->text;
    return false;
  }

  shader = Shader{};
  if (const Json *info = node->get("info")) {
    shader.id = info->str("id");
    shader.name = info->str("name");
    shader.author = info->str("author");
    shader.username = info->str("username");
    shader.description = info->str("description");
    shader.likes = info->integer("likes", info->integer("like"));
    shader.views = info->integer("viewed", info->integer("view"));
    if (const Json *tags = info->get("tags"))
      for (const auto &tag : tags->items)
        if (tag.isString())
          shader.tags.push_back(tag.text);
  }

  const Json *passes = node->get("renderpass");
  if (passes == nullptr || !passes->isArray() || passes->items.empty()) {
    error = "the document has no \"renderpass\" array";
    return false;
  }
  for (const auto &item : passes->items) {
    Pass pass;
    pass.name = item.str("name");
    pass.type = item.str("type");
    pass.code = item.str("code");
    if (const Json *inputs = item.get("inputs"))
      for (const auto &in : inputs->items) {
        PassInput pi;
        pi.id = int(in.integer("id"));
        pi.channel = int(in.integer("channel"));
        pi.ctype = in.str("ctype");
        pi.src = in.str("src");
        if (const Json *sampler = in.get("sampler")) {
          pi.filter = sampler->str("filter", "linear");
          pi.wrap = sampler->str("wrap", "clamp");
          pi.vflip = sampler->num("vflip", 1.0) != 0.0;
          pi.srgb = sampler->num("srgb", 0.0) != 0.0;
        }
        // Shadertoy numbers the inputs of a pass consecutively, and some
        // exports omit the id; keep what was there but fall back to the slot.
        if (pi.id <= 0)
          pi.id = int(pass.inputs.size());
        pass.inputs.push_back(std::move(pi));
      }
    if (const Json *outputs = item.get("outputs"))
      for (const auto &out2 : outputs->items) {
        PassOutput po;
        po.id = int(out2.integer("id"));
        po.channel = int(out2.integer("channel"));
        pass.outputs.push_back(po);
      }
    // Sound passes are not rendered here, and a pass with no type is common
    // code by convention.
    if (pass.type.empty())
      pass.type = "image";
    if (pass.type == "sound")
      continue;
    shader.passes.push_back(std::move(pass));
  }
  if (shader.passes.empty()) {
    error = "the document has no pass this renderer can draw";
    return false;
  }
  // Shadertoy stores the shared prologue as a "common" pass; keeping it as the
  // first entry means the renderer puts it ahead of every other pass.
  for (size_t i = 1; i < shader.passes.size(); ++i)
    if (shader.passes[i].type == "common") {
      Pass common = shader.passes[i];
      shader.passes.erase(shader.passes.begin() + ptrdiff_t(i));
      shader.passes.insert(shader.passes.begin(), std::move(common));
      break;
    }
  return true;
}

inline bool readProjectFile(const std::string &path, Shader &shader,
                            std::string &error) {
  std::string text;
  if (!readFile(path, text, error))
    return false;
  return readProject(text, shader, error);
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

inline std::string jsonEscape(const std::string &in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (const char raw : in) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        static const char *hex = "0123456789abcdef";
        out += "\\u00";
        out += hex[(c >> 4) & 0xF];
        out += hex[c & 0xF];
      } else {
        out += raw;
      }
    }
  }
  return out;
}

inline std::string jsonQuote(const std::string &in) {
  return "\"" + jsonEscape(in) + "\"";
}

inline std::string writeProject(const Shader &shader) {
  std::string out;
  out += "{\n  \"Shader\": {\n    \"info\": {\n";
  out += "      \"id\": " + jsonQuote(shader.id) + ",\n";
  out += "      \"author\": " + jsonQuote(shader.author) + ",\n";
  out += "      \"username\": " + jsonQuote(shader.username) + ",\n";
  out += "      \"name\": " + jsonQuote(shader.name) + ",\n";
  out += "      \"description\": " + jsonQuote(shader.description) + ",\n";
  out += "      \"likes\": " + std::to_string(shader.likes) + ",\n";
  out += "      \"viewed\": " + std::to_string(shader.views) + ",\n";
  out += "      \"tags\": [";
  for (size_t i = 0; i < shader.tags.size(); ++i)
    out += (i == 0 ? "" : ", ") + jsonQuote(shader.tags[i]);
  out += "]\n    },\n    \"renderpass\": [\n";

  for (size_t i = 0; i < shader.passes.size(); ++i) {
    const Pass &pass = shader.passes[i];
    out += "      {\n        \"type\": " + jsonQuote(pass.type) + ",\n";
    out += "        \"name\": " + jsonQuote(pass.name) + ",\n";
    out += "        \"code\": " + jsonQuote(pass.code) + ",\n";

    // A common block has neither inputs nor outputs.
    if (pass.type != "common") {
      if (!pass.inputs.empty()) {
        out += "        \"inputs\": [\n";
        for (size_t j = 0; j < pass.inputs.size(); ++j) {
          const PassInput &in = pass.inputs[j];
          out += "          {";
          out += "\"id\": " + std::to_string(in.id);
          out += ", \"channel\": " + std::to_string(in.channel);
          out += ", \"ctype\": " + jsonQuote(in.ctype);
          out += ", \"src\": " + jsonQuote(in.src);
          out += ", \"sampler\": {";
          out += "\"filter\": " + jsonQuote(in.filter);
          out += ", \"wrap\": " + jsonQuote(in.wrap);
          out += ", \"vflip\": ";
          out += in.vflip ? "true" : "false";
          out += ", \"srgb\": ";
          out += in.srgb ? "true" : "false";
          out += "}}";
          out += (j + 1 == pass.inputs.size() ? "\n" : ",\n");
        }
        out += "        ],\n";
      }
      out += "        \"outputs\": [\n";
      for (size_t j = 0; j < pass.outputs.size(); ++j) {
        out += "          {\"id\": " + std::to_string(pass.outputs[j].id) +
               ", \"channel\": " + std::to_string(pass.outputs[j].channel) + "}";
        out += (j + 1 == pass.outputs.size() ? "\n" : ",\n");
      }
      out += "        ]\n";
    }
    out += (i + 1 == shader.passes.size() ? "      }\n" : "      },\n");
  }
  out += "    ]\n  }\n}\n";
  return out;
}

inline bool writeProjectFile(const std::string &path, const Shader &shader,
                             std::string &error) {
  // Write to a sibling and rename, so a failure cannot leave a half written
  // shader where the user's only copy was.
  const std::string temp = path + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "cannot write " + temp;
      return false;
    }
    out << writeProject(shader);
    out.flush();
    if (!out) {
      error = "cannot write " + temp;
      return false;
    }
  }
  // rename() replaces the destination in one step on POSIX, so the old file
  // stays put if anything above fails. Removing it first would be the one way
  // to lose it.
  if (std::rename(temp.c_str(), path.c_str()) != 0) {
    std::remove(temp.c_str());
    error = "cannot replace " + path;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Local textures
// ---------------------------------------------------------------------------

/// Decodes a texture from disk into the CV_8UC4 RGBA the renderer uploads. Any
/// channel count OpenCV can read is accepted; anything else is refused with a
/// message instead of an exception.
inline bool loadLocalTexture(const std::string &path, cv::Mat &rgba,
                             std::string &error) {
  const cv::Mat decoded = cv::imread(path, cv::IMREAD_UNCHANGED);
  if (decoded.empty()) {
    error = "cannot decode " + path;
    return false;
  }
  if (decoded.depth() != CV_8U) {
    error = path + " is not 8 bit";
    return false;
  }
  switch (decoded.channels()) {
  case 1:
    cv::cvtColor(decoded, rgba, cv::COLOR_GRAY2RGBA);
    break;
  case 3:
    cv::cvtColor(decoded, rgba, cv::COLOR_BGR2RGBA);
    break;
  case 4:
    rgba = decoded;
    break;
  default:
    error = path + " has " + std::to_string(decoded.channels()) + " channels";
    return false;
  }
  return true;
}

}  // namespace shadertoy

#endif  // MODULES_V4D_SAMPLES_SHADERTOY_PROJECT_HPP_