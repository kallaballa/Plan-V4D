// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include "../../include/opencv2/v4d/detail/sourcecontext.hpp"
#include "../../include/opencv2/v4d/v4d.hpp"
#include <opencv2/imgproc.hpp>

namespace cv {
namespace v4d {
namespace detail {

SourceContext::SourceContext(cv::Ptr<FrameBufferContext> mainFbContext) : mainFbContext_(mainFbContext) {
}

int SourceContext::execute(const cv::Rect& vp, std::function<void()> fn) {
    CV_UNUSED(vp);
    if(V4D::get<bool>(V4D::Keys::DISABLE_VIDEO_IO))
		return 1;

    const cv::Size sz = V4D::get<cv::Size>(V4D::Keys::SIZE);
    const signed long long fcnt = Global::get<size_t>(Global::Keys::FRAME_CNT);

//    std::cerr << "fcnt / lastFCount: " << fcnt << " / " << lastFCount_ << std::endl;

    if (hasContext()) {
        CLExecScope_t scope(getCLExecContext());
        if(fcnt == lastFCount_) {
            fn();
            return lastIdx_;
        } else if (V4D::instance()->hasSource()) {
        	auto src = V4D::instance()->getSource();

        	if(src->isOpen()) {
				auto p = src->operator ()();
		        CV_Assert(p.first > 0);

		        if(p.second.empty()) {
					CV_Error(cv::Error::StsError, "End of stream");
				}

		        CV_Assert(p.second.type() == CV_8UC3 || p.second.type() == CV_8UC4);

		        cv::resize(p.second, p.second, sz, 0.0, 0.0, cv::INTER_LINEAR);

		        if(p.second.channels() == 3)
		        	cv::cvtColor(p.second, sourceBuffer(), cv::COLOR_RGB2BGRA);
		        else
		        	cv::cvtColor(p.second, sourceBuffer(), cv::COLOR_RGBA2BGRA);
		        fn();
		        lastFCount_ = fcnt;
		        lastIdx_ = p.first;
		        return p.first;
        	}
        }
        return 0;
    } else {
        if(fcnt == lastFCount_) {
            fn();
            return lastIdx_;
        } else if (V4D::instance()->hasSource()) {
        	auto src = V4D::instance()->getSource();

        	if(src->isOpen()) {
				auto p = src->operator ()();
		        CV_Assert(p.first > 0);

				if(p.second.empty()) {
					CV_Error(cv::Error::StsError, "End of stream");
				}

		        CV_Assert(p.second.type() == CV_8UC3 || p.second.type() == CV_8UC4);

		        cv::resize(p.second, p.second, sz, 0.0, 0.0, cv::INTER_LINEAR);

		        if(p.second.channels() == 3)
		        	cv::cvtColor(p.second, sourceBuffer(), cv::COLOR_RGB2BGRA);
		        else
		        	cv::cvtColor(p.second, sourceBuffer(), cv::COLOR_RGBA2BGRA);
		        fn();
                lastIdx_ = p.first;
                lastFCount_ = fcnt;
                return p.first;
        	}
        }
        return 0;
    }
}

bool SourceContext::hasContext() {
    return !context_.empty();
}

void SourceContext::copyContext() {
    context_ = CLExecContext_t::getCurrent();
}

CLExecContext_t SourceContext::getCLExecContext() {
    return context_;
}

cv::UMat& SourceContext::sourceBuffer() {
	return sourceBuffer_;
}
}
}
}
