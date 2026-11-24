/* Mélangeur factice : le son du portage passe par port/snd, pas par ce type. */
#ifndef AUDIO_MIXER_H
#define AUDIO_MIXER_H
#include "common/scummsys.h"
namespace Audio {
class AudioStream;
class SoundHandle { public: SoundHandle() : _val(0xFFFFFFFF) {} uint32 _val; };
class Mixer {
public:
	enum SoundType { kPlainSoundType = 0, kMusicSoundType, kSFXSoundType, kSpeechSoundType };
	enum { kMaxChannelVolume = 255, kMaxMixerVolume = 256 };
	void playStream(SoundType, SoundHandle *, AudioStream *, int = -1, byte = 255, int8 = 0) {}
	void stopHandle(SoundHandle) {}
	bool isSoundHandleActive(SoundHandle) { return false; }
	void muteSoundType(SoundType, bool) {}
	void setVolumeForSoundType(SoundType, int) {}
	void stopAll() {}
};
}
#endif
