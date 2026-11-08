// tagwritetest's own host shim: the files as they are (the preview's hostshim maps paths and carries the whole app with it)
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <direct.h>
std::string cleanGenre(const std::string& g) { return g; }   // (library.cpp's reads ID3v1 numbers; none here)
namespace plat { const char* configDir() { return "."; } }   // (coverio asks; nothing is kept here)
// the PC build's file calls (winshim.h): the paths as they are (the test's are plain ASCII); (fopen) etc. pass by winshim's macros
FILE* gfFopen(const char* p, const char* m) { return (fopen)(p, m); }
int gfRemove(const char* p) { return (remove)(p); }
int gfRename(const char* a, const char* b) { return (rename)(a, b); }
namespace plat { void placeThread(int, int) {} }
