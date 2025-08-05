// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include <opencv2/v4d/v4d.hpp>
#include <opencv2/core/utility.hpp>

#include <opencv2/features2d.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/optflow.hpp>

#include <cmath>
#include <vector>
#include <set>
#include <string>
#include <random>
#include <tuple>
#include <array>
#include <utility>

using std::vector;
using std::string;

using namespace cv::v4d;

struct GlowEffect {
	struct Temp {
		cv::UMat src_;
		cv::UMat dst_;
		cv::UMat dst16_;
		cv::UMat high_;
		cv::UMat blur_;
		cv::UMat low_;
	} temp_;
public:

	//Glow post-processing effect
	void perform(const cv::UMat& srcFloat, cv::UMat& dstFloat, const int& ksize, const int& gain) {
		srcFloat.convertTo(temp_.src_, CV_8U, 127.0);

		cv::bitwise_not(temp_.src_, temp_.dst_);

	    //Resize for some extra performance
	    cv::resize(temp_.dst_, temp_.low_, cv::Size(), 0.5, 0.5);
	    //Cheap blur
	    cv::boxFilter(temp_.low_, temp_.blur_, -1, cv::Size(ksize, ksize), cv::Point(-1,-1), true, cv::BORDER_REPLICATE);
	    //Back to original size
	    cv::resize(temp_.blur_, temp_.high_, srcFloat.size());

	    //Multiply the src with a blurred version of itself and convert back to CV_8U
	    cv::multiply(temp_.dst_, temp_.high_, temp_.dst_, 1.0/255.0, CV_8U);

	    cv::bitwise_not(temp_.dst_, temp_.dst_);
        temp_.dst_.convertTo(dstFloat, CV_32F, 1.0/255.0);
        //apply gain
        cv::multiply(dstFloat, 0.01 * gain, dstFloat);
        // add the images and truncate to 1.0
        cv::add(srcFloat, dstFloat, dstFloat);
        cv::threshold(dstFloat, dstFloat, 1.0, 0.0, cv::THRESH_TRUNC);
	}
};

struct BloomEffect {
	struct Temp {
		cv::UMat bgrFloat_;
		cv::UMat hlsFloat_;
		cv::UMat lsFoat_;
		cv::UMat ls16_;
		cv::UMat blur_;
		cv::UMat logFloat_;
		cv::UMat baseFloat_;
		cv::UMat blurFloat_;
		std::vector<cv::UMat> hlsChannels_;
	} temp_;
public:
	//Bloom post-processing effect
	void perform(const cv::UMat& srcFloat, cv::UMat &dstFloat, int ksize = 3, float gain = 4) {
	    //remove alpha channel
        cv::cvtColor(srcFloat, temp_.bgrFloat_, cv::COLOR_BGRA2BGR);
        //convert to hls
        cv::cvtColor(temp_.bgrFloat_, temp_.hlsFloat_, cv::COLOR_BGR2HLS);
        //split channels
        cv::split(temp_.hlsFloat_, temp_.hlsChannels_);
        //multiply lightness and saturation and convert to 16U
        cv::multiply(temp_.hlsChannels_[1], temp_.hlsChannels_[2], temp_.lsFoat_);
        //convert to U8 for faster blur
        temp_.lsFoat_.convertTo(temp_.blur_, CV_8U, 255);
        //blur
        cv::boxFilter(temp_.blur_, temp_.blur_, -1, cv::Size(ksize, ksize), cv::Point(-1,-1), true, cv::BORDER_REPLICATE);
        //convert to BGRA
        cv::cvtColor(temp_.blur_, temp_.blur_, cv::COLOR_GRAY2BGRA);
        //convert to float and apply gain
        temp_.blur_.convertTo(temp_.blurFloat_, CV_32F, 1.0/255.0);
        // apply gain
        cv::multiply(temp_.blurFloat_, 0.01 * gain, temp_.blurFloat_);
        // add the images and truncate to 1.0
        cv::add(srcFloat, temp_.blurFloat_, dstFloat);
        cv::threshold(dstFloat, dstFloat, 1.0, 0.0, cv::THRESH_TRUNC);
	}
};

