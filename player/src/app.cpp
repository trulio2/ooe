// DS Player: a minimal SDL3 frontend for the melonDS core. One window shows
// both screens; the same code runs as a desktop executable and, through
// Emscripten, as a WebAssembly page.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "Args.h"
#include "GPU3D_Soft.h"
#include "NDS.h"
#include "NDSCart.h"
#include "frontend.h"
#include "save_file.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr int kScreenWidth = 256;
constexpr int kScreenHeight = 192;
constexpr double kFrameRate = 33513982.0 / 560190.0;  // 59.8261 Hz
constexpr double kFrameNs = 1e9 / kFrameRate;
constexpr int kSampleRate = 48000;
constexpr int kMaxCatchUpFrames = 4;
constexpr uint64_t kFastForwardBudgetNs = 14'000'000;
constexpr int kMaxQueuedAudioBytes = kSampleRate * 4 / 8;  // 125 ms of stereo s16
constexpr int16_t kStickThreshold = 16000;
constexpr int kMinTouchFrames = 3;

#ifdef __EMSCRIPTEN__
constexpr int kWindowScale = 3;
#else
constexpr int kWindowScale = 2;
#endif

// Bit positions in melonDS's key mask (active low when handed to the core).
enum DSKey {
    KeyA,
    KeyB,
    KeySelect,
    KeyStart,
    KeyRight,
    KeyLeft,
    KeyUp,
    KeyDown,
    KeyR,
    KeyL,
    KeyX,
    KeyY,
};

struct KeyBinding {
    SDL_Scancode scancode;
    DSKey key;
};

constexpr KeyBinding kKeyboardBindings[] = {
    {SDL_SCANCODE_X, KeyA},
    {SDL_SCANCODE_Z, KeyB},
    {SDL_SCANCODE_S, KeyX},
    {SDL_SCANCODE_A, KeyY},
    {SDL_SCANCODE_Q, KeyL},
    {SDL_SCANCODE_W, KeyR},
    {SDL_SCANCODE_RETURN, KeyStart},
    {SDL_SCANCODE_BACKSPACE, KeySelect},
    {SDL_SCANCODE_RSHIFT, KeySelect},
    {SDL_SCANCODE_UP, KeyUp},
    {SDL_SCANCODE_DOWN, KeyDown},
    {SDL_SCANCODE_LEFT, KeyLeft},
    {SDL_SCANCODE_RIGHT, KeyRight},
};

struct ButtonBinding {
    SDL_GamepadButton button;
    DSKey key;
};

// Positional: the right face button is the DS's A, as on the DS itself.
constexpr ButtonBinding kGamepadBindings[] = {
    {SDL_GAMEPAD_BUTTON_EAST, KeyA},
    {SDL_GAMEPAD_BUTTON_SOUTH, KeyB},
    {SDL_GAMEPAD_BUTTON_NORTH, KeyX},
    {SDL_GAMEPAD_BUTTON_WEST, KeyY},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, KeyL},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, KeyR},
    {SDL_GAMEPAD_BUTTON_BACK, KeySelect},
    {SDL_GAMEPAD_BUTTON_START, KeyStart},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, KeyUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, KeyDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, KeyLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, KeyRight},
};

enum class Layout {
    Vertical,
    Horizontal,
};

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* screens = nullptr;
    SDL_AudioStream* audio = nullptr;
    std::vector<SDL_Gamepad*> gamepads;

    std::unique_ptr<melonDS::NDS> nds;
    SaveFile save;
    std::string romPath;
    std::string message = "Drop a .nds file here";
    bool stopped = false;

    // A press and release can arrive between two emulated frames; keys pressed
    // since the last frame and a fresh touch are held long enough to be seen.
    uint32_t heldKeys = 0;
    uint32_t keysSinceFrame = 0;
    bool touching = false;
    int touchHoldFrames = 0;
    int touchX = 0;
    int touchY = 0;

    Layout layout = Layout::Vertical;
    bool linearFilter = false;
    bool fastForward = false;
    bool vsync = false;
    uint64_t lastTickNs = 0;
    double frameDebtNs = 0;

    std::string title = "DS Player";
    int framesThisSecond = 0;
    uint64_t secondStartNs = 0;

    std::mutex pendingMutex;
    std::string pendingRom;
    std::atomic<bool> dialogOpen = false;
};

App* g_app = nullptr;

SDL_FRect TopScreenRect(const App& app) {
    return {0, 0, kScreenWidth, kScreenHeight};
}

