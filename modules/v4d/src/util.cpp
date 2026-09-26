// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/core/ocl.hpp>

#include "../include/opencv2/v4d/v4d.hpp"
#include "../include/opencv2/v4d/detail/gl.hpp"

#include <csignal>
#include <unistd.h>
#include <chrono>
#include <mutex>
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <cmath>
#include <regex>

using std::cerr;
using std::endl;

namespace cv {
namespace v4d {

namespace detail {
void print_nth_line(const std::string& str, int n);
void print_nth_line(const std::string& str, int n) {
    std::istringstream stream(str);
    std::string line;
    int currentLine = 1;

    while (std::getline(stream, line)) {
        if (currentLine == n) {
            std::cout << line << std::endl;
            return;
        }
        ++currentLine;
    }

    std::cout << "Line " << n << " does not exist." << std::endl;
}
}

#ifdef _WIN32
#include <windows.h>
const DWORD MS_VC_EXCEPTION=0x406D1388;

#pragma pack(push,8)
typedef struct tagTHREADNAME_INFO
{
   DWORD dwType; // Must be 0x1000.
   LPCSTR szName; // Pointer to name (in user addr space).
   DWORD dwThreadID; // Thread ID (-1=caller thread).
   DWORD dwFlags; // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)


void setThreadName( const char* threadName)
{
    setThreadName(GetCurrentThreadId(),threadName);
}

void setThreadName( const char* threadName)
{
    DWORD threadId = ::GetThreadId( static_cast<HANDLE>( std::this_thread::.native_handle() ) );
    setThreadName(threadId,threadName);
}

#else
void setThreadName(const char* threadName)
{
   pthread_setname_np(pthread_self(),threadName);
}
#endif

CV_EXPORTS void copy_cross(const cv::UMat& src, cv::UMat& dst) {
	if(dst.empty())
		dst.create(src.size(), src.type());
	Mat m = dst.getMat(cv::ACCESS_WRITE);
	src.copyTo(m);
}

void gl_check_error(const std::filesystem::path& file, unsigned int line, const char* expression) {
    int errorCode = glGetError();
    if (errorCode != 0) {
        std::stringstream ss;
        ss << "GL failed in " << file.filename() << " (" << line << ") : " << "\nExpression:\n   "
                << expression << "\nError code:\n   " << errorCode;
        CV_LOG_WARNING(nullptr, ss.str());
    }
}

void init_fragment_shader(unsigned int handles[2], const char* fshader) {
    struct Shader {
        GLenum type;
        const char* source;
    } s = { GL_FRAGMENT_SHADER, fshader };

    handles[0] = glCreateProgram();

	;
	handles[1] = glCreateShader(s.type);
	glShaderSource(handles[1] , 1, (const GLchar**) &s.source, NULL);
	glCompileShader(handles[1] );

	GLint compiled;
	glGetShaderiv(handles[1] , GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		std::cerr << " failed to compile:" << std::endl;
		GLint logSize;
		glGetShaderiv(handles[1], GL_INFO_LOG_LENGTH, &logSize);
		char* logMsg = new char[logSize];
		glGetShaderInfoLog(handles[1] , logSize, NULL, logMsg);
        std::cerr <<  logMsg << std::endl;

		delete[] logMsg;

		exit (EXIT_FAILURE);
	}
	glAttachShader(handles[0], handles[1]);

    /* link  and error check */
    glLinkProgram(handles[0]);

    GLint linked;
    glGetProgramiv(handles[0], GL_LINK_STATUS, &linked);
    if (!linked) {
        std::cerr << "Shader program failed to link: " << fshader << std::endl;
        GLint logSize;
        glGetProgramiv(handles[0], GL_INFO_LOG_LENGTH, &logSize);
        char* logMsg = new char[logSize];
        glGetProgramInfoLog(handles[0], logSize, NULL, logMsg);
        std::cerr <<  logMsg << std::endl;

        delete[] logMsg;

        exit (EXIT_FAILURE);
    }
}

void init_shaders(unsigned int handles[3], const string vShader, const string fShader, const string outputAttributeName0, const string outputAttributeName1, const string outputAttributeName2, const string outputAttributeName3) {
#if !defined(OPENCV_V4D_USE_ES3)
	const string version="#version 330\n";
#else
	const string version="#version 300 es\n";
#endif
	string vs = (version + vShader);
	string fs = (version + fShader);
	struct Shader {
        GLenum type;
        const char* source;
    } shaders[2] = { { GL_VERTEX_SHADER, vs.c_str() }, { GL_FRAGMENT_SHADER, fs.c_str() } };

    GLuint program = glCreateProgram();
    handles[0] = program;

    for (int i = 0; i < 2; ++i) {
        Shader& s = shaders[i];
        GLuint shader = glCreateShader(s.type);
        handles[i + 1] = shader;
        glShaderSource(shader, 1, (const GLchar**) &s.source, NULL);
        glCompileShader(shader);

        GLint compiled;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            std::cerr << " failed to compile:" << std::endl;
            GLint logSize;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logSize);
            char* logMsg = new char[logSize];
            glGetShaderInfoLog(shader, logSize, NULL, logMsg);

            std::cerr << shaders[i].source << std::endl <<  logMsg << std::endl;
            std::regex rex(R"(\d+:(\d+)\(\d+\))");

            std::cmatch cm;
            if (std::regex_search(logMsg, cm, rex)) {
            	for(size_t j = 0; j < cm.size(); ++j)
            		print_nth_line(shaders[j].source, atoi(std::string(cm[1]).c_str()));
            }
            std::cerr << std::endl;

            delete[] logMsg;

            exit (EXIT_FAILURE);
        }

        glAttachShader(program, shader);
    }
#if !defined(OPENCV_V4D_USE_ES3)
    /* Link output */
    if(!outputAttributeName0.empty())
        glBindFragDataLocation(program, 0, outputAttributeName0.c_str());
    if(!outputAttributeName1.empty())
        glBindFragDataLocation(program, 1, outputAttributeName1.c_str());
    if(!outputAttributeName2.empty())
        glBindFragDataLocation(program, 2, outputAttributeName2.c_str());
    if(!outputAttributeName3.empty())
        glBindFragDataLocation(program, 3, outputAttributeName3.c_str());