struct PostProcessor {
    struct Temp {
        cv::UMat bgrFloat_;
        cv::UMat hlsFloat_;
        std::vector<cv::UMat> hlsChannels_;
    } temp_;

	GlowEffect glow_;
	BloomEffect bloom_;
public:
	//Post-processing modes for the foreground
	enum Modes {
	    GLOW,
	    BLOOM,
	    DISABLED
	};

	void perform(const cv::UMat& srcFloat, cv::UMat& dstFloat, const Modes& mode, const int& ksize, const int& gain) {
	    switch (mode) {
	    case GLOW:
	        glow_.perform(srcFloat, dstFloat, ksize, gain);
	        break;
	    case BLOOM:
	        bloom_.perform(srcFloat, dstFloat, ksize, gain);
	        break;
	    case DISABLED:
	        srcFloat.copyTo(dstFloat);
	        break;
	    default:
	        break;
	    }
	}
};

class FeaturePoints {
	cv::Ptr<cv::FastFeatureDetector> detector_;
	vector<cv::KeyPoint> tmpKeyPoints_;
public:
	FeaturePoints() {
	}

	FeaturePoints(cv::Ptr<cv::FastFeatureDetector> detector) : detector_(detector) {
	}

	void detect(const cv::UMat& src, vector<cv::Point2f>& output) {
		detector_->detect(src, tmpKeyPoints_);

	    output.clear();
	    for (const auto &kp : tmpKeyPoints_) {
	        output.push_back(kp.pt);
	    }
	}
};

class SceneChange {
	float lastMovement_ = 0;
public:
	bool detect(const std::vector<cv::Point2f>& detectedPoints, const float& sceneChangeThresh, const float& sceneChangeThreshDiff, const cv::Size& sz) {
	    float movement = detectedPoints.size() / float(sz.width * sz.height);
	    float relation = movement > 0 && lastMovement_ > 0 ? std::max(movement, lastMovement_) / std::min(movement, lastMovement_) : 0;
	    float relM = relation * log10(1.0f + (movement * 9.0));
	    float relLM = relation * log10(1.0f + (lastMovement_ * 9.0));

	    bool result = ((movement > 0 && lastMovement_ > 0 && relation > 0)
	            && (relM < sceneChangeThresh && relLM < sceneChangeThresh && fabs(relM - relLM) < sceneChangeThreshDiff));

	    lastMovement_ = (lastMovement_ + movement) / 2.0f;
	    return !result;
	}
};

class BackgroundStyle {
	struct Temp {
	    cv::UMat tmp_;
	    cv::UMat post_;
	    cv::UMat backgroundGrey_;
	    vector<cv::UMat> channels_;
	} temp_;

public:
	enum Modes {
	    GREY,
	    COLOR,
	    VALUE,
	    BLACK
	};

	void apply(const cv::UMat& srcFloat, cv::UMat& dstFloat, const Modes& bgMode) {
	    //Dependin on bgMode prepare the background in different ways

		switch (bgMode) {
	    case GREY:
	        cv::cvtColor(srcFloat, temp_.backgroundGrey_, cv::COLOR_BGRA2GRAY);
	        cv::cvtColor(temp_.backgroundGrey_, dstFloat, cv::COLOR_GRAY2BGRA);
	        break;
	    case VALUE:
	        cv::cvtColor(srcFloat, temp_.tmp_, cv::COLOR_BGRA2BGR);
	        cv::cvtColor(temp_.tmp_, temp_.tmp_, cv::COLOR_BGR2HSV);
 	        split(temp_.tmp_, temp_.channels_);
	        cv::cvtColor(temp_.channels_[2], dstFloat, cv::COLOR_GRAY2BGRA);
	        break;
	    case COLOR:
	    	srcFloat.copyTo(dstFloat);
	        break;
	    case BLACK:
	    	dstFloat = cv::Scalar::all(0);
	        break;
	    default:
	        break;
	    }
	}
};

