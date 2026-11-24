#pragma once
// ============================================================
// UCOnline2 -- a local answer for the ISteamUGC workshop query
//
// WHY
// A game that asks for its workshop content does it in two steps: it
// sends the query and gets a call handle back, and then it *waits* for
// the call result.  Space Engineers does exactly that in
// MySteamWorkshop.GetSubscribedModsBlocking() (Sandbox.Game.dll), where
// the wait is a resetEvent.WaitOne() without a timeout:
//
//     myWorkshopQuery.Run();            // sends the query
//     resetEvent.WaitOne();             // waits for the answer, forever
//
// If the answer never arrives the game hangs, and no error is reported
// anywhere -- the thread simply stops.  That is what happens when the
// query is forwarded to the real client and the client does not answer.
//
// So the query never leaves.  SendQueryUGCRequest() is answered here
// with an empty result ("the query succeeded, there is nothing in it"),
// and the answer reaches the game through the same call-result machinery
// the rest of the emulator uses.
//
// The query handle itself still comes from the real interface: creating
// one is a purely local operation (no network traffic), and it lets the
// game set tags, filters and return flags on it exactly as usual.  Only
// the send is intercepted.
//
// ISteamUGC vtable indices (STEAMUGC_INTERFACE_VERSION021):
//    4: SendQueryUGCRequest
// ISteamUtils vtable indices:
//   13: GetAPICallResult
// ============================================================

#include "../platform.h"
#include "../hook_shim.h"
#include "../globals.h"
#include "../callback_dispatcher.h"
#include "../sdk/steam_api.h"
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace UcoUGCQueryEmu
{
    inline std::recursive_mutex s_lock;
    // call handle -> (callback id, the raw result bytes)
    inline std::map<SteamAPICall_t, std::pair<int, std::vector<uint8_t>>> s_calls;
    // A high range, so a handle of ours never collides with one of the
    // real client (which counts them from one).
    inline SteamAPICall_t s_nextCall = 0x5300000000000000ULL;

    inline bool IsOurCall(SteamAPICall_t hCall)
    {
        std::lock_guard<std::recursive_mutex> g(s_lock);
        return s_calls.find(hCall) != s_calls.end();
    }

    inline bool GetAPICallResult(SteamAPICall_t hCall, void* pBuf, int cubBuf, int, bool* pbFailed)
    {
        std::lock_guard<std::recursive_mutex> g(s_lock);
        auto it = s_calls.find(hCall);
        if (it == s_calls.end()) return false;
        if (pbFailed) *pbFailed = false;
        int n = (int)it->second.second.size();
        if (pBuf && cubBuf >= n) memcpy(pBuf, it->second.second.data(), n);
        return true;
    }

    // Hand the completion over to the game.  The callback id and the size of
    // the result are the ones the *game* uses -- the dispatcher matches a
    // completion by both, and our SDK header may be older than the one the
    // game was built against.
    inline void PostCompletion(SteamAPICall_t hCall, int iCallback, uint32 cubParam)
    {
        SteamAPICallCompleted_t done{};
        done.m_hAsyncCall = hCall;
        done.m_iCallback = iCallback;
        done.m_cubParam = cubParam;
        GetDispatcher()->PostCallback(SteamAPICallCompleted_t::k_iCallback, &done,
            sizeof(done), g_ClientUser, false, 10);
        UCOLOG("[UCOnline2] UGC query call %llu completed (callback %d, %u bytes)",
            (unsigned long long)hCall, iCallback, cubParam);
    }

    // The empty answer: the query succeeded, there is simply nothing in it.
    inline SteamAPICall_t SendQueryUGCRequest(UGCQueryHandle_t hQuery)
    {
        std::lock_guard<std::recursive_mutex> g(s_lock);

        SteamUGCQueryCompleted_t res{};
        res.m_handle = hQuery;
        res.m_eResult = k_EResultOK;
        res.m_unNumResultsReturned = 0;
        res.m_unTotalMatchingResults = 0;
        res.m_bCachedData = false;

        SteamAPICall_t hCall = s_nextCall++;
        std::vector<uint8_t> data(sizeof(res));
        memcpy(data.data(), &res, sizeof(res));
        s_calls[hCall] = { (int)SteamUGCQueryCompleted_t::k_iCallback, std::move(data) };

        UCOLOG("[UCOnline2] ISteamUGC::SendQueryUGCRequest -> empty answer (call %llu); the query was not sent to Steam",
            (unsigned long long)hCall);

        // Hand the completion over right away, so the game finds it in the
        // callback queue no matter how it collects callbacks.  The size we
        // announce is ours -- the game may have been built against a newer
        // SDK with a bigger struct, which is why RegisterCallResult() posts
        // a second time, with the game's own size.
        PostCompletion(hCall, (int)SteamUGCQueryCompleted_t::k_iCallback, (uint32)sizeof(SteamUGCQueryCompleted_t));
        return hCall;
    }
}

