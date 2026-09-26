// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include <opencv2/v4d/v4d.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/xobjdetect.hpp>
#include <opencv2/video/tracking.hpp>
#include <opencv2/tracking.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

using std::vector;
using std::string;


using namespace cv::v4d;

//One tracked pedestrian: a KCF tracker plus the smoothed bounding box it produces.
struct Track {
	//Smoothed bounding box (in downscaled coordinates)
	cv::Rect smoothed_;
	//KCF tracker
	cv::Ptr<cv::Tracker> tracker_;
	//Consecutive failed updates (a miss is only counted on frames the tracker is
	//actually refreshed, so the real drop latency is missCount * kcfEvery frames)
	int missCount_ = 0;
	//True if this track already consumed a detection during the current detect pass
	//(prevents one track from being matched by several overlapping detections)
	bool matched_ = false;
};

//Per-pedestrian tracking state, shared by all workers. Only ever touched by
//the single worker executing the tracking branch.
struct Detection {
	//brute force detections
	std::vector<cv::Rect> locations_;
	//probability of detected object being a pedestrian - currently always set to 1.0
	std::vector<double> probs_;
	//detected pedestrian locations as boxes
	std::vector<std::vector<double>> boxes_;
	//the active trackers, one per tracked pedestrian
	std::vector<Track> tracks_;
	//Descriptor used for pedestrian detection
	cv::HOGDescriptor hog_;
	//Faster tracking parameters
	cv::TrackerKCF::Params params_;
	//counts processed frames to gate periodic re-detection
	uint64_t detectCnt_ = 0;
};

class PedestrianDemoPlan : public V4DPlan {
private:
	//GUI-tunable tracking parameters. Written on the main thread by gui() (under
	//the shared mutex via RWS) and snapshotted by the tracking worker (via CS).
	struct TrackParams {
		//Frames between HOG re-detection passes while tracks are healthy
		int detectInterval_ = 2;
		//Maximum number of simultaneously tracked pedestrians. Bounds the cost of
		//the per-frame KCF updates (one tracker per pedestrian).
		int maxTracks_ = 15;
		//Consecutive failed tracker updates after which a track is dropped
		int maxMiss_ = 2;
		//Refresh every KCF tracker every kcfEvery_ frames (1 = every frame)
		int kcfEvery_ = 2;
		//Exponential smoothing of the published box toward tracker output
		float smoothFactor_ = 0.1f;
		//Exponential smoothing of a live track's box toward its re-detection
		float anchorFactor_ = 0.5f;
	};
	static TrackParams trackParams_;

	struct Params {
		cv::Size downSize_;
		cv::Size_<float> scale_;
	} params_;

	struct Frames {
		//BGRA
		cv::UMat background_;
    	//RGB
    	cv::UMat videoFrame_, videoFrameBGR_, videoFrameDown_;
    	//GREY
    	cv::UMat videoFrameDownGrey_;
	} frames_;

	class NonMaxSupression {
	private:
		//adapted from cv::dnn_objdetect::InferBbox
		static inline bool pair_comparator(std::pair<double, size_t> l1, std::pair<double, size_t> l2) {
			return l1.first > l2.first;
		}

