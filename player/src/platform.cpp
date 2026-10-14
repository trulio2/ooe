// melonDS::Platform for a single-window SDL3 frontend: plain stdio files,
// std threading primitives, SDL timers, and stubs for hardware this player
// does not expose (Wi-Fi, camera, microphone, add-ons, AAC).

#include "Platform.h"
#include "SPI_Firmware.h"
#include "frontend.h"

#include <SDL3/SDL.h>

#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace melonDS::Platform {

static fs::path ToPath(const std::string& utf8) {
    return fs::u8path(utf8);
}

void SignalStop(StopReason reason, void* userdata) {
    frontend::SignalStop(userdata, static_cast<int>(reason));
}

std::string GetLocalFilePath(const std::string& filename) {
    return filename;
}

static std::FILE* OpenStdFile(const std::string& path, const char* mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(ToPath(path).c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

FileHandle* OpenFile(const std::string& path, FileMode mode) {
    if ((mode & (FileMode::Read | FileMode::Write)) == FileMode::None) return nullptr;

    const bool exists = FileExists(path);
    if ((mode & FileMode::Write) && (mode & FileMode::NoCreate) && !exists) return nullptr;

    std::string modeString;
    if (mode & FileMode::Append)
        modeString = (mode & FileMode::Read) ? "a+" : "a";
    else if (mode & FileMode::Write) {
        if ((mode & FileMode::Preserve) && exists)
            modeString = "r+";
        else
            modeString = (mode & FileMode::Read) ? "w+" : "w";
    } else
        modeString = "r";

    if (!(mode & FileMode::Text)) modeString += "b";

    return reinterpret_cast<FileHandle*>(OpenStdFile(path, modeString.c_str()));
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode) {
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string& name) {
    std::error_code error;
    return fs::is_regular_file(ToPath(name), error);
}

bool LocalFileExists(const std::string& name) {
    return FileExists(GetLocalFilePath(name));
}

bool CheckFileWritable(const std::string& filepath) {
    const bool existed = FileExists(filepath);
    std::FILE* file = OpenStdFile(filepath, "ab");
    if (!file) return false;
    std::fclose(file);
    if (!existed) {
        std::error_code error;
        fs::remove(ToPath(filepath), error);
    }
    return true;
}

bool CheckLocalFileWritable(const std::string& filepath) {
    return CheckFileWritable(GetLocalFilePath(filepath));
}

static std::FILE* Std(FileHandle* file) {
    return reinterpret_cast<std::FILE*>(file);
}

bool CloseFile(FileHandle* file) {
    return std::fclose(Std(file)) == 0;
}

bool IsEndOfFile(FileHandle* file) {
    return std::feof(Std(file)) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file) {
    return std::fgets(str, count, Std(file)) != nullptr;
}

u64 FilePosition(FileHandle* file) {
    return static_cast<u64>(std::ftell(Std(file)));
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin) {
    int whence = SEEK_SET;
    if (origin == FileSeekOrigin::Current)
        whence = SEEK_CUR;
    else if (origin == FileSeekOrigin::End)
        whence = SEEK_END;
    return std::fseek(Std(file), static_cast<long>(offset), whence) == 0;
}

void FileRewind(FileHandle* file) {
    std::rewind(Std(file));
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file) {
    return std::fread(data, size, count, Std(file));
}

bool FileFlush(FileHandle* file) {
    return std::fflush(Std(file)) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file) {
    return std::fwrite(data, size, count, Std(file));
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int written = std::vfprintf(Std(file), fmt, args);
    va_end(args);
    return written < 0 ? 0 : static_cast<u64>(written);
}

u64 FileLength(FileHandle* file) {
    std::FILE* stream = Std(file);
    const long position = std::ftell(stream);
    std::fseek(stream, 0, SEEK_END);
    const long length = std::ftell(stream);
    std::fseek(stream, position, SEEK_SET);
    return length < 0 ? 0 : static_cast<u64>(length);
}

void Log(LogLevel level, const char* fmt, ...) {
    if (level == LogLevel::Debug) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(level >= LogLevel::Warn ? stderr : stdout, fmt, args);
    va_end(args);
}

struct Thread {
    std::thread thread;
};

Thread* Thread_Create(std::function<void()> func) {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    (void)func;
    Log(LogLevel::Error, "threads are not available in this build\n");
    return nullptr;
#else
    return new Thread{std::thread(std::move(func))};
#endif
}

void Thread_Free(Thread* thread) {
    if (thread && thread->thread.joinable()) thread->thread.detach();
    delete thread;
}

void Thread_Wait(Thread* thread) {
    if (thread && thread->thread.joinable()) thread->thread.join();
}

struct Semaphore {
    std::mutex mutex;
    std::condition_variable condition;
    int count = 0;
};

Semaphore* Semaphore_Create() {
    return new Semaphore();
}

void Semaphore_Free(Semaphore* sema) {
    delete sema;
}

void Semaphore_Reset(Semaphore* sema) {
    std::lock_guard lock(sema->mutex);
    sema->count = 0;
}

void Semaphore_Wait(Semaphore* sema) {
    std::unique_lock lock(sema->mutex);
    sema->condition.wait(lock, [sema] { return sema->count > 0; });
    sema->count--;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms) {
    std::unique_lock lock(sema->mutex);
    if (timeout_ms == 0) {
        if (sema->count == 0) return false;
    } else if (!sema->condition.wait_for(lock, std::chrono::milliseconds(timeout_ms), [sema] {
                   return sema->count > 0;
               }))
        return false;
    sema->count--;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count) {
    {
        std::lock_guard lock(sema->mutex);
        sema->count += count;
    }
    sema->condition.notify_all();
}

struct Mutex {
    std::mutex mutex;
};

Mutex* Mutex_Create() {
    return new Mutex();
}

void Mutex_Free(Mutex* mutex) {
    delete mutex;
}

void Mutex_Lock(Mutex* mutex) {
    mutex->mutex.lock();
}

void Mutex_Unlock(Mutex* mutex) {
    mutex->mutex.unlock();
}

bool Mutex_TryLock(Mutex* mutex) {
    return mutex->mutex.try_lock();
}

void Sleep(u64 usecs) {
    SDL_DelayNS(usecs * 1000);
}

u64 GetMSCount() {
    return SDL_GetTicks();
}

u64 GetUSCount() {
    return SDL_GetTicksNS() / 1000;
}

void WriteNDSSave(const u8* savedata, u32 savelen, u32, u32, void* userdata) {
    frontend::WriteNDSSave(userdata, savedata, savelen);
}

void WriteGBASave(const u8*, u32, u32, u32, void*) {
}
void WriteFirmware(const Firmware&, u32, u32, void*) {
}
void WriteDateTime(int, int, int, int, int, int, void*) {
}

void MP_Begin(void*) {
}
void MP_End(void*) {
}
int MP_SendPacket(u8*, int, u64, void*) {
    return 0;
}
int MP_RecvPacket(u8*, u64*, void*) {
    return 0;
}
int MP_SendCmd(u8*, int, u64, void*) {
    return 0;
}
int MP_SendReply(u8*, int, u64, u16, void*) {
    return 0;
}
int MP_SendAck(u8*, int, u64, void*) {
    return 0;
}
int MP_RecvHostPacket(u8*, u64*, void*) {
    return 0;
}
u16 MP_RecvReplies(u8*, u64, u16, void*) {
    return 0;
}

int Net_SendPacket(u8*, int, void*) {
    return 0;
}
int Net_RecvPacket(u8*, void*) {
    return 0;
}

void Camera_Start(int, void*) {
}
void Camera_Stop(int, void*) {
}
void Camera_CaptureFrame(int, u32*, int, int, bool, void*) {
}

void Mic_Start(void*) {
}
void Mic_Stop(void*) {
}
int Mic_ReadInput(s16*, int, void*) {
    return 0;
}

AACDecoder* AAC_Init() {
    return nullptr;
}
void AAC_DeInit(AACDecoder*) {
}
bool AAC_Configure(AACDecoder*, int, int) {
    return false;
}
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) {
    return false;
}

bool Addon_KeyDown(KeyType, void*) {
    return false;
}
void Addon_RumbleStart(u32, void*) {
}
void Addon_RumbleStop(void*) {
}
float Addon_MotionQuery(MotionQueryType, void*) {
    return 0.0f;
}

DynamicLibrary* DynamicLibrary_Load(const char*) {
    return nullptr;
}
void DynamicLibrary_Unload(DynamicLibrary*) {
}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) {
    return nullptr;
}

}  // namespace melonDS::Platform
