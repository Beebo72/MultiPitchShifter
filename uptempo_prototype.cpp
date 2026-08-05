#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <objbase.h>
#include <shobjidl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "import/rubberband-src/rubberband/rubberband-c.h"

enum class Screen {
    Library,
    Player,
    PlayerEffects,
    Preferences,
    Export
};

struct Track {
    std::string title;
    std::string subtitle;
    std::wstring path;
    COLORREF art;
    HBITMAP thumbnail;
    bool active;
};

struct Button {
    RECT bounds;
    int action;
};

struct AudioData {
    unsigned sampleRate = 0;
    unsigned channels = 0;
    std::vector<float> samples;
    std::vector<float> waveform;
};

struct PlaybackData {
    unsigned sampleRate = 0;
    unsigned channels = 0;
    std::vector<int16_t> samples;
};

static const COLORREF teal = RGB(39, 113, 132);
static const COLORREF tealDark = RGB(19, 52, 61);
static const COLORREF black = RGB(0, 0, 0);
static const COLORREF panel = RGB(48, 47, 43);
static const COLORREF panel2 = RGB(34, 40, 40);
static const COLORREF white = RGB(246, 246, 250);
static const COLORREF muted = RGB(190, 188, 198);
static const COLORREF accent = RGB(255, 158, 11);
static const COLORREF red = RGB(248, 60, 52);
static const COLORREF sliderBlue = RGB(111, 164, 180);

static Screen screen = Screen::Library;
static bool drawerOpen = false;
static bool playing = false;
static bool reverseTrack = true;
static bool preserveFormant = false;
static bool useFfmpeg = true;
static bool ignoreFocus = true;
static bool mediaMarkers = false;
static bool showFilenames = false;
static int scrollOffset = 0;
static double pitchSemitones = -12.0;
static double tempoPercent = 100.0;
static AudioData loadedAudio;
static std::wstring loadedPath;
static std::string audioStatus = "Import a WAV file to play audio";
static std::mutex audioMutex;
static std::atomic<bool> rendering(false);
static std::atomic<unsigned> renderGeneration(0);
static HWAVEOUT waveOut = NULL;
static std::vector<WAVEHDR> waveHeaders;
static std::vector<std::vector<int16_t>> waveChunks;
static PlaybackData playback;
static size_t playbackFrame = 0;
static std::vector<Button> buttons;
static std::vector<Track> tracks;
static HWND mainWindow = NULL;

static RECT rc(int l, int t, int r, int b) {
    RECT out = {l, t, r, b};
    return out;
}

static std::string narrow(const std::wstring &value) {
    if (value.empty()) return "";
    int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, NULL, 0, NULL, NULL);
    std::string out(bytes > 0 ? bytes - 1 : 0, '\0');
    if (bytes > 1) WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &out[0], bytes, NULL, NULL);
    return out;
}

static std::wstring widen(const std::string &value) {
    if (value.empty()) return L"";
    int chars = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, NULL, 0);
    std::wstring out(chars > 0 ? chars - 1 : 0, L'\0');
    if (chars > 1) MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &out[0], chars);
    return out;
}

static std::wstring moduleDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring out(path);
    size_t slash = out.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : out.substr(0, slash);
}

static std::wstring libraryPath() {
    return moduleDir() + L"\\uptempo_imports.txt";
}

static std::wstring fileNameOf(const std::wstring &path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

static std::wstring folderOf(const std::wstring &path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

static std::string stripExtension(const std::wstring &path) {
    std::wstring name = fileNameOf(path);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name = name.substr(0, dot);
    return narrow(name);
}

static COLORREF colorFromPath(const std::wstring &path) {
    unsigned hash = 2166136261u;
    for (wchar_t ch : path) {
        hash ^= (unsigned)ch;
        hash *= 16777619u;
    }
    return RGB(50 + (hash & 0x7f), 65 + ((hash >> 8) & 0x6f), 85 + ((hash >> 16) & 0x6f));
}

static HBITMAP loadShellThumbnail(const std::wstring &path, int size) {
    IShellItemImageFactory *factory = NULL;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), NULL, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return NULL;
    SIZE thumbSize = {size, size};
    HBITMAP bitmap = NULL;
    hr = factory->GetImage(thumbSize, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &bitmap);
    if (FAILED(hr)) {
        hr = factory->GetImage(thumbSize, SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY, &bitmap);
    }
    factory->Release();
    return SUCCEEDED(hr) ? bitmap : NULL;
}

static bool samePath(const std::wstring &a, const std::wstring &b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

static void saveLibrary() {
    std::ofstream out(narrow(libraryPath()), std::ios::binary | std::ios::trunc);
    for (const Track &track : tracks) {
        out << narrow(track.path) << "\n";
    }
}

static void addImportedFile(const std::wstring &path, bool persist) {
    if (path.empty()) return;
    for (const Track &track : tracks) {
        if (samePath(track.path, path)) return;
    }

    WIN32_FILE_ATTRIBUTE_DATA attrs = {};
    std::string subtitle = narrow(folderOf(path));
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs)) {
        SYSTEMTIME utc = {}, local = {};
        FileTimeToSystemTime(&attrs.ftLastWriteTime, &utc);
        SystemTimeToTzSpecificLocalTime(NULL, &utc, &local);
        char date[64];
        wsprintfA(date, "Imported - %04d-%02d-%02d", local.wYear, local.wMonth, local.wDay);
        subtitle = date;
    }

    for (Track &track : tracks) track.active = false;
    Track track;
    track.title = stripExtension(path);
    track.subtitle = subtitle;
    track.path = path;
    track.art = colorFromPath(path);
    track.thumbnail = loadShellThumbnail(path, 96);
    track.active = true;
    tracks.push_back(track);
    if (persist) saveLibrary();
}

