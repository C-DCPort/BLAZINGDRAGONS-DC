#ifndef COMMON_SAVEFILE_H
#define COMMON_SAVEFILE_H
#include "common/stream.h"
#include "common/str.h"
#include "common/array.h"
namespace Common {
typedef SeekableReadStream InSaveFile;
typedef WriteStream OutSaveFile;
typedef Array<String> StringArray;
class SaveFileManager {
public:
	virtual ~SaveFileManager() {}
	virtual InSaveFile *openForLoading(const String &name) = 0;
	virtual OutSaveFile *openForSaving(const String &name, bool compress = true) = 0;
	virtual bool removeSavefile(const String &name) = 0;
	virtual StringArray listSavefiles(const String &pattern) = 0;
};
}
#endif
