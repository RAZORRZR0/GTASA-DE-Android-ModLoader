#include <mod/amlmod.h>
#include <mod/logger.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <climits>
#include <unistd.h>

MYMOD(net.gtamod.loadingmusic, SALoadingMusic, 1.0, gtamod)
NEEDGAME(com.rockstargames.gtasa.de)

extern "C" {
    int stb_vorbis_decode_memory(const unsigned char *mem, int len, int *channels, int *sample_rate, short **output);
}

// ----------------------------------------------------------------------------
// Configuration (SALoadingMusic.ini)
// ----------------------------------------------------------------------------
static float g_volume = 1.25f;
static float g_fadeOutSeconds = 0.75f;

static inline bool IniLookup(const char* path, const char* section, const char* key, char* out, size_t outLen) {
    out[0] = '\0';
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512], cur[128] = "";
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '[') {
            char* e = strchr(p, ']');
            if (!e) continue;
            size_t n = (size_t)(e - p - 1);
            if (n >= sizeof(cur)) n = sizeof(cur) - 1;
            memcpy(cur, p + 1, n);
            cur[n] = '\0';
            continue;
        }
        if (strcasecmp(cur, section) != 0) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        char* k = p;
        char* ke = eq;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) --ke;
        if ((size_t)(ke - k) != strlen(key) || strncasecmp(k, key, (size_t)(ke - k)) != 0) continue;
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        char* ve = v + strlen(v);
        while (ve > v && (ve[-1] == '\n' || ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) --ve;
        size_t n = (size_t)(ve - v);
        if (n >= outLen) n = outLen - 1;
        memcpy(out, v, n);
        out[n] = '\0';
        found = true;
    }
    fclose(f);
    return found;
}

static void LoadConfig() {
    char path[PATH_MAX];
    const char* cfgDir = aml->GetConfigPath();
    snprintf(path, sizeof(path), "%s%sSALoadingMusic.ini", cfgDir, (cfgDir[0] && cfgDir[strlen(cfgDir) - 1] == '/') ? "" : "/");

    char val[64];
    if (IniLookup(path, "SALoadingMusic", "Volume", val, sizeof(val))) {
        float v = (float)atof(val);
        if (v >= 0.0f) g_volume = v;
    }
    if (IniLookup(path, "SALoadingMusic", "FadeOutSeconds", val, sizeof(val))) {
        float f = (float)atof(val);
        if (f >= 0.0f) g_fadeOutSeconds = f;
    }
}

extern "C" __attribute__((visibility("default"))) void GTAML_OnConfigChanged() {
    LoadConfig();
    logger->Info("Config reloaded: Volume=%.2f, FadeOutSeconds=%.2f", g_volume, g_fadeOutSeconds);
}

// ----------------------------------------------------------------------------
// Audio playback & decoding
// ----------------------------------------------------------------------------
constexpr int kSampleRate = 48000;
constexpr int kChunkSamples = 2048; // Stereo frames per buffer callback (~42.7 ms)

struct PlayState {
    int trackIndex = -1;
    size_t sampleOffset = 0;
    bool isPlaying = false;
    bool isFading = false;
    float currentFade = 1.0f;
    float fadeStep = 0.0f;
    struct timespec startTime{};
};

static std::mutex g_audioLock;
static PlayState g_playState;
static std::vector<std::vector<int16_t>> g_tracks;
static std::atomic<bool> g_tracksLoaded{false};
static std::atomic<bool> g_workerStarted{false};
static std::atomic<bool> g_wantPlay{false};
static int g_lastTrack = -1;

// OpenSL ES globals
static SLObjectItf g_engineObj = nullptr;
static SLEngineItf g_engineItf = nullptr;
static SLObjectItf g_outputMixObj = nullptr;
static SLObjectItf g_playerObj = nullptr;
static SLPlayItf g_playItf = nullptr;
static SLAndroidSimpleBufferQueueItf g_bqItf = nullptr;

static int16_t g_audioBuf[2][kChunkSamples * 2]; // 2 channels stereo
static int g_bufIdx = 0;

// UE4 functions
typedef bool (*LoadFileToArray_Fn)(void* Result, const char16_t* Filename, uint32_t Flags);
typedef void (*ResizeTo_Fn)(void* self, int32_t newSize);

