// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include <opencv2/v4d/v4d.hpp>
#include <opencv2/video/tracking.hpp>
#include <opencv2/video/background_segm.hpp>
#include <opencv2/geometry/2d.hpp>

#include <string>

using std::vector;
using std::string;


using namespace cv::v4d;

class PedestrianDemoPlan : public V4DPlan {
private:
	struct Params {
		cv::Size downSize_;
		cv::Size_<float> scale_;
		cv::Rect newTracked_;
	} params_;

	struct Frames {
		//BGRA
		cv::UMat background_;
		//RGB
		cv::UMat videoFrame_, videoFrameBGR_, videoFrameDown_;
		//GREY
		cv::UMat videoFrameDownGrey_;
	} frames_;

    struct Detection {
		//detected pedestrian locations rectangles
		std::vector<cv::Rect> locations_;
		//detected pedestrian locations as boxes
		vector<vector<double>> boxes_;
		//probability of detected object being a pedestrian - currently always set to 1.0
		vector<double> probs_;
		//MIL tracker used for tracking the detected pedestrian
		cv::Ptr<cv::TrackerMIL> tracker_;
		//initialize tracker only once
		bool trackerInit_ = false;
		//If tracking fails re-detect
		bool redetect_ = true;
		//Background subtractor used for pedestrian detection
		cv::Ptr<cv::BackgroundSubtractorMOG2> bgSubtractor_;
		//Foreground mask used for detection
		cv::UMat fgMask_;
    } detection_;

    inline static cv::Rect tracked_ = cv::Rect(0,0,0,0);

    constexpr static auto dontRedect_ = [](const Detection& detection){ return detection.trackerInit_ && !detection.redetect_; };
    constexpr static auto doRedect_ = [](const Detection& detection){ return !detection.trackerInit_ || detection.redetect_; };

	Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

	static void prepare_frames(const Params& params, Frames &frames) {
		cv::resize(frames.videoFrameBGR_, frames.videoFrameDown_, params.downSize_);
		cv::cvtColor(frames.videoFrameDown_, frames.videoFrameDownGrey_, cv::COLOR_RGB2GRAY);
		frames.videoFrame_.copyTo(frames.background_);
	}

	static void present(cv::UMat& framebuffer, const cv::UMat& background) {
		cv::add(background, framebuffer, framebuffer);
	};

	class Detector {
	public:
		void detect(const cv::UMat& videoFrameDownGrey, Detection& detection, Params& params) const {
			detection.redetect_ = true;
			//Apply background subtraction to detect moving foreground objects
			detection.bgSubtractor_->apply(videoFrameDownGrey, detection.fgMask_);
			//Threshold to clean up the mask
			cv::threshold(detection.fgMask_, detection.fgMask_, 200, 255, cv::THRESH_BINARY);

			//Find contours of moving objects
			std::vector<std::vector<cv::Point>> contours;
			cv::findContours(detection.fgMask_, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

			if (!contours.empty()) {
				//Find the largest contour as the most likely pedestrian
				double maxArea = 0;
				int maxIdx = -1;
				for (size_t i = 0; i < contours.size(); ++i) {
					double area = cv::contourArea(contours[i]);
					if (area > maxArea) {
						maxArea = area;
						maxIdx = i;
					}
				}

				if (maxIdx >= 0 && maxArea > 100) {
					cv::Rect bbox = cv::boundingRect(contours[maxIdx]);
					detection.locations_.push_back(bbox);
					detection.boxes_.push_back({double(bbox.x), double(bbox.y), double(bbox.x + bbox.width), double(bbox.y + bbox.height)});
					detection.probs_.push_back(1.0);

					params.newTracked_ = bbox;
					detection.redetect_ = false;

					// Create a fresh tracker instance for the new target (OpenCV trackers cannot be re-initialized)
					cv::TrackerMIL::Params milParams;
					milParams.samplerInitInRadius = 6;
					milParams.samplerSearchWinSize = 25;
					milParams.samplerInitMaxNegNum = 65;
					detection.tracker_ = cv::TrackerMIL::create(milParams);
					bool trackerCreated = !detection.tracker_.empty();
					try {
						if(!detection.trackerInit_) {
			                                cv::TrackerMIL::Params milParams;
		                                        milParams.samplerInitInRadius = 6;
                                		        milParams.samplerSearchWinSize = 25;
                		                        milParams.samplerInitMaxNegNum = 65;
		                                        detection.tracker_ = cv::TrackerMIL::create(milParams);
							detection.tracker_->init(videoFrameDownGrey, params.newTracked_);
							detection.trackerInit_ = true;
						} else 
							detection.redetect_ = true;
					} catch (...) {
					   std::cerr << "Tracker not usuable" << std::endl;
					   detection.trackerInit_ = false;
                                           detection.redetect_ = true;
					}
				}
			}
		}
	} detector_;

	class Tracking {
	private:
	    void limitFunc(const double& in, const double& max, const int& val, int& limited) const {
            if(fabs(in) > max) {
                if(in < 0.0) {
                    limited = std::round(val + (max / 2.0));
                } else {
                    limited = std::round(val - (max / 2.0));
                }
            } else {
                if(in < 0.0) {
                    limited = std::round(val + (in / 2.0));
                } else {
                    limited = std::round(val - (in / 2.0));
                }
            }
	    }

