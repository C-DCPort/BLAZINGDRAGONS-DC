#ifndef AUDIO_MIDIPLAYER_H
#define AUDIO_MIDIPLAYER_H
#include "common/scummsys.h"
namespace Audio {
struct MidiDriverStub { uint32 getBaseTempo() { return 0; } };
class MidiPlayer {
public:
	virtual ~MidiPlayer() {}
	virtual void setVolume(int) {}
	virtual void sendToChannel(byte, uint32) {}
	void stop() {}
	bool isPlaying() const { return false; }
protected:
	MidiDriverStub *_driver = nullptr;
};
}
#endif