class Compositor {
	BackgroundStyle backgroundStyle_;
	PostProcessor postProcessor_;

	struct Temp {
		cv::UMat bgFloat_;
		cv::UMat fgFloat_;
		cv::UMat fbFloat_;
		cv::UMat oldFgFloat_;
	} temp_;
public:
	//Compose the different layers into the final image
	void perform(const cv::UMat& background, const cv::UMat& oldForeground, cv::UMat& foreground, cv::UMat& composed, cv::UMat& framebuffer, const BackgroundStyle::Modes& bgMode, const PostProcessor::Modes& ppMode, const int& ksize, const int& gain) {
		background.convertTo(temp_.bgFloat_, CV_32F, 1.0/255.0);
		backgroundStyle_.apply(temp_.bgFloat_, temp_.bgFloat_, bgMode);

		if(!foreground.empty()) {
		    foreground.convertTo(temp_.fgFloat_, CV_32F, 1.0/255.0);

		    postProcessor_.perform(temp_.fgFloat_, temp_.fgFloat_, ppMode, ksize, gain);
		} else {
		    temp_.bgFloat_.copyTo(temp_.fgFloat_);
		}

		oldForeground.convertTo(temp_.oldFgFloat_, CV_32F, 1.0/255.0);
	    cv::addWeighted(temp_.fgFloat_, 0.3333, temp_.oldFgFloat_, 0.6667, 0.0, temp_.fgFloat_, -1);
        cv::multiply(temp_.fgFloat_, 1.3333, temp_.fgFloat_);
	    temp_.fgFloat_.convertTo(foreground, CV_8U, 255.0);

        cv::add(temp_.bgFloat_, temp_.fgFloat_, temp_.fbFloat_);
        temp_.fbFloat_.convertTo(composed, CV_8U, 255.0);
	    composed.copyTo(framebuffer);
	}
};

class SparseOpticalFlow {
	struct Temp {
	    vector<cv::Point2f> hull_;
	    vector<cv::Point2f> nextPoints_, trimmedPoints_;;
		vector<std::tuple<float, int, cv::Point2f>> prevPoints_;
		vector<std::tuple<float, int, cv::Point2f>> newPoints_;
		vector<cv::Point2f> upTrimmedPoints_, upNextPoints_;
		std::vector<uchar> status_;
		std::vector<float> err_;
	} temp_;

	std::random_device rd_;
	std::mt19937 rng_;
public:
	SparseOpticalFlow() : rng_(rd_()) {

	}

