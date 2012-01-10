/**@addtogroup Filter Steam limiter filter hook DLL.
 * @{@file
 *
 * This DLL is injected into the Steam process to filter the set of hosts that
 * we will allow Steam to connect to for the purpose of downloading content.
 *
 * @author Nigel Bree <nigel.bree@gmail.com>
 *
 * Copyright (C) 2011 Nigel Bree; All Rights Reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Although I did have an earlier version which tried to use vectored exception
 * handling for detouring the connect function, this version is simpler and is
 * built to use the built-in patching facilities in Windows DLLs, as per
 * http://blogs.msdn.com/b/oldnewthing/archive/2011/09/21/10214405.aspx
 */

#include "../nolocale.h"
#include <winsock2.h>

#include "glob.h"
#include "filterrule.h"


/**
 * For declaring exported callable functions from the injection shim.
 */

#define STEAMDLL(type)  extern "C" __declspec (dllexport) type __stdcall

/**
 * The prototype of the main function we're hooking is:
 *   int WSAAPI connect (SOCKET s, const struct sockaddr *name, int namelen);
 */

typedef int (WSAAPI * ConnectFunc) (SOCKET s, const sockaddr * name, int namelen);

/**
 * The prototype of the legacy sockets DNS name lookup API is:
 *   struct hostent * gethostbyname (const char * name);
 *
 * There are three functions that could be used for name resolution; this is
 * the original BSD Sockets one, and as the oldest it uses a fixed buffer for
 * the result that means no support at all for IPv6 is possible.
 *
 * The IETF-sanctioned replacement for this is getaddrinfo (), which is better
 * (although still problematic to use, being completely synchronous like all
 * the BSD socket APIs) and Microsoft have their own wide-character version for
 * applications which want to explicitly use Unicode or UTF-8 encodings. Older
 * applications written before IPv6 still tend to use gethostbyname (), so this
 * is normally the one to filter.
 */

typedef struct hostent * (WSAAPI * GetHostFunc) (const char * name);

/**
 * The prototype of the legacy sockets inet_addr () function.
 */

typedef unsigned long (WSAAPI * inet_addr_func) (const char * addr);

/**
 * The prototype of the legacy sockets recv () function.
 */

typedef int   (WSAAPI * RecvFunc) (SOCKET s, char * buf, int len, int flags);

/**
 * The prototype of the legacy sockets recvfrom () function.
 */

typedef int   (WSAAPI * RecvFromFunc) (SOCKET s, char * buf, int len, int flags,
                                       sockaddr * from, int * fromLen);

/**
 * The prototype of the modern asynchronous WSARecv () function.
 */

typedef int   (WSAAPI * WSARecvFunc) (SOCKET s, LPWSABUF buffers,
                                      unsigned long count,
                                      unsigned long * received,
                                      unsigned long * flags,
                                      OVERLAPPED * overlapped,
                                      LPWSAOVERLAPPED_COMPLETION_ROUTINE handler);

/**
 * The prototype of WSAGetOverlappedResult (), used with WSASend () and
 * WSARecv ().
 */

typedef BOOL  (WSAAPI * WSAGetOverlappedFunc) (SOCKET s, OVERLAPPED * overlapped,
                                               unsigned long * length, BOOL wait,
                                               unsigned long * flags);

/**
 * For representing hooked functions, and wrapping up the hook and unhook
 * machinery.
 *
 * Just a placeholder for now, but I intend to factor the hook machinery into
 * here and make it a separate file at some point, along with making the hook
 * attach and detach operations maintain state for easier unhooking.
 */

struct ApiHook {
        FARPROC         m_original;
        FARPROC         m_resume;
        FARPROC         m_hook;
        unsigned char   m_save [8];
        unsigned char   m_thunk [16];

                        ApiHook ();
                      ~ ApiHook ();

        bool            operator == (int) const { return m_resume == 0; }
        bool            operator != (int) const { return m_resume != 0; }