    #else
    CV_UNUSED(outputAttributeName0);
#endif
    /* link  and error check */
    glLinkProgram(program);

    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
    	std::cerr << "Shader program failed to link: " << fShader << std::endl;
        GLint logSize;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logSize);
        char* logMsg = new char[logSize];
        glGetProgramInfoLog(program, logSize, NULL, logMsg);

        std::cerr <<  logMsg << std::endl;

        delete[] logMsg;

        exit (EXIT_FAILURE);
    }

    GL_CHECK(glValidateProgram(program));
    GLint validateStatus;
    GL_CHECK(glGetProgramiv(program, GL_VALIDATE_STATUS, &validateStatus));
    if (validateStatus == GL_FALSE) {
        char infoLog[1024];
        GL_CHECK(glGetProgramInfoLog(program, 1024, NULL, infoLog));
        std::cerr << "ERROR::PROGRAM::VALIDATION_FAILED\n" << infoLog << std::endl;
    }
}

std::string get_gl_vendor()  {
    std::ostringstream oss;
        oss << reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        return oss.str();
}

std::string get_gl_info() {
    std::ostringstream oss;
    oss << "\n\t" << reinterpret_cast<const char*>(glGetString(GL_VERSION))
            << "\n\t" << reinterpret_cast<const char*>(glGetString(GL_RENDERER)) << endl;
    return oss.str();
}

std::string get_cl_info() {
    std::stringstream ss;
#ifdef HAVE_OPENCL
    if(cv::ocl::useOpenCL()) {
		std::vector<cv::ocl::PlatformInfo> plt_info;
		cv::ocl::getPlatfomsInfo(plt_info);
		const cv::ocl::Device& defaultDevice = cv::ocl::Device::getDefault();
		cv::ocl::Device current;
		ss << endl;
		for (const auto& info : plt_info) {
			for (int i = 0; i < info.deviceNumber(); ++i) {
				ss << "\t";
				info.getDevice(current, i);
				if (defaultDevice.name() == current.name())
					ss << "* ";
				else
					ss << "  ";
				ss << info.version() << " = " << info.name() << endl;
				ss << "\t\t  GL sharing: "
						<< (current.isExtensionSupported("cl_khr_gl_sharing") ? "true" : "false")
						<< endl;
				ss << "\t\t  VAAPI media sharing: "
						<< (current.isExtensionSupported("cl_intel_va_api_media_sharing") ?
								"true" : "false") << endl;
			}
		}
    }
#endif
    return ss.str();
}

bool is_intel_va_supported() {
#ifdef HAVE_OPENCL
	if(cv::ocl::useOpenCL()) {
		try {
			std::vector<cv::ocl::PlatformInfo> plt_info;
			cv::ocl::getPlatfomsInfo(plt_info);
			cv::ocl::Device current;
			for (const auto& info : plt_info) {
				for (int i = 0; i < info.deviceNumber(); ++i) {
					info.getDevice(current, i);
					return current.isExtensionSupported("cl_intel_va_api_media_sharing");
				}
			}
		} catch (std::exception& ex) {
			cerr << "Intel VAAPI query failed: " << ex.what() << endl;
		} catch (...) {
			cerr << "Intel VAAPI query failed" << endl;
		}
	}
#endif
    return false;
}