static LoadFileToArray_Fn fnLoadFileToArray = nullptr;
static ResizeTo_Fn fnResizeTo = nullptr;

static std::vector<int16_t> ResampleMono(const int16_t* input, int inSamples, int inRate, int outRate) {
    if (inRate == outRate) {
        return std::vector<int16_t>(input, input + inSamples);
    }
    int64_t outSamples = (int64_t)inSamples * outRate / inRate;
    std::vector<int16_t> output(outSamples);
    double ratio = (double)inRate / (double)outRate;
    for (int64_t i = 0; i < outSamples; ++i) {
        double srcPos = i * ratio;
        int64_t srcIdx = (int64_t)srcPos;
        double frac = srcPos - srcIdx;
        int16_t s0 = (srcIdx < inSamples) ? input[srcIdx] : 0;
        int16_t s1 = (srcIdx + 1 < inSamples) ? input[srcIdx + 1] : s0;
        double sample = s0 + frac * (s1 - s0);
        if (sample > 32767.0) sample = 32767.0;
        else if (sample < -32768.0) sample = -32768.0;
        output[i] = (int16_t)sample;
    }
    return output;
}

static const uint8_t* FindOggStream(const uint8_t* data, size_t size, int32_t* outLen) {
    if (size < 0x20) return nullptr;
    for (size_t i = 0x0c; i + 4 <= size; ++i) {
        if (data[i] == 'O' && data[i + 1] == 'g' && data[i + 2] == 'g' && data[i + 3] == 'S') {
            if (i >= 12) {
                int32_t len = 0;
                memcpy(&len, data + i - 12, sizeof(int32_t));
                if (len > 0) {
                    if (i + (size_t)len > size) len = (int32_t)(size - i);
                    if (outLen) *outLen = len;
                    return data + i;
                }
            }
        }
    }
    return nullptr;
}

static void FillBufferLocked(int16_t* outBuf, int numFrames) {
    if (!g_playState.isPlaying || g_playState.trackIndex < 0 || g_playState.trackIndex >= (int)g_tracks.size()) {
        memset(outBuf, 0, numFrames * 2 * sizeof(int16_t));
        return;
    }

    const auto& track = g_tracks[g_playState.trackIndex];
    if (track.empty()) {
        memset(outBuf, 0, numFrames * 2 * sizeof(int16_t));
        return;
    }

    // 300 s backstop timeout check
    struct timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - g_playState.startTime.tv_sec) +
                     (now.tv_nsec - g_playState.startTime.tv_nsec) * 1e-9;
    if (elapsed >= 300.0 && !g_playState.isFading) {
        logger->Info("Stop: backstop (300s timeout)");
        if (g_fadeOutSeconds <= 0.0f) {
            g_playState.isPlaying = false;
            memset(outBuf, 0, numFrames * 2 * sizeof(int16_t));
            return;
        }
        g_playState.isFading = true;
        g_playState.currentFade = 1.0f;
        g_playState.fadeStep = 1.0f / (g_fadeOutSeconds * kSampleRate);
    }

    for (int i = 0; i < numFrames; ++i) {
        if (g_playState.sampleOffset >= track.size()) {
            // Loop track if loading takes longer than song
            g_playState.sampleOffset = 0;
        }

        if (g_playState.isFading) {
            g_playState.currentFade -= g_playState.fadeStep;
            if (g_playState.currentFade <= 0.0f) {
                g_playState.currentFade = 0.0f;
                g_playState.isPlaying = false;
                for (; i < numFrames; ++i) {
                    outBuf[2 * i] = 0;
                    outBuf[2 * i + 1] = 0;
                }
                logger->Info("Stop: fade complete");
                return;
            }
        }

        int16_t rawSample = track[g_playState.sampleOffset++];
        int32_t val = (int32_t)(rawSample * g_volume * g_playState.currentFade);
        if (val > 32767) val = 32767;
        else if (val < -32768) val = -32768;

        outBuf[2 * i] = (int16_t)val;
        outBuf[2 * i + 1] = (int16_t)val;
    }
}

