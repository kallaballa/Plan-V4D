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

#include <opencv2/core/opencl/runtime/opencl_core.hpp>
#include <opencv2/core/opencl/runtime/opencl_gl.hpp>

#ifndef __ANDROID__
#ifdef __APPLE__
#include <OpenCL/cl_gl_ext.h>
#else
#include <CL/cl_gl.h>
#endif
#endif

#endif

#endif /* MODULES_V4D_INCLUDE_OPENCV2_V4D_DETAIL_CL_HPP_ */