		//adapted from cv::dnn_objdetect::InferBbox
		static void intersection_over_union(std::vector<std::vector<double> > *boxes, std::vector<double> *base_box, std::vector<double> *iou) {
			double g_xmin = (*base_box)[0];
			double g_ymin = (*base_box)[1];
			double g_xmax = (*base_box)[2];
			double g_ymax = (*base_box)[3];
			double base_box_w = g_xmax - g_xmin;
			double base_box_h = g_ymax - g_ymin;
			for (size_t b = 0; b < (*boxes).size(); ++b) {
				double xmin = std::max((*boxes)[b][0], g_xmin);
				double ymin = std::max((*boxes)[b][1], g_ymin);
				double xmax = std::min((*boxes)[b][2], g_xmax);
				double ymax = std::min((*boxes)[b][3], g_ymax);

				// Intersection
				double w = std::max(static_cast<double>(0.0), xmax - xmin);
				double h = std::max(static_cast<double>(0.0), ymax - ymin);
				// Union
				double test_box_w = (*boxes)[b][2] - (*boxes)[b][0];
				double test_box_h = (*boxes)[b][3] - (*boxes)[b][1];

				double inter_ = w * h;
				double union_ = test_box_h * test_box_w + base_box_h * base_box_w - inter_;
				(*iou)[b] = inter_ / (union_ + 1e-7);
			}
		}
	public:
		//adapted from cv::dnn_objdetect::InferBbox
		std::vector<bool> perform(std::vector<std::vector<double> > *boxes, std::vector<double> *probs, const double threshold = 0.1) {
			std::vector<bool> keep(((*probs).size()));
			std::fill(keep.begin(), keep.end(), true);
			std::vector<size_t> prob_args_sorted((*probs).size());

			std::vector<std::pair<double, size_t> > temp_sort((*probs).size());
			for (size_t tidx = 0; tidx < (*probs).size(); ++tidx) {
				temp_sort[tidx] = std::make_pair((*probs)[tidx], static_cast<size_t>(tidx));
			}
			std::sort(temp_sort.begin(), temp_sort.end(), pair_comparator);

			for (size_t idx = 0; idx < temp_sort.size(); ++idx) {
				prob_args_sorted[idx] = temp_sort[idx].second;
			}

			for (std::vector<size_t>::iterator itr = prob_args_sorted.begin(); itr != prob_args_sorted.end() - 1; ++itr) {
				size_t idx = itr - prob_args_sorted.begin();
				std::vector<double> iou_(prob_args_sorted.size() - idx - 1);
				std::vector<std::vector<double> > temp_boxes(iou_.size());
				for (size_t bb = 0; bb < temp_boxes.size(); ++bb) {
					std::vector<double> temp_box(4);
					for (size_t b = 0; b < 4; ++b) {
						temp_box[b] = (*boxes)[prob_args_sorted[idx + bb + 1]][b];
					}
					temp_boxes[bb] = temp_box;
				}
				intersection_over_union(&temp_boxes, &(*boxes)[prob_args_sorted[idx]], &iou_);
				for (std::vector<double>::iterator _itr = iou_.begin(); _itr != iou_.end(); ++_itr) {
					size_t iou_idx = _itr - iou_.begin();
					if (*_itr > threshold) {
						keep[prob_args_sorted[idx + iou_idx + 1]] = false;
					}
				}
			}
			return keep;
		}
	} nms;

	//Per-pedestrian tracking state, shared by all workers. Only ever touched by
	//the single worker executing the tracking branch.
	inline static Detection detection_;

	//Smoothed bounding boxes of all tracked pedestrians, published by the tracking
	//branch and consumed by the drawing node.
	inline static std::vector<cv::Rect> trackedBoxes_;
	//Scratch buffer of the single worker running the tracking branch.
	std::vector<cv::Rect> outBoxes_;

	Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

	static void prepare_frames(const Params& params, Frames &frames) {
		cv::resize(frames.videoFrameBGR_, frames.videoFrameDown_, params.downSize_);
		cv::cvtColor(frames.videoFrameDown_, frames.videoFrameDownGrey_, cv::COLOR_RGB2GRAY);
		frames.videoFrame_.copyTo(frames.background_);
	}

	static void present(cv::UMat& framebuffer, const cv::UMat& background) {
		cv::add(background, framebuffer, framebuffer);
	};

	static double iou(const cv::Rect& a, const cv::Rect& b) {
		const cv::Rect inter = a & b;
		const double interArea = inter.area();
		const double unionArea = a.area() + b.area() - interArea;
		return unionArea > 0.0 ? interArea / unionArea : 0.0;
	}

	static void smooth(cv::Rect& oldBox, const cv::Rect& newBox, float factor = 0.3f) {
		oldBox.x = cvRound(oldBox.x + factor * (newBox.x - oldBox.x));
		oldBox.y = cvRound(oldBox.y + factor * (newBox.y - oldBox.y));
		oldBox.width = cvRound(oldBox.width + factor * (newBox.width - oldBox.width));
		oldBox.height = cvRound(oldBox.height + factor * (newBox.height - oldBox.height));
	}

