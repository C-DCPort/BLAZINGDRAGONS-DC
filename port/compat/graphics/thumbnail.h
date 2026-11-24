#ifndef GRAPHICS_THUMBNAIL_H
#define GRAPHICS_THUMBNAIL_H
namespace Common { class SeekableReadStream; class WriteStream; }
namespace Graphics {
struct Surface;
inline bool loadThumbnail(Common::SeekableReadStream &, Graphics::Surface *&thumb, bool = false) { thumb = nullptr; return true; }
inline bool saveThumbnail(Common::WriteStream &) { return true; }
inline bool skipThumbnail(Common::SeekableReadStream &) { return true; }
}
#endif
