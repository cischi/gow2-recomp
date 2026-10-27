// Vendored FFmpeg subset (thirdparty/ffmpeg, xenia_ffmpeg): the
// xmaframes decoder is found and opened as in XmaContextNew::Setup/PrepareDecoder
// of xenia-canary (2 channels, 48 000 Hz; FFmpeg 8 ch_layout API); full cleanup
// with no errors in FFmpeg's log.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdarg>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
}

namespace {
std::atomic<int> g_errors{0};

void CountErrors(void*, int level, const char*, va_list) {
  if (level <= AV_LOG_ERROR) g_errors.fetch_add(1);
}
}  // namespace

TEST_CASE("ffmpeg_smoke: xmaframes decoder found, opened with 2 channels and 48 kHz, freed") {
  av_log_set_callback(CountErrors);
  g_errors = 0;

  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  REQUIRE(codec != nullptr);
  CHECK(codec->type == AVMEDIA_TYPE_AUDIO);

  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  REQUIRE(ctx != nullptr);
  av_channel_layout_default(&ctx->ch_layout, 2);
  ctx->sample_rate = 48000;
  CHECK(avcodec_open2(ctx, codec, nullptr) == 0);
  CHECK(avcodec_is_open(ctx) != 0);

  // Reopening with other parameters, like PrepareDecoder (free + alloc + open2).
  avcodec_free_context(&ctx);
  ctx = avcodec_alloc_context3(codec);
  REQUIRE(ctx != nullptr);
  av_channel_layout_default(&ctx->ch_layout, 1);
  ctx->sample_rate = 44100;
  CHECK(avcodec_open2(ctx, codec, nullptr) == 0);

  AVFrame* frame = av_frame_alloc();
  REQUIRE(frame != nullptr);
  AVPacket* packet = av_packet_alloc();
  REQUIRE(packet != nullptr);

  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&ctx);
  CHECK(packet == nullptr);
  CHECK(frame == nullptr);
  CHECK(ctx == nullptr);
  CHECK(g_errors.load() == 0);

  av_log_set_callback(av_log_default_callback);
}