static void loadLibrary() {
    std::ifstream in(narrow(libraryPath()), std::ios::binary);
    std::string lineText;
    while (std::getline(in, lineText)) {
        if (!lineText.empty() && lineText.back() == '\r') lineText.pop_back();
        if (!lineText.empty()) addImportedFile(widen(lineText), false);
    }
    if (!tracks.empty()) tracks.front().active = true;
}

static Track *activeTrack() {
    for (Track &track : tracks) {
        if (track.active) return &track;
    }
    return tracks.empty() ? NULL : &tracks.front();
}

static void stopPlayback() {
    if (!waveOut) return;
    waveOutReset(waveOut);
    for (WAVEHDR &hdr : waveHeaders) {
        if (hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(waveOut, &hdr, sizeof(hdr));
    }
    waveOutClose(waveOut);
    waveOut = NULL;
    waveHeaders.clear();
    waveChunks.clear();
    playbackFrame = 0;
}

static bool readWavPcm16(const std::wstring &path, AudioData &out) {
    std::ifstream file(narrow(path), std::ios::binary);
    if (!file) return false;

    auto readU16 = [&]() -> uint16_t {
        unsigned char b[2] = {};
        file.read((char *)b, 2);
        return (uint16_t)(b[0] | (b[1] << 8));
    };
    auto readU32 = [&]() -> uint32_t {
        unsigned char b[4] = {};
        file.read((char *)b, 4);
        return (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
    };

    char id[4] = {};
    file.read(id, 4);
    if (std::string(id, 4) != "RIFF") return false;
    readU32();
    file.read(id, 4);
    if (std::string(id, 4) != "WAVE") return false;

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sampleRate = 0;
    std::vector<int16_t> pcm;
    while (file.read(id, 4)) {
        uint32_t chunkSize = readU32();
        std::streampos next = file.tellg();
        next += (std::streamoff)(chunkSize + (chunkSize & 1u));
        std::string chunk(id, 4);
        if (chunk == "fmt ") {
            format = readU16();
            channels = readU16();
            sampleRate = readU32();
            readU32();
            readU16();
            bits = readU16();
        } else if (chunk == "data") {
            if (format != 1 || bits != 16 || channels == 0) return false;
            pcm.resize(chunkSize / sizeof(int16_t));
            file.read((char *)pcm.data(), (std::streamsize)(pcm.size() * sizeof(int16_t)));
        }
        file.seekg(next);
    }

    if (pcm.empty() || sampleRate == 0 || channels == 0) return false;
    out.sampleRate = sampleRate;
    out.channels = channels;
    out.samples.resize(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) out.samples[i] = (float)pcm[i] / 32768.0f;

    size_t frames = out.samples.size() / out.channels;
    const size_t bins = 512;
    out.waveform.assign(bins, 0.0f);
    for (size_t i = 0; i < bins; ++i) {
        size_t start = i * frames / bins;
        size_t end = std::max(start + 1, (i + 1) * frames / bins);
        float peak = 0.0f;
        for (size_t f = start; f < end && f < frames; ++f) {
            float mono = 0.0f;
            for (unsigned ch = 0; ch < out.channels; ++ch) mono += out.samples[f * out.channels + ch];
            mono /= (float)out.channels;
            peak = std::max(peak, std::abs(mono));
        }
        out.waveform[i] = peak;
    }
    return true;
}

static void loadAudioForActiveTrack() {
    Track *track = activeTrack();
    if (!track) return;
    if (samePath(loadedPath, track->path)) return;

    stopPlayback();
    AudioData decoded;
    if (readWavPcm16(track->path, decoded)) {
        std::lock_guard<std::mutex> lock(audioMutex);
        loadedAudio = std::move(decoded);
        loadedPath = track->path;
        audioStatus = "Ready";
    } else {
        std::lock_guard<std::mutex> lock(audioMutex);
        loadedAudio = AudioData();
        loadedPath = track->path;
        audioStatus = "Audio playback currently supports PCM 16-bit WAV files";
    }
}

static PlaybackData renderWithRubberBand(const AudioData &audio, double semitones, double tempo,
                                         bool formants, unsigned generation) {
    PlaybackData out;
    if (audio.samples.empty() || audio.channels == 0 || audio.sampleRate == 0) return out;

    double pitchScale = std::pow(2.0, semitones / 12.0);
    double timeRatio = 100.0 / std::max(10.0, tempo);
    int options = RubberBandOptionProcessOffline | RubberBandOptionEngineFiner |
                  RubberBandOptionPitchHighQuality | RubberBandOptionChannelsTogether;
    if (formants) options |= RubberBandOptionFormantPreserved;

    RubberBandState rb = rubberband_new(audio.sampleRate, audio.channels, options, timeRatio, pitchScale);
    if (!rb) return out;
    size_t frames = audio.samples.size() / audio.channels;
    rubberband_set_expected_input_duration(rb, (unsigned)frames);
    rubberband_set_max_process_size(rb, 2048);

    std::vector<std::vector<float>> channels(audio.channels, std::vector<float>(2048));
    std::vector<const float *> inPtrs(audio.channels);
    std::vector<std::vector<float>> outChannels(audio.channels, std::vector<float>(4096));
    std::vector<float *> outPtrs(audio.channels);
    std::vector<float> rendered;
    rendered.reserve((size_t)(audio.samples.size() * timeRatio) + 4096);

    auto fillBlock = [&](size_t pos, size_t count) {
        for (unsigned ch = 0; ch < audio.channels; ++ch) {
            for (size_t i = 0; i < count; ++i) channels[ch][i] = audio.samples[(pos + i) * audio.channels + ch];
            inPtrs[ch] = channels[ch].data();
        }
    };
    auto retrieve = [&]() {
        while (true) {
            int available = rubberband_available(rb);
            if (available <= 0) break;
            unsigned request = (unsigned)std::min(available, 4096);
            for (unsigned ch = 0; ch < audio.channels; ++ch) outPtrs[ch] = outChannels[ch].data();
            unsigned got = rubberband_retrieve(rb, outPtrs.data(), request);
            for (unsigned i = 0; i < got; ++i) {
                for (unsigned ch = 0; ch < audio.channels; ++ch) rendered.push_back(outChannels[ch][i]);
            }
        }
    };

    for (size_t pass = 0; pass < 2; ++pass) {
        for (size_t pos = 0; pos < frames; pos += 2048) {
            if (generation != renderGeneration.load()) {
                rubberband_delete(rb);
                return PlaybackData();
            }
            size_t count = std::min<size_t>(2048, frames - pos);
            fillBlock(pos, count);
            int final = (pos + count >= frames);
            if (pass == 0) rubberband_study(rb, inPtrs.data(), (unsigned)count, final);
            else {
                rubberband_process(rb, inPtrs.data(), (unsigned)count, final);
                retrieve();
            }
        }
        if (pass == 0) rubberband_calculate_stretch(rb);
    }
    retrieve();
    rubberband_delete(rb);

    out.sampleRate = audio.sampleRate;
    out.channels = audio.channels;
    out.samples.resize(rendered.size());
    for (size_t i = 0; i < rendered.size(); ++i) {
        float v = std::max(-1.0f, std::min(1.0f, rendered[i]));
        out.samples[i] = (int16_t)std::lrint(v * 32767.0f);
    }
    return out;
}

static void queueNextChunk();

static void CALLBACK waveCallback(HWAVEOUT, UINT msg, DWORD_PTR, DWORD_PTR param1, DWORD_PTR) {
    if (msg != WOM_DONE) return;
    WAVEHDR *hdr = (WAVEHDR *)param1;
    if (waveOut && hdr && (hdr->dwFlags & WHDR_PREPARED)) {
        waveOutUnprepareHeader(waveOut, hdr, sizeof(WAVEHDR));
    }
    queueNextChunk();
}

static void queueNextChunk() {
    if (!waveOut || playback.samples.empty()) return;
    size_t totalFrames = playback.samples.size() / playback.channels;
    if (playbackFrame >= totalFrames) {
        playing = false;
        if (mainWindow) InvalidateRect(mainWindow, NULL, FALSE);
        return;
    }
    const size_t chunkFrames = 2048;
    size_t frames = std::min(chunkFrames, totalFrames - playbackFrame);
    size_t sampleCount = frames * playback.channels;
    waveChunks.emplace_back(sampleCount);
    std::copy(playback.samples.begin() + playbackFrame * playback.channels,
              playback.samples.begin() + playbackFrame * playback.channels + sampleCount,
              waveChunks.back().begin());
    waveHeaders.push_back(WAVEHDR());
    WAVEHDR &hdr = waveHeaders.back();
    hdr.lpData = (LPSTR)waveChunks.back().data();
    hdr.dwBufferLength = (DWORD)(sampleCount * sizeof(int16_t));
    playbackFrame += frames;
    waveOutPrepareHeader(waveOut, &hdr, sizeof(hdr));
    waveOutWrite(waveOut, &hdr, sizeof(hdr));
}

static void startWaveOut() {
    stopPlayback();
    if (playback.samples.empty() || playback.channels == 0) return;
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = (WORD)playback.channels;
    fmt.nSamplesPerSec = playback.sampleRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * sizeof(int16_t));
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    MMRESULT result = waveOutOpen(&waveOut, WAVE_MAPPER, &fmt, (DWORD_PTR)waveCallback, 0, CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        waveOut = NULL;
        audioStatus = "Could not open Windows audio output";
        return;
    }
    playbackFrame = 0;
    size_t totalFrames = playback.samples.size() / playback.channels;
    size_t chunkCount = (totalFrames + 2047) / 2048;
    waveHeaders.reserve(chunkCount);
    waveChunks.reserve(chunkCount);
    for (int i = 0; i < 4; ++i) queueNextChunk();
}

static void rerenderAudio(bool autoPlay) {
    loadAudioForActiveTrack();
    AudioData source;
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        source = loadedAudio;
        if (source.samples.empty()) return;
        audioStatus = "Rendering Rubber Band preview...";
    }
    unsigned generation = ++renderGeneration;
    rendering = true;
    double semitones = pitchSemitones;
    double tempo = tempoPercent;
    bool formants = preserveFormant;
    std::thread([source, autoPlay, generation, semitones, tempo, formants]() {
        PlaybackData next = renderWithRubberBand(source, semitones, tempo, formants, generation);
        if (generation != renderGeneration.load()) {
            return;
        }
        if (next.samples.empty()) {
            rendering = false;
            audioStatus = "Rubber Band render failed";
            if (mainWindow) PostMessage(mainWindow, WM_APP + 1, 0, 0);
            return;
        }
        stopPlayback();
        playback = std::move(next);
        audioStatus = "Ready";
        rendering = false;
        if (autoPlay) {
            playing = true;
            startWaveOut();
        }
        if (mainWindow) PostMessage(mainWindow, WM_APP + 1, 0, 0);
    }).detach();
    if (mainWindow) InvalidateRect(mainWindow, NULL, FALSE);
}

