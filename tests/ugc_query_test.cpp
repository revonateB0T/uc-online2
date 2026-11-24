// ============================================================
// Ask the real Steam client for the Space Engineers workshop.
//
// The game sends its workshop query through ISteamUGC and then waits
// for the call result.  This test does the same thing by hand -- same
// interface, same manual dispatch pump the game uses -- so we can see
// whether Steam answers at all, and what it answers.
//
//   tests/ugc_query_test [path-to-libsteam_api.so]
//
// The queries differ only in the app id they name:
//
//   subscribed/Invalid/244850  exactly what the game sends
//   subscribed/244850/244850   the creator app named explicitly
//   all/Invalid/244850         the "all items" query
//
// ============================================================
#include <dlfcn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

#include "isteamugc.h"
#include "isteamutils.h"

// Steamworks.NET's CallbackDispatcher works with these two structs; the
// layout is what the SDK has (steam_api_internal.h).
struct Msg_t
{
    int32_t  m_hSteamUser;
    int      m_iCallback;
    uint8_t* m_pubParam;
    int      m_cubParam;
};

typedef void* (*Fn_Init)(void);
typedef void* (*Fn_GetUGC)(void);
typedef void* (*Fn_GetUser)(void);
typedef uint64_t (*Fn_GetSteamID)(intptr_t);
typedef UGCQueryHandle_t (*Fn_CreateQueryUser)(intptr_t, AccountID_t, EUserUGCList,
    EUGCMatchingUGCType, EUserUGCListSortOrder, AppId_t, AppId_t, uint32);
typedef UGCQueryHandle_t (*Fn_CreateQueryAll)(intptr_t, EUGCQuery, EUGCMatchingUGCType,
    AppId_t, AppId_t, uint32);
typedef SteamAPICall_t (*Fn_Send)(intptr_t, UGCQueryHandle_t);
typedef bool (*Fn_ManualInit)(void);
typedef void (*Fn_RunFrame)(uint32);
typedef bool (*Fn_GetNext)(uint32, Msg_t*);
typedef bool (*Fn_GetResult)(uint32, SteamAPICall_t, void*, int, int, bool*);
typedef void (*Fn_FreeLast)(uint32);
typedef uint32 (*Fn_GetPipe)(void);
typedef uint32 (*Fn_GetNumResults)(intptr_t, UGCQueryHandle_t, uint32);

static void* Sym(void* h, const char* name)
{
    void* p = dlsym(h, name);
    if (!p)
    {
        fprintf(stderr, "missing export: %s\n", name);
        exit(2);
    }
    return p;
}

// Wait for the completion of one call. Returns true when it arrived.
typedef bool (*Fn_UtilsGetResult)(intptr_t, SteamAPICall_t, void*, int, int, bool*);

typedef bool (S_CALLTYPE *Fn_BGetCallback)(uint32_t hPipe, Msg_t* pMsg);

// The real client module, loaded into this process -- the same instance the
// emulator talks to, so its callback queue is the one we are looking at.
static void* g_client = dlopen("/home/tarsin/.local/share/Steam/linux64/steamclient.so",
    RTLD_NOW | RTLD_GLOBAL);

static bool WaitForCall(uint32_t pipe, SteamAPICall_t call, Fn_RunFrame runFrame,
    Fn_GetNext getNext, Fn_GetResult getResult, Fn_FreeLast freeLast, int seconds,
    intptr_t utils, Fn_UtilsGetResult utilsGetResult)
{
    bool sawManual = false;
    bool sawDirect = false;

    for (int i = 0; i < seconds * 20; ++i)
    {
        runFrame(pipe);

        Msg_t msg;
        // Ask the client's own queue directly -- the same process, so it is
        // the same queue the emulator reads from.
        while (getNext(pipe, &msg))
        {
            if (msg.m_iCallback == SteamAPICallCompleted_t::k_iCallback)
            {
                SteamAPICallCompleted_t done;
                memcpy(&done, msg.m_pubParam, sizeof(done));
                if (done.m_hAsyncCall == call)
                {
                    void* buf = calloc(1, done.m_cubParam ? done.m_cubParam : 1);
                    bool failed = false;
                    bool ok = getResult(pipe, call, buf, done.m_cubParam,
                        done.m_iCallback, &failed);
                    printf("    completion: callback=%d size=%u ok=%d failed=%d\n",
                        done.m_iCallback, done.m_cubParam, (int)ok, (int)failed);
                    if (ok && !failed && done.m_iCallback == SteamUGCQueryCompleted_t::k_iCallback)
                    {
                        SteamUGCQueryCompleted_t res;
                        memcpy(&res, buf, sizeof(res) < (size_t)done.m_cubParam ? sizeof(res) : (size_t)done.m_cubParam);
                        printf("    result: eResult=%d returned=%u total=%u\n",
                            res.m_eResult, res.m_unNumResultsReturned, res.m_unTotalMatchingResults);
                    }
                    free(buf);
                    freeLast(pipe);
                    sawManual = true;
                    if (sawDirect)
                        return true;
                }
            }
            freeLast(pipe);
        }
        usleep(50000);
    }
    if (!sawManual)
        printf("    manual dispatch: NO completion arrived\n");
    return sawManual;
}

