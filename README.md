# Plan-V4D: A High-Performance Visualization Module for OpenCV

Plan-V4D is a C++20 framework for building high-performance, graphical applications, tightly integrated with the OpenCV ecosystem. It combines **Plan**, a compile-time task graph engine, with **V4D**, a versatile 2D/3D graphics runtime.

This combination allows developers to create complex, parallelized computer vision and visualization pipelines with a clean, declarative syntax.

![Beauty Filter Demo](modules/v4d/doc/beauty.png)
*A real-time "beauty filter" demo running in Plan-V4D, showcasing DNN-based face and landmark detection, image segmentation, and multi-band blending.*

## Core Concepts

-   **A Compile-Time Task Graph**: When you write a `Plan`, you are not writing a typical program. Instead, you are **describing an entire processing pipeline at compile time**. Through C++20 template metaprogramming, the structure of your code *is* the structure of the task graph. This graph is "baked" into your binary, meaning there is no runtime graph-building overhead, which is key to its performance. Think of it like designing a factory assembly line in a blueprint, which is then constructed once, perfectly optimized.

-   **Edges for Data Flow**: To build this graph, the compiler needs to know exactly how each task interacts with data. You declare this using **edge-calls** (`R` for Read, `RW` for Read-Write, etc.). This is how you draw the conveyor belts in the assembly line analogy. By being explicit about data access, you allow the Plan engine to automatically manage memory and schedule tasks for maximum parallelism, eliminating entire classes of concurrency bugs.

-   **Contexts as Workstations**: The V4D runtime provides **contexts** (`nvg`, `gl`, `fb`, `imgui`, `plain`) which are the workstations on your assembly line. Each is a specialized environment for a specific task, like 2D vector drawing, raw OpenGL rendering, or running standard OpenCV functions.

-   **How it Compares**: Plan-V4D is like **GStreamer**, but with a compile-time graph that is type-safe C++. It has the pipeline-thinking of a node-based compositor like **Blender or Nuke**, but implemented in code. It delivers the performance of a low-level tasking library like **Intel TBB**, but is specialized for the computer vision and graphics domain.

## Learn More

The best way to learn Plan-V4D is to follow the **[Tutorials](modules/v4d/tutorials/00-intro.markdown)**. They have been written to guide you from the foundational concepts to building complex, real-world applications like the ones shown above.

## Key Features

-   **Task Graph Engine**: Write incredibly fast, parallel code by describing your application as a task graph.
-   **2D Vector Graphics**: High-quality 2D rendering and text via an integrated **NanoVG** backend.
-   **GUI**: Create simple yet powerful user interfaces with the integrated **Dear ImGui** backend.
-   **3D Graphics**: Access the full power of the GPU with direct **OpenGL** rendering. An optional **bgfx** backend is also available.
-   **Efficient Video Pipeline**: A simple `Source`/`Sink` architecture allows for efficient reading, processing, displaying, and saving of videos, with transparent hardware acceleration where possible (e.g., VAAPI).
-   **Deep OpenCV Integration**: Built as an OpenCV module, V4D seamlessly works with `cv::UMat` for GPU-accelerated processing and integrates with other modules like `dnn`, `face`, and `optflow`.
-   **Advanced Composition**: Build complex applications by chaining contexts, creating multi-layer rendering pipelines, and even building hierarchical applications with **sub-plans**.

## Selected Examples

| ![Optical Flow Visualization](modules/v4d/doc/optflow.png) | ![Interactive Shader Demo](modules/v4d/doc/shader.png) | ![Pedestrian Tracking](modules/v4d/doc/pedestrian.png) |
| :---: | :---: | :---: |
| **Optical Flow** ([source](modules/v4d/samples/optflow-demo.cpp)) | **Custom Shader** ([source](modules/v4d/samples/shader-demo.cpp)) | **Detection & Tracking** ([source](modules/v4d/samples/pedestrian-demo.cpp)) |

## Instructions for Linux (Ubuntu 24.04.1 LTS - Noble Numbat)

### Optional: Create a chroot if you are using another distribution or Ubuntu version 