static void BqCallback(SLAndroidSimpleBufferQueueItf bq, void* /*context*/) {
    std::lock_guard<std::mutex> lock(g_audioLock);
    if (!g_playState.isPlaying) {
        if (g_playItf) (*g_playItf)->SetPlayState(g_playItf, SL_PLAYSTATE_STOPPED);
        return;
    }

    FillBufferLocked(g_audioBuf[g_bufIdx], kChunkSamples);
    (*bq)->Enqueue(bq, g_audioBuf[g_bufIdx], kChunkSamples * 2 * sizeof(int16_t));
    g_bufIdx = 1 - g_bufIdx;
}

static bool InitOpenSL() {
    if (g_playerObj) return true;

    SLresult res;
    res = slCreateEngine(&g_engineObj, 0, nullptr, 0, nullptr, nullptr);
    if (res != SL_RESULT_SUCCESS) { logger->Error("slCreateEngine failed: %d", (int)res); return false; }
    res = (*g_engineObj)->Realize(g_engineObj, SL_BOOLEAN_FALSE);
    if (res != SL_RESULT_SUCCESS) { logger->Error("Realize engine failed: %d", (int)res); return false; }
    res = (*g_engineObj)->GetInterface(g_engineObj, SL_IID_ENGINE, &g_engineItf);
    if (res != SL_RESULT_SUCCESS) { logger->Error("GetInterface engine failed: %d", (int)res); return false; }

    res = (*g_engineItf)->CreateOutputMix(g_engineItf, &g_outputMixObj, 0, nullptr, nullptr);
    if (res != SL_RESULT_SUCCESS) { logger->Error("CreateOutputMix failed: %d", (int)res); return false; }
    res = (*g_outputMixObj)->Realize(g_outputMixObj, SL_BOOLEAN_FALSE);
    if (res != SL_RESULT_SUCCESS) { logger->Error("Realize OutputMix failed: %d", (int)res); return false; }

    SLDataLocator_AndroidSimpleBufferQueue bqLocator = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
        2
    };
    SLDataFormat_PCM pcmFormat = {
        SL_DATAFORMAT_PCM,
        2, // Stereo
        SL_SAMPLINGRATE_48,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
        SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource audioSrc = { &bqLocator, &pcmFormat };

    SLDataLocator_OutputMix mixLocator = {
        SL_DATALOCATOR_OUTPUTMIX,
        g_outputMixObj
    };
    SLDataSink audioSink = { &mixLocator, nullptr };

    const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
    const SLboolean req[] = { SL_BOOLEAN_TRUE };

    res = (*g_engineItf)->CreateAudioPlayer(g_engineItf, &g_playerObj, &audioSrc, &audioSink, 1, ids, req);
    if (res != SL_RESULT_SUCCESS) { logger->Error("CreateAudioPlayer failed: %d", (int)res); return false; }
    res = (*g_playerObj)->Realize(g_playerObj, SL_BOOLEAN_FALSE);
    if (res != SL_RESULT_SUCCESS) { logger->Error("Realize AudioPlayer failed: %d", (int)res); return false; }

    res = (*g_playerObj)->GetInterface(g_playerObj, SL_IID_PLAY, &g_playItf);
    if (res != SL_RESULT_SUCCESS) { logger->Error("GetInterface Play failed: %d", (int)res); return false; }

    res = (*g_playerObj)->GetInterface(g_playerObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_bqItf);
    if (res != SL_RESULT_SUCCESS) { logger->Error("GetInterface BufferQueue failed: %d", (int)res); return false; }

    res = (*g_bqItf)->RegisterCallback(g_bqItf, BqCallback, nullptr);
    if (res != SL_RESULT_SUCCESS) { logger->Error("RegisterCallback failed: %d", (int)res); return false; }

    return true;
}

