// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// VanGoghPost -- the varnish and the light, done in raw GL (never fb())
//
// The painting is drawn with nvg() into the framebuffer; this node then reads
// that framebuffer back, lets luminous paint bloom out of it, softens the whole
// into velvety impasto with a living swirl, and lays a lit-from-the-room
// vignette over it -- and never touches fb(). The trick is that a gl() node
// runs against the same texture-backed framebuffer nvg() just filled, so
// discovering it with glGetIntegerv(GL_FRAMEBUFFER_BINDING), blitting it into a
// texture of our own, and painting a full-screen quad back into it is all plain
// GL: correct on desktop GL 3.2 core and on the OpenGL ES 3.0 path, where the
// shader below compiles unchanged (the #version line is prepended by
// cv::v4d::init_shaders for whichever backend is in use).
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_POST_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_POST_HPP_

#include <opencv2/v4d/v4d.hpp>

namespace sandcraft {

// GL_CHECK in <v4d/util.hpp> wraps a *single* expression and chokes on the
// commas of any multi-argument GL call (glDrawArrays, glVertexAttribPointer,
// glTexImage2D, the glBlitFramebuffer ...). Redefine it here, variadic, so it
// wraps calls of any arity while still checking for GL errors in debug builds.
// Only VanGoghPost uses GL_CHECK in this translation unit -- and the variadic
// form is a superset of the original, so a later single-expression use in this
// TU still checks as before.
#undef GL_CHECK
#ifdef NDEBUG
#define GL_CHECK(...) \
  do {                \
    __VA_ARGS__;      \
  } while (0)
#else
#define GL_CHECK(...)                                                     \
  do {                                                                    \
    __VA_ARGS__;                                                          \
    cv::v4d::gl_check_error(__FILE__, __LINE__, #__VA_ARGS__);            \
  } while (0)
#endif

struct VanGoghPost {
  GLuint quadVAO_ = 0;
  GLuint quadVBO_ = 0;
  GLuint program_ = 0;
  GLuint sceneFBO_ = 0;
  GLuint sceneTex_ = 0;
  cv::Size texSize_;
  GLint uTex_ = -1, uRes_ = -1, uTexel_ = -1, uTime_ = -1, uPaint_ = -1,
        uGlow_ = -1, uBrush_ = -1, uSwirl_ = -1, uVig_ = -1, uGrade_ = -1;

  // A full-screen triangle-strip. The UV layout matches the runtime's own blit
  // quad, so sampling and writing are a 1:1 texel copy of whatever the nvg()
  // base left behind -- the effect never rotates or mirrors the painting.
  static constexpr float kQuad_[16] = {// pos       uv
                                       -1.0f, 1.0f,  0.0f, 1.0f, -1.0f, -1.0f,
                                       0.0f,  0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                       1.0f,  -1.0f, 1.0f, 0.0f};

  static const char *vertSrc() {
    return R"(
			precision highp float;
			layout(location = 0) in vec2 aPos;
			layout(location = 1) in vec2 aUV;
			out vec2 vUV;
			void main() {
				vUV = aUV;
				gl_Position = vec4(aPos, 0.0, 1.0);
			}
		)";
  }

  static const char *fragSrc() {
    return R"(
			precision highp float;

			in vec2 vUV;
			out vec4 fragColor;

			uniform sampler2D uTex;
			uniform vec2  uResolution;
			uniform vec2  uTexel;
			uniform float uTime;
			uniform float uPaint;
			uniform float uGlow;
			uniform float uBrush;
			uniform float uSwirl;
			uniform float uVignette;
			uniform vec3  uGrade;

			float hash21(vec2 p) {
				p = fract(p * vec2(123.34, 456.21));
				p += dot(p, p + 45.32);
				return fract(p.x * p.y);
			}

			float vnoise(vec2 p) {
				vec2 i = floor(p);
				vec2 f = fract(p);
				float a = hash21(i);
				float b = hash21(i + vec2(1.0, 0.0));
				float c = hash21(i + vec2(0.0, 1.0));
				float d = hash21(i + vec2(1.0, 1.0));
				vec2 u = f * f * (3.0 - 2.0 * f);
				return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
			}

			float fbm(vec2 p) {
				float s = 0.0;
				float a = 0.5;
				for (int i = 0; i < 4; ++i) {
					s += a * vnoise(p);
					p *= 2.03;
					a *= 0.5;
				}
				return s;
			}

			// Anisotropic-Kuwahara-style soften: over a 3x3 neighbourhood take the
			// corner quadrant with the least colour variance, so interiors melt
			// into velvety paint while the brightest edges stay put. Used gently,
			// it is a varnish that unifies the strokes rather than erases them.
			vec3 oilPaint(vec2 uv) {
				vec2 t = uTexel;
				vec3 sum[4];
				vec3 sumSq[4];
				for (int q = 0; q < 4; ++q) {
					vec2 corner = (q == 0) ? vec2(-1.0, 1.0) :
					               (q == 1) ? vec2(1.0, 1.0) :
					               (q == 2) ? vec2(-1.0, -1.0) : vec2(1.0, -1.0);
					vec2 base = corner * t;
					vec3 s = vec3(0.0);
					vec3 sq = vec3(0.0);
					for (int j = 0; j < 2; ++j) {
						for (int k = 0; k < 2; ++k) {
							vec3 c = texture(uTex, uv + base + vec2(float(j) * t.x, float(k) * t.y)).rgb;
							s += c;
							sq += c * c;
						}
					}
					sum[q] = s / 4.0;
					sumSq[q] = sq / 4.0;
				}
				float bestVar = 1.0e9;
				vec3 best = sum[0];
				for (int q = 0; q < 4; ++q) {
					vec3 v = max(sumSq[q] - sum[q] * sum[q], vec3(0.0));
					float var = v.x + v.y + v.z;
					if (var < bestVar) { bestVar = var; best = sum[q]; }
				}
				return best;
			}

			// The light that does not stop at the edge of a material: two offset
			// rings weighted by luminance, so fire, sparks and the painted stars
			// throw out the halo Van Gogh gave them across the whole painting.
			vec3 bloom(vec2 uv) {
				// A sum, not a mean (the old acc/max(weight) returned a mean that
				// held a constant magnitude over ANY region with a highlight, which
				// is what blew the bright piles out): the halo now scales with how
				// many neighbours are a highlight, so a dense fire or star glows
				// while a broad field of bright-but-not-hot paint keeps its texture.
				// The smoothstep floors are high, so only the hottest pixels count.
				vec3 acc = vec3(0.0);
				for (int i = 0; i < 8; ++i) {
					float ang = 6.2831853 * float(i) / 8.0;
					vec3 c = texture(uTex, uv + vec2(cos(ang), sin(ang)) * uTexel * 4.0).rgb;
					float lum = dot(c, vec3(0.299, 0.587, 0.114));
					acc += c * smoothstep(0.90, 1.0, lum);
				}
				for (int i = 0; i < 8; ++i) {
					float ang = 6.2831853 * float(i) / 8.0 + 0.4;
					vec3 c = texture(uTex, uv + vec2(cos(ang), sin(ang)) * uTexel * 11.0).rgb;
					float lum = dot(c, vec3(0.299, 0.587, 0.114));
					acc += c * smoothstep(0.86, 1.0, lum) * 0.6;
				}
				return acc / 16.0;
			}

			void main() {
				vec2 uv = vUV;

				// living paint: a slow swirl drifts the sample along a noise field
				vec2 flow = vec2(fbm(uv * 3.0 + vec2(uTime * 0.03, 0.0)),
				                 fbm(uv * 3.0 + vec2(0.0, uTime * 0.03) + 17.3)) - 0.5;
				uv += flow * uSwirl;

				vec3 col = texture(uTex, uv).rgb;
				col = mix(col, oilPaint(uv), clamp(uPaint, 0.0, 1.0));
				col += bloom(uv) * uGlow;

				// a faint bristle sheen along the flow, as if raked light crossed
				// the ridges of the paint
				float bristle = fbm(vec2((uv.x + flow.x * 0.15) * (uResolution.x * 0.08),
				                         uv.y * 4.0));
				col *= mix(1.0, 0.93 + 0.14 * bristle, uBrush);

				// warm Van Gogh grade, then a lit-from-the-room vignette
				col *= uGrade;
				col = pow(max(col, vec3(0.0)), vec3(0.96, 1.0, 1.06));
				vec2 d = uv - 0.5;
				col *= clamp(1.0 - uVignette * dot(d, d) * 1.6, 0.0, 1.0);

				fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
			}
		)";
  }

