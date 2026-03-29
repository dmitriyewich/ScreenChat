#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <shlobj.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

static_assert(sizeof(void*) == 4, "ScreenChat.asi must be built for Win32.");

struct ID3DXFont;
struct ID3DXSprite;
struct ID3DXRenderToSurface;

namespace {

constexpr DWORD kGtaLoadStateAddress = 0x00C8D4C0;
constexpr std::uintptr_t kGameLoopAddress = 0x00561B10;
constexpr char kConfigSection[] = "ScreenChat";
constexpr char kConfigPathKey[] = "SaveDirectory";
constexpr char kConfigKeyKey[] = "ScreenshotKey";
constexpr int kDefaultScreenshotKey = VK_F8;
constexpr char kDefaultFilePrefix[] = "chat1";
constexpr int kDefaultChatX = 45;
constexpr int kDefaultChatY = 10;
constexpr UINT kChatEntryCount = 100;
constexpr int kD3DXIFF_PNG = 3;
constexpr DWORD kChatLineTextFormat = 0x00000400;

using D3DXSaveSurfaceToFileA_t =
    HRESULT(WINAPI*)(LPCSTR, int, LPDIRECT3DSURFACE9, const PALETTEENTRY*, const RECT*);

using GameLoopFn = void(__cdecl*)();
GameLoopFn g_originalGameLoop = nullptr;

HMODULE g_module = nullptr;

enum class SampVersion {
    R1,
    R3_1,
    R5_2,
    DL_R1,
};

struct SampVersionInfo {
    DWORD entryPointRva;
    SampVersion version;
    const char* name;
    std::uint32_t pChatOffset;
    std::uint32_t chatPositionXOffset;
    std::uint32_t chatPositionYOffset;
};

constexpr std::array<SampVersionInfo, 5> kSupportedVersions{{
    {0x31DF13, SampVersion::R1, "R1", 0x21A0E4, 0x00063DB1, 0x00063DA0},
    {0x0CC490, SampVersion::R3_1, "R3-1", 0x26E8C8, 0x00067201, 0x000671F0},
    {0x0CC4D0, SampVersion::R3_1, "R3-1", 0x26E8C8, 0x00067201, 0x000671F0},
    {0x0CBC90, SampVersion::R5_2, "R5-2", 0x26EB80, 0x00067981, 0x00067217},
    {0x0FDB60, SampVersion::DL_R1, "DL-R1", 0x2ACA10, 0x000673F1, 0x000673E0},
}};

#pragma pack(push, 1)
struct ChatEntry {
    std::uint32_t systemTime;
    char prefix[28];
    char text[144];
    std::uint8_t unknown[64];
    int type;
    D3DCOLOR textColor;
    D3DCOLOR prefixColor;
};

struct FontRenderer {
    ID3DXFont* chatFont;
    ID3DXFont* littleFont;
    ID3DXFont* chatShadowFont;
    ID3DXFont* littleShadowFont;
    ID3DXFont* carNumberFont;
    ID3DXSprite* tempSprite;
    IDirect3DDevice9* d3dDevice;
    char* textBuffer;
};

struct ChatInfo {
    int pageSize;
    char* lastMessageText;
    int chatWindowMode;
    std::uint8_t timestampsEnabled;
    std::uint32_t logFileExists;
    char chatLogPath[MAX_PATH + 1];
    void* gameUi;
    void* editBackground;
    void* scrollBar;
    D3DCOLOR textColor;
    D3DCOLOR infoColor;
    D3DCOLOR debugColor;
    DWORD chatWindowBottom;
    ChatEntry entries[kChatEntryCount];
    FontRenderer* fontRenderer;
    ID3DXSprite* chatTextSprite;
    ID3DXSprite* sprite;
    IDirect3DDevice9* d3dDevice;
    int renderMode;
    ID3DXRenderToSurface* renderToSurface;
    IDirect3DTexture9* texture;
    IDirect3DSurface9* surface;
    D3DDISPLAYMODE displayMode;
    int pad[2];
    int redraw;
    int previousScrollBarPosition;
    int fontSizeY;
    int timestampWidth;
};

struct ScrollBarInfo {
    std::uint8_t padding[142];
    int currentPosition;
    int pageSize;
};
#pragma pack(pop)

static_assert(sizeof(ChatEntry) == 0xFC, "Unexpected ChatEntry layout.");
static_assert(offsetof(ChatInfo, scrollBar) == 0x11E, "Unexpected ChatInfo::scrollBar offset.");
static_assert(offsetof(ChatInfo, entries) == 0x132, "Unexpected ChatInfo::entries offset.");
static_assert(offsetof(ChatInfo, fontRenderer) == 0x63A2, "Unexpected ChatInfo::fontRenderer offset.");
static_assert(offsetof(ChatInfo, texture) == 0x63BA, "Unexpected ChatInfo::texture offset.");
static_assert(offsetof(ChatInfo, surface) == 0x63BE, "Unexpected ChatInfo::surface offset.");
static_assert(offsetof(ChatInfo, fontSizeY) == 0x63E2, "Unexpected ChatInfo::fontSizeY offset.");
static_assert(offsetof(ChatInfo, timestampWidth) == 0x63E6, "Unexpected ChatInfo::timestampWidth offset.");
static_assert(offsetof(ScrollBarInfo, currentPosition) == 142, "Unexpected ScrollBarInfo::currentPosition offset.");
static_assert(offsetof(ScrollBarInfo, pageSize) == 146, "Unexpected ScrollBarInfo::pageSize offset.");

struct Config {
    int screenshotKey = kDefaultScreenshotKey;
    std::string saveDirectory;
    std::string path;
};

struct RuntimeState {
    Config config;
    HMODULE sampModule = nullptr;
    const SampVersionInfo* version = nullptr;
    HMODULE d3dxModule = nullptr;
    D3DXSaveSurfaceToFileA_t saveSurfaceToFile = nullptr;
    bool hookInstalled = false;
    bool hotkeyPressed = false;
};

RuntimeState g_state;

template <typename T>
bool SafeRead(const void* address, T& value) {
    __try {
        value = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string FormatString(const char* format, ...) {
    char buffer[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);
    return std::string(buffer);
}

void DebugLog(const char* format, ...) {
    char buffer[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);

    std::string message = std::string("[ScreenChat] ") + buffer + "\n";
    OutputDebugStringA(message.c_str());
}

template <typename T>
void SafeRelease(T*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

template <typename Fn>
Fn BindMethod(void* object, std::size_t index) {
    if (object == nullptr) {
        return nullptr;
    }

    auto** vtable = *reinterpret_cast<void***>(object);
    return reinterpret_cast<Fn>(vtable[index]);
}

std::string Trim(std::string value) {
    const auto isSpace = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };

    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::string ToUpperAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool FileExists(const std::string& path) {
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::string& path) {
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }

    if (left.back() == '\\' || left.back() == '/') {
        return left + right;
    }
    return left + "\\" + right;
}

std::string ExpandEnvironmentStringsIfNeeded(const std::string& value) {
    const DWORD needed = ExpandEnvironmentStringsA(value.c_str(), nullptr, 0);
    if (needed == 0) {
        return value;
    }

    std::string expanded(needed, '\0');
    const DWORD written = ExpandEnvironmentStringsA(value.c_str(), expanded.data(), needed);
    if (written == 0 || written > needed) {
        return value;
    }

    expanded.resize(written > 0 ? written - 1 : 0);
    return expanded;
}

std::string BuildConfigPath() {
    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(g_module, modulePath, MAX_PATH) == 0) {
        return "ScreenChat.ini";
    }

    std::string path(modulePath);
    const std::size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) {
        return "ScreenChat.ini";
    }

    path.erase(slash + 1);
    path += "ScreenChat.ini";
    return path;
}

std::string GetDocumentsDirectory() {
    char documentsPath[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documentsPath))
        && documentsPath[0] != '\0') {
        return documentsPath;
    }

    char* userProfile = nullptr;
    std::size_t envLength = 0;
    if (_dupenv_s(&userProfile, &envLength, "USERPROFILE") == 0 && userProfile != nullptr) {
        std::string result = JoinPath(userProfile, "Documents");
        free(userProfile);
        return result;
    }

    return {};
}

