// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#ifndef MODULES_V4D_INCLUDE_OPENCV2_V4D_DETAIL_GL_HPP_
#define MODULES_V4D_INCLUDE_OPENCV2_V4D_DETAIL_GL_HPP_

#if !defined(OPENCV_V4D_USE_ES3)
#define OPENCV_V4D_GL_SHADER_VERSION "#version 330"
#define GL_GLEXT_PROTOTYPES
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include "GL/glcorearb.h"
#endif
#else
#if !defined(__ANDROID__)
#define OPENCV_V4D_GL_SHADER_VERSION "#version 300 es"
#else
#define OPENCV_V4D_GL_SHADER_VERSION "#version 300 es"
#endif
#if !defined(__ANDROID__)
#include "GLES2/gl2ext.h"
#endif
#include "GLES3/gl3.h"
#endif

#endif /* MODULES_V4D_INCLUDE_OPENCV2_V4D_DETAIL_GL_HPP_ */
