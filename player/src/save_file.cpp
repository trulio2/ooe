#include "save_file.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace fs = std::filesystem;

namespace {
constexpr uint64_t kIdleFlushMs = 1000;
}

void SaveFile::Open(std::string path) {
    path_ = std::move(path);
    data_.clear();
    dirty_ = false;
}

std::vector<uint8_t> SaveFile::Load() const {
    std::ifstream file(fs::u8path(path_), std::ios::binary);
    if (!file) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

void SaveFile::Write(const uint8_t* data, uint32_t length, uint64_t nowMs) {
    data_.assign(data, data + length);
    lastWriteMs_ = nowMs;
    dirty_ = true;
}

void SaveFile::FlushIfIdle(uint64_t nowMs) {
    if (dirty_ && nowMs - lastWriteMs_ >= kIdleFlushMs) Flush();
}

void SaveFile::Flush() {
    if (!dirty_ || path_.empty()) return;

    const fs::path target = fs::u8path(path_);
    fs::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(
            reinterpret_cast<const char*>(data_.data()), static_cast<std::streamsize>(data_.size())
        );
        if (!file) {
            SDL_Log("could not write save %s", path_.c_str());
            return;
        }
    }
    std::error_code error;
    fs::rename(temporary, target, error);
    if (error) {
        SDL_Log("could not replace save %s: %s", path_.c_str(), error.message().c_str());
        return;
    }
    dirty_ = false;
    SDL_Log("saved %s (%zu bytes)", path_.c_str(), data_.size());

#ifdef __EMSCRIPTEN__
    // /saves is an IndexedDB mount; push the new file to the browser's store.
    EM_ASM({
        FS.syncfs(
            false, function(error) {
                if (error) console.warn('[save] could not persist:', error);
            }
        );
    });
#endif
}
