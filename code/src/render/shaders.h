#pragma once

#include <string>

struct ID3D10Blob;

namespace mc::shaders
{
    extern const char* const map_source;

    // Compiles HLSL at startup (d3dcompiler_47 ships with Windows). Offline compilation comes later.
    bool compile(const char* source, const char* name, const char* entry, const char* target, ID3D10Blob** blob,
                 std::string* error);
}