        FARPROC         makeThunk (unsigned char * data, size_t length);
        void            unhook (void);

        bool            attach (void * address, FARPROC hook);
        bool            attach (void * hook, HMODULE lib, const char * name);
};

/**
 * Specialize ApiHook for the function calling signatures.
 */

template <class F>
struct Hook : public ApiHook {
        F               operator * (void) {
                return (F) m_resume;
        }

        bool            attach (F hook, HMODULE lib, const char * name);
};

/**
 * This is the original connect () function, just past the patch area.
 */

Hook<ConnectFunc>       g_connectResume;

/**
 * This is the original gethostbyname () function, just past the patch area.
 */

Hook<GetHostFunc>       g_gethostResume;

/**
 * The original inet_addr () function, just past the patch area.
 */

Hook<inet_addr_func>    g_inet_addr_resume;

/**
 * The original recv () function, just past the patch area.
 */

Hook<RecvFunc>          g_recvResume;

/**
 * The original recvfrom () function, just past the patch area.
 */

Hook<RecvFromFunc>      g_recvfromResume;

/**
 * The original WSARecv () function, just past the patch area.
 */

Hook<WSARecvFunc>       g_wsaRecvResume;

/**
 * The original WSAGetOverlappedResult (), just past the patch area.
 */

Hook<WSAGetOverlappedFunc> g_wsaGetOverlappedResume;

/**
 * The Telstra IP address we're after in network byte order.
 */

FilterRules     g_rules (27030);

/**
 * Hook for the connect () function; check if we want to rework it, or just
 * continue on to the original.
 *
 * Mainly our aim was to block port 27030 which is the good Steam 'classic' CDN
 * but Valve have *two* amazingly half-baked and badly designed HTTP download
 * systems as well. One - "CDN" - uses DNS tricks, while the other one - "CS" -
 * is rather more nasty, and almost impossible to filter cleanly out from the
 * legitimate use of HTTP inside Steam - for CS servers only numeric IPs are
 * passed over HTTP and they aren't parsed by API functions we can hook like
 * RtlIpv4AddressToStringEx (which exists in Windows XPSP3 even though it's
 * not documented).
 */

int WSAAPI connectHook (SOCKET s, const sockaddr * name, int namelen) {
        /*
         * Capture the caller's return address so we can map it to a module and
         * thus a module name, to potentially include in the filter string.
         *
         * An alternative to this is to use RtlCaptureStackBacktrace to get the
         * caller (and potentially more of the stack), but sinec we're x86 only
         * for now this should do fine.
         */

        HMODULE         module = 0;

#if     0
        unsigned long   addr = ((unsigned long *) & s) [- 1];
        GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            (LPCWSTR) addr, & module);
#endif

        sockaddr_in   * replace = 0;
        if (name->sa_family != AF_INET ||
            ! g_rules.match ((const sockaddr_in *) name, module, & replace)) {
                /*
                 * Just forward on to the original.
                 */

                return (* g_connectResume) (s, name, namelen);
        }

        /*
         * If no replacement is specified, deny the connection. This isn't used
         * for Steam blocking in most cases because it responds to this by just
         * trying another server.
         */

        if (replace == 0 || replace->sin_addr.S_un.S_addr == INADDR_NONE) {
                OutputDebugStringA ("Connect refused\r\n");
                SetLastError (WSAECONNREFUSED);
                return SOCKET_ERROR;
        }

        OutputDebugStringA ("Connect redirected\r\n");

        /*
         * Redirect the connection; put the rewritten address into a temporary
         * so the change isn't visible to the caller (Steam doesn't appear to
         * care either way, but it's best to be careful).
         */

        sockaddr_in   * base = (sockaddr_in *) name;
        sockaddr_in     temp;
        temp.sin_family = base->sin_family;
        temp.sin_port = replace->sin_port != 0 ? replace->sin_port :
                        base->sin_port;
        temp.sin_addr = replace->sin_addr.S_un.S_addr != 0 ?
                        replace->sin_addr : base->sin_addr;
                
        return (* g_connectResume) (s, (sockaddr *) & temp, sizeof (temp));
}