#### Build minbase chroot (basically installing a minimal Ubuntu into the directory plan-v4d-noble)
```bash
sudo debootstrap --variant=minbase --arch=amd64 noble plan-v4d-noble http://archive.ubuntu.com/ubuntu/
```

#### Bind /dev - WARNING: Don't delete the chroot without umounting plan-v4d-noble/dev or your system will crash
```bash
sudo mount --bind /dev/ plan-v4d-noble/dev
```

#### Enter the chroot - from here on you are inside the ubuntu chroot and you cannot access the files of your system anymore until you leave it (e.g: by running "exit")
```bash
sudo chroot plan-v4d-noble/
```

#### Setup essential virtual filesystems - NOTE: umount those after you are done with the chroot
```bash
mount -t proc none /proc
mount -t sysfs none /sys
mount -t tmpfs none /tmp
mount -t devpts none /dev/pts
```

### Configure apt
```bash
echo "deb http://archive.ubuntu.com/ubuntu noble main universe" > /etc/apt/sources.list
```

### Install required packages
```bash
apt update
apt install vainfo clinfo libqt5opengl5-dev freeglut3-dev ocl-icd-opencl-dev libavcodec-dev libavdevice-dev libavfilter-dev libavformat-dev libavutil-dev libpostproc-dev libswresample-dev libswscale-dev libglfw3-dev libstb-dev libglew-dev cmake make git-core build-essential opencl-clhpp-headers pkg-config zlib1g-dev doxygen libxinerama-dev libxcursor-dev libxi-dev libva-dev yt-dlp wget intel-opencl-icd ca-certificates
```

### Optional: install packaging tools
```bash
apt install ubuntu-dev-tools dh-cmake gdebi
```

### EITHER: Minimal Plan-V4D build WITHOUT examples, demos and packages
```bash
git clone --branch GCV https://github.com/kallaballa/opencv.git
git clone https://github.com/kallaballa/Plan-V4D.git
mkdir opencv/build
cd opencv/build
```

