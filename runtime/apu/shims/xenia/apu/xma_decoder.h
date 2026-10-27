// Shim for xenia-canary's src/xenia/apu/xma_decoder.h
// (https://github.com/xenia-canary/xenia-canary @ e6cd2e4bc998609ca25e24a24c8e32b66c5908ad,
// BSD-3-Clause): it does not declare XmaDecoder. The contexts include it without using it;
// the role of XmaDecoder (registers, worker, context allocation) belongs to gow2
// (runtime/kernel/xma.cpp and runtime/apu). It only brings in xenia/base/filesystem.h, which
// the original reaches through xenia/kernel/xthread.h and XmaContext::DumpRaw uses.
#ifndef XENIA_APU_XMA_DECODER_H_
#define XENIA_APU_XMA_DECODER_H_
#include "xenia/base/filesystem.h"
#endif  // XENIA_APU_XMA_DECODER_H_