std::string GetDefaultSaveDirectory() {
    const std::string documents = GetDocumentsDirectory();
    if (!documents.empty()) {
        return JoinPath(documents, "GTA San Andreas User Files\\Gallery");
    }
    return "Gallery";
}

std::optional<int> ParseInteger(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }

    char* end = nullptr;
    const long parsed = std::strtol(std::string(value).c_str(), &end, 0);
    if (end == nullptr || *end != '\0') {
        return std::nullopt;
    }
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

std::optional<int> ParseVirtualKey(std::string value) {
    value = ToUpperAscii(Trim(std::move(value)));
    if (value.empty()) {
        return std::nullopt;
    }

    if (value == "NONE" || value == "DISABLED" || value == "OFF") {
        return 0;
    }

    if (const auto numeric = ParseInteger(value)) {
        if (*numeric >= 0 && *numeric <= 0xFF) {
            return *numeric;
        }
    }

    if (value.size() == 1) {
        const char ch = value[0];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            return static_cast<unsigned char>(ch);
        }
    }

    if (value.size() >= 2 && value[0] == 'F') {
        if (const auto functionNumber = ParseInteger(value.substr(1))) {
            if (*functionNumber >= 1 && *functionNumber <= 24) {
                return VK_F1 + (*functionNumber - 1);
            }
        }
    }

    struct NamedKey {
        const char* name;
        int vk;
    };

    constexpr std::array<NamedKey, 31> kNamedKeys{{
        {"BACKSPACE", VK_BACK},
        {"TAB", VK_TAB},
        {"ENTER", VK_RETURN},
        {"RETURN", VK_RETURN},
        {"SHIFT", VK_SHIFT},
        {"CTRL", VK_CONTROL},
        {"CONTROL", VK_CONTROL},
        {"ALT", VK_MENU},
        {"ESC", VK_ESCAPE},
        {"ESCAPE", VK_ESCAPE},
        {"SPACE", VK_SPACE},
        {"PGUP", VK_PRIOR},
        {"PAGEUP", VK_PRIOR},
        {"PGDN", VK_NEXT},
        {"PAGEDOWN", VK_NEXT},
        {"END", VK_END},
        {"HOME", VK_HOME},
        {"LEFT", VK_LEFT},
        {"UP", VK_UP},
        {"RIGHT", VK_RIGHT},
        {"DOWN", VK_DOWN},
        {"INS", VK_INSERT},
        {"INSERT", VK_INSERT},
        {"DEL", VK_DELETE},
        {"DELETE", VK_DELETE},
        {"PRINTSCREEN", VK_SNAPSHOT},
        {"PRTSC", VK_SNAPSHOT},
        {"PAUSE", VK_PAUSE},
        {"CAPSLOCK", VK_CAPITAL},
        {"NUMLOCK", VK_NUMLOCK},
        {"SCROLLLOCK", VK_SCROLL},
    }};

    for (const auto& key : kNamedKeys) {
        if (value == key.name) {
            return key.vk;
        }
    }

    if (value.rfind("NUMPAD", 0) == 0) {
        if (const auto number = ParseInteger(value.substr(6))) {
            if (*number >= 0 && *number <= 9) {
                return VK_NUMPAD0 + *number;
            }
        }
    }

    if (value == "ADD") {
        return VK_ADD;
    }
    if (value == "SUBTRACT") {
        return VK_SUBTRACT;
    }
    if (value == "MULTIPLY") {
        return VK_MULTIPLY;
    }
    if (value == "DIVIDE") {
        return VK_DIVIDE;
    }
    if (value == "DECIMAL") {
        return VK_DECIMAL;
    }

    return std::nullopt;
}