  void makeProgram() {
    unsigned int handles[3];
    cv::v4d::init_shaders(handles, vertSrc(), fragSrc(), "fragColor");
    program_ = handles[0];
    uTex_ = glGetUniformLocation(program_, "uTex");
    uRes_ = glGetUniformLocation(program_, "uResolution");
    uTexel_ = glGetUniformLocation(program_, "uTexel");
    uTime_ = glGetUniformLocation(program_, "uTime");
    uPaint_ = glGetUniformLocation(program_, "uPaint");
    uGlow_ = glGetUniformLocation(program_, "uGlow");
    uBrush_ = glGetUniformLocation(program_, "uBrush");
    uSwirl_ = glGetUniformLocation(program_, "uSwirl");
    uVig_ = glGetUniformLocation(program_, "uVignette");
    uGrade_ = glGetUniformLocation(program_, "uGrade");
  }

  // Create the offscreen texture + FBO when first needed and on every resize.
  void ensureSize(int w, int h) {
    if (sceneFBO_ != 0 && texSize_ == cv::Size(w, h))
      return;
    if (sceneTex_) {
      glDeleteTextures(1, &sceneTex_);
      sceneTex_ = 0;
    }
    if (sceneFBO_) {
      glDeleteFramebuffers(1, &sceneFBO_);
      sceneFBO_ = 0;
    }
    texSize_ = cv::Size(w, h);
    GL_CHECK(glGenTextures(1, &sceneTex_));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, sceneTex_));
    GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                          GL_UNSIGNED_BYTE, nullptr));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GL_CHECK(glGenFramebuffers(1, &sceneFBO_));
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_));
    GL_CHECK(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, sceneTex_, 0));
  }

  // Runs inside a gl() node on the worker's own GL context.
  void process() {
    GLint prevFBO = 0;
    GL_CHECK(glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO));
    GLint vp[4] = {0, 0, 0, 0};
    GL_CHECK(glGetIntegerv(GL_VIEWPORT, vp));
    const int W = vp[2], H = vp[3];
    if (W <= 0 || H <= 0)
      return;
    ensureSize(W, H);

    // Capture the base nvg left behind into our own texture (a plain colour
    // blit, no fb()).
    GL_CHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevFBO));
    GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneFBO_));
    GL_CHECK(
        glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST));

    // Paint the treated image back into the same framebuffer in one quad.
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO));
    GL_CHECK(glViewport(0, 0, W, H));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);

    GL_CHECK(glUseProgram(program_));
    GL_CHECK(glUniform1i(uTex_, 0));
    GL_CHECK(glUniform2f(uRes_, (float)W, (float)H));
    GL_CHECK(glUniform2f(uTexel_, 1.0f / (float)W, 1.0f / (float)H));
    GL_CHECK(glUniform1f(uTime_, (float)cv::v4d::seconds()));
    GL_CHECK(glUniform1f(uPaint_, 0.42f));
    GL_CHECK(glUniform1f(uGlow_, 0.45f));
    GL_CHECK(glUniform1f(uBrush_, 0.5f));
    GL_CHECK(glUniform1f(uSwirl_, 0.0045f));
    GL_CHECK(glUniform1f(uVig_, 0.60f));
    GL_CHECK(glUniform3f(uGrade_, 1.03f, 1.0f, 0.94f));
    GL_CHECK(glActiveTexture(GL_TEXTURE0));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, sceneTex_));
    GL_CHECK(glBindVertexArray(quadVAO_));
    GL_CHECK(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));

    // Leave GL the way we found it; the overlay nvg node sets its own state.
    GL_CHECK(glBindVertexArray(0));
    GL_CHECK(glUseProgram(0));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO));
    glEnable(GL_BLEND);
  }

  void init() {
    GL_CHECK(glGenVertexArrays(1, &quadVAO_));
    GL_CHECK(glGenBuffers(1, &quadVBO_));
    GL_CHECK(glBindVertexArray(quadVAO_));
    GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, quadVBO_));
    GL_CHECK(glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad_), kQuad_,
                          GL_STATIC_DRAW));
    GL_CHECK(glEnableVertexAttribArray(0));
    GL_CHECK(glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                                   (void *)0));
    GL_CHECK(glEnableVertexAttribArray(1));
    GL_CHECK(glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                                   (void *)(2 * sizeof(float))));
    GL_CHECK(glBindVertexArray(0));
    GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, 0));
    makeProgram();
  }

  void destroy() {
    if (sceneTex_) {
      glDeleteTextures(1, &sceneTex_);
      sceneTex_ = 0;
    }
    if (sceneFBO_) {
      glDeleteFramebuffers(1, &sceneFBO_);
      sceneFBO_ = 0;
    }
    if (program_) {
      glDeleteProgram(program_);
      program_ = 0;
    }
    if (quadVBO_) {
      glDeleteBuffers(1, &quadVBO_);
      quadVBO_ = 0;
    }
    if (quadVAO_) {
      glDeleteVertexArrays(1, &quadVAO_);
      quadVAO_ = 0;
    }
    texSize_ = cv::Size();
  }
};

} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_POST_HPP_