	static void erase_dead(std::vector<Track>& tracks, const TrackParams& tp) {
		tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
			[tp](const Track& t) { return t.missCount_ >= tp.maxMiss_; }), tracks.end());
	}

	static void update_tracking(const cv::UMat& videoFrameDownGrey, Detection& detection, NonMaxSupression& nms, const TrackParams& tp, std::vector<cv::Rect>& outBoxes) {
		++detection.detectCnt_;

		const int detectInterval = std::max(1, tp.detectInterval_);
		const int kcfEvery = std::max(1, tp.kcfEvery_);

		//Re-run HOG detection on a fixed cadence (right away on the first frame).
		//The interval is longer than before because the HOG sweep is the most
		//expensive single operation; between passes the KCF trackers carry the state.
		const bool doDetect = detection.detectCnt_ == 1 || (detection.detectCnt_ % size_t(detectInterval)) == 0;

		std::vector<cv::Rect> detections;
		if (doDetect) {
			//Detect pedestrians with the HOG descriptor. Classic groupRectangles
			//grouping (no meanshift) and a coarser scale pyramid are notably faster
			//than the previous settings at a small cost in detection density.
			detection.locations_.clear();
			detection.hog_.detectMultiScale(videoFrameDownGrey, detection.locations_, 0, cv::Size(), cv::Size(), 1.2, 2.0, false);
			if (!detection.locations_.empty()) {
				detection.boxes_.clear();
				detection.probs_.clear();
				for (const auto& rect : detection.locations_) {
					detection.boxes_.push_back( { double(rect.x), double(rect.y), double(rect.x + rect.width), double(rect.y + rect.height) });
					detection.probs_.push_back(1.0);
				}

				//use nms to filter overlapping boxes (https://medium.com/analytics-vidhya/non-max-suppression-nms-6623e6572536)
				std::vector<bool> keep = nms.perform(&detection.boxes_, &detection.probs_, 0.1);
				detections.reserve(keep.size());
				for (size_t i = 0; i < keep.size(); ++i) {
					if (keep[i])
						detections.push_back(detection.locations_[i]);
				}
			}
		}

		//Update the KCF trackers. With many pedestrians the tracker updates dominate
		//the per-frame cost, so trackers are refreshed on a staggered cadence
		//(every kcfEvery_ frames per tracker). At kcfEvery_==2 this halves the
		//tracker load while every track is still refreshed at least once every two
		//frames.
		for (size_t i = 0; i < detection.tracks_.size(); ++i) {
			Track& track = detection.tracks_[i];
			if ((i + size_t(detection.detectCnt_)) % size_t(kcfEvery))
				continue;
			cv::Rect predicted;
			if (track.tracker_->update(videoFrameDownGrey, predicted)) {
				track.missCount_ = 0;
				smooth(track.smoothed_, predicted, tp.smoothFactor_);
			} else {
				++track.missCount_;
			}
		}

		if (doDetect) {
			//Consume each detection at most once so that overlapping detections
			//cannot re-anchor or restart the same track twice.
			for (auto& track : detection.tracks_)
				track.matched_ = false;

			for (const cv::Rect& det : detections) {
				//A detection overlapping a live track re-anchors that tracker.
				auto best = detection.tracks_.end();
				double bestIou = 0.1;
				for (auto it = detection.tracks_.begin(); it != detection.tracks_.end(); ++it) {
					if (it->matched_ || it->missCount_ > 0)
						continue;
					const double ov = iou(det, it->smoothed_);
					if (ov > bestIou) {
						bestIou = ov;
						best = it;
					}
				}
				if (best != detection.tracks_.end()) {
					smooth(best->smoothed_, det, tp.anchorFactor_);
					best->matched_ = true;
					continue;
				}

				//A detection overlapping a lost track re-initializes its tracker.
				best = detection.tracks_.end();
				bestIou = 0.1;
				for (auto it = detection.tracks_.begin(); it != detection.tracks_.end(); ++it) {
					if (it->matched_ || it->missCount_ <= 0)
						continue;
					const double ov = iou(det, it->smoothed_);
					if (ov > bestIou) {
						bestIou = ov;
						best = it;
					}
				}

				if (best != detection.tracks_.end()) {
					best->tracker_ = cv::TrackerKCF::create(detection.params_);
					best->tracker_->init(videoFrameDownGrey, det);
					best->smoothed_ = det;
					best->missCount_ = 0;
					best->matched_ = true;
				} else if (detection.tracks_.size() < size_t(tp.maxTracks_)) {
					//Unmatched detection: start tracking a new pedestrian, unless
					//the per-frame tracker budget is already exhausted.
					Track track;
					track.smoothed_ = det;
					track.tracker_ = cv::TrackerKCF::create(detection.params_);
					track.tracker_->init(videoFrameDownGrey, det);
					detection.tracks_.push_back(std::move(track));
				}
			}
		}

		//Drop tracks that have been lost for too long.
		erase_dead(detection.tracks_, tp);

		//Publish the current pedestrian boxes for rendering.
		outBoxes.clear();
		outBoxes.reserve(detection.tracks_.size());
		for (const auto& track : detection.tracks_)
			outBoxes.push_back(track.smoothed_);
	}

	static void copy_boxes(const std::vector<cv::Rect>& src, std::vector<cv::Rect>& dst) {
		dst = src;
	}

	class ObjectMarker {
	public:
		void draw(const cv::Size& sz, const Params& params, const std::vector<cv::Rect>& trackedBoxes) const {
			//Draw an ellipse around every tracked pedestrian
			using namespace cv::v4d::nvg;
			clearScreen();
			if (trackedBoxes.empty())
				return;
			beginPath();
			strokeWidth(std::fmax(5.0, sz.width / 960.0));
			strokeColor(cv::v4d::convert_pix(cv::Scalar(0, 127, 255, 200), cv::COLOR_HLS2BGR));
			for (const auto& box : trackedBoxes) {
				float width = box.width * params.scale_.width;
				float height = box.height * params.scale_.height;
				float cx = (params.scale_.width * box.x + (width / 2));
				float cy = (params.scale_.height * box.y + (height / 2));
				ellipse(cx, cy, (width / 2), (height / 2));
			}
			stroke();
		}
	} marker_;