/**
 * Hook for the gethostbyname () Sockets address-resolution function.
 */

struct hostent * WSAAPI gethostHook (const char * name) {
        sockaddr_in   * replace = 0;
        if (! g_rules.match (name, & replace) ||
            (replace != 0 && replace->sin_addr.S_un.S_addr == INADDR_ANY)) {
                /*
                 * If there's no matching rule, or the matching rule is a
                 * passthrough, then let things slide.
                 */

                return (* g_gethostResume) (name);
        }

        if (replace == 0 || replace->sin_addr.S_un.S_addr == INADDR_NONE) {
                /*
                 * On Windows, WSAGetLastError () and WSASetLastError () are
                 * just thin wrappers around GetLastError ()/SetLastError (),
                 * so we can use SetLastError ().
                 *
                 * Set up the right error number for a nonexistent host, since
                 * this classic BSD sockets API doesn't have a proper error
                 * reporting mechanism and that's what most code looks for to
                 * see what went wrong.
                 */

                OutputDebugStringA ("gethostbyname refused\r\n");
                SetLastError (WSAHOST_NOT_FOUND);
                return 0;
        }

        OutputDebugStringA ("gethostbyname redirected\r\n");

        /*
         * Replacing a DNS result raises the question of storage, which for
         * base Windows sockets is per-thread; we could also choose to use a
         * non-thread-safe global like most UNIX implementations.
         *
         * One obvious trick would be to let the underlying call return the
         * structure which we modify, but the tradeoff there is that if it
         * fails we don't have a pointer.
         *
         * So for now, cheese out and use a global; also, copy the address
         * rather than point at the replacement.
         */

static  hostent         result;
static  unsigned long   addr;
static  unsigned long * addrList [2] = { & addr };

        addr = replace->sin_addr.S_un.S_addr;
        result.h_addrtype = AF_INET;
        result.h_addr_list = (char **) addrList;
        result.h_aliases = 0;
        result.h_length = sizeof (addr);
        result.h_name = "remapped.local";

        return & result;
}

/**
 * For measuring bandwidth.
 */

class Meter {
public:
typedef CRITICAL_SECTION        Mutex;

private:
        Mutex           m_lock;
        unsigned long   m_now;
        unsigned long   m_currentBytes;

        unsigned long   m_last;
        long long       m_total;

        void            newTick (unsigned long now);

public:
                        Meter ();

        void            operator += (int bytes);
};

Meter :: Meter () : m_now (GetTickCount ()), m_currentBytes (0),
                m_last (0), m_total (0) {
        InitializeCriticalSection (& m_lock);
}

void Meter :: newTick (unsigned long now) {
        unsigned long   delta = now - m_now;
        if (delta < 1)
                return;

        unsigned long   bytes = m_currentBytes;
        m_currentBytes = 0;

        m_total += bytes;

        unsigned long   delta2 = m_now - m_last;

#if     0
        char            buf [80];
        wsprintfA (buf, "%d %d %d %X\r\n",
                   delta, bytes, delta2,
                   (unsigned long) m_total);
        OutputDebugStringA (buf);
#endif

        m_last = m_now;
        m_now = now;
}

void Meter :: operator += (int bytes) {
        if (bytes == SOCKET_ERROR)
                bytes = 0;

        EnterCriticalSection (& m_lock);

        unsigned long   now = GetTickCount ();
        newTick (now);

        m_currentBytes += bytes;

        LeaveCriticalSection (& m_lock);
}

Meter           g_meter;

/**
 * Hook the recv () API, to measure received bandwidth.
 *
 * Most of the time the underlying socket will probably be in non-blocking mode
 * as any use of this API will be from old code which has had to struggle with
 * the broken original UNIX sockets API design.
 */