#### Configuring a Wayland (-DWITH_WAYLAND=ON) build without examples (-DBUILD_EXAMPLES=OFF) and packages (-DBUILD_PACKAGE=OFF)
```bash
cmake -DWITH_WAYLAND=ON -DOPENCV_V4D_ENABLE_ES3=OFF -DCMAKE_CXX_FLAGS="-DCL_TARGET_OPENCL_VERSION=120" -DCMAKE_MODULE_LINKER_FLAGS="/usr/local/lib64/" -DINSTALL_BIN_EXAMPLES=OFF -DOPENCV_CUSTOM_PACKAGE_INFO=ON -DCPACK_PACKAGE_VERSION_MAJOR=4 -DCPACK_PACKAGE_VERSION_MINOR=10 -DCPACK_PACKAGE_VERSION_PATCH=0 -DCPACK_PACKAGE_VERSION=4:10.0-yourname -DCMAKE_BUILD_TYPE=Release -DCPACK_PACKAGE_CONTACT="you@example.com" -DOPENCV_GENERATE_PKGCONFIG=ON -DCPACK_PACKAGE_VENDOR=yourname -DCPACK_DEBIAN_PACKAGE_DEPENDS="libqt5opengl5,freeglut3,ocl-icd-libopencl1,libavcodec58,libavdevice58,libavfilter7,libavformat58,libavutil56,libpostproc55,libswresample3,libswscale5,libglfw3,libstb0,libglew2.2,zlib1g,libxinerama1,libxcursor1,libxi6,libva2,intel-opencl-icd,ca-certificates" -DINSTALL_CREATE_DISTRIB=ON -DCPACK_BINARY_DEB=ON -DCV_TRACE=OFF -DBUILD_SHARED_LIBS=ON -DWITH_OPENGL=ON -DOPENCV_ENABLE_EGL=ON -DOPENCV_ENABLE_GLX=ON -DOPENCV_FFMPEG_ENABLE_LIBAVDEVICE=ON -DWITH_QT=ON -DWITH_FFMPEG=ON -DOPENCV_FFMPEG_SKIP_BUILD_CHECK=ON -DWITH_VA=ON -DWITH_VA_INTEL=ON -DWITH_1394=OFF -DWITH_ADE=OFF -DWITH_VTK=OFF -DWITH_EIGEN=OFF -DWITH_GTK=OFF -DWITH_GTK_2_X=OFF -DWITH_IPP=OFF -DWITH_JASPER=OFF -DWITH_WEBP=OFF -DWITH_OPENEXR=OFF -DWITH_OPENVX=OFF -DWITH_OPENNI=OFF -DWITH_OPENNI2=OFF-DWITH_TBB=OFF -DWITH_TIFF=OFF -DWITH_OPENCL=ON -DWITH_OPENCL_SVM=OFF -DWITH_OPENCLAMDFFT=OFF -DWITH_OPENCLAMDBLAS=OFF -DWITH_GPHOTO2=OFF -DWITH_LAPACK=OFF -DWITH_ITT=OFF -DWITH_QUIRC=ON -DBUILD_ZLIB=OFF -DBUILD_opencv_apps=OFF -DBUILD_opencv_calib3d=ON -DBUIlD_opencv_ccalib=OFF -DBUILD_opencv_dnn=ON -DBUILD_opencv_features2d=ON -DBUILD_opencv_flann=ON -DBUILD_opencv_gapi=OFF -DBUILD_opencv_ml=OFF -DBUILD_opencv_photo=ON -DBUILD_opencv_imgcodecs=ON -DBUILD_opencv_shape=OFF -DBUILD_opencv_videoio=ON -DBUILD_opencv_videostab=OFF -DBUILD_opencv_highgui=ON -DBUILD_opencv_superres=OFF -DBUILD_opencv_stitching=ON -DBUILD_opencv_java=OFF -DBUILD_opencv_js=OFF -DBUILD_opencv_python2=OFF -DBUILD_opencv_python3=OFF -DBUILD_opencv_alphamat=OFF -DBUILD_opencv_aruco=OFF -DBUILD_opencv_barcode=OFF -DBUILD_opencv_bgsegm=OFF -DBUILD_opencv_bioinspired=OFF -DBUILD_opencv_ccalib=ON -DBUILD_opencv_cnn_3dobj=OFF -DBUILD_opencv_cudaarithm=OFF -DBUILD_opencv_cudabgsegm=OFF -DBUILD_opencv_cudacodec=OFF -DBUILD_opencv_cudafeatures2d=OFF -DBUILD_opencv_cudafilters=OFF -DBUILD_opencv_cudaimgproc=OFF -DBUILD_opencv_cudalegacy=OFF -DBUILD_opencv_cudaobjdetect=OFF -DBUILD_opencv_cudaoptflow=OFF -DBUILD_opencv_cudastereo=OFF -DBUILD_opencv_cudawarping=OFF -DBUILD_opencv_cudev=OFF -DBUILD_opencv_cvv=OFF -DBUILD_opencv_datasets=OFF -DBUILD_opencv_dnn_objdetect=OFF -DBUILD_opencv_dnns_easily_fooled=OFF -DBUILD_opencv_dnn_superres=OFF -DBUILD_opencv_dpm=OFF -DBUILD_opencv_face=ON -DBUILD_opencv_freetype=OFF -DBUILD_opencv_fuzzy=OFF -DBUILD_opencv_hdf=OFF -DBUILD_opencv_hfs=OFF -DBUILD_opencv_img_hash=OFF -DBUILD_opencv_intensity_transform=OFF -DBUILD_opencv_julia=OFF -DBUILD_opencv_line_descriptor=OFF -DBUILD_opencv_matlab=OFF -DBUILD_opencv_mcc=OFF -DBUILD_opencv_optflow=ON -DBUILD_opencv_ovis=OFF -DBUILD_opencv_phase_unwrapping=OFF -DBUILD_opencv_plot=ON -DBUILD_opencv_quality=OFF -DBUILD_opencv_rapid=OFF -DBUILD_opencv_README.md=OFF -DBUILD_opencv_reg=OFF -DBUILD_opencv_rgbd=OFF -DBUILD_opencv_saliency=OFF -DBUILD_opencv_sfm=OFF -DBUILD_opencv_shape=OFF -DBUILD_opencv_stereo=OFF -DBUILD_opencv_structured_light=OFF -DBUILD_opencv_superres=OFF -DBUILD_opencv_surface_matching=OFF -DBUILD_opencv_text=OFF -DBUILD_opencv_tracking=ON -DBUILD_opencv_videostab=OFF -DBUILD_opencv_viz=OFF -DBUILD_opencv_wechat_qrcode=OFF -DBUILD_opencv_xfeatures2d=OFF -DBUILD_opencv_ximgproc=ON -DBUILD_opencv_xobjdetect=OFF -DBUILD_opencv_xphoto=OFF -DBUILD_opencv_world=OFF -DBUILD_EXAMPLES=OFF -DBUILD_PACKAGE=OFF -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_DOCS=OFF -DWITH_PTHREADS_PF=ON -DCV_ENABLE_INTRINSICS=ON -DBUILD_opencv_video=ON -DBUILD_opencv_v4d=ON -DGBFX_CONFIG_MULTITHREADED=OFF -DBGFX_CONFIG_PASSIVE=ON -DOPENCV_EXTRA_MODULES_PATH="../../Plan-V4D/modules" ..
```
### OR: Full Plan-V4D build WITH examples, demos and packages
```bash
git clone --branch GCV https://github.com/kallaballa/opencv.git
git clone https://github.com/kallaballa/Plan-V4D.git
mkdir opencv/build
cd opencv/build
```
#### Configuring a Wayland (-DWITH_WAYLAND=ON) build with examples (-DBUILD_EXAMPLES=ON) and packages (-DBUILD_PACKAGE=ON) - NOTE: you might want to update the package info (-DCPACK_PACKAGE_VERSION, -DCPACK_PACKAGE_CONTACT, -DCPACK_PACKAGE_VENDOR)
```bash
cmake -DWITH_WAYLAND=ON -DOPENCV_V4D_ENABLE_ES3=OFF -DCMAKE_CXX_FLAGS="-DCL_TARGET_OPENCL_VERSION=120" -DCMAKE_MODULE_LINKER_FLAGS="/usr/local/lib64/" -DINSTALL_BIN_EXAMPLES=OFF -DOPENCV_CUSTOM_PACKAGE_INFO=ON -DCPACK_PACKAGE_VERSION_MAJOR=4 -DCPACK_PACKAGE_VERSION_MINOR=10 -DCPACK_PACKAGE_VERSION_PATCH=0 -DCPACK_PACKAGE_VERSION=4:10.0-yourname -DCMAKE_BUILD_TYPE=Release -DCPACK_PACKAGE_CONTACT="you@example.com" -DOPENCV_GENERATE_PKGCONFIG=ON -DCPACK_PACKAGE_VENDOR=yourname -DCPACK_DEBIAN_PACKAGE_DEPENDS="libqt5opengl5,freeglut3,ocl-icd-libopencl1,libavcodec58,libavdevice58,libavfilter7,libavformat58,libavutil56,libpostproc55,libswresample3,libswscale5,libglfw3,libstb0,libglew2.2,zlib1g,libxinerama1,libxcursor1,libxi6,libva2,intel-opencl-icd,ca-certificates" -DINSTALL_CREATE_DISTRIB=ON -DCPACK_BINARY_DEB=ON -DCV_TRACE=OFF -DBUILD_SHARED_LIBS=ON -DWITH_OPENGL=ON -DOPENCV_ENABLE_EGL=ON -DOPENCV_ENABLE_GLX=ON -DOPENCV_FFMPEG_ENABLE_LIBAVDEVICE=ON -DWITH_QT=ON -DWITH_FFMPEG=ON -DOPENCV_FFMPEG_SKIP_BUILD_CHECK=ON -DWITH_VA=ON -DWITH_VA_INTEL=ON -DWITH_1394=OFF -DWITH_ADE=OFF -DWITH_VTK=OFF -DWITH_EIGEN=OFF -DWITH_GTK=OFF -DWITH_GTK_2_X=OFF -DWITH_IPP=OFF -DWITH_JASPER=OFF -DWITH_WEBP=OFF -DWITH_OPENEXR=OFF -DWITH_OPENVX=OFF -DWITH_OPENNI=OFF -DWITH_OPENNI2=OFF-DWITH_TBB=OFF -DWITH_TIFF=OFF -DWITH_OPENCL=ON -DWITH_OPENCL_SVM=OFF -DWITH_OPENCLAMDFFT=OFF -DWITH_OPENCLAMDBLAS=OFF -DWITH_GPHOTO2=OFF -DWITH_LAPACK=OFF -DWITH_ITT=OFF -DWITH_QUIRC=ON -DBUILD_ZLIB=OFF -DBUILD_opencv_apps=OFF -DBUILD_opencv_calib3d=ON -DBUIlD_opencv_ccalib=OFF -DBUILD_opencv_dnn=ON -DBUILD_opencv_features2d=ON -DBUILD_opencv_flann=ON -DBUILD_opencv_gapi=OFF -DBUILD_opencv_ml=OFF -DBUILD_opencv_photo=ON -DBUILD_opencv_imgcodecs=ON -DBUILD_opencv_shape=OFF -DBUILD_opencv_videoio=ON -DBUILD_opencv_videostab=OFF -DBUILD_opencv_highgui=ON -DBUILD_opencv_superres=OFF -DBUILD_opencv_stitching=ON -DBUILD_opencv_java=OFF -DBUILD_opencv_js=OFF -DBUILD_opencv_python2=OFF -DBUILD_opencv_python3=OFF -DBUILD_opencv_alphamat=OFF -DBUILD_opencv_aruco=OFF -DBUILD_opencv_barcode=OFF -DBUILD_opencv_bgsegm=OFF -DBUILD_opencv_bioinspired=OFF -DBUILD_opencv_ccalib=ON -DBUILD_opencv_cnn_3dobj=OFF -DBUILD_opencv_cudaarithm=OFF -DBUILD_opencv_cudabgsegm=OFF -DBUILD_opencv_cudacodec=OFF -DBUILD_opencv_cudafeatures2d=OFF -DBUILD_opencv_cudafilters=OFF -DBUILD_opencv_cudaimgproc=OFF -DBUILD_opencv_cudalegacy=OFF -DBUILD_opencv_cudaobjdetect=OFF -DBUILD_opencv_cudaoptflow=OFF -DBUILD_opencv_cudastereo=OFF -DBUILD_opencv_cudawarping=OFF -DBUILD_opencv_cudev=OFF -DBUILD_opencv_cvv=OFF -DBUILD_opencv_datasets=OFF -DBUILD_opencv_dnn_objdetect=OFF -DBUILD_opencv_dnns_easily_fooled=OFF -DBUILD_opencv_dnn_superres=OFF -DBUILD_opencv_dpm=OFF -DBUILD_opencv_face=ON -DBUILD_opencv_freetype=OFF -DBUILD_opencv_fuzzy=OFF -DBUILD_opencv_hdf=OFF -DBUILD_opencv_hfs=OFF -DBUILD_opencv_img_hash=OFF -DBUILD_opencv_intensity_transform=OFF -DBUILD_opencv_julia=OFF -DBUILD_opencv_line_descriptor=OFF -DBUILD_opencv_matlab=OFF -DBUILD_opencv_mcc=OFF -DBUILD_opencv_optflow=ON -DBUILD_opencv_ovis=OFF -DBUILD_opencv_phase_unwrapping=OFF -DBUILD_opencv_plot=ON -DBUILD_opencv_quality=OFF -DBUILD_opencv_rapid=OFF -DBUILD_opencv_README.md=OFF -DBUILD_opencv_reg=OFF -DBUILD_opencv_rgbd=OFF -DBUILD_opencv_saliency=OFF -DBUILD_opencv_sfm=OFF -DBUILD_opencv_shape=OFF -DBUILD_opencv_stereo=OFF -DBUILD_opencv_structured_light=OFF -DBUILD_opencv_superres=OFF -DBUILD_opencv_surface_matching=OFF -DBUILD_opencv_text=OFF -DBUILD_opencv_tracking=ON -DBUILD_opencv_videostab=OFF -DBUILD_opencv_viz=OFF -DBUILD_opencv_wechat_qrcode=OFF -DBUILD_opencv_xfeatures2d=OFF -DBUILD_opencv_ximgproc=ON -DBUILD_opencv_xobjdetect=OFF -DBUILD_opencv_xphoto=OFF -DBUILD_opencv_world=OFF -DBUILD_EXAMPLES=ON -DBUILD_PACKAGE=ON -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_DOCS=OFF -DWITH_PTHREADS_PF=ON -DCV_ENABLE_INTRINSICS=ON -DBUILD_opencv_video=ON -DBUILD_opencv_v4d=ON -DGBFX_CONFIG_MULTITHREADED=OFF -DBGFX_CONFIG_PASSIVE=ON -DOPENCV_EXTRA_MODULES_PATH="../../Plan-V4D/modules" ..
```