static void importFiles(HWND owner) {
    wchar_t buffer[65536] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Media files\0*.mp4;*.mkv;*.avi;*.mov;*.webm;*.wmv;*.mp3;*.wav;*.flac;*.m4a;*.aac;*.ogg\0All files\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = sizeof(buffer) / sizeof(buffer[0]);
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Import media files";

    if (!GetOpenFileNameW(&ofn)) return;

    std::wstring first = buffer;
    wchar_t *cursor = buffer + first.size() + 1;
    if (*cursor == L'\0') {
        addImportedFile(first, false);
    } else {
        std::wstring directory = first;
        while (*cursor) {
            std::wstring name = cursor;
            addImportedFile(directory + L"\\" + name, false);
            cursor += name.size() + 1;
        }
    }
    saveLibrary();
}

static void addButton(int l, int t, int r, int b, int action) {
    Button button;
    button.bounds = rc(l, t, r, b);
    button.action = action;
    buttons.push_back(button);
}

static void fill(HDC dc, RECT r, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

static void roundFill(HDC dc, RECT r, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

static void strokeRound(HDC dc, RECT r, int radius, COLORREF color, int width = 2) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

static void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width = 2) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

static void text(HDC dc, const std::string &value, int x, int y, int size, bool bold = false,
                 COLORREF color = white, int maxWidth = 0) {
    HFONT font = CreateFontA(-size, 0, 0, 0, bold ? FW_BOLD : FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT bounds = rc(x, y, maxWidth > 0 ? x + maxWidth : 2000, y + size * 3);
    DrawTextA(dc, value.c_str(), -1, &bounds, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

static void centerText(HDC dc, const std::string &value, RECT bounds, int size, bool bold = true,
                       COLORREF color = white) {
    HFONT font = CreateFontA(-size, 0, 0, 0, bold ? FW_BOLD : FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextA(dc, value.c_str(), -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

static void drawHamburger(HDC dc, int x, int y, COLORREF color = white) {
    line(dc, x, y, x + 25, y, color, 3);
    line(dc, x, y + 10, x + 25, y + 10, color, 3);
    line(dc, x, y + 20, x + 25, y + 20, color, 3);
}

static void drawBack(HDC dc, int x, int y, COLORREF color = white) {
    line(dc, x + 24, y, x, y + 22, color, 4);
    line(dc, x, y + 22, x + 24, y + 44, color, 4);
    line(dc, x + 2, y + 22, x + 48, y + 22, color, 4);
}

static void drawChevronDown(HDC dc, int x, int y) {
    line(dc, x, y, x + 15, y + 14, white, 4);
    line(dc, x + 15, y + 14, x + 30, y, white, 4);
}

static void drawSearch(HDC dc, int x, int y) {
    HPEN pen = CreatePen(PS_SOLID, 4, white);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x, y, x + 26, y + 26);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    line(dc, x + 21, y + 21, x + 38, y + 38, white, 4);
}

static void drawDownload(HDC dc, int x, int y, COLORREF color = white) {
    line(dc, x + 18, y, x + 18, y + 28, color, 5);
    line(dc, x + 5, y + 18, x + 18, y + 31, color, 5);
    line(dc, x + 31, y + 18, x + 18, y + 31, color, 5);
    line(dc, x + 2, y + 42, x + 34, y + 42, color, 4);
}

static void drawShuffle(HDC dc, int x, int y, COLORREF color = white) {
    line(dc, x, y + 34, x + 18, y + 16, color, 4);
    line(dc, x + 18, y + 16, x + 32, y + 16, color, 4);
    line(dc, x + 24, y + 8, x + 34, y + 16, color, 4);
    line(dc, x + 24, y + 24, x + 34, y + 16, color, 4);
    line(dc, x, y + 16, x + 18, y + 34, color, 4);
    line(dc, x + 18, y + 34, x + 32, y + 34, color, 4);
    line(dc, x + 24, y + 26, x + 34, y + 34, color, 4);
    line(dc, x + 24, y + 42, x + 34, y + 34, color, 4);
}

static void drawPlayPause(HDC dc, RECT r) {
    roundFill(dc, r, 28, red);
    if (playing) {
        fill(dc, rc(r.left + 27, r.top + 20, r.left + 38, r.bottom - 20), white);
        fill(dc, rc(r.right - 38, r.top + 20, r.right - 27, r.bottom - 20), white);
    } else {
        POINT p[3] = {{r.left + 34, r.top + 18}, {r.left + 34, r.bottom - 18}, {r.right - 24, (r.top + r.bottom) / 2}};
        HBRUSH brush = CreateSolidBrush(white);
        HGDIOBJ old = SelectObject(dc, brush);
        Polygon(dc, p, 3);
        SelectObject(dc, old);
        DeleteObject(brush);
    }
}

static void drawCheckbox(HDC dc, RECT r, bool checked) {
    if (checked) {
        roundFill(dc, r, 3, white);
        line(dc, r.left + 8, r.top + 19, r.left + 17, r.top + 28, black, 4);
        line(dc, r.left + 17, r.top + 28, r.right - 7, r.top + 8, black, 4);
    } else {
        strokeRound(dc, r, 3, white, 3);
    }
}

static void drawSwitch(HDC dc, RECT r, bool on) {
    roundFill(dc, r, 36, on ? sliderBlue : RGB(36, 36, 36));
    strokeRound(dc, r, 36, RGB(150, 150, 164), 3);
    int knob = (r.bottom - r.top) - 12;
    int left = on ? r.right - knob - 8 : r.left + 8;
    roundFill(dc, rc(left, r.top + 6, left + knob, r.bottom - 6), knob, white);
}

static void drawStatusAndNav(HDC dc, int w, int h) {
    fill(dc, rc(0, 0, w, h), black);
}

static void drawTopBar(HDC dc, int w, const std::string &title, bool back, bool menu = false) {
    fill(dc, rc(0, 0, w, 76), teal);
    if (back) {
        drawBack(dc, 24, 18);
    } else if (menu) {
        drawHamburger(dc, 24, 28);
        addButton(0, 0, 64, 76, 10);
    }
    text(dc, title, 74, 21, 27, true);
}

static void drawTrackArt(HDC dc, RECT r, const Track &track) {
    if (track.thumbnail) {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, track.thumbnail);
        BITMAP bmp = {};
        GetObject(track.thumbnail, sizeof(bmp), &bmp);
        StretchBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                   mem, 0, 0, bmp.bmWidth, bmp.bmHeight, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        strokeRound(dc, r, 6, RGB(20, 20, 20), 1);
        return;
    }
    roundFill(dc, r, 6, track.art);
    line(dc, r.left + 8, r.top + 8, r.right - 8, r.bottom - 8, RGB(80, 190, 170), 3);
    line(dc, r.left + 8, r.bottom - 10, r.right - 10, r.top + 11, RGB(80, 65, 185), 3);
    HBRUSH brush = CreateSolidBrush(black);
    HGDIOBJ old = SelectObject(dc, brush);
    Ellipse(dc, r.left + 19, r.top + 18, r.left + 55, r.top + 54);
    SelectObject(dc, old);
    DeleteObject(brush);
}

static void drawMiniPlayer(HDC dc, int w, int h) {
    Track *track = activeTrack();
    if (!track) return;
    int y = h - 78;
    fill(dc, rc(0, y, w, h), panel);
    drawChevronDown(dc, 28, y + 39);
    text(dc, track->title, 74, y + 12, 21, true, white, w - 170);
    text(dc, track->subtitle, 74, y + 44, 16, true, muted, w - 170);
    drawShuffle(dc, w - 120, y + 34);
    text(dc, playing ? "II" : ">", w - 56, y + 31, 28, true);
    addButton(0, y, w, h, 2);
}

static void drawLibrary(HDC dc, int w, int h) {
    drawStatusAndNav(dc, w, h);
    drawTopBar(dc, w, "Recently Played", false, true);
    drawChevronDown(dc, 278, 30);
    drawSearch(dc, w - 108, 24);
    drawHamburger(dc, w - 44, 25);

    fill(dc, rc(0, 76, w, h), black);
    int y = 86 - scrollOffset;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].active) fill(dc, rc(0, y - 6, w, y + 78), RGB(13, 24, 25));
        drawTrackArt(dc, rc(22, y + 6, 84, y + 68), tracks[i]);
        text(dc, tracks[i].title, 106, y + 10, 23, true, white, w - 132);
        text(dc, tracks[i].subtitle, 106, y + 45, 18, true, muted, w - 118);
        addButton(0, y, w, y + 82, 1000 + (int)i);
        y += 88;
    }
    if (tracks.empty()) {
        text(dc, "No imported files yet", 28, 146, 24, true);
        text(dc, "Click + to import videos or audio. Video thumbnails use the first frame Windows can provide.", 28, 190, 17, true, muted, w - 56);
    }

    RECT fab = rc(w - 94, h - 116, w - 22, h - 44);
    roundFill(dc, fab, 24, red);
    centerText(dc, "+", fab, 34, false);
    addButton(fab.left, fab.top, fab.right, fab.bottom, 40);
    drawMiniPlayer(dc, w, h);
}

static void drawPlayerHeader(HDC dc, int w) {
    Track *track = activeTrack();
    std::string title = track ? track->title : "No file selected";
    std::string subtitle = track ? track->subtitle : "Import a file from the library";
    fill(dc, rc(0, 0, w, 76), teal);
    drawChevronDown(dc, 28, 30);
    text(dc, title, 74, 12, 24, true, white, w - 230);
    text(dc, subtitle, 74, 43, 18, true, white, w - 230);
    drawDownload(dc, w - 168, 22);
    text(dc, "R", w - 101, 24, 28, true);
    drawShuffle(dc, w - 44, 19);
    addButton(0, 0, 64, 76, 1);
    addButton(w - 186, 0, w - 138, 76, 5);
}

static void drawWaveform(HDC dc, int w, int top, int height) {
    int center = top + height / 2;
    std::vector<float> wave;
    std::string status;
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        wave = loadedAudio.waveform;
        status = audioStatus;
    }
    if (wave.empty()) {
        text(dc, status, 24, center - 16, 17, true, muted, w - 48);
        return;
    }
    int bars = std::min<int>((int)wave.size(), std::max(40, (w - 44) / 5));
    for (int i = 0; i < bars; ++i) {
        float value = wave[(size_t)i * wave.size() / bars];
        int amp = 8 + (int)(std::min(1.0f, value * 2.2f) * (height / 2 - 18));
        int x = 18 + i * (w - 44) / bars;
        line(dc, x, center - amp, x, center + amp, accent, 3);
    }
    text(dc, rendering ? "rendering..." : "00:00.00", w - 128, top + 4, 17, false, muted);
    POINT tri[3] = {{w - 76, top + 54}, {w - 55, top + 54}, {w - 66, top + 72}};
    HBRUSH brush = CreateSolidBrush(accent);
    HGDIOBJ old = SelectObject(dc, brush);
    Polygon(dc, tri, 3);
    SelectObject(dc, old);
    DeleteObject(brush);
}

static void drawSlider(HDC dc, int x, int y, int w, double pos, bool blueLeft = false) {
    roundFill(dc, rc(x, y, x + w, y + 8), 8, RGB(62, 73, 73));
    if (blueLeft) roundFill(dc, rc(x, y, x + (int)(w * pos), y + 8), 8, sliderBlue);
    int knobX = x + (int)(w * pos);
    line(dc, knobX, y - 12, knobX, y + 24, white, 3);
}

static void drawPitchPanel(HDC dc, int w, int y) {
    roundFill(dc, rc(6, y, w - 6, y + 210), 18, panel);
    text(dc, "Pitch", 68, y + 34, 22, true);
    text(dc, "-", 158, y + 32, 26, true);
    addButton(146, y + 20, 188, y + 72, 50);
    strokeRound(dc, rc(202, y + 24, 296, y + 76), 32, RGB(135, 135, 150), 2);
    char pitchText[32];
    sprintf(pitchText, "%.1f", pitchSemitones);
    centerText(dc, pitchText, rc(202, y + 24, 296, y + 76), 24, true);
    text(dc, "+", 322, y + 31, 28, false);
    addButton(312, y + 20, 354, y + 72, 51);
    text(dc, "O", w - 60, y + 34, 25, true);
    addButton(w - 74, y + 20, w - 24, y + 76, 52);
    drawSlider(dc, 82, y + 88, w - 110, (pitchSemitones + 24.0) / 48.0);

    text(dc, "Tempo", 68, y + 126, 22, true);
    text(dc, "-", 158, y + 124, 26, true);
    addButton(146, y + 112, 188, y + 164, 53);
    strokeRound(dc, rc(202, y + 116, 296, y + 168), 32, RGB(135, 135, 150), 2);
    char tempoText[32];
    sprintf(tempoText, "%.0f%%", tempoPercent);
    centerText(dc, tempoText, rc(202, y + 116, 296, y + 168), 24, true);
    text(dc, "+", 322, y + 123, 28, false);
    addButton(312, y + 112, 354, y + 164, 54);
    text(dc, "O", w - 60, y + 126, 25, true);
    addButton(w - 74, y + 112, w - 24, y + 168, 55);
    drawSlider(dc, 68, y + 180, w - 98, (tempoPercent - 50.0) / 150.0, true);
}

static void drawTransport(HDC dc, int w, int h) {
    int y = h - 148;
    fill(dc, rc(0, y, w, h), panel);
    text(dc, "|<", 36, y + 30, 28, true);
    text(dc, "<<", 120, y + 34, 28, true, RGB(95, 95, 95));
    drawPlayPause(dc, rc(w / 2 - 38, y + 20, w / 2 + 38, y + 96));
    text(dc, ">>", w - 150, y + 34, 28, true, RGB(95, 95, 95));
    text(dc, ">|", w - 62, y + 30, 28, true);
    text(dc, "off", 40, y + 112, 19, true, RGB(145, 145, 145));
    text(dc, "-10", 128, y + 112, 17, true);
    text(dc, "+10", w / 2 - 12, y + 112, 17, true);
    text(dc, "1", w - 146, y + 112, 19, true);
    drawShuffle(dc, w - 62, y + 108);
    addButton(w / 2 - 50, y + 10, w / 2 + 50, y + 106, 4);
}

static void drawPlayer(HDC dc, int w, int h, bool effects) {
    drawStatusAndNav(dc, w, h);
    fill(dc, rc(0, 76, w, h), black);
    drawPlayerHeader(dc, w);

    int y = 82 - scrollOffset;
    if (!effects) {
        drawWaveform(dc, w, y + 8, 238);
        y += 282;
    } else {
        y += 22;
    }

    roundFill(dc, rc(22, y, 86, y + 44), 12, teal);
    text(dc, "AB", 51, y + 10, 18, false);
    text(dc, "X", w - 132, y + 7, 27, true);
    drawShuffle(dc, w - 64, y + 5);
    y += 72;

    roundFill(dc, rc(22, y, w - 22, y + 22), 8, sliderBlue);
    line(dc, effects ? 94 : w - 64, y - 3, effects ? 94 : w - 64, y + 27, accent, 2);
    text(dc, "00:00", 22, y + 28, 20, false, muted);
    text(dc, "00:00", w - 76, y + 28, 20, false, muted);
    y += 72;

    drawPitchPanel(dc, w, y);
    y += 224;

    if (effects) {
        roundFill(dc, rc(6, y, w / 2 - 8, y + 92), 14, panel);
        text(dc, "Preserv\ne\nformant", 26, y + 16, 20, true);
        drawSwitch(dc, rc(w / 2 - 86, y + 26, w / 2 - 28, y + 68), preserveFormant);
        addButton(6, y, w / 2 - 8, y + 92, 20);
        roundFill(dc, rc(w / 2 + 8, y, w - 6, y + 92), 14, panel);
        text(dc, "Reverse\ntrack", w / 2 + 30, y + 20, 20, true);
        drawSwitch(dc, rc(w - 94, y + 26, w - 26, y + 68), reverseTrack);
        addButton(w / 2 + 8, y, w - 6, y + 92, 21);
        y += 102;
        roundFill(dc, rc(6, y, w / 2 - 8, y + 72), 12, panel);
        text(dc, "Separate\ninstruments", 64, y + 12, 20, true);
        roundFill(dc, rc(w / 2 + 8, y, w - 6, y + 72), 12, panel);
        text(dc, "Remove\nvocals", w / 2 + 66, y + 12, 20, true);
        y += 84;
        roundFill(dc, rc(6, y, w - 6, y + 96), 14, panel);
        text(dc, "EFFECTS", 26, y + 28, 22, true);
        strokeRound(dc, rc(140, y + 27, 166, y + 53), 28, white, 3);
        centerText(dc, "?", rc(140, y + 25, 166, y + 55), 18, true);
        roundFill(dc, rc(w / 2 - 28, y + 17, w / 2 + 28, y + 73), 16, sliderBlue);
        centerText(dc, "+", rc(w / 2 - 28, y + 17, w / 2 + 28, y + 73), 30, false);
        text(dc, "[]", w / 2 + 74, y + 30, 24, true);
        text(dc, "Folder", w - 76, y + 32, 16, true);
    }

    drawTransport(dc, w, h);
    addButton(0, 76, w, h - 148, effects ? 2 : 3);
}

static void drawPreferences(HDC dc, int w, int h) {
    drawStatusAndNav(dc, w, h);
    drawTopBar(dc, w, "Preferences", true);
    addButton(0, 0, 72, 76, 1);
    fill(dc, rc(0, 76, w, h), black);
    int y = 100 - scrollOffset;
    text(dc, "App theme", 22, y, 19, true);
    y += 66;
    text(dc, "Use FFmpeg", 22, y, 20, true);
    text(dc, "Use FFmpeg library for decoding\nfiles. Recommended.", 22, y + 28, 18, true, muted, w - 110);
    drawCheckbox(dc, rc(w - 66, y + 24, w - 42, y + 48), useFfmpeg);
    addButton(0, y, w, y + 88, 30);
    y += 112;
    text(dc, "Ignore audio focus", 22, y, 20, true);
    text(dc, "Keep playing even when other apps\nmake sound.", 22, y + 28, 18, true, muted, w - 110);
    drawCheckbox(dc, rc(w - 66, y + 24, w - 42, y + 48), ignoreFocus);
    addButton(0, y, w, y + 88, 31);
    y += 118;
    text(dc, "Media controls skip markers", 22, y, 20, true);
    text(dc, "Media controls should skip markers\ninstead of tracks", 22, y + 28, 18, true, muted, w - 95);
    drawCheckbox(dc, rc(w - 66, y + 24, w - 42, y + 48), mediaMarkers);
    addButton(0, y, w, y + 88, 32);
    y += 116;
    text(dc, "Show filenames in track lists", 22, y, 20, true);
    drawCheckbox(dc, rc(w - 66, y + 1, w - 42, y + 25), showFilenames);
    addButton(0, y - 16, w, y + 52, 33);
    y += 70;
    text(dc, "Default playlist cover art", 22, y, 19, true);
    y += 74;
    text(dc, "Pitch/time stretching algorithm", 22, y, 20, true);
    text(dc, "Choose 'fastest' if 'highest quality' glitches on\nyour device", 22, y + 28, 18, true, muted, w - 45);
}

static void drawRadio(HDC dc, int x, int y, bool selected) {
    HPEN pen = CreatePen(PS_SOLID, 3, white);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x, y, x + 28, y + 28);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    if (selected) {
        HPEN p2 = CreatePen(PS_SOLID, 5, white);
        oldPen = SelectObject(dc, p2);
        Ellipse(dc, x + 7, y + 7, x + 21, y + 21);
        SelectObject(dc, oldPen);
        DeleteObject(p2);
    }
}