	//Visualize the sparse optical flow
	void visualize(const cv::UMat &prevGrey, const cv::UMat &nextGrey, const vector<cv::Point2f> &detectedPoints, const float& maxStroke, const size_t& maxPoints, const float& pointLoss, cv::Scalar_<float> effectColor) {
		//less then 5 points is a degenerate case (e.g. the corners of a video frame)
	    if (detectedPoints.size() > 4) {
	        cv::convexHull(detectedPoints, temp_.hull_, false, true);
	        float area = cv::contourArea(temp_.hull_);
	        //make sure the area of the point cloud is positive
	        if (area > 0) {
	            float density = (detectedPoints.size() / area);
	            //stroke size is biased by the area of the point cloud
	            float strokeSize = maxStroke * pow(area / (nextGrey.cols * nextGrey.rows), 0.33f);
	            //max points is biased by the densitiy of the point cloud
	            size_t currentMaxPoints = ceil(density * maxPoints);

	            //lose a number of random points specified by pointLossPercent
	            std::shuffle(temp_.prevPoints_.begin(), temp_.prevPoints_.end(), rng_);
	            temp_.prevPoints_.resize(ceil(temp_.prevPoints_.size() * (1.0f - (pointLoss / 100.0f))));
	            temp_.trimmedPoints_.clear();
	            for(size_t i = 0; i < temp_.prevPoints_.size(); ++i) {
	            	temp_.trimmedPoints_.push_back(std::get<2>(temp_.prevPoints_[i]));
	            }

	            //calculate how many newly detected points to add
	            size_t copyn = std::min(detectedPoints.size(), (size_t(std::ceil(currentMaxPoints)) - temp_.trimmedPoints_.size()));
	            if (temp_.trimmedPoints_.size() < currentMaxPoints) {
	                std::copy(detectedPoints.begin(), detectedPoints.begin() + copyn, std::back_inserter(temp_.trimmedPoints_));
	            }

	            //calculate the sparse optical flow
	            cv::calcOpticalFlowPyrLK(prevGrey, nextGrey, temp_.trimmedPoints_, temp_.nextPoints_, temp_.status_, temp_.err_);
	            temp_.newPoints_.clear();
	            if (temp_.trimmedPoints_.size() > 1 && temp_.nextPoints_.size() > 1) {
	                //scale the points to original size
	            	temp_.upNextPoints_.clear();
	            	temp_.upTrimmedPoints_.clear();
	                for (cv::Point2f pt : temp_.trimmedPoints_) {
	                	temp_.upTrimmedPoints_.push_back(pt);
	                }

	                for (cv::Point2f pt : temp_.nextPoints_) {
	                	temp_.upNextPoints_.push_back(pt);
	                }

	                for (size_t i = 0; i < temp_.trimmedPoints_.size(); i++) {
	                    if (temp_.status_[i] == 1 //point was found in prev and new set
	                            && temp_.err_[i] < (1.0 / density) //with a higher density be more sensitive to the feature error
	                            && temp_.upNextPoints_[i].y >= 0 && temp_.upNextPoints_[i].x >= 0 //check bounds
	                            && temp_.upNextPoints_[i].y < nextGrey.rows && temp_.upNextPoints_[i].x < nextGrey.cols //check bounds
	                            ) {
	                        float len = hypot(fabs(temp_.upTrimmedPoints_[i].x - temp_.upNextPoints_[i].x), fabs(temp_.upTrimmedPoints_[i].y - temp_.upNextPoints_[i].y));
	                        if(len > strokeSize) {
	                        	temp_.newPoints_.push_back({len, i, temp_.nextPoints_[i]});
	                        }
	                    }
	                }
//	                std::cerr << "new points:" << temp_.newPoints_.size() << std::endl;
	                if(temp_.newPoints_.empty())
	                	return;
	                float total = 0;
	                float mean = 0;
	                for (size_t i = 0; i < temp_.newPoints_.size(); i++) {
	                	total += std::get<0>(temp_.newPoints_[i]);
	                }

	                mean = total / temp_.newPoints_.size();

	                using namespace cv::v4d::nvg;
	                //start drawing
	                beginPath();
	                strokeWidth(strokeSize);
	                strokeColor(cv::Scalar(effectColor[2], effectColor[1], effectColor[0], effectColor[3]) * 255.0);

	                for (size_t i = 0; i < temp_.newPoints_.size(); i++) {
	                	size_t idx = std::get<1>(temp_.newPoints_[i]);
	                	float len = std::get<0>(temp_.newPoints_[i]);
	                	if(len < mean * 2.0) {
	                		moveTo(temp_.upTrimmedPoints_[idx].x, temp_.upTrimmedPoints_[idx].y);
	                		lineTo(temp_.upNextPoints_[idx].x, temp_.upNextPoints_[idx].y);
	                	}
	                }
	                //end drawing
	                stroke();
	            }
	            temp_.prevPoints_ = temp_.newPoints_;
	        }
	    }
	}

};