std::string VirtualKeyToString(int vk) {
    if (vk <= 0) {
        return "NONE";
    }

    if (vk >= 'A' && vk <= 'Z') {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= '0' && vk <= '9') {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return FormatString("F%d", (vk - VK_F1) + 1);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return FormatString("NUMPAD%d", (vk - VK_NUMPAD0));
    }

    switch (vk) {
    case VK_BACK:
        return "BACKSPACE";
    case VK_TAB:
        return "TAB";
    case VK_RETURN:
        return "ENTER";
    case VK_SHIFT:
        return "SHIFT";
    case VK_CONTROL:
        return "CTRL";
    case VK_MENU:
        return "ALT";
    case VK_ESCAPE:
        return "ESC";
    case VK_SPACE:
        return "SPACE";
    case VK_PRIOR:
        return "PGUP";
    case VK_NEXT:
        return "PGDN";
    case VK_END:
        return "END";
    case VK_HOME:
        return "HOME";
    case VK_LEFT:
        return "LEFT";
    case VK_UP:
        return "UP";
    case VK_RIGHT:
        return "RIGHT";
    case VK_DOWN:
        return "DOWN";
    case VK_INSERT:
        return "INS";
    case VK_DELETE:
        return "DEL";
    case VK_SNAPSHOT:
        return "PRINTSCREEN";
    case VK_PAUSE:
        return "PAUSE";
    case VK_CAPITAL:
        return "CAPSLOCK";
    case VK_NUMLOCK:
        return "NUMLOCK";
    case VK_SCROLL:
        return "SCROLLLOCK";
    case VK_ADD:
        return "ADD";
    case VK_SUBTRACT:
        return "SUBTRACT";
    case VK_MULTIPLY:
        return "MULTIPLY";
    case VK_DIVIDE:
        return "DIVIDE";
    case VK_DECIMAL:
        return "DECIMAL";
    default:
        return FormatString("0x%02X", vk & 0xFF);
    }
}

