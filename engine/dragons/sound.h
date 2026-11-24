/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#ifndef DRAGONS_SOUND_H
#define DRAGONS_SOUND_H

#include "common/scummsys.h"
#include "audio/mixer.h"
#include "audio/audiostream.h"
#include "dragons/midimusicplayer.h"


namespace Dragons {

class DragonsEngine;
class BigfileArchive;
class DragonRMS;
class VabSound;
struct SpeechLocation;

typedef struct Voice {
	int16 soundID;
	Audio::SoundHandle handle;

	Voice() {
		soundID = -1;
	}
} Voice;

#define NUM_VOICES 25

class SoundManager {
public:
	SoundManager(DragonsEngine *vm, BigfileArchive* bigFileArchive, DragonRMS *dragonRms);
	~SoundManager();

	void loadMsf(uint32 sceneId);
	void playOrStopSound(uint16 soundId);

	void playMusic(int16 song);
	void playSpeech(uint32 textIndex);
	bool isSpeechPlaying();
	int speechDurationMs();
	void resumeMusic();
	void syncSoundSettings();

public:
	uint16 _dat_8006bb60_sound_related;

public:
	struct Impl;
private:
	/* Port Dreamcast : l'implémentation (mélangeur logiciel + séquenceur) est dans port/snd. */
	DragonsEngine *_vm;
	BigfileArchive *_bigFileArchive;
	DragonRMS *_dragonRMS;
	Impl *_impl;
	void SomeInitSound_FUN_8003f64c();
	void initVabData();
	void playSound(uint16 soundId, uint16 i);
	void stopSound(uint16 id, uint16 i);
	bool getSpeechLocation(uint32 talkId, struct SpeechLocation *location);
	bool isVoicePlaying(uint16 soundID);
	void stopAllVoices();
};

} // End of namespace Dragons

#endif //DRAGONS_SOUND_H