class OptflowDemoPlan : public Plan {
private:
	constexpr static auto UMAT_CREATE = _OLM_(void, cv::UMat, &cv::UMat::create, cv::Size, int, cv::UMatUsageFlags);
	constexpr static auto UMAT_DIVIDE_= _OL_(void, cv::divide, cv::InputArray, cv::InputArray, cv::OutputArray, double, int);
	constexpr static auto UMAT_COPY_TO_= _OLMC_(void, cv::UMat, &cv::UMat::copyTo, cv::OutputArray);
	constexpr static auto UMAT_RESHAPE_ = _OLMC_(cv::UMat, cv::UMat, &cv::UMat::reshape, int, int);

	static struct Params {

		PostProcessor::Modes postProcMode_ = PostProcessor::GLOW;
		// Intensity of glow or bloom defined by kernel size. The default scales with the image diagonal.
		int kernelSize_ = 9;
		//The intensity of the glow or bloom filter
		int gain_ = 70;
		//Convert the background to greyscale
		BackgroundStyle::Modes backgroundMode_ = BackgroundStyle::GREY;
		// Peak thresholds for the scene change detection. Lowering them makes the detection more sensitive but
		// the default should be fine.
		float sceneChangeThresh_ = 0.04f;
		float sceneChangeThreshDiff_ = 0.01f;
		// The theoretical maximum number of points to track which is scaled by the density of detected points
		// and therefor is usually much smaller.
		int maxPoints_ = 300000;
		// How many of the tracked points to lose intentionally, in percent.
		float pointLoss_ = 5;
		// The theoretical maximum size of the drawing stroke which is scaled by the area of the convex hull
		// of tracked points and therefor is usually much smaller.
		int maxStroke_ = 2;
		// Red, green, blue and alpha. All from 0.0f to 1.0f
		cv::Scalar_<float> effectColor_ = {1.0f, 0.5f, 0.0f, 0.8f};
		//display on-screen FPS
		bool showFps_ = true;

		bool fullscreen_ = false;
	} params_;

	struct Frames {
		//BGRA
		cv::UMat background_, foreground_, composed_, oldForeground_;

		//GREY
		cv::UMat foregroundGrey_, prevGrey_, nextGrey_;
	} frames_;

	FeaturePoints featurePoints_;
	SceneChange sceneChange_;
	SparseOpticalFlow sparseOptflow_;
	Compositor compositor_;
	inline static vector<cv::Point2f> detectedPoints_;

	Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    OptflowDemoPlan() {
    }

    void gui() override {
		imgui([](Params& params){
	        using namespace ImGui;

	        Begin("Effects");
	        Text("Background");
	        thread_local const char* bgm_items[4] = {"Grey", "Color", "Value", "Black"};
	        thread_local int* bgm = (int*)&params.backgroundMode_;
	        ListBox("Mode", bgm, bgm_items, 4, 4);
	        Text("Points");
	        SliderInt("Max. Points", &params.maxPoints_, 10, 10000000);
	        SliderFloat("Point Loss", &params.pointLoss_, 0.0f, 100.0f);
	        Text("Optical flow");
	        SliderInt("Max. Stroke Size", &params.maxStroke_, 1, 100);
	        ColorPicker4("Color", params.effectColor_.val);
	        End();

	        Begin("Post Processing");
	        thread_local const char* ppm_items[3] = {"Glow", "Bloom", "None"};
	        thread_local int* ppm = (int*)&params.postProcMode_;
	        ListBox("Effect",ppm, ppm_items, 3, 3);
	        SliderInt("Kernel Size",&params.kernelSize_, 1, 63);
	        SliderInt("Bloom Gain", &params.gain_, 2, 100);
	        End();

	        Begin("Settings");
	        Text("Scene Change Detection");
	        SliderFloat("Threshold", &params.sceneChangeThresh_, 0.01f, 1.0f);
	        SliderFloat("Threshold Diff", &params.sceneChangeThreshDiff_, 0.001f, 1.0f);
	        End();

			Begin("Window");
//			if(Checkbox("Show FPS", &params.showFps_)) {
//
//			}

			if(Button("Fullscreen")) {
			    params.fullscreen_ = !params.fullscreen_;
			};

			if(Button("Offscreen")) {
			    V4D::set(V4D::Keys::VISIBLE, !V4D::get<bool>(V4D::Keys::VISIBLE));
			};

			End();
	    }, params_);
	}