int WSAAPI recvHook (SOCKET s, char * buf, int len, int flags) {
        int             result;
        result = (* g_recvResume) (s, buf, len, flags);
        g_meter += result;
        return result;
}

int WSAAPI recvfromHook (SOCKET s, char * buf, int len, int flags,
                         sockaddr * from, int * fromLen) {
        int             result;
        result = (* g_recvfromResume) (s, buf, len, flags, from, fromLen);
        g_meter += result;
        return result;
}

/**
 * Hook the WSARecv () API, to measure received bandwidth.
 *
 * This is a more interesting case because of the OVERLAPPED option; real, true
 * asynchronous I/O, and the traditional Steam CDN download does use it. Even
 * with AIO, there are several wrinkles thanks to the best part of AIO under
 * Windows, which is completion ports (which Steam doesn't use, although lots
 * of the serious code we might want to apply this to in future does).
 *
 * To deal with capturing overlapped completions fully, we need to also hook
 * WSAGetOverlappedResult () and probably WSAWaitForMultipleObjects (), which
 * will give us the option of slicing the end-user's original I/O up using a
 * custom OVERLAPPED buffer of our own in the slices (useful when we get to
 * trying to apply a bandwidth limit).
 */

int WSAAPI wsaRecvHook (SOCKET s, LPWSABUF buffers, unsigned long count,
                        unsigned long * received, unsigned long * flags,
                        OVERLAPPED * overlapped,
                        LPWSAOVERLAPPED_COMPLETION_ROUTINE handler) {
        if (overlapped != 0 || handler != 0) {
                int             result;
                result = (* g_wsaRecvResume) (s, buffers, count, received, flags,
                                              overlapped, handler);

                if (result == 0 && overlapped != 0) {
                        /**
                         * Synchronous success, process here.
                         */

                        g_meter += overlapped->InternalHigh;
                }
                return result;
        }

        bool            ignore;
        ignore = flags != 0 && (* flags & MSG_PEEK) != 0;

        int             result;
        result = (* g_wsaRecvResume) (s, buffers, count, received, flags,
                                      overlapped, handler);
        if (result != SOCKET_ERROR && ! ignore)
                g_meter += * received;
        return result;
}

BOOL WSAAPI wsaGetOverlappedHook (SOCKET s, OVERLAPPED * overlapped,
                                  unsigned long * length, BOOL wait,
                                  unsigned long * flags) {
        BOOL            result;
        result = (* g_wsaGetOverlappedResume) (s, overlapped, length, wait, flags);

        return result;
}

/**
 * Write a 32-bit value into the output in Intel byte order.
 */

unsigned char * writeOffset (unsigned char * dest, unsigned long value) {
        * dest ++ = value & 0xFF;
        value >>= 8;
        * dest ++ = value & 0xFF;
        value >>= 8;
        * dest ++ = value & 0xFF;
        value >>= 8;
        * dest ++ = value & 0xFF;
        return dest;
}

/**
 * Code-generation stuff.
 */

#define PUSH_IMM8       0x6A
#define JMP_LONG        0xE9
#define JMP_SHORT       0xEB

#define MOV_EDI_EDI     0xFF8B
#define JMP_SHORT_MINUS5 (0xF900 + JMP_SHORT)

/**
 * Record our HMODULE value for unloading.
 */

HMODULE         g_instance;

/**
 * Set up the address to direct the content server connections to.
 */

int setFilter (wchar_t * address) {
        bool            result = g_rules.install (address);
        if (result) {
                /*
                 * For now, always append this black-hole DNS rule to the main
                 * rule set. Later on this might change but this will do until
                 * the full situation with the HTTP CDN is revealed.
                 *
                 * Since rules are processed in order, this still allows custom
                 * rules to redirect these DNS lookups to take place, as those
                 * will take precedence to this catch-all.
                 */

                g_rules.append (L"content?.steampowered.com=");
        }

        return result ? 1 : 0;
}

