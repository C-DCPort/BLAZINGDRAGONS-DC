#ifndef AUDIO_AUDIOSTREAM_H
#define AUDIO_AUDIOSTREAM_H
#include "common/scummsys.h"
namespace Audio {
class AudioStream { public: virtual ~AudioStream() {} };
class QueuingAudioStream : public AudioStream {};
}
#endif
