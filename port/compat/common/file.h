/* Fichier en lecture seule, chemin relatif au dossier des données du jeu. */
#ifndef COMMON_FILE_H
#define COMMON_FILE_H
#include "common/stream.h"
#include "common/str.h"
#include "common/path.h"
namespace Common {
void setGameDataPath(const char *path);
void setGameDataFallback(const char *path);   /* dossier où chercher les fichiers absents du premier */
class File : public SeekableReadStream {
public:
	File() : _f(nullptr), _size(0), _eos(false) {}
	~File() override { close(); }
	bool open(const Path &filename);
	bool open(const char *filename);
	void close();
	bool isOpen() const { return _f != nullptr; }
	static bool exists(const Path &filename);
	bool err() const override { return false; }
	void clearErr() override { _eos = false; }
	bool eos() const override { return _eos; }
	uint32 read(void *dataPtr, uint32 dataSize) override;
	int64 pos() const override;
	int64 size() const override { return _size; }
	bool seek(int64 offset, int whence = SEEK_SET) override;
private:
	void *_f;
	int64 _size;
	bool _eos;
};
}
#endif
