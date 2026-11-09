#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "MasterServer.h"

#include <detours.h>

#include <atomic>
#include <cstdint>

namespace
{
    constexpr const char* kHost = "nwn2.mst.valhallalegends.com";
    constexpr const char* kPort = "6121";
    constexpr DWORD kFirstDelayMs = 60 * 1000; // let the module load first
    constexpr DWORD kIntervalMs = 5 * 60 * 1000;

    using BindFunc = int(WSAAPI*)(SOCKET, const sockaddr*, int);
    BindFunc g_realBind = nullptr;

    std::shared_ptr<Logger> g_logger;
    std::atomic<uint16_t> g_gamePort = 0;
    HANDLE g_portFound = nullptr;

    int WSAAPI BindHook(SOCKET s, const sockaddr* name, int nameLength)
    {
        int result = g_realBind(s, name, nameLength);

        // The game port is the first UDP bind to a set port. sin_port lines up for IPv4 and IPv6.
        if (result == 0 && g_gamePort == 0 && name && nameLength >= static_cast<int>(sizeof(sockaddr_in))
            && (name->sa_family == AF_INET || name->sa_family == AF_INET6))
        {
            uint16_t port = ::ntohs(reinterpret_cast<const sockaddr_in*>(name)->sin_port);
            int type = 0;
            int size = sizeof(type);
            if (port != 0
                && ::getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &size) == 0
                && type == SOCK_DGRAM)
            {
                g_gamePort = port;
                ::SetEvent(g_portFound);
            }
        }
        return result;
    }

    /// The master checks the port in the packet at our address, so this can use its own socket.
    bool CheckIn(uint16_t port)
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* master = nullptr;
        if (::getaddrinfo(kHost, kPort, &hints, &master) != 0 || !master)
        {
            return false;
        }

        bool sent = false;
        SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET)
        {
            const char packet[6] = { 'B', 'M', 'S', 'T', static_cast<char>(port & 0xFF), static_cast<char>(port >> 8) };
            sent = ::sendto(s, packet, sizeof(packet), 0, master->ai_addr, static_cast<int>(master->ai_addrlen))
                == sizeof(packet);
            ::closesocket(s);
        }
        ::freeaddrinfo(master);
        return sent;
    }

    DWORD WINAPI CheckInThread(void*)
    {
        WSADATA wsa;
        ::WSAStartup(MAKEWORD(2, 2), &wsa);

        ::WaitForSingleObject(g_portFound, INFINITE);
        uint16_t port = g_gamePort;
        (*g_logger)("master_server: game port {}; checking in with {} every 5 minutes.", port, kHost);

        ::Sleep(kFirstDelayMs);

        // Log changes only.
        bool reached = true;
        for (;;)
        {
            bool sent = CheckIn(port);
            if (sent != reached)
            {
                if (sent)
                {
                    (*g_logger)("master_server: reaching {} again.", kHost);
                }
                else
                {
                    (*g_logger)(Logger::Level::Warning, "master_server: couldn't reach {}; will keep trying.", kHost);
                }
                reached = sent;
            }
            ::Sleep(kIntervalMs);
        }
    }
}

namespace nwn2mod
{
    bool MasterServer::Install(std::shared_ptr<Logger> logger)
    {
        g_logger = std::move(logger);

        HMODULE ws2 = ::LoadLibraryW(L"ws2_32.dll");
        g_realBind = ws2 ? reinterpret_cast<BindFunc>(::GetProcAddress(ws2, "bind")) : nullptr;
        if (!g_realBind)
        {
            (*g_logger)("master_server: couldn't find bind in ws2_32.dll; the server won't be listed.");
            return false;
        }

        g_portFound = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

        ::DetourTransactionBegin();
        ::DetourUpdateThread(::GetCurrentThread());
        ::DetourAttach(&(PVOID&)g_realBind, reinterpret_cast<PVOID>(&BindHook));
        LONG attached = ::DetourTransactionCommit();
        if (attached != NO_ERROR)
        {
            (*g_logger)("master_server: Detours refused to attach (error {}); the server won't be listed.", attached);
            return false;
        }

        HANDLE thread = ::CreateThread(nullptr, 0, CheckInThread, nullptr, 0, nullptr);
        if (thread)
        {
            ::CloseHandle(thread);
        }
        return true;
    }
}
