// avion.dll (docs/sdk.md, "Embedding: API 12"): the C99 SDK as a dynamic library, for hosts in any language (C#, Python, Node,
// Rust, ...) and any compiler. Its exports are every function include/media/c_api.h declares (avion.def, generated from the header
// by CMake); the code is media_sdk's, linked in whole. Nothing runs when the library loads: no thread, no allocation in DllMain.
#include "media/c_api.h"
static_assert(MEDIA_API_VERSION == 17u, "avion.dll exports API 17");