    void setup() override {
    	construct(RW(featurePoints_), F(cv::FastFeatureDetector::create, V(10), V(false), V(cv::FastFeatureDetector::TYPE_9_16)));

    	plain(UMAT_CREATE,
                    RW(frames_.foreground_),
                    size_,
                    V(CV_8UC4),
                    V(cv::USAGE_DEFAULT)
        );

        plain(UMAT_CREATE,
                    RW(frames_.prevGrey_),
                    size_,
                    V(CV_8UC1),
                    V(cv::USAGE_DEFAULT)
        );
    }

	void infer() override {
	    set(V4D::Keys::FULLSCREEN, CS(params_.fullscreen_));

	    capture(RW(frames_.background_));

        plain(cv::cvtColor, R(frames_.background_), RW(frames_.nextGrey_), V(cv::COLOR_RGBA2GRAY), V(0), V(cv::ALGO_HINT_DEFAULT));
        plain(cv::cvtColor, R(frames_.foreground_), RW(frames_.foregroundGrey_), V(cv::COLOR_RGBA2GRAY), V(0), V(cv::ALGO_HINT_DEFAULT));
        plain(UMAT_COPY_TO_, R(frames_.foreground_), RW(frames_.oldForeground_));

        plain(&FeaturePoints::detect, RW(featurePoints_),
                R(frames_.nextGrey_),
                RWS(detectedPoints_)
        );

        branch(
                !F(&SceneChange::detect, RW(sceneChange_),
		             RS(detectedPoints_),
				     CS(params_.sceneChangeThresh_),
				     CS(params_.sceneChangeThreshDiff_),
				     size_
                )
		)
            ->clear()
		    ->nvg(&SparseOpticalFlow::visualize, RW(sparseOptflow_),
                    R(frames_.prevGrey_),
                    R(frames_.nextGrey_),
                    RS(detectedPoints_),
                    CS(params_.maxStroke_),
                    CS(params_.maxPoints_),
                    CS(params_.pointLoss_),
                    CS(params_.effectColor_)
            )
            ->fb(UMAT_COPY_TO_, RW(frames_.foreground_))
        ->endBranch();

		fb<4>(&Compositor::perform, RW(compositor_),
		                                R(frames_.background_),
                                        RW(frames_.oldForeground_),
		                                RW(frames_.foreground_),
		                                RW(frames_.composed_),
		                                CS(params_.backgroundMode_),
		                                CS(params_.postProcMode_),
		                                CS(params_.kernelSize_),
		                                CS(params_.gain_)
		);

		plain(UMAT_COPY_TO_, R(frames_.nextGrey_), RW(frames_.prevGrey_));

        write(R(frames_.composed_));
	}
};

OptflowDemoPlan::Params OptflowDemoPlan::params_;

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: optflow-demo <input-video-file>" << endl;
        exit(1);
    }

    cv::Rect viewport(0, 0, 1920, 1080);
	cv::Ptr<V4D> runtime = V4D::init(viewport, "Sparse Optical Flow Demo", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
	auto src = Source::make(runtime, argv[1]);
//	auto sink = Sink::make(runtime, "optflow-demo.mkv", 60, cv::Size(1280, 720));
	runtime->setSource(src);
//	runtime->setSink(sink);
	Plan::run<OptflowDemoPlan>(0);

    return 0;
}