	public:
		void perform(const cv::UMat& videoFrameDownGrey, Detection& detection, Params& params, const cv::Rect& tracked) const {
			params.newTracked_ = tracked;
			if(params.newTracked_.width == 0 || params.newTracked_.height == 0 || !detection.tracker_->update(videoFrameDownGrey, params.newTracked_)) {
				detection.redetect_ = true;
				detection.trackerInit_ = false;
			} else {
				detection.redetect_ = false;
			}
		}

		void save(const Params& params, const cv::Size& sz, cv::Rect& tracked) const {
		    const cv::Rect oldTracked = tracked;

		    const double diffX = oldTracked.x - params.newTracked_.x;
			const double diffY = oldTracked.y - params.newTracked_.y;
			const double diffW = oldTracked.width - params.newTracked_.width;
			const double diffH = oldTracked.height - params.newTracked_.height;
			const double excenter = std::hypotf(diffX, diffY);
			
			if(excenter > ((sz.width + sz.height) / 160.0)) {
                limitFunc(diffX, excenter / 3.0, oldTracked.x, tracked.x);
                limitFunc(diffY, excenter / 3.0, oldTracked.y, tracked.y);
                limitFunc(diffW, excenter / 3.0, oldTracked.width, tracked.width);
                limitFunc(diffH, excenter / 3.0, oldTracked.height, tracked.height);
			}
		}
	} tracking;

	class ObjectMarker {
	public:
		void draw(const cv::Size& sz, const Params& params, const cv::Rect& tracked) const {
		//Draw an ellipse around the tracked pedestrian
		using namespace cv::v4d::nvg;
		float width = tracked.width * params.scale_.width;
		float height = tracked.height * params.scale_.height;
		float cx = (params.scale_.width * tracked.x + (width / 2));
		float cy = (params.scale_.height * tracked.y + (height / 2));
		clearScreen();
		beginPath();
		strokeWidth(std::fmax(5.0, sz.width / 960.0));
		strokeColor(cv::v4d::convert_pix(cv::Scalar(0, 127, 255, 200), cv::COLOR_HLS2BGR));
		ellipse(cx, cy, (width / 1.25), (height / 1.5));
		stroke();
		}
	} marker_;
public:
    void setup() override {
    	plain([](const cv::Size& sz, Detection& detection, Frames& frames, Params& params){
    		detection.tracker_ = cv::TrackerMIL::create();
    		detection.bgSubtractor_ = cv::createBackgroundSubtractorMOG2(500, 16, false);
    		params.downSize_ = { sz.width / 4 , sz.height / 4 };
    		params.scale_ = { 4.0f, 4.0f };
    		frames.videoFrame_.create(sz, CV_8UC4);
    		frames.videoFrameBGR_.create(sz, CV_8UC3);
    		frames.videoFrameDownGrey_.create(sz, CV_8UC1);
    		detection.fgMask_.create(params.downSize_, CV_8UC1);
    	}, size_, RW(detection_), RW(frames_), RW(params_));
	}

	void infer() override {
		capture(RW(frames_.videoFrame_));

		plain(cv::cvtColor,R(frames_.videoFrame_), RW(frames_.videoFrameBGR_),V(cv::COLOR_BGRA2RGB), V(0), V(cv::ALGO_HINT_DEFAULT))
		->plain(prepare_frames, R(params_), RW(frames_));

		//Try to track the pedestrian (if we currently are tracking one), else re-detect using background subtraction
		branch(BranchType::SINGLE, doRedect_, R(detection_))
			->plain(&Detector::detect, R(detector_), R(frames_.videoFrameDownGrey_), RW(detection_), RW(params_))
		->elseBranch()
			->plain(&Tracking::perform, R(tracking), R(frames_.videoFrameDownGrey_), RW(detection_), RW(params_), CS(tracked_))
		->endBranch();

		plain(&Tracking::save, R(tracking), R(params_), size_, RWS(tracked_))
        ->nvg(&ObjectMarker::draw, R(marker_), size_, R(params_), CS(tracked_))
        ->fb(present, R(frames_.background_));

		write();
	}
};


int main(int argc, char **argv) {
  cv::samples::addSamplesDataSearchPath(V4D_ASSETS_PATH);

  std::string videoFile = (argc > 1) ? argv[1] : cv::samples::findFile("videos/dance.mp4");
  if (videoFile.empty()) {
      std::cerr << "Usage: pedestrian-demo <video-input>" << std::endl;
      return 1;
  }

  cv::Rect viewport(0, 0, 1920, 1080);
  cv::Ptr<V4D> runtime = V4D::init(viewport, "Pedestrian Demo", AllocateFlags::NANOVG | AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);
  auto src = Source::make(runtime, videoFile);
//    auto sink = Sink::make(runtime, "pedestrian-demo.mkv", 60, viewport.size());
    runtime->setSource(src);
//    runtime->setSink(sink);
    V4DPlan::run<PedestrianDemoPlan>(2);
    return 0;
}