Config LoadConfig() {
    Config config;
    config.path = BuildConfigPath();
    config.saveDirectory = GetDefaultSaveDirectory();

    char rawKey[64] = {};
    GetPrivateProfileStringA(
        kConfigSection,
        kConfigKeyKey,
        "",
        rawKey,
        static_cast<DWORD>(sizeof(rawKey)),
        config.path.c_str());
    if (const auto parsedKey = ParseVirtualKey(rawKey); parsedKey.has_value()) {
        config.screenshotKey = *parsedKey;
    }

    char rawDirectory[MAX_PATH * 4] = {};
    GetPrivateProfileStringA(
        kConfigSection,
        kConfigPathKey,
        "",
        rawDirectory,
        static_cast<DWORD>(sizeof(rawDirectory)),
        config.path.c_str());

    const std::string normalizedDirectory = ExpandEnvironmentStringsIfNeeded(Trim(rawDirectory));
    if (!normalizedDirectory.empty()) {
        config.saveDirectory = normalizedDirectory;
    }

    WritePrivateProfileStringA(
        kConfigSection,
        kConfigKeyKey,
        VirtualKeyToString(config.screenshotKey).c_str(),
        config.path.c_str());
    WritePrivateProfileStringA(
        kConfigSection,
        kConfigPathKey,
        config.saveDirectory.c_str(),
        config.path.c_str());

    return config;
}

bool EnsureDirectoryExists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (DirectoryExists(path)) {
        return true;
    }

    const int result = SHCreateDirectoryExA(nullptr, path.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || DirectoryExists(path);
}

std::string MakeCapturePath() {
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    char stem[128] = {};
    _snprintf_s(
        stem,
        sizeof(stem),
        _TRUNCATE,
        "%s_%04u%02u%02u_%02u%02u%02u",
        kDefaultFilePrefix,
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond);

    std::string path = JoinPath(g_state.config.saveDirectory, std::string(stem) + ".png");
    if (!FileExists(path)) {
        return path;
    }

    for (int suffix = 1; suffix < 100; ++suffix) {
        path = JoinPath(g_state.config.saveDirectory, FormatString("%s_%02d.png", stem, suffix));
        if (!FileExists(path)) {
            return path;
        }
    }

    return JoinPath(g_state.config.saveDirectory, std::string(stem) + "_99.png");
}

std::string HrToHex(HRESULT hr) {
    return FormatString("0x%08X", static_cast<unsigned int>(hr));
}

HMODULE LoadD3dxLibrary() {
    constexpr std::array<const char*, 4> kLibraryNames{{
        "d3dx9_43.dll",
        "d3dx9_42.dll",
        "d3dx9_41.dll",
        "d3dx9_40.dll",
    }};

    for (const char* libraryName : kLibraryNames) {
        HMODULE module = LoadLibraryA(libraryName);
        if (module != nullptr) {
            g_state.saveSurfaceToFile =
                reinterpret_cast<D3DXSaveSurfaceToFileA_t>(GetProcAddress(module, "D3DXSaveSurfaceToFileA"));
            if (g_state.saveSurfaceToFile != nullptr) {
                DebugLog("Loaded %s.", libraryName);
                return module;
            }

            FreeLibrary(module);
        }
    }

    DebugLog("Failed to load d3dx9_xx.dll.");
    return nullptr;
}