bool is_clgl_sharing_supported() {
#ifdef HAVE_OPENCL
	if(cv::ocl::haveOpenCL()) {
		try {
			if(!cv::ocl::useOpenCL())
				return false;
			std::vector<cv::ocl::PlatformInfo> plt_info;
			cv::ocl::getPlatfomsInfo(plt_info);
			cv::ocl::Device current;
			for (const auto& info : plt_info) {
				for (int i = 0; i < info.deviceNumber(); ++i) {
					info.getDevice(current, i);
					return current.isExtensionSupported("cl_khr_gl_sharing");
				}
			}
		} catch (std::exception& ex) {
			cerr << "CL-GL sharing query failed: " << ex.what() << endl;
		} catch (...) {
			cerr << "CL-GL sharing query failed with unknown error." << endl;
		}
	}
#endif
    return false;
}
// ============================================================================
// Process-wide shutdown
//
// A signal is delivered to the process, not to one plan, so a request to stop
// cannot be scoped to a run: SIGINT has to end every plan that is running. That
// makes this the one piece of state that is deliberately shared process-wide.
// The counterpart - stopping a single run - lives in V4D::RunState and is
// reached through V4D::requestFinish().
//
// Three properties matter here:
//
//  * the flag is written from a signal handler, so it is a lock free atomic and
//    the handler does nothing else (the previous version took a std::mutex in
//    the handler, which is undefined behaviour and can deadlock);
//  * the handlers are installed once per set of runs instead of on every call
//    to keep_running() (the previous version re-installed them on every query,
//    because it never recorded that it had done so);
//  * they are restored when the last run ends, so a run does not permanently
//    change the signal disposition of the process it happens to be embedded in.
// ============================================================================
namespace {
std::atomic<bool> g_finish_requested { false };

// Number of runs that currently rely on the handlers being installed.
std::atomic<int> g_handler_users { 0 };

// Handlers that were in place before we installed ours, restored on the way out
// and chained to while we are installed.
void (*g_prev_int)(int) = nullptr;
void (*g_prev_term)(int) = nullptr;
std::mutex g_handler_mtx;

extern "C" void on_shutdown_signal(int sig);

void chain_to_previous(void (*previous)(int), int sig) {
    // SIG_DFL/SIG_IGN are not callable, and chaining to ourselves would loop.
    if(previous == nullptr || previous == SIG_DFL || previous == SIG_IGN ||
       previous == on_shutdown_signal)
        return;
    previous(sig);
}

extern "C" void on_shutdown_signal(int sig) {
    g_finish_requested.store(true);
    if(sig == SIGTERM) {
        chain_to_previous(g_prev_term, sig);
    } else {
        chain_to_previous(g_prev_int, sig);
    }
}
} // namespace

void install_shutdown_handlers() {
    std::lock_guard<std::mutex> guard(g_handler_mtx);
    if(g_handler_users.fetch_add(1) > 0)
        return; // already installed for another run

    // Read the disposition that is in place *before* touching anything, so the
    // remembered handler is the one of the host program and not our own.
    struct sigaction old_int, old_term;
    sigaction(SIGINT, nullptr, &old_int);
    sigaction(SIGTERM, nullptr, &old_term);
    g_prev_int = old_int.sa_handler;
    g_prev_term = old_term.sa_handler;

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_shutdown_signal;
    sigemptyset(&action.sa_mask);
    // Deliberately no SA_RESTART: a run blocked in a syscall should get the
    // chance to observe the request instead of being restarted into it.
    action.sa_flags = 0;

    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

void remove_shutdown_handlers() {
    std::lock_guard<std::mutex> guard(g_handler_mtx);
    if(g_handler_users.fetch_sub(1) != 1)
        return; // other runs still rely on them

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = g_prev_int;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    action.sa_handler = g_prev_term;
    sigaction(SIGTERM, &action, nullptr);
    g_prev_int = nullptr;
    g_prev_term = nullptr;
}

bool finish_requested() {
    return g_finish_requested.load();
}

void request_finish() {
    g_finish_requested.store(true);
}

void reset_finish() {
    g_finish_requested.store(false);
}

/*!
 * Whether the process shall keep going. Note that this is a pure query: it no
 * longer installs the signal handlers as a side effect. A run installs them for
 * as long as it lasts (see #install_shutdown_handlers), so a program that never
 * starts a plan keeps the signal disposition it had.
 */
bool keep_running() {
    return !finish_requested();
}


float aspect_preserving_scale(const cv::Size& scaled, const cv::Size& unscaled) {
	double scale;
	double scaleX =  double(scaled.width) / unscaled.width;
	double scaleY = double(scaled.height) / unscaled.height;

	if(scaleX < 1.0 && scaleY >= 1.0) {
		scale =  double(unscaled.height) / scaled.height;
	} else if(scaleX >= 1.0 && scaleY < 1.0) {
		scale =  double(unscaled.width) / scaled.width;
	} else
		scale = std::min(scaleX, scaleY);

	return scale;
}

void resize_preserving_aspect_ratio(const cv::UMat& src, cv::UMat& output, const cv::Size& dstSize, const cv::Scalar& bgcolor) {
    cv::UMat tmp;

    double f = aspect_preserving_scale(dstSize, src.size());
	cv::resize(src, tmp, cv::Size(), f, f);

	int top = std::abs((dstSize.height - tmp.rows) / 2);
	int down = std::abs((dstSize.height - tmp.rows + 1) / 2);
	int left = std::abs((dstSize.width - tmp.cols) / 2);
	int right = std::abs((dstSize.width - tmp.cols + 1) / 2);

	cv::copyMakeBorder(tmp, output, top, down, left, right, cv::BORDER_CONSTANT, bgcolor);
}

}
}