### Build
```bash
make -j8
```

### Optional: Build packages
```bash
cpack DEB
```

### Cleaning up and leaving the chroot
```bash
umount proc
umount sys
umount tmp
umount dev/pts
exit #After this command you are back outside the chroot with access to your system.
```

### Unbind dev
```bash
sudo umount plan-v4d-noble/dev
```

## Instructions for Mac OS X Using Homebrew

### Install Required Dependencies
First, ensure you have Homebrew installed on your system. Then, install the required dependencies:

```bash
# Update Homebrew
brew update

# Install packages
brew install cmake git qt@5 glfw glew zlib doxygen wget yt-dlp
brew install clinfo # For OpenCL information
brew install opencl-headers # OpenCL headers
```

### Clone Repositories
```bash
git clone --branch GCV https://github.com/kallaballa/opencv.git
git clone https://github.com/kallaballa/Plan-V4D.git
mkdir opencv/build
cd opencv/build
```

### Full Plan-V4D Build (with examples, demos, and packages)
```bash
cmake -DOPENCV_V4D_ENABLE_ES3=ON -DCMAKE_CXX_FLAGS="-DCL_TARGET_OPENCL_VERSION=120" -DINSTALL_BIN_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release -DINSTALL_CREATE_DISTRIB=OFF -DBUILD_SHARED_LIBS=ON -DWITH_OPENGL=ON -DOPENCV_ENABLE_EGL=ON -DOPENCV_ENABLE_GLX=OFF -DOPENCV_FFMPEG_ENABLE_LIBAVDEVICE=ON -DWITH_QT=ON -DWITH_FFMPEG=ON -DOPENCV_FFMPEG_SKIP_BUILD_CHECK=ON -DWITH_VA=ON -DWITH_VA_INTEL=ON -DBUILD_opencv_calib3d=ON -DBUILD_opencv_dnn=ON -DBUILD_opencv_features2d=ON -DBUILD_opencv_flann=ON -DBUILD_opencv_photo=ON -DBUILD_opencv_imgcodecs=ON -DBUILD_opencv_videoio=ON -DBUILD_opencv_highgui=ON -DBUILD_opencv_stitching=ON -DBUILD_opencv_ccalib=ON -DBUILD_opencv_face=ON -DBUILD_opencv_optflow=ON -DBUILD_opencv_plot=ON -DBUILD_opencv_tracking=ON -DBUILD_opencv_ximgproc=ON -DBUILD_opencv_world=OFF -DBUILD_EXAMPLES=ON -DBUILD_PACKAGE=OFF -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_DOCS=OFF -DWITH_PTHREADS_PF=ON -DBUILD_opencv_video=ON -DBUILD_opencv_v4d=ON -DGBFX_CONFIG_MULTITHREADED=OFF -DBGFX_CONFIG_PASSIVE=ON -DOPENCV_EXTRA_MODULES_PATH="../../Plan-V4D/modules" ..
```

### Build the Project
```bash
make -j$(sysctl -n hw.ncpu)
```