const SampVersionInfo* DetectSampVersion(HMODULE sampModule) {
    if (sampModule == nullptr) {
        return nullptr;
    }

    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(sampModule);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }

    IMAGE_NT_HEADERS32 ntHeaders = {};
    if (!SafeRead(reinterpret_cast<const std::uint8_t*>(sampModule) + dosHeader->e_lfanew, ntHeaders)) {
        return nullptr;
    }
    if (ntHeaders.Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }

    const DWORD entryPoint = ntHeaders.OptionalHeader.AddressOfEntryPoint;
    for (const auto& info : kSupportedVersions) {
        if (info.entryPointRva == entryPoint) {
            return &info;
        }
    }

    return nullptr;
}

bool EnsureSampReady() {
    HMODULE sampModule = GetModuleHandleA("samp.dll");
    if (sampModule == nullptr) {
        g_state.sampModule = nullptr;
        g_state.version = nullptr;
        return false;
    }

    if (g_state.sampModule == sampModule && g_state.version != nullptr) {
        return true;
    }

    g_state.sampModule = sampModule;
    g_state.version = DetectSampVersion(sampModule);
    if (g_state.version != nullptr) {
        DebugLog("Detected SA:MP version %s.", g_state.version->name);
    } else {
        DebugLog("Unsupported SA:MP version detected.");
    }

    return g_state.version != nullptr;
}

ChatInfo* GetChatObject() {
    if (!EnsureSampReady() || g_state.version == nullptr) {
        return nullptr;
    }

    ChatInfo* chat = nullptr;
    if (!SafeRead(reinterpret_cast<const std::uint8_t*>(g_state.sampModule) + g_state.version->pChatOffset, chat)) {
        return nullptr;
    }
    return chat;
}

bool ReadChatOrigin(int& x, int& y) {
    if (!EnsureSampReady() || g_state.version == nullptr) {
        x = kDefaultChatX;
        y = kDefaultChatY;
        return false;
    }

    int readX = 0;
    int readY = 0;
    if (SafeRead(reinterpret_cast<const std::uint8_t*>(g_state.sampModule) + g_state.version->chatPositionXOffset, readX)
        && SafeRead(reinterpret_cast<const std::uint8_t*>(g_state.sampModule) + g_state.version->chatPositionYOffset, readY)
        && readX >= 0
        && readY >= 0) {
        x = readX;
        y = readY;
        return true;
    }

    x = kDefaultChatX;
    y = kDefaultChatY;
    return false;
}

std::string ReadFixedString(const char* buffer, std::size_t size) {
    return std::string(buffer, strnlen_s(buffer, size));
}

bool IsHexColorCode(const char* text) {
    if (text[0] != '{' || text[7] != '}') {
        return false;
    }

    for (int i = 1; i <= 6; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
    }
    return true;
}

std::string StripColorCodes(const std::string& text) {
    std::string result;
    result.reserve(text.size());

    for (std::size_t index = 0; index < text.size();) {
        if (index + 7 < text.size() && IsHexColorCode(text.c_str() + index)) {
            index += 8;
            continue;
        }

        result.push_back(text[index]);
        ++index;
    }

    return result;
}

bool IsLineEmpty(const std::string& prefix, const std::string& text) {
    if (!prefix.empty()) {
        return false;
    }
    return text.empty() || text == " ";
}

int MeasureTextWidth(ID3DXFont* font, const std::string& text, int fallbackCharacterWidth) {
    if (text.empty()) {
        return 0;
    }

    if (font == nullptr) {
        return static_cast<int>(text.size()) * fallbackCharacterWidth;
    }

    using DrawTextA_t =
        int(__stdcall*)(void*, ID3DXSprite*, const char*, int, RECT*, DWORD, D3DCOLOR);

    DrawTextA_t drawText = BindMethod<DrawTextA_t>(font, 14);
    if (drawText == nullptr) {
        return static_cast<int>(text.size()) * fallbackCharacterWidth;
    }

    RECT rect = {};
    drawText(font, nullptr, text.c_str(), -1, &rect, kChatLineTextFormat, 0xFFFFFFFFu);
    return std::max<int>(0, rect.right - rect.left);
}