SDL_FRect BottomScreenRect(const App& app) {
    if (app.layout == Layout::Horizontal) return {kScreenWidth, 0, kScreenWidth, kScreenHeight};
    return {0, kScreenHeight, kScreenWidth, kScreenHeight};
}

void ApplyLayout(App& app, bool resizeWindow) {
    const bool horizontal = app.layout == Layout::Horizontal;
    const int width = horizontal ? kScreenWidth * 2 : kScreenWidth;
    const int height = horizontal ? kScreenHeight : kScreenHeight * 2;
    SDL_SetRenderLogicalPresentation(
        app.renderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX
    );
    if (resizeWindow && !(SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN))
        SDL_SetWindowSize(app.window, width * kWindowScale, height * kWindowScale);
}

void SetClock(melonDS::NDS& nds) {
    SDL_Time now = 0;
    SDL_DateTime date{};
    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &date, true)) return;
    nds.RTC.SetDateTime(date.year, date.month, date.day, date.hour, date.minute, date.second);
}

std::string SavePathFor(const std::string& romPath) {
    fs::path rom = fs::u8path(romPath);
#ifdef __EMSCRIPTEN__
    return "/saves/" + rom.stem().u8string() + ".sav";
#else
    return rom.replace_extension(".sav").u8string();
#endif
}

std::string GameTitle(const melonDS::NDS& nds) {
    const auto* cart = nds.GetNDSCart();
    if (!cart) return {};
    const auto& header = cart->GetHeader();
    std::string title(header.GameTitle, sizeof(header.GameTitle));
    title.erase(std::find(title.begin(), title.end(), '\0'), title.end());
    return title;
}

bool LoadRom(App& app, const std::string& path) {
    size_t size = 0;
    void* bytes = SDL_LoadFile(path.c_str(), &size);
    if (!bytes) {
        app.message = "Could not read the ROM";
        SDL_Log("could not read %s: %s", path.c_str(), SDL_GetError());
        return false;
    }
    if (size < 0x200 || size > 0x40000000) {
        SDL_free(bytes);
        app.message = "That is not a DS ROM";
        return false;
    }
    auto rom = std::make_unique<melonDS::u8[]>(size);
    std::memcpy(rom.get(), bytes, size);
    SDL_free(bytes);

    // Write out the running game's save first: reloading the same ROM must
    // read the newest data back.
    app.save.Flush();
    SaveFile save;
    save.Open(SavePathFor(path));
    const std::vector<uint8_t> saveData = save.Load();

    melonDS::NDSCart::NDSCartArgs cartArgs;
    if (!saveData.empty()) {
        cartArgs.SRAM = std::make_unique<melonDS::u8[]>(saveData.size());
        std::memcpy(cartArgs.SRAM.get(), saveData.data(), saveData.size());
        cartArgs.SRAMLength = static_cast<melonDS::u32>(saveData.size());
    }
    auto cart = melonDS::NDSCart::ParseROM(
        std::move(rom), static_cast<melonDS::u32>(size), &app, std::move(cartArgs)
    );
    if (!cart) {
        app.message = "That is not a DS ROM";
        return false;
    }

    app.nds.reset();

    melonDS::NDSArgs args;
    args.OutputSampleRate = kSampleRate;
    auto nds = std::make_unique<melonDS::NDS>(std::move(args), &app);
    nds->SetNDSCart(std::move(cart));
    nds->Reset();
    if (nds->NeedsDirectBoot()) nds->SetupDirectBoot(fs::u8path(path).filename().u8string());
    SetClock(*nds);
    nds->Start();

    app.nds = std::move(nds);
    app.save = std::move(save);
    app.romPath = path;
    app.stopped = false;
    app.message.clear();
    app.frameDebtNs = 0;
    if (app.audio) SDL_ClearAudioStream(app.audio);

    const std::string title = GameTitle(*app.nds);
    app.title = title.empty() ? "DS Player" : "DS Player - " + title;
    SDL_SetWindowTitle(app.window, app.title.c_str());
    SDL_Log(
        "loaded %s (%s, save %s, %s boot)",
        path.c_str(),
        title.c_str(),
        saveData.empty() ? "new" : "existing",
        app.nds->NeedsDirectBoot() ? "direct" : "firmware"
    );
    return true;
}

