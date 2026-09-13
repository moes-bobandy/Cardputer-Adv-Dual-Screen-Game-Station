#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#define CHECK_MS  500
#define GAP_MS    15000

#ifndef PATH_MAX
#define PATH_MAX 261
#endif

namespace share
{
    bool gameIsSaving();
    const char* gameSaveBasename(const char* path);

    bool gameSaveBuildPath(
        char* dst, size_t dstLen,
        const char* saveDir,          // "/sd/gbc_saves", "/sd/sms_saves", ...
        const char* romPathOrName,
        const char* defaultName 
    );

    bool gameSaveEnsureParentReady(const char* baseDir);
    bool gameSaveIsTrivialSram(const uint8_t* p, size_t n);
    uint32_t gameSaveCrc32Update(uint32_t crc, const uint8_t* buf, size_t len);

    void setGameIsSaving(bool isSaving);
} // namespace share