void GetVisibleChatRange(const ChatInfo* chat, int& fromIndex, int& toIndex) {
    int currentPosition = 0;
    int pageSize = chat != nullptr ? chat->pageSize : 0;

    if (chat != nullptr && chat->scrollBar != nullptr) {
        const auto* scrollBar = reinterpret_cast<const ScrollBarInfo*>(chat->scrollBar);
        int currentPositionRead = 0;
        int pageSizeRead = 0;
        if (SafeRead(&scrollBar->currentPosition, currentPositionRead)) {
            currentPosition = currentPositionRead;
        }
        if (SafeRead(&scrollBar->pageSize, pageSizeRead) && pageSizeRead > 0) {
            pageSize = pageSizeRead;
        }
    }

    if (pageSize <= 0) {
        pageSize = static_cast<int>(kChatEntryCount);
    }

    currentPosition = std::clamp(currentPosition, 0, static_cast<int>(kChatEntryCount));
    fromIndex = currentPosition;
    toIndex = std::clamp(currentPosition + pageSize, 0, static_cast<int>(kChatEntryCount));
}

bool BuildChatRect(const ChatInfo* chat, RECT& rect, std::string& error) {
    if (chat == nullptr) {
        error = "chat not found";
        return false;
    }

    int fromIndex = 0;
    int toIndex = 0;
    GetVisibleChatRange(chat, fromIndex, toIndex);
    if (toIndex <= fromIndex) {
        error = "visible range is empty";
        return false;
    }

    int firstNonEmpty = -1;
    int lastNonEmpty = -1;

    for (int index = fromIndex; index < toIndex; ++index) {
        const ChatEntry& entry = chat->entries[index];
        const std::string prefix = StripColorCodes(ReadFixedString(entry.prefix, sizeof(entry.prefix)));
        const std::string text = StripColorCodes(ReadFixedString(entry.text, sizeof(entry.text)));

        if (!IsLineEmpty(prefix, text)) {
            if (firstNonEmpty < 0) {
                firstNonEmpty = index;
            }
            lastNonEmpty = index;
        }
    }

    if (firstNonEmpty < 0) {
        error = "visible area not found";
        return false;
    }

    int stringHeight = chat->fontSizeY;
    if (stringHeight <= 0) {
        stringHeight = 12;
    }
    const int lineStep = stringHeight + 1;

    int originX = kDefaultChatX;
    int originY = kDefaultChatY;
    ReadChatOrigin(originX, originY);

    ID3DXFont* chatFont = chat->fontRenderer != nullptr ? chat->fontRenderer->chatFont : nullptr;
    const int fallbackCharacterWidth = std::max(6, (stringHeight / 2) + 1);

    int maxWidth = 0;
    for (int index = firstNonEmpty; index <= lastNonEmpty; ++index) {
        const ChatEntry& entry = chat->entries[index];
        const std::string prefix = StripColorCodes(ReadFixedString(entry.prefix, sizeof(entry.prefix)));
        const std::string text = StripColorCodes(ReadFixedString(entry.text, sizeof(entry.text)));

        int lineWidth = 0;
        if (!prefix.empty()) {
            lineWidth += MeasureTextWidth(chatFont, prefix, fallbackCharacterWidth) + 5;
        }
        if (!text.empty() && text != " ") {
            lineWidth += MeasureTextWidth(chatFont, text, fallbackCharacterWidth);
        }

        maxWidth = std::max(maxWidth, lineWidth);
    }

    if (chat->timestampsEnabled != 0) {
        maxWidth += std::max(0, chat->timestampWidth) + 5;
    }

    const int horizontalPadding = std::clamp((stringHeight * 35) / 100, 2, 8);
    const int verticalPadding = std::clamp((stringHeight * 20) / 100, 1, 4);
    constexpr int outerPadding = 5;

    const int rawTop = originY + (firstNonEmpty - fromIndex) * lineStep;
    const int rawBottom = rawTop + (lastNonEmpty - firstNonEmpty + 1) * lineStep;

    rect.left = std::max<LONG>(0, static_cast<LONG>(originX - 1 - outerPadding));
    rect.top = std::max<LONG>(0, static_cast<LONG>(rawTop - verticalPadding - outerPadding));
    rect.right = std::max<LONG>(rect.left + 1, static_cast<LONG>(originX + maxWidth + horizontalPadding + outerPadding));
    rect.bottom = std::max<LONG>(rect.top + 1, static_cast<LONG>(rawBottom + verticalPadding + outerPadding));
    return true;
}

