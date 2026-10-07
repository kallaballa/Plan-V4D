#ifndef MODULES_V4D_INCLUDE_OPENCV2_V4D_DETAIL_CL_HPP_
#define MODULES_V4D_INCLUDE_OPENCV2_V4D_DETAIL_CL_HPP_

// HAVE_OPENCL comes from the generated cvconfig.h, which this header must pull
// in itself: its includers may reach this file before any other OpenCV header,
// and without cvconfig.h the guard below would silently skip the CL includes.
#include <opencv2/core/cvdef.h>

#ifdef HAVE_OPENCL

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif

// OpenCV's runtime headers rename every CL entry point to a function pointer
// resolved at call time from the driver OpenCV dlopen()s. Using them here
// keeps this module free of undefined CL symbols - a requirement on Android,
// where the APK's shared object is loaded wholesale before OpenCL is even
// loaded, so a raw clCreateFromGLTexture reference would fail the dlopen.
#include <opencv2/core/opencl/runtime/opencl_core.hpp>
#include <opencv2/core/opencl/runtime/opencl_gl.hpp>

#ifndef cl_khr_gl_sharing
// Stock OpenCV provides the GL-sharing half only when built with HAVE_OPENGL.
// Fall back to the raw declarations: resolved against a libOpenCL that is
// linked in (desktop) or already loaded globally.
#ifdef __APPLE__
#include <OpenCL/cl_gl_ext.h>
#else
#include <CL/cl_gl.h>
#endif
#endif

#endif

#endif /* MODULES_V4D_INCLUDE_OPENCV2_V4D_DETAIL_CL_HPP_ */
