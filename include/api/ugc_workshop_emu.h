#pragma once
// ============================================================
// UCOnline2 -- Workshop/UGC emulation for ogAppId
//
// When ogAppId is set, the real Steam client runs under the spoofed
// AppId (480/Spacewar) and therefore has zero Workshop items for the
// real game. This module hooks the four ISteamUGC methods the game
// calls to discover installed Workshop content, and answers them by
// scanning the filesystem:
//
//   <steam library>/steamapps/workshop/content/<ogAppId>/
//
// The appworkshop_<ogAppId>.acf manifest is parsed for per-item
// metadata (size, timestamp). Items not in the ACF but present on
// disk are still reported with best-effort values.
//
// The layout of a Steam library is the same on both platforms, but the
// places a library can live are not: on Windows the game directory
// (...\steamapps\common\<Game>) is the only place that is looked at,
// while on Linux the game may sit in ~/.steam/steam/steamapps/common,
// the Workshop content may live in ~/.local/share/Steam/steamapps, and
// $SteamPath points at yet another root. So the library root is chosen
// from a list of candidates -- the one that actually holds
// workshop/content/<ogAppId> wins.
//
// ISteamUGC vtable indices (STEAMUGC_INTERFACE_VERSION021):
//   74: GetNumSubscribedItems
//   75: GetSubscribedItems
//   76: GetItemState
//   77: GetItemInstallInfo
// ============================================================

#include "../platform.h"
#include "../hook_shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// The install folder handed back to the game. Windows caps a path at
// MAX_PATH; on Linux the limit is PATH_MAX (4096), but a Workshop
// folder never gets close to that -- and the struct is copied around,
// so a smaller buffer keeps the memory footprint down.
#if PLAT_IS_WINDOWS
#define UCO_WORKSHOP_FOLDER_MAX MAX_PATH
#else
#define UCO_WORKSHOP_FOLDER_MAX 1024
#endif

struct UcoWorkshopItem
{
    uint64        id;
    uint64        sizeOnDisk;
    uint32        timeUpdated;
    char          folder[UCO_WORKSHOP_FOLDER_MAX];
};

// Copy a path into a fixed buffer, truncating when it does not fit.
static inline void UcoCopyBounded(char* dst, size_t dstSize, const std::string& src)
{
    if (!dst || dstSize == 0) return;
    const size_t copy = (src.size() < dstSize - 1) ? src.size() : dstSize - 1;
    memcpy(dst, src.c_str(), copy);
    dst[copy] = '\0';
}

class UcoWorkshopEmu
{
public:
    static bool          s_bScanned;
    static bool          s_bEnabled;
    static std::vector<UcoWorkshopItem> s_Items;
    static char          s_ContentPath[MAX_PATH];