bool ClampRectToSurface(IDirect3DSurface9* surface, RECT& rect, std::string& error) {
    D3DSURFACE_DESC desc = {};
    const HRESULT hr = surface->GetDesc(&desc);
    if (FAILED(hr)) {
        error = "GetDesc failed: " + HrToHex(hr);
        return false;
    }

    rect.left = std::clamp<LONG>(rect.left, 0, static_cast<LONG>(desc.Width));
    rect.top = std::clamp<LONG>(rect.top, 0, static_cast<LONG>(desc.Height));
    rect.right = std::clamp<LONG>(rect.right, 0, static_cast<LONG>(desc.Width));
    rect.bottom = std::clamp<LONG>(rect.bottom, 0, static_cast<LONG>(desc.Height));

    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        error = "capture rect is outside of the source surface";
        return false;
    }

    return true;
}

bool SaveRectFromSurface(IDirect3DSurface9* surface, RECT rect, const std::string& path, std::string& error) {
    if (surface == nullptr) {
        error = "surface is null";
        return false;
    }
    if (g_state.saveSurfaceToFile == nullptr) {
        error = "D3DXSaveSurfaceToFileA is unavailable";
        return false;
    }
    if (!ClampRectToSurface(surface, rect, error)) {
        return false;
    }

    const HRESULT hr = g_state.saveSurfaceToFile(path.c_str(), kD3DXIFF_PNG, surface, nullptr, &rect);
    if (FAILED(hr)) {
        error = "D3DXSaveSurfaceToFileA failed: " + HrToHex(hr);
        return false;
    }

    return true;
}

IDirect3DSurface9* CopyTextureLevel0ToSystemSurface(IDirect3DTexture9* texture, const ChatInfo* chat, std::string& error) {
    if (texture == nullptr) {
        error = "texture is null";
        return nullptr;
    }

    D3DSURFACE_DESC desc = {};
    HRESULT hr = texture->GetLevelDesc(0, &desc);
    if (FAILED(hr)) {
        error = "GetLevelDesc failed: " + HrToHex(hr);
        return nullptr;
    }

    IDirect3DSurface9* sourceSurface = nullptr;
    hr = texture->GetSurfaceLevel(0, &sourceSurface);
    if (FAILED(hr) || sourceSurface == nullptr) {
        error = "GetSurfaceLevel failed: " + HrToHex(hr);
        return nullptr;
    }

    IDirect3DDevice9* device = nullptr;
    hr = texture->GetDevice(&device);
    if ((FAILED(hr) || device == nullptr) && chat != nullptr && chat->d3dDevice != nullptr) {
        device = chat->d3dDevice;
        device->AddRef();
        hr = S_OK;
    }

    if (FAILED(hr) || device == nullptr) {
        SafeRelease(sourceSurface);
        error = "failed to get D3D device: " + HrToHex(hr);
        return nullptr;
    }

    IDirect3DSurface9* systemSurface = nullptr;
    hr = device->CreateOffscreenPlainSurface(
        desc.Width,
        desc.Height,
        desc.Format,
        D3DPOOL_SYSTEMMEM,
        &systemSurface,
        nullptr);
    if (FAILED(hr) || systemSurface == nullptr) {
        SafeRelease(device);
        SafeRelease(sourceSurface);
        error = "CreateOffscreenPlainSurface failed: " + HrToHex(hr);
        return nullptr;
    }

    hr = device->GetRenderTargetData(sourceSurface, systemSurface);
    SafeRelease(device);
    SafeRelease(sourceSurface);

    if (FAILED(hr)) {
        SafeRelease(systemSurface);
        error = "GetRenderTargetData failed: " + HrToHex(hr);
        return nullptr;
    }

    return systemSurface;
}