int main(int argc, char** argv)
{
    const char* soPath = (argc > 1) ? argv[1] : "build/x64/libsteam_api.so";

    void* h = dlopen(soPath, RTLD_NOW | RTLD_LOCAL);
    if (!h)
    {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    Fn_Init       init       = (Fn_Init)Sym(h, "SteamAPI_Init");
    Fn_GetUGC     getUGC     = (Fn_GetUGC)Sym(h, "SteamAPI_SteamUGC_v021");
    Fn_GetUser    getUser    = (Fn_GetUser)Sym(h, "SteamAPI_SteamUser_v023");
    Fn_GetSteamID getSteamID = (Fn_GetSteamID)Sym(h, "SteamAPI_ISteamUser_GetSteamID");
    Fn_CreateQueryUser createUser = (Fn_CreateQueryUser)Sym(h, "SteamAPI_ISteamUGC_CreateQueryUserUGCRequest");
    Fn_CreateQueryAll createAll   = (Fn_CreateQueryAll)Sym(h, "SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage");
    Fn_Send       send       = (Fn_Send)Sym(h, "SteamAPI_ISteamUGC_SendQueryUGCRequest");
    Fn_ManualInit manualInit = (Fn_ManualInit)Sym(h, "SteamAPI_ManualDispatch_Init");
    Fn_RunFrame   runFrame   = (Fn_RunFrame)Sym(h, "SteamAPI_ManualDispatch_RunFrame");
    Fn_GetNext    getNext    = (Fn_GetNext)Sym(h, "SteamAPI_ManualDispatch_GetNextCallback");
    Fn_GetResult  getResult  = (Fn_GetResult)Sym(h, "SteamAPI_ManualDispatch_GetAPICallResult");
    Fn_FreeLast   freeLast   = (Fn_FreeLast)Sym(h, "SteamAPI_ManualDispatch_FreeLastCallback");
    Fn_GetPipe    getPipe    = (Fn_GetPipe)Sym(h, "SteamAPI_GetHSteamPipe");
    Fn_GetPipe    getUtilsPipe = getPipe;
    Fn_UtilsGetResult utilsGetResult =
        (Fn_UtilsGetResult)Sym(h, "SteamAPI_ISteamUtils_GetAPICallResult");
    Fn_GetUGC     getUtils   = (Fn_GetUGC)Sym(h, "SteamAPI_SteamUtils_v010");

    init();
    manualInit();

    intptr_t ugc  = (intptr_t)getUGC();
    intptr_t user = (intptr_t)getUser();
    uint64_t steamId = getSteamID(user);
    uint32_t accountId = (uint32_t)(steamId & 0xFFFFFFFFu);
    uint32_t pipe = getPipe();

    printf("steamId=%llu accountId=%u pipe=%u\n",
        (unsigned long long)steamId, accountId, pipe);

    const AppId_t SE = 244850;

    struct Variant { const char* name; bool user; AppId_t creator; AppId_t consumer; };
    const Variant variants[] = {
        { "subscribed creator=Invalid consumer=244850 (what the game sends)", true,  0, SE },
        { "subscribed creator=244850  consumer=244850",                          true,  SE, SE },
        { "all       creator=Invalid consumer=244850",                         false, 0, SE },
        { "subscribed creator=244850  consumer=480  (client's own app id)",    true,  SE, 480 },
        { "all       creator=244850  consumer=480",                            false, SE, 480 },
        { "subscribed creator=480    consumer=480  (Spacewar's own)",          true,  480, 480 },
    };

    for (const auto& v : variants)
    {
        printf("\n== %s\n", v.name);

        UGCQueryHandle_t handle;
        if (v.user)
            handle = createUser(ugc, accountId, k_EUserUGCList_Subscribed,
                k_EUGCMatchingUGCType_Items, k_EUserUGCListSortOrder_TitleAsc,
                v.creator, v.consumer, 1);
        else
            handle = createAll(ugc, k_EUGCQuery_RankedByPublicationDate,
                k_EUGCMatchingUGCType_Items, v.creator, v.consumer, 1);

        if (!handle || handle == 0xFFFFFFFFFFFFFFFFull)
        {
            printf("    could not create the query\n");
            continue;
        }

        SteamAPICall_t call = send(ugc, handle);
        printf("    handle=%llu call=%llu\n",
            (unsigned long long)handle, (unsigned long long)call);

        if (!WaitForCall(pipe, call, runFrame, getNext, getResult, freeLast,
                getenv("UCO_QUICK") ? 3 : 10,
                (intptr_t)getUtils(), utilsGetResult))
            printf("    TIMEOUT: no completion\n");

        if (getenv("UCO_QUICK"))
            break;
    }

    return 0;
}