#ifndef __EMSCRIPTEN__
void SDLCALL OnRomChosen(void* userdata, const char* const* files, int) {
    App& app = *static_cast<App*>(userdata);
    if (files && files[0]) {
        std::lock_guard lock(app.pendingMutex);
        app.pendingRom = files[0];
    }
    app.dialogOpen = false;
}

void OpenRomDialog(App& app) {
    if (app.dialogOpen.exchange(true)) return;
    static const SDL_DialogFileFilter filters[] = {
        {"Nintendo DS ROM", "nds"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(
        OnRomChosen, &app, app.window, filters, SDL_arraysize(filters), nullptr, false
    );
}
#endif

uint32_t GamepadKeys(const App& app) {
    uint32_t keys = 0;
    for (SDL_Gamepad* pad : app.gamepads) {
        for (const ButtonBinding& binding : kGamepadBindings)
            if (SDL_GetGamepadButton(pad, binding.button)) keys |= 1u << binding.key;

        const Sint16 x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        const Sint16 y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (x < -kStickThreshold) keys |= 1u << KeyLeft;
        if (x > kStickThreshold) keys |= 1u << KeyRight;
        if (y < -kStickThreshold) keys |= 1u << KeyUp;
        if (y > kStickThreshold) keys |= 1u << KeyDown;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kStickThreshold)
            keys |= 1u << KeyL;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > kStickThreshold)
            keys |= 1u << KeyR;
    }
    return keys;
}

void RunFrame(App& app) {
    const uint32_t held = app.heldKeys | app.keysSinceFrame | GamepadKeys(app);
    app.nds->SetKeyMask(~held & 0xFFF);
    if (app.touching || app.touchHoldFrames > 0)
        app.nds->TouchScreen(app.touchX, app.touchY);
    else
        app.nds->ReleaseScreen();
    app.nds->RunFrame();
    app.keysSinceFrame = 0;
    if (app.touchHoldFrames > 0) app.touchHoldFrames--;
}

void PumpAudio(App& app) {
    int16_t buffer[1024 * 2];
    int samples = 0;
    while ((samples = app.nds->SPU.ReadOutput(buffer, 1024)) > 0) {
        if (!app.audio || app.fastForward) continue;
        if (SDL_GetAudioStreamQueued(app.audio) > kMaxQueuedAudioBytes) continue;
        SDL_PutAudioStreamData(app.audio, buffer, samples * 2 * sizeof(int16_t));
    }
}