public:
	PedestrianDemoPlan() {
		_shared(trackParams_);
	}

	void gui() override {
		imgui([](TrackParams& tp, const std::vector<cv::Rect>& boxes) {
			using namespace ImGui;
			Begin("Tracking");
			SliderInt("Re-detect interval", &tp.detectInterval_, 1, 60);
			SliderInt("Max pedestrians", &tp.maxTracks_, 1, 100);
			SliderInt("Miss threshold", &tp.maxMiss_, 1, 30);
			SliderInt("Tracker refresh period", &tp.kcfEvery_, 1, 4);
			SliderFloat("Box smoothing", &tp.smoothFactor_, 0.05f, 0.95f);
			SliderFloat("Re-anchor factor", &tp.anchorFactor_, 0.05f, 0.95f);
			Text("Active tracks: %zu", boxes.size());
			End();
		}, RWS(trackParams_), CS(trackedBoxes_));
	}

    void setup() override {
    	plain([](const cv::Size& sz, Detection& detection, Frames& frames, Params& params){
    		detection.params_.desc_pca = cv::TrackerKCF::GRAY;
    		detection.params_.compress_feature = false;
    		detection.params_.compressed_size = 1;
    		detection.hog_.setSVMDetector(cv::HOGDescriptor::getDefaultPeopleDetector());
    		params.downSize_ = { sz.width / 4 , sz.height / 4 };
    		params.scale_ = { 2.0f, 2.0f };
    		frames.videoFrame_.create(sz, CV_8UC4);
    		frames.videoFrameBGR_.create(sz, CV_8UC3);
    		frames.videoFrameDownGrey_.create(sz, CV_8UC1);
    	}, size_, RWS(detection_), RW(frames_), RW(params_));
	}

	void infer() override {
		capture(RW(frames_.videoFrame_));

		plain(cv::cvtColor,R(frames_.videoFrame_), RW(frames_.videoFrameBGR_),V(cv::COLOR_BGRA2RGB), V(0), V(cv::ALGO_HINT_DEFAULT))
		->plain(prepare_frames, R(params_), RW(frames_));

		//Detect pedestrians and update the per-pedestrian trackers. Runs on a single
		//worker each frame, so the KCF trackers in shared state (detection_) are only
		//ever touched by one thread. The results are published to trackedBoxes_ which
		//all workers snapshot for rendering.
		branch(BranchType::SINGLE, always_)
			->plain(update_tracking, R(frames_.videoFrameDownGrey_), RWS(detection_), RW(nms), CS(trackParams_), RW(outBoxes_))
			->plain(copy_boxes, R(outBoxes_), RWS(trackedBoxes_))
		->endBranch();

		nvg(&ObjectMarker::draw, R(marker_), size_, R(params_), CS(trackedBoxes_))
        ->fb(present, R(frames_.background_));

		write();
	}
};


PedestrianDemoPlan::TrackParams PedestrianDemoPlan::trackParams_;

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: pedestrian-demo <video-input>" << std::endl;
        exit(1);
    }

    cv::Rect viewport(0, 0, 1920, 1080);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Pedestrian Demo", AllocateFlags::NANOVG | AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);
    auto src = Source::make(runtime, argv[1]);
//    auto sink = Sink::make(runtime, "pedestrian-demo.mkv", 60, viewport.size());
    runtime->setSource(src);
//    runtime->setSink(sink);
    V4DPlan::run<PedestrianDemoPlan>(2);
    return 0;
}