static void drawInput(HDC dc, RECT r, const std::string &label, const std::string &value, int size = 20) {
    strokeRound(dc, r, 6, RGB(145, 145, 160), 2);
    fill(dc, rc(r.left + 16, r.top - 10, r.left + 16 + (int)label.size() * 11, r.top + 8), black);
    text(dc, label, r.left + 20, r.top - 12, 15, true);
    text(dc, value, r.left + 20, r.top + 20, size, true, white, r.right - r.left - 34);
}

static void drawExport(HDC dc, int w, int h) {
    Track *track = activeTrack();
    std::string title = track ? track->title : "untitled";
    std::string subtitle = track ? track->subtitle : "";
    drawStatusAndNav(dc, w, h);
    drawTopBar(dc, w, "Export", true);
    text(dc, "X", 28, 21, 32, false);
    addButton(0, 0, 72, 76, 2);
    fill(dc, rc(0, 76, w, h), black);

    RECT fab = rc(w - 94, 42, w - 22, 114);
    roundFill(dc, fab, 24, red);
    drawDownload(dc, w - 76, 106);

    int sy = -scrollOffset;
    drawInput(dc, rc(22, 106 + sy, w - 166, 216 + sy), "Exported file name",
              title + "\n-12.00 semitones", 20);
    text(dc, "mp3", w - 148, 148 + sy, 20, true);
    drawChevronDown(dc, w - 74, 155 + sy);
    text(dc, "Stereo", w - 158, 202 + sy, 20, true);
    drawChevronDown(dc, w - 64, 210 + sy);

    text(dc, "Bit rate", 22, 242 + sy, 18, true);
    const char *rates[] = {
        "Original bit rate (195 kbps)",
        "128 kbps (smaller file, lower quality)",
        "256 kbps (larger file, higher quality)",
        "320 kbps (largest file, highest quality)"
    };
    for (int i = 0; i < 4; ++i) {
        int y = 286 + sy + i * 64;
        drawRadio(dc, 28, y, i == 1);
        text(dc, rates[i], 64, y + 3, 18, true, muted, w - 74);
    }

    text(dc, "Pitch/time\nstretching\nalgorithm", 22, 532 + sy, 18, true);
    text(dc, "Highest quality", w - 252, 550 + sy, 20, true);
    drawChevronDown(dc, w - 66, 558 + sy);
    text(dc, "Track details", 22, 624 + sy, 18, true);
    drawInput(dc, rc(22, 668 + sy, w - 22, 728 + sy), "Title", title + " -12.00 semitones", 19);
    drawInput(dc, rc(22, 742 + sy, w - 22, 802 + sy), "Artist", subtitle, 19);
    drawInput(dc, rc(22, 816 + sy, w - 22, 876 + sy), "", "Album", 19);
}