// Emulated frames per real second, in the title once a second.
void UpdateSpeedTitle(App& app, int frames, uint64_t nowNs) {
    app.framesThisSecond += frames;
    if (nowNs - app.secondStartNs < 1'000'000'000) return;
    const double seconds = (nowNs - app.secondStartNs) / 1e9;
    const int fps = static_cast<int>(app.framesThisSecond / seconds + 0.5);
    app.framesThisSecond = 0;
    app.secondStartNs = nowNs;
    SDL_SetWindowTitle(app.window, (app.title + " - " + std::to_string(fps) + " fps").c_str());
}

void UploadScreens(App& app) {
    melonDS::GPU& gpu = app.nds->GPU;
    const int front = gpu.FrontBuffer;
    if (!gpu.Framebuffer[front][0] || !gpu.Framebuffer[front][1]) return;
    const SDL_Rect top{0, 0, kScreenWidth, kScreenHeight};
    const SDL_Rect bottom{0, kScreenHeight, kScreenWidth, kScreenHeight};
    SDL_UpdateTexture(app.screens, &top, gpu.Framebuffer[front][0].get(), kScreenWidth * 4);
    SDL_UpdateTexture(app.screens, &bottom, gpu.Framebuffer[front][1].get(), kScreenWidth * 4);
}

void Render(App& app) {
    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
    SDL_RenderClear(app.renderer);

    if (app.nds && !app.stopped) {
        const SDL_FRect topSource{0, 0, kScreenWidth, kScreenHeight};
        const SDL_FRect bottomSource{0, kScreenHeight, kScreenWidth, kScreenHeight};
        const SDL_FRect top = TopScreenRect(app);
        const SDL_FRect bottom = BottomScreenRect(app);
        SDL_RenderTexture(app.renderer, app.screens, &topSource, &top);
        SDL_RenderTexture(app.renderer, app.screens, &bottomSource, &bottom);
    } else {
        SDL_SetRenderDrawColor(app.renderer, 220, 220, 220, 255);
        SDL_RenderDebugText(app.renderer, 8, 8, "DS Player");
        SDL_RenderDebugText(app.renderer, 8, 24, app.message.c_str());
#ifndef __EMSCRIPTEN__
        SDL_RenderDebugText(app.renderer, 8, 40, "or press Ctrl+O");
#endif
    }
    SDL_RenderPresent(app.renderer);
}

void UpdateTouch(App& app, float x, float y, bool pressed) {
    const SDL_FRect bottom = BottomScreenRect(app);
    if (pressed && !app.touching) {
        const SDL_FPoint point{x, y};
        if (!SDL_PointInRectFloat(&point, &bottom)) return;
        app.touchHoldFrames = kMinTouchFrames;
    }
    app.touching = pressed;
    app.touchX = std::clamp(static_cast<int>(x - bottom.x), 0, kScreenWidth - 1);
    app.touchY = std::clamp(static_cast<int>(y - bottom.y), 0, kScreenHeight - 1);
}

void ToggleFullscreen(App& app) {
    const bool fullscreen = SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN;
    SDL_SetWindowFullscreen(app.window, !fullscreen);
}

void HandleHotkey(App& app, const SDL_KeyboardEvent& key) {
    const bool ctrl = key.mod & SDL_KMOD_CTRL;
    switch (key.scancode) {
        case SDL_SCANCODE_F2:
            app.layout = app.layout == Layout::Vertical ? Layout::Horizontal : Layout::Vertical;
            ApplyLayout(app, true);
            break;
        case SDL_SCANCODE_F4:
            app.linearFilter = !app.linearFilter;
            SDL_SetTextureScaleMode(
                app.screens, app.linearFilter ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST
            );
            break;
        case SDL_SCANCODE_F11:
            ToggleFullscreen(app);
            break;
        case SDL_SCANCODE_RETURN:
            if (key.mod & SDL_KMOD_ALT) ToggleFullscreen(app);
            break;
        case SDL_SCANCODE_R:
            if (ctrl && !app.romPath.empty()) LoadRom(app, app.romPath);
            break;
#ifndef __EMSCRIPTEN__
        case SDL_SCANCODE_O:
            if (ctrl) OpenRomDialog(app);
            break;
#endif
        default:
            break;
    }
}

void SetKey(App& app, SDL_Scancode scancode, bool down) {
    for (const KeyBinding& binding : kKeyboardBindings) {
        if (binding.scancode != scancode) continue;
        if (down) {
            app.heldKeys |= 1u << binding.key;
            app.keysSinceFrame |= 1u << binding.key;
        } else
            app.heldKeys &= ~(1u << binding.key);
    }
}

}  // namespace

namespace frontend {
void WriteNDSSave(void* userdata, const uint8_t* data, uint32_t length) {
    static_cast<App*>(userdata)->save.Write(data, length, SDL_GetTicks());
}

void SignalStop(void* userdata, int reason) {
    App& app = *static_cast<App*>(userdata);
    app.stopped = true;
    app.message = reason == static_cast<int>(melonDS::Platform::PowerOff)
                      ? "The console powered off"
                      : "Emulation stopped";
    app.save.Flush();
}
}  // namespace frontend

#ifdef __EMSCRIPTEN__
extern "C" EMSCRIPTEN_KEEPALIVE void ds_player_flush_save() {
    if (g_app) g_app->save.Flush();
}

// Restarts the current game, picking up a save the page wrote into /saves.
extern "C" EMSCRIPTEN_KEEPALIVE void ds_player_reload() {
    if (g_app && !g_app->romPath.empty()) LoadRom(*g_app, g_app->romPath);
}
#endif

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    SDL_SetAppMetadata("DS Player", "0.1", "io.github.ds-player");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    auto* app = new App();
    *appstate = app;
    g_app = app;

    SDL_WindowFlags flags = 0;
#ifndef __EMSCRIPTEN__
    flags |= SDL_WINDOW_RESIZABLE;