bool CaptureVisibleChat(std::string& savedPath, std::string& error) {
    if (g_state.saveSurfaceToFile == nullptr) {
        error = "d3dx9_xx.dll not found";
        return false;
    }

    ChatInfo* chat = GetChatObject();
    if (chat == nullptr) {
        error = "chat not found";
        return false;
    }

    RECT captureRect = {};
    if (!BuildChatRect(chat, captureRect, error)) {
        return false;
    }

    if (!EnsureDirectoryExists(g_state.config.saveDirectory)) {
        error = "failed to create directory: " + g_state.config.saveDirectory;
        return false;
    }

    savedPath = MakeCapturePath();

    if (chat->surface != nullptr) {
        std::string surfaceError;
        if (SaveRectFromSurface(chat->surface, captureRect, savedPath, surfaceError)) {
            return true;
        }
        DebugLog("Direct surface save failed, trying texture fallback: %s", surfaceError.c_str());
    }

    if (chat->texture == nullptr) {
        error = "chat surface and texture are null";
        return false;
    }

    IDirect3DSurface9* systemSurface = CopyTextureLevel0ToSystemSurface(chat->texture, chat, error);
    if (systemSurface == nullptr) {
        return false;
    }

    const bool result = SaveRectFromSurface(systemSurface, captureRect, savedPath, error);
    SafeRelease(systemSurface);
    return result;
}

void OnGameLoop() {
    if (g_state.config.screenshotKey <= 0) {
        return;
    }

    const bool isPressed = (GetAsyncKeyState(g_state.config.screenshotKey) & 0x8000) != 0;
    if (isPressed && !g_state.hotkeyPressed) {
        std::string savedPath;
        std::string error;
        if (CaptureVisibleChat(savedPath, error)) {
            DebugLog("Saved chat screenshot to %s", savedPath.c_str());
        } else {
            DebugLog("Capture failed: %s", error.c_str());
        }
    }
    g_state.hotkeyPressed = isPressed;
}

void __cdecl HookedGameLoop() {
    if (g_originalGameLoop != nullptr) {
        g_originalGameLoop();
    }

    OnGameLoop();
}

void WaitUntilGameLoaded() {
    const auto* loadState = reinterpret_cast<const volatile DWORD*>(kGtaLoadStateAddress);
    while (*loadState < 9) {
        Sleep(10);
    }
}

bool InstallGameLoopHook() {
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        DebugLog("MH_Initialize failed: %d", static_cast<int>(initStatus));
        return false;
    }

    const MH_STATUS createStatus = MH_CreateHook(
        reinterpret_cast<void*>(kGameLoopAddress),
        &HookedGameLoop,
        reinterpret_cast<void**>(&g_originalGameLoop));
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
        DebugLog("MH_CreateHook failed: %d", static_cast<int>(createStatus));
        return false;
    }

    const MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(kGameLoopAddress));
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
        DebugLog("MH_EnableHook failed: %d", static_cast<int>(enableStatus));
        return false;
    }

    g_state.hookInstalled = true;
    DebugLog("Game loop hook installed.");
    return true;
}

DWORD WINAPI InitializePluginThread(void*) {
    g_state.config = LoadConfig();
    g_state.d3dxModule = LoadD3dxLibrary();
    WaitUntilGameLoaded();
    InstallGameLoopHook();
    return 0;
}

void ShutdownPlugin() {
    if (g_state.hookInstalled) {
        MH_DisableHook(reinterpret_cast<void*>(kGameLoopAddress));
        MH_RemoveHook(reinterpret_cast<void*>(kGameLoopAddress));
        g_state.hookInstalled = false;
    }
    MH_Uninitialize();

    if (g_state.d3dxModule != nullptr) {
        FreeLibrary(g_state.d3dxModule);
        g_state.d3dxModule = nullptr;
        g_state.saveSurfaceToFile = nullptr;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);

        HANDLE thread = CreateThread(nullptr, 0, &InitializePluginThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        ShutdownPlugin();
    }

    return TRUE;
}