static void drawDrawer(HDC dc, int w, int h) {
    fill(dc, rc(0, 0, w, h), RGB(0, 0, 0));
    drawLibrary(dc, w, h);
    HBRUSH shade = CreateSolidBrush(RGB(0, 0, 0));
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 110, 0};
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT full = rc(0, 0, w, h);
    FillRect(mem, &full, shade);
    AlphaBlend(dc, 0, 0, w, h, mem, 0, 0, w, h, blend);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    DeleteObject(shade);

    int dw = std::min(w - 68, 360);
    roundFill(dc, rc(0, 0, dw, h), 26, panel);
    fill(dc, rc(0, 0, dw, 158), teal);
    text(dc, ">>>>", 28, 78, 30, true);
    text(dc, "Up Tempo", 108, 82, 26, true);

    int y = 198;
    const char *items[] = {"Download   My tracks", "Bag        Separation jobs", "Cloud      Backup settings",
                           "Info       About", "...        More apps", "?          Help",
                           "Gear       Preferences", "Heart      Rate this app", "Share      Share this app"};
    for (int i = 0; i < 9; ++i) {
        if (i == 3 || i == 7) line(dc, 36, y - 24, dw - 36, y - 24, RGB(145, 140, 150), 1);
        text(dc, items[i], 42, y, 20, true);
        if (i == 6) addButton(0, y - 12, dw, y + 42, 6);
        y += (i == 2 || i == 6) ? 82 : 64;
    }
    addButton(dw, 0, w, h, 11);
}