/**
 * Copy some code from a patch target into a thunk.
 *
 * This is used if the target doesn't contain the patch NOP; we copy the code
 * to a temporary area so that when we resume the original function, we call
 * the relocated code which then branches back to the original flow.
 *
 * This relies on being able to measure instruction lengths to a degree to know
 * how much to relocate; doing this in general for x86 isn't too bad, but since
 * function entry points are highly idiomatic we probably won't need to solve
 * the general problem (I've written a full general patcher that does this in
 * the past, but don't have access to that code anymore).
 */

FARPROC ApiHook :: makeThunk (unsigned char * data, size_t bytes) {
        memcpy (m_thunk, data, bytes);

        m_thunk [bytes] = JMP_LONG;
        writeOffset (m_thunk + bytes + 1, m_thunk - data);

        unsigned long   protect = 0;
        if (! VirtualProtect (m_thunk, sizeof (m_thunk),
                              PAGE_EXECUTE_READWRITE, & protect))
                return 0;

        return (FARPROC) & m_thunk;
}

/**
 * Use the built-in Windows run-time patching system for core DLLs.
 *
 * In almost all cases this works fine; there are a few stray APIs in the Win32
 * ecosystem where the initial two-byte NOP isn't present (e.g. inet_addr) and
 * so some form of thunk-based redirection can be needed.
 *
 * For the inet_addr case the 5 bytes of regular NOP space is still there, and
 */

bool ApiHook :: attach (void * address, FARPROC hook) {
        if (address == 0)
                return false;

        m_hook = hook;
        m_original = (FARPROC) address;

        /*
         * Check for the initial MOV EDI, EDI two-byte NOP in the target
         * function, to signify the presence of a free patch area.
         *
         * We'll rely on the x86's support for unaligned access here, as we're
         * always going to be doing this on an x86 or something with equivalent
         * support for unaligned access.
         */

        unsigned char * data = (unsigned char *) address;
        memcpy (m_save, data - 5, 8);
        m_resume = 0;

        if (* (unsigned short *) data == MOV_EDI_EDI) {
                /*
                 * No need for a thunk, the resume point can be where we want.
                 */

                m_resume = (FARPROC) (data + 2);
        } else if (* data == PUSH_IMM8) {
                m_resume = makeThunk (data, 2);
        } else
                return false;

        /*
         * Write a branch to the hook stub over the initial NOP of the target
         * function.
         */

        unsigned long   protect = 0;
        if (! VirtualProtect (data - 5, 7, PAGE_EXECUTE_READWRITE, & protect))
                return false;

        /*
         * Put the long jump to the detour first (in space which is reserved
         * for just this purpose in code compiled for patching), then put the
         * short branch to the long jump in the two-byte slot at the regular
         * function entry point.
         */

        data [- 5] = JMP_LONG;
        writeOffset (data - 4, (unsigned char *) hook - data);
        * (unsigned short *) data = JMP_SHORT_MINUS5;

        return true;
}

/**
 * Add some idiomatic wrapping for using GetProcAddress.
 */

bool ApiHook :: attach (void * hook, HMODULE lib, const char * name) {
        FARPROC         func = GetProcAddress (lib, name);
        if (! func) {
                OutputDebugStringA ("No function: ");
                OutputDebugStringA (name);
                OutputDebugStringA ("\r\n");

                m_resume = 0;
                return false;
        }

        if (! attach (func, (FARPROC) hook)) {
                OutputDebugStringA ("Can't hook: ");
                OutputDebugStringA (name);
                OutputDebugStringA ("\r\n");

                m_resume = 0;
                return false;
        }

        return true;
}

/**
 * Remove an attached hook.
 *
 * In case the target DLL is actually unloaded, the write to the detour point
 * is guarded with a Win32 SEH block to avoid problems with the writes.
 */

void ApiHook :: unhook (void) {
        if (m_resume == 0)
                return;

        __try {
                memcpy ((unsigned char *) m_original - 5, m_save, 7);
        } __finally {
                m_original = m_resume = 0;
        }
}

