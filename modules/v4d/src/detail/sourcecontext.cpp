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

    const cv::Size sz = V4D::get<cv::Size>(V4D::Keys::SIZE);
    if(sourceBuffer().empty()) {
        sourceBuffer().create(sz, CV_8UC4);
    }

    if (hasContext()) {
        CLExecScope_t scope(getCLExecContext());
        if (V4D::instance()->hasSource()) {
            auto src = V4D::instance()->getSource();

            //only one capture per sequence
            if(src->isOpen()) {
                auto frame = src->operator ()();

                if(frame.empty()) {
                    CV_Error(cv::Error::StsError, "End of stream");
                }

                CV_Assert(frame.type() == CV_8UC3 || frame.type() == CV_8UC4);

                if(vp.size() != sz) {
                    sourceBuffer().setTo(cv::Scalar(0,0,0,0));
                }

                cv::resize(frame, frame, vp.size(), 0.0, 0.0, cv::INTER_LINEAR);

                cv::Rect flipped = cv::Rect(vp.x, sz.height - (vp.y + vp.height), vp.width, vp.height);
                if(frame.channels() == 3)
                    cv::cvtColor(frame, sourceBuffer()(flipped), cv::COLOR_RGB2BGRA);
                else
                    cv::cvtColor(frame, sourceBuffer()(flipped), cv::COLOR_RGBA2BGRA);

                GlobalState::apply<uint64_t>(GlobalState::Keys::CAPTURE_CNT, [](uint64_t& v){
                    ++v;
                    return v;
                });
            }
            fn();
            return 1;

        }
        return 0;
    } else {
        if (V4D::instance()->hasSource()) {
            auto src = V4D::instance()->getSource();

            //only one capture per sequence
            if(src->isOpen()) {
                auto frame = src->operator ()();

                if(frame.empty()) {
                    CV_Error(cv::Error::StsError, "End of stream");
                }

                CV_Assert(frame.type() == CV_8UC3 || frame.type() == CV_8UC4);

                if(vp.size() != sz) {
                    sourceBuffer().setTo(cv::Scalar(0,0,0,0));
                }

                cv::resize(frame, frame, vp.size(), 0.0, 0.0, cv::INTER_LINEAR);

                cv::Rect flipped = cv::Rect(vp.x, sz.height - (vp.y + vp.height), vp.width, vp.height);
                if(frame.channels() == 3)
                    cv::cvtColor(frame, sourceBuffer()(flipped), cv::COLOR_RGB2BGRA);
                else
                    cv::cvtColor(frame, sourceBuffer()(flipped), cv::COLOR_RGBA2BGRA);

                GlobalState::apply<uint64_t>(GlobalState::Keys::CAPTURE_CNT, [](uint64_t& v){
                    ++v;
                    return v;
                });
            }
            fn();

            return 1;
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
