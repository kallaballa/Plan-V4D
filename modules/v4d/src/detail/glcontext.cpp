// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include "opencv2/v4d/detail/gl.hpp"
#include "opencv2/v4d/detail/glcontext.hpp"

namespace cv {
namespace v4d {
namespace detail {
GLContext::GLContext(const int32_t& idx, cv::Ptr<FrameBufferContext> fbContext) :
        idx_(idx), mainFbContext_(fbContext), glFbContext_(FrameBufferContext::make("OpenGL" + std::to_string(idx), fbContext)) {
}

int GLContext::execute(const cv::Rect& vp, std::function<void()> fn) {
	FrameBufferContext::WindowScope winScope(fbCtx());
	FrameBufferContext::GLScope glScope(fbCtx(), GL_FRAMEBUFFER);
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, vp.size().width, vp.size().height);
	glViewport(vp.x, vp.y, vp.width, vp.height);
	glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	fn();
	glDisable(GL_SCISSOR_TEST);
	return 1;
}

const int32_t& GLContext::getIndex() const {
	return idx_;
}

cv::Ptr<FrameBufferContext> GLContext::fbCtx() {
    return glFbContext_;
}

}
}
}