    static void Init(uint32 ogAppId)
    {
        if (ogAppId == 0) return;

        const std::vector<std::string> roots = CandidateSteamAppsRoots();

        // The library that actually holds this game's Workshop content.
        std::string chosenRoot;
        std::string chosenContent;
        for (size_t i = 0; i < roots.size(); ++i)
        {
            const std::string content = roots[i] + PLAT_SEP_STR "workshop" PLAT_SEP_STR
                                      + "content" PLAT_SEP_STR + std::to_string(ogAppId);

            const DWORD attr = GetFileAttributesA(content.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            {
                chosenRoot    = roots[i];
                chosenContent = content;
                break;
            }
        }

        if (chosenRoot.empty())
        {
            UCOLOG("[UCOnline2] WorkshopEmu: no workshop content directory for AppId %u%s%s",
                ogAppId,
                roots.empty() ? "" : " under ",
                roots.empty() ? "" : roots[0].c_str());
            return;
        }

        UcoCopyBounded(s_ContentPath, sizeof(s_ContentPath), chosenContent);

        s_bEnabled = true;
        UCOLOG("[UCOnline2] WorkshopEmu: enabled for AppId %u, content dir: %s",
            ogAppId, s_ContentPath);

        // Parse the ACF manifest for metadata
        const std::string acfPath = chosenRoot + PLAT_SEP_STR "workshop" PLAT_SEP_STR
                                  + "appworkshop_" + std::to_string(ogAppId) + ".acf";
        ParseAcf(acfPath.c_str());

        // Scan the content directory for workshop items
        ScanContentDir();
    }

    static bool Enabled() { return s_bEnabled; }

    static uint32 GetNumSubscribedItems()
    {
        if (!s_bScanned) ScanContentDir();
        return (uint32)s_Items.size();
    }

    static uint32 GetSubscribedItems(uint64* pvecPublishedFileID, uint32 cMaxEntries)
    {
        if (!s_bScanned) ScanContentDir();
        uint32 count = (uint32)s_Items.size();
        if (count > cMaxEntries) count = cMaxEntries;
        for (uint32 i = 0; i < count; ++i)
            pvecPublishedFileID[i] = s_Items[i].id;
        return count;
    }

    static uint32 GetItemState(uint64 nPublishedFileID)
    {
        if (!s_bScanned) ScanContentDir();
        for (const auto& item : s_Items)
        {
            if (item.id == nPublishedFileID)
                return 5; // k_EItemStateSubscribed (1) | k_EItemStateInstalled (4)
        }
        return 0; // k_EItemStateNone
    }

    static bool GetItemInstallInfo(uint64 nPublishedFileID, uint64* punSizeOnDisk,
                                   char* pchFolder, uint32 cchFolderSize, uint32* punTimeStamp)
    {
        if (!s_bScanned) ScanContentDir();
        for (const auto& item : s_Items)
        {
            if (item.id == nPublishedFileID)
            {
                if (punSizeOnDisk) *punSizeOnDisk = item.sizeOnDisk;
                if (pchFolder) strncpy_s(pchFolder, cchFolderSize, item.folder, _TRUNCATE);
                if (punTimeStamp) *punTimeStamp = item.timeUpdated;
                return true;
            }
        }
        return false;
    }

private:
    // Lightweight ACF parser -- only extracts WorkshopItemsInstalled entries
    struct AcfItemInfo
    {
        uint64 id;
        uint64 size;
        uint32 timeupdated;
    };
    static std::vector<AcfItemInfo> s_AcfItems;

    // The Steam libraries that could hold this game's Workshop content, most
    // likely first:
    //
    //   1) the library the game itself is installed in
    //      (<library>/steamapps/common/<Game>)
    //   2) $SteamPath -- the Steam client sets it for the running game
    //   3) the Steam root the steamclient was loaded from
    //   4) the usual Linux Steam locations
    //
    // Only the root that actually contains workshop/content/<ogAppId> is
    // used; the rest are just candidates.
    static std::vector<std::string> CandidateSteamAppsRoots()
    {
        std::vector<std::string> roots;

        // 1) Derive the library from the game's own directory.
        char exePath[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0)
        {
            char gameDir[MAX_PATH] = { 0 };
            _snprintf_s(gameDir, MAX_PATH, _TRUNCATE, "%s", exePath);
            PathRemoveFileSpecA(gameDir);

            // Case-insensitive search for "common" between two separators:
            // <library>/steamapps/common/<Game> -> <library>/steamapps.
            // The LAST match wins, in case the path has "common" earlier.
            char* pCommon = nullptr;
            for (char* p = gameDir; *p; ++p)
            {
                if (PLAT_IS_DIR_SEP(*p) &&
                    _strnicmp(p + 1, "common", 6) == 0 &&
                    PLAT_IS_DIR_SEP(p[7]))
                {
                    pCommon = p;
                }
            }

            if (pCommon)
            {
                *pCommon = '\0';
                roots.push_back(std::string(gameDir));
            }
        }

        // 2) $SteamPath.
        const char* steamPath = getenv("SteamPath");
        if (steamPath && steamPath[0])
            roots.push_back(std::string(steamPath) + PLAT_SEP_STR "steamapps");

        // 3) The Steam installation steamclient was loaded from. On Windows
        //    that IS the Steam directory (the registry hands out the path
        //    of steamclient64.dll with the file name already stripped);
        //    on Linux it is <steam>/linux64 or <steam>/sdk64, so the Steam
        //    root is the parent directory.
        const char* installDir = SteamAPI_GetSteamInstallPath();
        if (installDir && installDir[0] && _stricmp(installDir, "UCOnline2_InvalidPath") != 0)
        {
        #if PLAT_IS_WINDOWS
            roots.push_back(std::string(installDir) + PLAT_SEP_STR "steamapps");
        #else
            char steamRoot[MAX_PATH] = { 0 };
            _snprintf_s(steamRoot, MAX_PATH, _TRUNCATE, "%s", installDir);
            if (PathRemoveFileSpecA(steamRoot))
                roots.push_back(std::string(steamRoot) + PLAT_SEP_STR "steamapps");
        #endif
        }

        // 4) The usual Linux Steam locations.
        #if !PLAT_IS_WINDOWS
        const std::string home = PlatHomeDir();
        if (!home.empty())
        {
            roots.push_back(home + "/.steam/steam/steamapps");
            roots.push_back(home + "/.local/share/Steam/steamapps");
        }
        #endif

        return roots;
    }

    static void ParseAcf(const char* path)
    {
        FILE* f = nullptr;
        fopen_s(&f, path, "r");
        if (!f) return;

        char line[1024];
        bool inInstalled = false;
        bool inItem = false;
        uint64 currentId = 0;
        uint64 currentSize = 0;
        uint32 currentTime = 0;
        int braceDepth = 0;

        while (fgets(line, sizeof(line), f))
        {
            // Trim leading whitespace
            char* p = line;
            while (*p == ' ' || *p == '\t') ++p;

            if (!inInstalled)
            {
                if (strstr(p, "\"WorkshopItemsInstalled\""))
                    inInstalled = true;
                continue;
            }

            if (*p == '{')
            {
                ++braceDepth;
                continue;
            }
            if (*p == '}')
            {
                --braceDepth;
                if (braceDepth <= 0)
                {
                    // End of WorkshopItemsInstalled block
                    if (inItem && currentId != 0)
                    {
                        AcfItemInfo info = { currentId, currentSize, currentTime };
                        s_AcfItems.push_back(info);
                    }
                    break;
                }
                if (inItem && currentId != 0)
                {
                    AcfItemInfo info = { currentId, currentSize, currentTime };
                    s_AcfItems.push_back(info);
                    inItem = false;
                    currentId = 0;
                    currentSize = 0;
                    currentTime = 0;
                }
                continue;
            }

            // Parse key-value: "key"  "value"
            if (*p == '"')
            {
                char key[256] = { 0 };
                char val[256] = { 0 };
                if (sscanf_s(p, "\"%255[^\"]\"", key, (unsigned)sizeof(key)) == 1)
                {
                    // The value is the next quoted string: the closing quote
                    // of the key, then the opening quote of the value.
                    // (A key with no value on the same line -- the item IDs
                    // inside WorkshopItemsInstalled -- simply has none.)
                    char* vp = strchr(p + 1, '"');
                    if (vp) vp = strchr(vp + 1, '"');
                    if (vp)
                    {
                        sscanf_s(vp, "\"%255[^\"]\"", val, (unsigned)sizeof(val));
                    }

                    if (braceDepth == 1)
                    {
                        // Top-level key inside WorkshopItemsInstalled = item ID
                        if (inItem && currentId != 0)
                        {
                            AcfItemInfo info = { currentId, currentSize, currentTime };
                            s_AcfItems.push_back(info);
                        }
                        inItem = true;
                        currentId = _strtoui64(key, nullptr, 10);
                        currentSize = 0;
                        currentTime = 0;
                    }
                    else if (braceDepth == 2 && inItem)
                    {
                        if (_stricmp(key, "size") == 0)
                            currentSize = _strtoui64(val, nullptr, 10);
                        else if (_stricmp(key, "timeupdated") == 0)
                            currentTime = (uint32)strtoul(val, nullptr, 10);
                    }
                }
            }
        }

        fclose(f);
        UCOLOG("[UCOnline2] WorkshopEmu: parsed %d items from ACF manifest", (int)s_AcfItems.size());
    }

    static void ScanContentDir()
    {
        s_bScanned = true;
        s_Items.clear();

        if (!s_bEnabled) return;

        char searchPath[MAX_PATH] = { 0 };
        {
            const std::string pattern = std::string(s_ContentPath) + PLAT_SEP_STR "*";
            UcoCopyBounded(searchPath, sizeof(searchPath), pattern);
        }

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(searchPath, &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;

        do
        {
            // The attributes are read from the path itself rather than from
            // the find data: the POSIX shim does not carry them over for the
            // entries FindNextFileA returns.
            const std::string fullPath = std::string(s_ContentPath) + PLAT_SEP_STR + fd.cFileName;

            WIN32_FILE_ATTRIBUTE_DATA attrData;
            if (!GetFileAttributesExA(fullPath.c_str(), GetFileExInfoStandard, &attrData))
                continue;
            if (!(attrData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == '.') continue;

            // Directory name should be a numeric workshop ID
            char* end = nullptr;
            uint64 id = _strtoui64(fd.cFileName, &end, 10);
            if (!end || *end != '\0' || id == 0) continue;

            UcoWorkshopItem item = {};
            item.id = id;
            UcoCopyBounded(item.folder, sizeof(item.folder), fullPath);

            // Look up metadata from ACF
            for (const auto& acf : s_AcfItems)
            {
                if (acf.id == id)
                {
                    item.sizeOnDisk = acf.size;
                    item.timeUpdated = acf.timeupdated;
                    break;
                }
            }

            // Fallback: use the directory's own mtime if no ACF data
            if (item.timeUpdated == 0)
            {
                ULARGE_INTEGER uli;
                uli.LowPart  = attrData.ftLastWriteTime.dwLowDateTime;
                uli.HighPart = attrData.ftLastWriteTime.dwHighDateTime;
                item.timeUpdated = (uint32)((uli.QuadPart - 116444736000000000ULL) / 10000000ULL);
            }

            s_Items.push_back(item);
        }
        while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
        UCOLOG("[UCOnline2] WorkshopEmu: found %d workshop items on disk", (int)s_Items.size());
    }
};

// Static member definitions
bool                          UcoWorkshopEmu::s_bScanned = false;
bool                          UcoWorkshopEmu::s_bEnabled = false;
std::vector<UcoWorkshopItem>  UcoWorkshopEmu::s_Items;
char                          UcoWorkshopEmu::s_ContentPath[MAX_PATH] = { 0 };
std::vector<UcoWorkshopEmu::AcfItemInfo> UcoWorkshopEmu::s_AcfItems;

// ============================================================
// ISteamUGC vtable hook functions
// ============================================================

typedef uint32 (S_CALLTYPE *Fn_GetNumSubscribedItems)(void* pThis, bool bIncludeLocallyDisabled);
typedef uint32 (S_CALLTYPE *Fn_GetSubscribedItems)(void* pThis, uint64* pvecPublishedFileID, uint32 cMaxEntries, bool bIncludeLocallyDisabled);
typedef uint32 (S_CALLTYPE *Fn_GetItemState)(void* pThis, uint64 nPublishedFileID);
typedef bool   (S_CALLTYPE *Fn_GetItemInstallInfo)(void* pThis, uint64 nPublishedFileID, uint64* punSizeOnDisk, char* pchFolder, uint32 cchFolderSize, uint32* punTimeStamp);

static Fn_GetNumSubscribedItems  g_pfnOriginalGetNumSubscribedItems  = nullptr;
static Fn_GetSubscribedItems     g_pfnOriginalGetSubscribedItems     = nullptr;
static Fn_GetItemState           g_pfnOriginalGetItemState           = nullptr;
static Fn_GetItemInstallInfo     g_pfnOriginalGetItemInstallInfo     = nullptr;
static bool                      g_bUgcLoggedFirst                   = false;

static uint32 S_CALLTYPE Hooked_GetNumSubscribedItems(void* pThis, bool bIncludeLocallyDisabled)
{
    uint32 emuCount = UcoWorkshopEmu::GetNumSubscribedItems();

    if (emuCount > 0)
    {
        if (!g_bUgcLoggedFirst)
        {
            UCOLOG("[UCOnline2] GetNumSubscribedItems hook: returning %u items from disk (ogAppId workshop)", emuCount);
            g_bUgcLoggedFirst = true;
        }
        return emuCount;
    }

    // Fall through to real Steam if we have no local items
    return g_pfnOriginalGetNumSubscribedItems(pThis, bIncludeLocallyDisabled);
}

static uint32 S_CALLTYPE Hooked_GetSubscribedItems(void* pThis, uint64* pvecPublishedFileID, uint32 cMaxEntries, bool bIncludeLocallyDisabled)
{
    uint32 emuCount = UcoWorkshopEmu::GetNumSubscribedItems();

    if (emuCount > 0)
        return UcoWorkshopEmu::GetSubscribedItems(pvecPublishedFileID, cMaxEntries);

    return g_pfnOriginalGetSubscribedItems(pThis, pvecPublishedFileID, cMaxEntries, bIncludeLocallyDisabled);
}

static uint32 S_CALLTYPE Hooked_GetItemState(void* pThis, uint64 nPublishedFileID)
{
    uint32 state = UcoWorkshopEmu::GetItemState(nPublishedFileID);
    if (state != 0)
        return state;

    return g_pfnOriginalGetItemState(pThis, nPublishedFileID);
}

static bool S_CALLTYPE Hooked_GetItemInstallInfo(void* pThis, uint64 nPublishedFileID, uint64* punSizeOnDisk, char* pchFolder, uint32 cchFolderSize, uint32* punTimeStamp)
{
    if (UcoWorkshopEmu::GetItemInstallInfo(nPublishedFileID, punSizeOnDisk, pchFolder, cchFolderSize, punTimeStamp))
        return true;

    return g_pfnOriginalGetItemInstallInfo(pThis, nPublishedFileID, punSizeOnDisk, pchFolder, cchFolderSize, punTimeStamp);
}

// Call this from InstallSteamSpoofHooks() to install the UGC hooks.
//
// Windows patches the function body (MinHook), Linux patches the vtable
// slot -- see hook_shim.h. Either way the game ends up calling the detour
// above.
static void UcoInstallWorkshopHooks(ISteamUGC* pUGC)
{
    if (!pUGC) return;

    void** ugcVT = *reinterpret_cast<void***>(pUGC);

    // ISteamUGC vtable layout (STEAMUGC_INTERFACE_VERSION021):
    // 74: GetNumSubscribedItems
    // 75: GetSubscribedItems
    // 76: GetItemState
    // 77: GetItemInstallInfo
    struct UgcHook { int index; void* detour; void** original; const char* name; };
    const UgcHook hooks[] = {
        { 74, (void*)&Hooked_GetNumSubscribedItems,  (void**)&g_pfnOriginalGetNumSubscribedItems,  "GetNumSubscribedItems" },
        { 75, (void*)&Hooked_GetSubscribedItems,     (void**)&g_pfnOriginalGetSubscribedItems,     "GetSubscribedItems" },
        { 76, (void*)&Hooked_GetItemState,           (void**)&g_pfnOriginalGetItemState,           "GetItemState" },
        { 77, (void*)&Hooked_GetItemInstallInfo,     (void**)&g_pfnOriginalGetItemInstallInfo,     "GetItemInstallInfo" },
    };

    int installed = 0;
    for (const UgcHook& h : hooks)
    {
        const PlatHookStatus st = PlatHookVTableMethod(ugcVT, h.index, h.detour, h.original);
        if (st == PLAT_HOOK_OK || st == PLAT_HOOK_ALREADY)
            ++installed;
        else
            UCOLOG("[UCOnline2] failed to hook ISteamUGC::%s (vtable[%d]): %d",
                h.name, h.index, (int)st);
    }

    UCOLOG("[UCOnline2] ISteamUGC Workshop hooks: %d/%d installed (%d items on disk)",
        installed, (int)(sizeof(hooks) / sizeof(hooks[0])),
        UcoWorkshopEmu::GetNumSubscribedItems());
}
