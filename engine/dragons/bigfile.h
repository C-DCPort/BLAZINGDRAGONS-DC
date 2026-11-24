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
#ifndef DRAGONS_BIGFILE_H
#define DRAGONS_BIGFILE_H

#include "common/array.h"
#include "common/file.h"
#include "disc.h"

namespace Dragons {

class DragonsEngine;

struct FileInfo {
	Common::String filename;
	uint32 offset;
	uint32 size;
	FileInfo() {
		offset = 0;
		size = 0;
		filename = "";
	}
};

class BigfileArchive {
private:
	DragonsEngine *_vm;
	Disc::File *_disc;
	uint16 _totalRecords;
	Common::Array<FileInfo> _fileInfoTbl;
	/* Port DC : BIGPACK.DAT (ressources compressées) à la place de BIGFILE.DAT quand il existe */
	bool _packed;
	uint32 *_packTbl;   /* par ressource : offset, taille compressée, taille d'origine, méthode */

public:
	BigfileArchive(DragonsEngine *vm, const char *filename);
	virtual ~BigfileArchive();

	byte *load(const char *filename, uint32 &dataSize);
	bool doesFileExist(const char *filename);

	/* Port DC : informations pour le préchargement */
	uint32 getResourceId(const char *filename);
	uint32 totalRecords() const { return _totalRecords; }
	const char *nameOf(uint32 id) const { return _fileInfoTbl[id].filename.c_str(); }
	/* octets à lire sur le disque pour cette ressource (compressée si BIGPACK.DAT) */
	uint32 diskSize(uint32 id) const { return _packed ? _packTbl[id * 4 + 1] : _fileInfoTbl[id].size; }
	bool isPacked() const { return _packed; }
	static bool isCached(uint32 id);

private:
	void loadFileInfoTbl();
};

} // End of namespace dragons

#endif //DRAGONS_BIGFILE_H
