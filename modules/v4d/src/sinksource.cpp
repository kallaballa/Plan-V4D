// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include "opencv2/v4d/sinksource.hpp"
#include "opencv2/v4d/v4d.hpp"
#include "opencv2/v4d/detail/sourcecontext.hpp"
#include <opencv2/core/utils/logger.hpp>

namespace cv {
namespace v4d {

cv::Ptr<SinkSource> SinkSource::makeVaSinkSource(cv::Ptr<V4D> window, const string& inputFilename, const string& outputFilename, const int fourcc, const int vaDeviceIndex) {
    cv::Ptr<cv::VideoCapture> capture = new cv::VideoCapture(inputFilename, cv::CAP_FFMPEG, {
            cv::CAP_PROP_HW_DEVICE, vaDeviceIndex, cv::CAP_PROP_HW_ACCELERATION,
            cv::VIDEO_ACCELERATION_VAAPI, cv::CAP_PROP_HW_ACCELERATION_USE_OPENCL, 1 });
    float fps = capture->get(cv::CAP_PROP_FPS);
    cv::Size frameSize(capture->get(cv::CAP_PROP_FRAME_WIDTH), capture->get(cv::CAP_PROP_FRAME_HEIGHT));

    std::dynamic_pointer_cast<detail::SourceContext>(window->sourceCtx())->copyContext();

    CV_LOG_INFO(nullptr, "Using a VA SinkSource");

    cv::Ptr<cv::VideoWriter> writer = new cv::VideoWriter(outputFilename, cv::CAP_FFMPEG,
            fourcc, fps, frameSize, {
                    cv::VIDEOWRITER_PROP_HW_DEVICE, vaDeviceIndex,
                    cv::VIDEOWRITER_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_VAAPI,
                    cv::VIDEOWRITER_PROP_HW_ACCELERATION_USE_OPENCL, 1 });

    if(!writer->isOpened()) {
        CV_Error(cv::Error::StsError, "Unable to initialize video sink in SinkSource.");
    }

    return new SinkSource([=](cv::UMat& frame) {
        (*capture) >> frame;
        return !frame.empty();
    }, fps, [=](const uint64_t& seq, const cv::UMat& frame) {
        cv::UMat context_corrected;
        cv::UMat converted;

        frame.copyTo(context_corrected);
        cv::resize(context_corrected, converted, frameSize);
        cvtColor(converted, converted, cv::COLOR_RGBA2RGB);

        (*writer) << converted;
        if(!writer->isOpened()) {
            writer->release();
            CV_Error(cv::Error::StsError, "Video write failed in SinkSource.");
        }
        return true;
    });
}

cv::Ptr<SinkSource> SinkSource::makeAnyHWSinkSource(const string& inputFilename, const string& outputFilename, const int fourcc) {
    cv::Ptr<cv::VideoCapture> capture = new cv::VideoCapture(inputFilename, cv::CAP_FFMPEG, {
            cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY
    });
    float fps = capture->get(cv::CAP_PROP_FPS);
    cv::Size frameSize(capture->get(cv::CAP_PROP_FRAME_WIDTH), capture->get(cv::CAP_PROP_FRAME_HEIGHT));

    cv::Ptr<cv::VideoWriter> writer = new cv::VideoWriter(outputFilename, cv::CAP_FFMPEG,
            fourcc, fps, frameSize, { cv::VIDEOWRITER_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY });

    if(!writer->isOpened()) {
        CV_Error(cv::Error::StsError, "Unable to initialize video sink in SinkSource.");
    }

    return new SinkSource([=](cv::UMat& frame) {
        (*capture) >> frame;
        return !frame.empty();
    }, fps, [=](const uint64_t& seq, const cv::UMat& frame) {
        cv::UMat context_corrected;
        cv::UMat converted;

        frame.copyTo(context_corrected);
        cv::resize(context_corrected, converted, frameSize);
        cvtColor(converted, converted, cv::COLOR_RGBA2RGB);

        (*writer) << converted;
        if(!writer->isOpened()) {
            writer->release();
            CV_Error(cv::Error::StsError, "Video write failed in SinkSource.");
        }
        return true;
    });
}

cv::Ptr<SinkSource> SinkSource::make(cv::Ptr<V4D> window, const string& inputFilename, const string& outputFilename) {
    int fourcc = 0;
    if(get_gl_vendor() == "NVIDIA Corporation") {
        fourcc = cv::VideoWriter::fourcc('H', '2', '6', '4');
    } else {
        fourcc = cv::VideoWriter::fourcc('V', 'P', '9', '0');
    }

    return make(window, inputFilename, outputFilename, fourcc);
}

cv::Ptr<SinkSource> SinkSource::make(cv::Ptr<V4D> window, const string& inputFilename, const string& outputFilename, int fourcc) {
#ifdef HAVE_VA
    if (is_intel_va_supported()) {
        return makeVaSinkSource(window, inputFilename, outputFilename, fourcc, 0);
    } else
#endif
    {
        try {
            return makeAnyHWSinkSource(inputFilename, outputFilename, fourcc);
        } catch(...) {
            CV_LOG_INFO(nullptr, "Failed to create hardware SinkSource");
        }
    }

    cv::Ptr<cv::VideoCapture> capture = new cv::VideoCapture(inputFilename, cv::CAP_FFMPEG);
    float fps = capture->get(cv::CAP_PROP_FPS);
    cv::Size frameSize(capture->get(cv::CAP_PROP_FRAME_WIDTH), capture->get(cv::CAP_PROP_FRAME_HEIGHT));

    cv::Ptr<cv::VideoWriter> writer = new cv::VideoWriter(outputFilename, cv::CAP_FFMPEG,
            fourcc, fps, frameSize);

    if(!writer->isOpened()) {
        CV_Error(cv::Error::StsError, "Unable to initialize video sink in SinkSource.");
    }

    return new SinkSource([=](cv::UMat& frame) {
        (*capture) >> frame;
        return !frame.empty();
    }, fps, [=](const uint64_t& seq, const cv::UMat& frame) {
        cv::UMat converted;
        cv::resize(frame, converted, frameSize);
        cvtColor(converted, converted, cv::COLOR_RGBA2RGB);

        (*writer) << converted;
        if(!writer->isOpened()) {
            writer->release();
            CV_Error(cv::Error::StsError, "Video write failed in SinkSource.");
        }
        return true;
    });
}

SinkSource::SinkSource(std::function<bool(cv::UMat&)> generator, float fps, std::function<bool(const uint64_t&, const cv::UMat&)> consumer) :
        generator_(generator), fps_(fps), consumer_(consumer) {
}

SinkSource::SinkSource() :
        open_(false), fps_(0) {
}

SinkSource::SinkSource(float fps) :
        open_(true), fps_(fps) {
}

void SinkSource::close() {
    std::lock_guard<std::mutex> lock(mtx_);
    open_ = false;
    cv_.notify_all();
}

SinkSource::~SinkSource() {
}

bool SinkSource::isReady() {
    std::lock_guard<std::mutex> guard(mtx_);
    return generator_ && consumer_;
}

bool SinkSource::isOpen() {
    std::lock_guard<std::mutex> guard(mtx_);
    return open_;
}

float SinkSource::fps() {
    return fps_;
}

cv::UMat SinkSource::operator()() {
    std::unique_lock<std::mutex> guard(mtx_);
    cv_.wait(guard, [this]() { return !buffer_.empty() || !open_ || generator_; });

    if(!buffer_.empty()) {
        auto it = buffer_.find(nextSeq_);
        if(it != buffer_.end()) {
            cv::UMat frame = it->second.clone();
            buffer_.erase(it);
            ++nextSeq_;
            while((it = buffer_.find(nextSeq_)) != buffer_.end()) {
                buffer_.erase(it);
                ++nextSeq_;
            }
            return frame;
        }
    }

    if(generator_) {
        guard.unlock();
        open_ = generator_(frame_);
        guard.lock();
        return frame_;
    }

    return cv::UMat();
}

void SinkSource::operator()(const uint64_t& seq, const cv::UMat& frame) {
    std::lock_guard<std::mutex> lock(mtx_);
    buffer_[seq] = frame;
    cv_.notify_one();

    if(consumer_) {
        auto it = buffer_.find(nextSeq_);
        while(it != buffer_.end()) {
            open_ = consumer_(nextSeq_, it->second);
            buffer_.erase(it);
            ++nextSeq_;
            it = buffer_.find(nextSeq_);
        }
    }

    if(buffer_.size() > 300) {
        CV_LOG_WARNING(nullptr, "Buffer overrun in SinkSource.");
        buffer_.clear();
    }
}

cv::Ptr<SinkSource> SinkSource::make(float fps) {
    return new SinkSource(fps);
}

} /* namespace v4d */
} /* namespace cv */
