// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#ifndef SRC_OPENCV_V4D_UTIL_HPP_
#define SRC_OPENCV_V4D_UTIL_HPP_

#include <opencv2/plan/util.hpp>

#include "source.hpp"
#include "sink.hpp"
#include "detail/framebuffercontext.hpp"
#include <filesystem>
#include <array>

namespace cv {
namespace v4d {

#define _OLM_(r,c,f, ...) static_cast<r (c::*)(__VA_ARGS__)>(f)
#define _OLMC_(r,c,f, ...) static_cast<r (c::*)(__VA_ARGS__) const>(f)

#define _OL_(r,f, ...) static_cast<r (*)(__VA_ARGS__)>(f)
#define _OLC_(r,f, ...) static_cast<r (*)(__VA_ARGS__) const>(f)

CV_EXPORTS void copy_cross(const cv::UMat& src, cv::UMat& dst);
CV_EXPORTS void setThreadName(const char* threadName);

template<typename T>
constexpr int matrix_depth() {
	if constexpr(std::is_same_v<T, uchar>) {
		return CV_8U;
	} else if constexpr(std::is_same_v<T, short>){
		return CV_16S;
	} else if constexpr(std::is_same_v<T, ushort>){
		return CV_16U;
	} else if constexpr(std::is_same_v<T, int>){
		return CV_32S;
	} else if constexpr(std::is_same_v<T, float>){
		return CV_32F;
	} else if constexpr(std::is_same_v<T, double>){
		return CV_64F;
	} else {
		static_assert(sizeof(T) == 0, "Type not supported for operation.");
		return 0;
	}
}

template<bool Tround> double doRound(double t) {
	if constexpr(Tround) {
		return std::round(t);
	} else {
		return t;
	}
}

template<int Tcode = -1, typename Tsrc, typename Tdst = Vec<typename Tsrc::value_type, Tsrc::channels>, bool Tround = std::is_floating_point_v<typename Tsrc::value_type> && std::is_integral_v<typename Tdst::value_type>>
Tdst convert_pix(const Tsrc &src, double alpha = 1.0, double beta = 0.0) {
	constexpr int srcCn = Tsrc::channels;
	constexpr int dstCn = Tdst::channels;

	using srcv_t = typename Tsrc::value_type;
	using dstv_t = typename Tdst::value_type;
	using src_internal_t = Vec<srcv_t, srcCn>;
	using intermediate_t = Vec<srcv_t, dstCn>;
	using dst_internal_t = Vec<dstv_t, dstCn>;
	static_assert((srcCn == 3 || srcCn == 4) && (dstCn == 3 || dstCn == 4), "Only 3 or 4 (src/dst) channels supported");
	constexpr int intermediateType = CV_MAKETYPE(
			matrix_depth<typename src_internal_t::value_type>(), dstCn);
	constexpr int dstType = CV_MAKETYPE(
			matrix_depth<typename dst_internal_t::value_type>(), dstCn);

	std::array<src_internal_t, 1> srcArr;
	if constexpr (srcCn == 3) {
		srcArr[0] = src_internal_t(src[0], src[1], src[2]);
	} else {
		srcArr[0] = src_internal_t(src[0], src[1], src[2], src[3]);
	}

	cv::Mat intermediateMat(cv::Size(1, 1), intermediateType);

	if constexpr (dstCn == srcCn) {
		intermediateMat = srcArr[0];
	} else if constexpr (srcCn == 3) {
		intermediateMat = intermediate_t(srcArr[0][0], srcArr[0][1],
				srcArr[0][2]);
	} else if constexpr (srcCn == 4) {
		intermediateMat = intermediate_t(srcArr[0][0], srcArr[0][1],
				srcArr[0][2], srcArr[0][3]);
	}

	if constexpr (Tcode >= 0) {
		cvtColor(srcArr, intermediateMat, Tcode, 0, cv::ALGO_HINT_DEFAULT);
	}

	std::array<dst_internal_t, 1> dstArr;
	if constexpr (!std::is_same<srcv_t, dstv_t>::value) {
		if constexpr (dstCn == srcCn) {
			intermediateMat.convertTo(dstArr, dstType);
		} else if constexpr (dstCn == 3) {
			cvtColor(intermediateMat, intermediateMat, cv::COLOR_BGRA2BGR);
			intermediateMat.convertTo(dstArr, dstType);
		} else if constexpr (dstCn == 4) {
			InputArray arrIm(intermediateMat);
			cvtColor(intermediateMat, intermediateMat, cv::COLOR_BGR2BGRA);
			intermediateMat.convertTo(dstArr, dstType);
		}
	} else {
		if constexpr (dstCn == srcCn) {
			dstArr[0] = intermediateMat.at<src_internal_t>(0.0);
		} else if constexpr (dstCn == 3) {
			auto im = intermediateMat.at<src_internal_t>(0.0);
			dstArr[0] = dst_internal_t(im[0], im[1], im[2]);
		} else if constexpr (dstCn == 4) {
			auto im = intermediateMat.at<src_internal_t>(0.0);
			if (intermediateMat.depth() == CV_32F
					|| intermediateMat.depth() == CV_64F) {
				dstArr[0] = dst_internal_t(im[0], im[1], im[2], 255.0);
			} else {
				dstv_t a = std::numeric_limits<dstv_t>::max();
				dstArr[0] = dst_internal_t(im[0], im[1], im[2], a);
			}
		}
	}

	Tdst dst;

	if constexpr (dstCn == 3) {
		dst = Tdst(dstArr[0][0], dstArr[0][1], dstArr[0][2]);
	} else if constexpr (dstCn == 4) {
		dst = Tdst(dstArr[0][0], dstArr[0][1], dstArr[0][2], dstArr[0][3]);
	}

	if (alpha != 1.0) {
		if constexpr (dstCn == 3) {
			dst[0] = doRound<Tround>(dst[0] * alpha);
			dst[1] = doRound<Tround>(dst[1] * alpha);
			dst[2] = doRound<Tround>(dst[2] * alpha);
		} else if constexpr (dstCn == 4) {
			dst[0] = doRound<Tround>(dst[0] * alpha);
			dst[1] = doRound<Tround>(dst[1] * alpha);
			dst[2] = doRound<Tround>(dst[2] * alpha);
			dst[3] = doRound<Tround>(dst[3] * alpha);
		}
	}

	if (beta != 0.0) {
		if constexpr (dstCn == 3) {
			dst[0] = doRound<Tround>(dst[0] + beta);
			dst[1] = doRound<Tround>(dst[1] + beta);
			dst[2] = doRound<Tround>(dst[2] + beta);
		} else if constexpr (dstCn == 4) {
			dst[0] = doRound<Tround>(dst[0] + beta);
			dst[1] = doRound<Tround>(dst[1] + beta);
			dst[2] = doRound<Tround>(dst[2] + beta);
			dst[3] = doRound<Tround>(dst[3] + beta);
		}
	}
	return dst;
}

inline double seconds() {
	return cv::getTickCount() / cv::getTickFrequency();
}

CV_EXPORTS void gl_check_error(const std::filesystem::path& file, unsigned int line, const char* expression);

#ifndef NDEBUG
#define GL_CHECK(expr)                            \
    expr;                                        \
    cv::v4d::gl_check_error(__FILE__, __LINE__, #expr);
#else
#define GL_CHECK(expr)                            \
    expr;
#endif

CV_EXPORTS void init_shaders(unsigned int handles[3], const string vShader, const string fShader, const string outputAttributeName0, const string outputAttributeName1 = "", const string outputAttributeName2 = "", const string outputAttributeName3 = "");
CV_EXPORTS void init_fragment_shader(unsigned int handles[2], const char* fshader);
CV_EXPORTS std::string get_gl_vendor();
CV_EXPORTS std::string get_gl_info();
CV_EXPORTS std::string get_cl_info();
CV_EXPORTS bool is_intel_va_supported();
CV_EXPORTS bool is_clgl_sharing_supported();
CV_EXPORTS bool keep_running();
CV_EXPORTS void request_finish();
CV_EXPORTS float aspect_preserving_scale(const cv::Size& scaled, const cv::Size& unscaled);
CV_EXPORTS void resize_preserving_aspect_ratio(const cv::UMat& src, cv::UMat& output, const cv::Size& dstSize, const cv::Scalar& bgcolor = {0,0,0,255});

}
}

#endif /* SRC_OPENCV_V4D_UTIL_HPP_ */
