// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Image Pipeline Demo
// Demonstrates: UMat processing in Plan graphs, cv:: operations as nodes,
//               branching on image properties, and multi-stage pipelines.

#include <opencv2/plan/plan.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <cmath>

using namespace cv;
using namespace cv::plan;

/*!
 * A plan that implements a simple image processing pipeline:
 * 1. Generate a synthetic image
 * 2. Apply Gaussian blur
 * 3. Detect edges (Canny)
 * 4. Compute statistics
 * 5. Branch based on edge density
 */
class ImagePipelinePlan : public Plan {
public:
    // Pipeline stages
    cv::UMat source_;
    cv::UMat blurred_;
    cv::UMat edges_;
    cv::UMat result_;

    // Statistics
    double edgeDensity_ = 0.0;
    double meanIntensity_ = 0.0;
    std::string quality_;

    // Parameters
    int blurSize_ = 5;
    double cannyLow_ = 50.0;
    double cannyHigh_ = 150.0;

    void setup() override {
        // Generate a synthetic test image (gradient + circles)
        source_.create(cv::Size(256, 256), CV_8UC1);

        plain([](cv::UMat& img) {
            cv::circle(img, cv::Point(64, 64), 30, cv::Scalar(255), 2);
            cv::circle(img, cv::Point(192, 192), 40, cv::Scalar(0), 2);
            cv::rectangle(img, cv::Point(100, 100), cv::Point(156, 156), cv::Scalar(200), 2);
        }, RW(source_));
    }

    void infer() override {
        // Stage 1: Blur
        plain([](cv::UMat& out, const cv::UMat& in, const int& ksize) {
            cv::GaussianBlur(in, out, cv::Size(ksize, ksize), 0);
        }, RW(blurred_), R(source_), R(blurSize_));

        // Stage 2: Edge detection
        plain([](cv::UMat& out, const cv::UMat& in, const double& low, const double& high) {
            cv::Canny(in, out, low, high);
        }, RW(edges_), R(blurred_), R(cannyLow_), R(cannyHigh_));

        // Stage 3: Compute statistics
        plain([](double& density, double& mean, const cv::UMat& edgeImg, const cv::UMat& srcImg) {
            density = static_cast<double>(cv::countNonZero(edgeImg)) /
                      (edgeImg.rows * edgeImg.cols);
            cv::Scalar m = cv::mean(srcImg);
            mean = m[0];
        }, RW(edgeDensity_), RW(meanIntensity_), R(edges_), R(source_));

        // Stage 4: Classify image quality based on edge density
        branch(R(edgeDensity_) > V(0.1))
            ->plain([](std::string& q) { q = "HIGH_DETAIL"; }, RW(quality_))
        ->elseBranch()
            ->branch(R(edgeDensity_) > V(0.02))
                ->plain([](std::string& q) { q = "MEDIUM_DETAIL"; }, RW(quality_))
            ->elseBranch()
                ->plain([](std::string& q) { q = "LOW_DETAIL"; }, RW(quality_))
            ->endBranch()
        ->endBranch();

        // Stage 5: Compose result (edges overlaid on source)
        plain([](cv::UMat& result, const cv::UMat& src, const cv::UMat& edgeImg) {
            cv::cvtColor(src, result, cv::COLOR_GRAY2BGR);
            cv::UMat edgesColor;
            cv::cvtColor(edgeImg, edgesColor, cv::COLOR_GRAY2BGR);
            cv::add(result, edgesColor, result);
        }, RW(result_), R(source_), R(edges_));
    }
};

int main() {
    std::cout << "=== Plan Module: Image Pipeline Demo ===" << std::endl;
    std::cout << std::endl;

    GlobalState::init_keys();
    LocalState::init_keys();

    auto pipeline = Plan::make<ImagePipelinePlan>();

    // Setup
    pipeline->setup();
    pipeline->makeGraph();
    pipeline->runGraph();
    pipeline->clearGraph();
    std::cout << "  Source image created: " << pipeline->source_.size() << std::endl;

    // Run with different parameters
    struct Config {
        int blurSize;
        double cannyLow;
        double cannyHigh;
    };

    Config configs[] = {
        {3, 30.0, 100.0},
        {5, 50.0, 150.0},
        {9, 80.0, 200.0},
        {15, 100.0, 250.0},
    };

    for (const auto& cfg : configs) {
        pipeline->blurSize_ = cfg.blurSize;
        pipeline->cannyLow_ = cfg.cannyLow;
        pipeline->cannyHigh_ = cfg.cannyHigh;

        pipeline->infer();
        pipeline->makeGraph();
        pipeline->runGraph();
        pipeline->clearGraph();

        std::cout << "\n  Config: blur=" << cfg.blurSize
                  << " canny=[" << cfg.cannyLow << "," << cfg.cannyHigh << "]"
                  << std::endl;
        std::cout << "    Edge density: " << std::fixed << std::setprecision(4)
                  << pipeline->edgeDensity_ << std::endl;
        std::cout << "    Mean intensity: " << std::setprecision(1)
                  << pipeline->meanIntensity_ << std::endl;
        std::cout << "    Quality: " << pipeline->quality_ << std::endl;
        std::cout << "    Result size: " << pipeline->result_.size()
                  << " channels=" << pipeline->result_.channels() << std::endl;
    }

    std::cout << "\nDone." << std::endl;
    return 0;
}