static void repaint(HWND hwnd) {
    InvalidateRect(hwnd, NULL, FALSE);
}

static void handleAction(int action) {
    if (action >= 1000) {
        int index = action - 1000;
        if (index >= 0 && index < (int)tracks.size()) {
            for (Track &track : tracks) track.active = false;
            tracks[index].active = true;
            screen = Screen::Player;
            scrollOffset = 0;
            drawerOpen = false;
            loadAudioForActiveTrack();
            rerenderAudio(false);
        }
        repaint(mainWindow);
        return;
    }

    switch (action) {
        case 1: screen = Screen::Library; scrollOffset = 0; drawerOpen = false; break;
        case 2: screen = Screen::Player; scrollOffset = 0; drawerOpen = false; loadAudioForActiveTrack(); break;
        case 3: screen = Screen::PlayerEffects; scrollOffset = 0; break;
        case 4:
            if (playing) {
                playing = false;
                stopPlayback();
            } else {
                if (playback.samples.empty()) rerenderAudio(true);
                else {
                    playing = true;
                    startWaveOut();
                }
            }
            break;
        case 5: screen = Screen::Export; scrollOffset = 0; break;
        case 6: screen = Screen::Preferences; scrollOffset = 0; drawerOpen = false; break;
        case 10: drawerOpen = true; break;
        case 11: drawerOpen = false; break;
        case 20: preserveFormant = !preserveFormant; rerenderAudio(playing); break;
        case 21: reverseTrack = !reverseTrack; break;
        case 30: useFfmpeg = !useFfmpeg; break;
        case 31: ignoreFocus = !ignoreFocus; break;
        case 32: mediaMarkers = !mediaMarkers; break;
        case 33: showFilenames = !showFilenames; break;
        case 40: importFiles(mainWindow); loadAudioForActiveTrack(); rerenderAudio(false); break;
        case 50: pitchSemitones = std::max(-24.0, pitchSemitones - 1.0); rerenderAudio(playing); break;
        case 51: pitchSemitones = std::min(24.0, pitchSemitones + 1.0); rerenderAudio(playing); break;
        case 52: pitchSemitones = 0.0; rerenderAudio(playing); break;
        case 53: tempoPercent = std::max(50.0, tempoPercent - 5.0); rerenderAudio(playing); break;
        case 54: tempoPercent = std::min(200.0, tempoPercent + 5.0); rerenderAudio(playing); break;
        case 55: tempoPercent = 100.0; rerenderAudio(playing); break;
        default: break;
    }
    repaint(mainWindow);
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            mainWindow = hwnd;
            return 0;
        case WM_LBUTTONDOWN: {
            int x = (short)LOWORD(lparam);
            int y = (short)HIWORD(lparam);
            for (int i = (int)buttons.size() - 1; i >= 0; --i) {
                RECT r = buttons[i].bounds;
                if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
                    handleAction(buttons[i].action);
                    return 0;
                }
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                if (drawerOpen) drawerOpen = false;
                else screen = Screen::Library;
                scrollOffset = 0;
                repaint(hwnd);
            }
            return 0;
        case WM_APP + 1:
            repaint(hwnd);
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            scrollOffset -= delta / 3;
            int maxScroll = 0;
            if (screen == Screen::Library) maxScroll = std::max(0, (int)tracks.size() * 88 - 420);
            else if (screen == Screen::PlayerEffects) maxScroll = 260;
            else if (screen == Screen::Preferences) maxScroll = 260;
            else if (screen == Screen::Export) maxScroll = 260;
            else maxScroll = 120;
            scrollOffset = std::max(0, std::min(scrollOffset, maxScroll));
            repaint(hwnd);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            int w = client.right - client.left;
            int h = client.bottom - client.top;
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            buttons.clear();
            SetBkMode(mem, TRANSPARENT);
            switch (screen) {
                case Screen::Library: drawLibrary(mem, w, h); break;
                case Screen::Player: drawPlayer(mem, w, h, false); break;
                case Screen::PlayerEffects: drawPlayer(mem, w, h, true); break;
                case Screen::Preferences: drawPreferences(mem, w, h); break;
                case Screen::Export: drawExport(mem, w, h); break;
            }
            if (drawerOpen) drawDrawer(mem, w, h);
            BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            ++renderGeneration;
            stopPlayback();
            saveLibrary();
            for (Track &track : tracks) {
                if (track.thumbnail) {
                    DeleteObject(track.thumbnail);
                    track.thumbnail = NULL;
                }
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    loadLibrary();

    WNDCLASSA wc = {};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = "UpTempoPrototypeWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);

    RECT desired = {0, 0, 430, 900};
    AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "Up Tempo C++ Prototype",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                desired.right - desired.left, desired.bottom - desired.top,
                                NULL, NULL, instance, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    CoUninitialize();
    return 0;
}
