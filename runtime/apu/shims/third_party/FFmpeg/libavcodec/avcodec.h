// Shim for the path third_party/FFmpeg/libavcodec/avcodec.h of xenia-canary
// (FFmpeg submodule of https://github.com/xenia-canary/xenia-canary @ e6cd2e4):
// forwards to the header of the FFmpeg subset vendored in thirdparty/ffmpeg
// (xenia_ffmpeg, LGPL-2.1-or-later; its root is among the xenia_ffmpeg
// includes). The XMA contexts include it inside extern "C".
#include <libavcodec/avcodec.h>