/**
 * Unhook all the hooked functions.
 */

void unhookAll (void) {
        g_connectResume.unhook ();
        g_gethostResume.unhook ();
        g_inet_addr_resume.unhook ();
        g_recvResume.unhook ();
        g_recvfromResume.unhook ();
        g_wsaRecvResume.unhook ();
        g_wsaGetOverlappedResume.unhook ();
}

/**
 * Simple default constructor.
 */

ApiHook :: ApiHook () : m_original (0), m_resume (0), m_hook (0) {
}

/**
 * Simple default destructor.
 */

ApiHook :: ~ ApiHook () {
        unhook ();
}

/**
 * Simple attach.
 *
 * In principle in future this should lead to the hook being attached to a list
 * of all currently hooked functions.
 */

template <class F>
bool Hook<F> :: attach (F hook, HMODULE lib, const char * name) {
        return ApiHook :: attach (hook, lib, name);
}

/**
 * Establish the hook filter we want on the connect function in WS2_32.DLL
 */

STEAMDLL (int) SteamFilter (wchar_t * address, wchar_t * result,
                            size_t * resultSize) {
        /*
         * If we've already been called, this is a call to re-bind the address
         * being monitored.
         */

        if (g_connectResume != 0)
                return setFilter (address);

        /*
         * Wait for the target module to be present, so as not to interfere
         * with any loading or initialization process in the host process.
         */

        HMODULE         ws2;
        for (;;) {
                ws2 = GetModuleHandleW (L"WS2_32.DLL");
                if (ws2 != 0)
                        break;

                Sleep (1000);
        }

        setFilter (address);

        bool            success;
        success = g_connectResume.attach (connectHook, ws2, "connect") &&
                  g_gethostResume.attach (gethostHook, ws2, "gethostbyname") &&
                  g_recvResume.attach (recvHook, ws2, "recv") &&
                  g_recvfromResume.attach (recvfromHook, ws2, "recvfrom") &&
                  g_wsaRecvResume.attach (wsaRecvHook, ws2, "WSARecv") &&
                  g_wsaGetOverlappedResume.attach (wsaGetOverlappedHook,
                                                   ws2, "WSAGetOverlappedResult");

        if (! success) {
                unhookAll ();
                return ~ 0UL;
        }

        OutputDebugStringA ("SteamFilter hook attached\n");

        /*
         * Since we loaded OK, we want to stay loaded; add one to the refcount
         * for LoadLibrary; using GetModuleHandleEx () we can do this in one
         * easy call.
         */

        GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            (LPCWSTR) SteamFilter,
                            & g_instance);

        return 1;
}

/**
 * Disable just the current hook.
 *
 * Do critical cleanup. Note that the Winsock DLL might actually have been
 * unloaded when we are called, so guard this with an exception handler in
 * case the hook address can't actually be written any more.
 */

void removeHook (void) {
        if (g_connectResume == 0)
                return;

        unhookAll ();
        OutputDebugStringA ("SteamFilter unhooked\n");
}

/**
 * Export an explicit unload entry point.
 *
 * This reduces the DLL LoadLibrary reference count by one to match the count
 * adjustment in SteamFilter () - the calling shim also holds a reference so
 * that this doesn't provoke an immediate unload, that happens in the caller.
 */

STEAMDLL (int) FilterUnload (void) {
        if (g_instance == 0)
                return 0;

        removeHook ();
        FreeLibrary (g_instance);
        g_instance = 0;
        return 1;
}

BOOL WINAPI DllMain (HINSTANCE instance, unsigned long reason, void *) {
        if (reason != DLL_PROCESS_DETACH)
                return TRUE;

        /*
         * Do critical cleanup. Note that the Winsock DLL might actually have
         * been unloaded when we are called, so we should guard this with an
         * exception handler in case the hook address can't actually be
         * written any more.
         */

        removeHook ();
}

/**@}*/