static void StartPlaybackLocked() {
    // StartLoadingTransition fires more than once per loading screen: keep the track already playing.
    if (g_tracks.empty() || (g_playState.isPlaying && !g_playState.isFading)) return;

    int numTracks = (int)g_tracks.size();
    int selected = 0;
    if (numTracks > 1) {
        if (g_lastTrack < 0) {
            selected = rand() % numTracks;
        } else {
            selected = (rand() % (numTracks - 1) + 1 + g_lastTrack) % numTracks;
        }
    }
    g_lastTrack = selected;

    g_playState.trackIndex = selected;
    g_playState.sampleOffset = 0;
    g_playState.currentFade = 1.0f;
    g_playState.fadeStep = 0.0f;
    g_playState.isFading = false;
    g_playState.isPlaying = true;
    clock_gettime(CLOCK_MONOTONIC, &g_playState.startTime);

    logger->Info("Start: track %d (%zu samples)", selected, g_tracks[selected].size());

    if (g_bqItf && g_playItf) {
        (*g_playItf)->SetPlayState(g_playItf, SL_PLAYSTATE_STOPPED);
        (*g_bqItf)->Clear(g_bqItf);

        g_bufIdx = 0;
        FillBufferLocked(g_audioBuf[0], kChunkSamples);
        (*g_bqItf)->Enqueue(g_bqItf, g_audioBuf[0], kChunkSamples * 2 * sizeof(int16_t));
        FillBufferLocked(g_audioBuf[1], kChunkSamples);
        (*g_bqItf)->Enqueue(g_bqItf, g_audioBuf[1], kChunkSamples * 2 * sizeof(int16_t));

        (*g_playItf)->SetPlayState(g_playItf, SL_PLAYSTATE_PLAYING);
    }
}

static void StopPlaybackLocked() {
    g_playState.isPlaying = false;
    g_playState.isFading = false;
    if (g_playItf) (*g_playItf)->SetPlayState(g_playItf, SL_PLAYSTATE_STOPPED);
    if (g_bqItf) (*g_bqItf)->Clear(g_bqItf);
}