#endif
    if (!SDL_CreateWindowAndRenderer(
            "DS Player",
            kScreenWidth * kWindowScale,
            kScreenHeight * 2 * kWindowScale,
            flags,
            &app->window,
            &app->renderer
        ))
    {
        SDL_Log("could not create the window: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    app->vsync = SDL_SetRenderVSync(app->renderer, 1);

    app->screens = SDL_CreateTexture(
        app->renderer,
        SDL_PIXELFORMAT_XRGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        kScreenWidth,
        kScreenHeight * 2
    );
    SDL_SetTextureScaleMode(app->screens, SDL_SCALEMODE_NEAREST);
    ApplyLayout(*app, false);

    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kSampleRate};
    app->audio =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (app->audio)
        SDL_ResumeAudioStreamDevice(app->audio);
    else
        SDL_Log("no audio: %s", SDL_GetError());

    if (argc > 1) LoadRom(*app, argv[1]);
#ifndef __EMSCRIPTEN__
    else
        OpenRomDialog(*app);
#endif

    app->lastTickNs = SDL_GetTicksNS();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    App& app = *static_cast<App*>(appstate);
    switch (event->type) {
        case SDL_EVENT_QUIT:
            return SDL_APP_SUCCESS;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const bool down = event->type == SDL_EVENT_KEY_DOWN;
            if (event->key.scancode == SDL_SCANCODE_TAB) app.fastForward = down;
            if (down && !event->key.repeat) HandleHotkey(app, event->key);
            if (!(event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) || !down)
                SetKey(app, event->key.scancode, down);
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event->button.button == SDL_BUTTON_LEFT) {
                SDL_ConvertEventToRenderCoordinates(app.renderer, event);
                UpdateTouch(
                    app,
                    event->button.x,
                    event->button.y,
                    event->type == SDL_EVENT_MOUSE_BUTTON_DOWN
                );
            }
            break;

        case SDL_EVENT_MOUSE_MOTION:
            if (app.touching) {
                SDL_ConvertEventToRenderCoordinates(app.renderer, event);
                UpdateTouch(app, event->motion.x, event->motion.y, true);
            }
            break;

        case SDL_EVENT_GAMEPAD_ADDED:
            if (SDL_Gamepad* pad = SDL_OpenGamepad(event->gdevice.which))
                app.gamepads.push_back(pad);
            break;

        case SDL_EVENT_GAMEPAD_REMOVED:
            for (auto it = app.gamepads.begin(); it != app.gamepads.end(); ++it) {
                if (SDL_GetGamepadID(*it) == event->gdevice.which) {
                    SDL_CloseGamepad(*it);
                    app.gamepads.erase(it);
                    break;
                }
            }
            break;

        case SDL_EVENT_DROP_FILE:
            if (event->drop.data) LoadRom(app, event->drop.data);
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            app.heldKeys = 0;
            app.touching = false;
            app.fastForward = false;
            break;

        default:
            break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    App& app = *static_cast<App*>(appstate);

#ifndef __EMSCRIPTEN__
    std::string pending;
    {
        std::lock_guard lock(app.pendingMutex);
        pending.swap(app.pendingRom);
    }
    if (!pending.empty()) LoadRom(app, pending);
#endif

    const uint64_t now = SDL_GetTicksNS();
    const uint64_t elapsed = std::min<uint64_t>(now - app.lastTickNs, 250'000'000);
    app.lastTickNs = now;

    if (app.nds && !app.stopped) {
        int frames = 0;
        if (app.fastForward) {
            do {
                RunFrame(app);
                frames++;
            } while (SDL_GetTicksNS() - now < kFastForwardBudgetNs && !app.stopped);
            app.frameDebtNs = 0;
        } else {
            app.frameDebtNs += static_cast<double>(elapsed);
            while (app.frameDebtNs >= kFrameNs && frames < kMaxCatchUpFrames && !app.stopped) {
                RunFrame(app);
                app.frameDebtNs -= kFrameNs;
                frames++;
            }
            // Too far behind to catch up: drop the backlog instead of spiralling.
            if (app.frameDebtNs > kFrameNs * kMaxCatchUpFrames) app.frameDebtNs = 0;
        }
        if (frames > 0) UploadScreens(app);
        PumpAudio(app);
        app.save.FlushIfIdle(SDL_GetTicks());
        UpdateSpeedTitle(app, frames, now);
    }

    Render(app);

#ifndef __EMSCRIPTEN__
    if (!app.vsync && !app.fastForward) {
        const uint64_t spent = SDL_GetTicksNS() - now;
        if (spent < static_cast<uint64_t>(kFrameNs))
            SDL_DelayPrecise(static_cast<uint64_t>(kFrameNs) - spent);
    }
#endif
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult) {
    App* app = static_cast<App*>(appstate);
    if (!app) return;
    app->save.Flush();
    app->nds.reset();
    for (SDL_Gamepad* pad : app->gamepads)
        SDL_CloseGamepad(pad);
    if (app->audio) SDL_DestroyAudioStream(app->audio);
    g_app = nullptr;
    delete app;
}