// ============================================================
// The hooks
//
// The detour replaces SendQueryUGCRequest, so the query never reaches
// the real client.  The original is kept, but never called -- a game
// that wants the real workshop list can be served by forwarding it
// again from the detour.

// The originals.  The query detour never calls them -- the query is not
// forwarded -- but the interface hooks need somewhere to put them.
static SteamAPICall_t (S_CALLTYPE* g_pfnOriginalSendQueryUGCRequest)(void*, UGCQueryHandle_t) = nullptr;
static bool (S_CALLTYPE* g_pfnOriginalUtilsGetAPICallResult)(void*, SteamAPICall_t, void*, int, int, bool*) = nullptr;

static SteamAPICall_t S_CALLTYPE Hooked_SendQueryUGCRequest(void* pThis, UGCQueryHandle_t handle)
{
    return UcoUGCQueryEmu::SendQueryUGCRequest(handle);
}

// ISteamUtils::GetAPICallResult is how a game that calls the interface
// directly (instead of going through the SDK's CallResult<> template)
// fetches the result of a call.  Without this hook it would ask the real
// client about a handle the real client does not know, get "not ready"
// and wait forever.
static bool S_CALLTYPE Hooked_Utils_GetAPICallResult(void* pThis, SteamAPICall_t hCall,
    void* pBuf, int cubBuf, int iExpected, bool* pbFailed)
{
    if (UcoUGCQueryEmu::IsOurCall(hCall))
    {
        UCOLOG("[UCOnline2] ISteamUtils::GetAPICallResult -> our call %llu",
            (unsigned long long)hCall);
        return UcoUGCQueryEmu::GetAPICallResult(hCall, pBuf, cubBuf, iExpected, pbFailed);
    }
    if (g_pfnOriginalUtilsGetAPICallResult)
        return g_pfnOriginalUtilsGetAPICallResult(pThis, hCall, pBuf, cubBuf, iExpected, pbFailed);
    if (pbFailed) *pbFailed = true;
    return false;
}

static void UcoInstallUGCQueryHooks(ISteamUGC* pUGC, ISteamUtils* pUtils)
{
    if (pUGC)
    {
        void** ugcVT = *reinterpret_cast<void***>(pUGC);
        const PlatHookStatus st = PlatHookVTableMethod(ugcVT, 4,
            (void*)&Hooked_SendQueryUGCRequest, (void**)&g_pfnOriginalSendQueryUGCRequest);
        if (st == PLAT_HOOK_OK || st == PLAT_HOOK_ALREADY)
            UCOLOG("[UCOnline2] ISteamUGC::SendQueryUGCRequest hook installed (vtable[4]) -- the workshop query is answered locally");
        else
            UCOLOG("[UCOnline2] failed to hook ISteamUGC::SendQueryUGCRequest (vtable[4]): %d", (int)st);
    }

    if (pUtils)
    {
        void** utilsVT = *reinterpret_cast<void***>(pUtils);
        const PlatHookStatus st = PlatHookVTableMethod(utilsVT, 13,
            (void*)&Hooked_Utils_GetAPICallResult, (void**)&g_pfnOriginalUtilsGetAPICallResult);
        if (st == PLAT_HOOK_OK || st == PLAT_HOOK_ALREADY)
            UCOLOG("[UCOnline2] ISteamUtils::GetAPICallResult hook installed (vtable[13])");
        else
            UCOLOG("[UCOnline2] failed to hook ISteamUtils::GetAPICallResult (vtable[13]): %d", (int)st);
    }
}