// ----------------------------------------------------------------------------
// Worker thread: loads & decodes tracks lazily
// ----------------------------------------------------------------------------
static void LoadTracksWorker() {
    InitOpenSL();
    int order[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    for (int i = 7; i > 0; --i) std::swap(order[i], order[rand() % (i + 1)]); // first decoded = first played

    for (int i : order) {
        char relPath[256];
        char16_t path[256];
        snprintf(relPath, sizeof(relPath), "../../../Gameface/Content/SanAndreas/Audio/SFX/GENRL/LOADING/S_%03d.uexp", i);
        for (int p = 0; relPath[p] != 0 && p < 255; ++p) path[p] = (char16_t)relPath[p];
        path[strlen(relPath)] = 0;

        struct UE4Array {
            uint8_t* data = nullptr;
            int32_t count = 0;
            int32_t max = 0;
        } arr{};

        bool ok = fnLoadFileToArray(&arr, path, 0);
        if (!ok || !arr.data || arr.count <= 0) {
            // Fallback attempt without ../../../
            snprintf(relPath, sizeof(relPath), "Gameface/Content/SanAndreas/Audio/SFX/GENRL/LOADING/S_%03d.uexp", i);
            for (int p = 0; relPath[p] != 0 && p < 255; ++p) path[p] = (char16_t)relPath[p];
            path[strlen(relPath)] = 0;
            ok = fnLoadFileToArray(&arr, path, 0);
        }

        if (!ok || !arr.data || arr.count <= 0) {
            continue; // Track missing in pak (stock mobile pak only has S_006 and S_007)
        }

        int32_t oggLen = 0;
        const uint8_t* oggPtr = FindOggStream(arr.data, (size_t)arr.count, &oggLen);
        if (!oggPtr || oggLen <= 0) {
            logger->Error("Track S_%03d.uexp: no Ogg stream found", i);
            if (fnResizeTo) fnResizeTo(&arr, 0);
            continue;
        }

        int channels = 0, sampleRate = 0;
        short* decoded = nullptr;
        int samples = stb_vorbis_decode_memory(oggPtr, oggLen, &channels, &sampleRate, &decoded);
        if (samples <= 0 || !decoded) {
            logger->Error("Track S_%03d.uexp: stb_vorbis decode failed", i);
            if (fnResizeTo) fnResizeTo(&arr, 0);
            continue;
        }

        if (channels != 1) {
            logger->Error("Track S_%03d.uexp has %d channels (expected 1), skipping", i, channels);
            free(decoded);
            if (fnResizeTo) fnResizeTo(&arr, 0);
            continue;
        }

        std::vector<int16_t> pcm = ResampleMono(decoded, samples, sampleRate, kSampleRate);
        free(decoded);
        if (fnResizeTo) fnResizeTo(&arr, 0);

        // Publish each track as soon as it is decoded: the first loading screen plays without waiting for all 8.
        std::lock_guard<std::mutex> lock(g_audioLock);
        g_tracks.push_back(std::move(pcm));
        if (g_wantPlay.load()) StartPlaybackLocked();
    }

    std::lock_guard<std::mutex> lock(g_audioLock);
    g_tracksLoaded = true;
    logger->Info("Tracks loaded: %zu", g_tracks.size());
}

static void OnLoadingStart() {
    g_wantPlay = true;
    if (!g_tracksLoaded.load()) {
        bool expected = false;
        if (g_workerStarted.compare_exchange_strong(expected, true)) {
            std::thread(LoadTracksWorker).detach();
        }
    } else {
        std::lock_guard<std::mutex> lock(g_audioLock);
        StartPlaybackLocked();
    }
}

static void OnLoadingStop(const char* reason) {
    g_wantPlay = false;
    std::lock_guard<std::mutex> lock(g_audioLock);
    if (!g_playState.isPlaying || g_playState.isFading) return;

    logger->Info("Stop: %s", reason);
    if (g_fadeOutSeconds <= 0.0f) {
        StopPlaybackLocked();
    } else {
        g_playState.isFading = true;
        g_playState.currentFade = 1.0f;
        g_playState.fadeStep = 1.0f / (g_fadeOutSeconds * kSampleRate);
    }
}

// ----------------------------------------------------------------------------
// Hooks
// ----------------------------------------------------------------------------
DECL_HOOKv(StartLoadingTransition, void* self) {
    StartLoadingTransition(self);
    OnLoadingStart();
}

static uintptr_t g_ue4Base;

DECL_HOOKv(CloseCurrentTransition, void* self) {
    uintptr_t ret = (uintptr_t)__builtin_return_address(0) - g_ue4Base;
    CloseCurrentTransition(self);
    // USanAndreasInterface::LoadDataInSlot calls Close right after Start (bl at 0x5652804) while the save still
    // loads; the loading screen really ends on a later Close (CGameLogic::Update, CCamera, ...).
    if (ret != 0x5652808) OnLoadingStop("transition finished");
}

DECL_HOOKv(Dtor_UUITransitionScreen, void* self) {
    OnLoadingStop("transition screen destroyed");
    Dtor_UUITransitionScreen(self);
}

// ----------------------------------------------------------------------------
// AML Mod Entry
// ----------------------------------------------------------------------------
extern "C" void OnModLoad() {
    logger->SetTag("LoadingMusic");
    void* ue4 = aml->GetLibHandle("libUE4.so");
    if (!ue4) {
        logger->Error("libUE4.so not found");
        return;
    }

    fnLoadFileToArray = (LoadFileToArray_Fn)aml->GetSym(ue4, "_ZN11FFileHelper15LoadFileToArrayER6TArrayIh22TSizedDefaultAllocatorILi32EEEPKDsj");
    fnResizeTo = (ResizeTo_Fn)aml->GetSym(ue4, "_ZN6TArrayIh22TSizedDefaultAllocatorILi32EEE8ResizeToEi");

    if (!fnLoadFileToArray) {
        logger->Error("FFileHelper::LoadFileToArray not found");
        return;
    }

    uintptr_t startLoading = aml->GetSym(ue4, "_ZN12UGameterface22StartLoadingTransitionEv");
    uintptr_t closeTransition = aml->GetSym(ue4, "_ZN12UGameterface22CloseCurrentTransitionEv");
    uintptr_t dtorTransitionScreen = aml->GetSym(ue4, "_ZN19UUITransitionScreenD0Ev");

    if (!startLoading || !closeTransition) {
        logger->Error("Transition hooks not found (start=%p, close=%p)", (void*)startLoading, (void*)closeTransition);
        return;
    }

    g_ue4Base = closeTransition - 0x4b1a670; // symbol address of this build (1.87.0)
    HOOK(StartLoadingTransition, startLoading);
    HOOK(CloseCurrentTransition, closeTransition);
    if (dtorTransitionScreen) {
        HOOK(Dtor_UUITransitionScreen, dtorTransitionScreen);
    }

    srand((unsigned int)time(nullptr));
    LoadConfig();
    logger->Info("Initialized (Volume=%.2f, FadeOutSeconds=%.2f)", g_volume, g_fadeOutSeconds);
}
