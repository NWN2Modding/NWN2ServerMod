#pragma once
// Lists the server on the NWN2 master server so NWNScry and the other lists show it.
// EE dropped the check-in 1.23 sent (xp_bugfix forwarded it to Skywing's master) but still
// answers the master's queries. This sends the check-in instead, every 5 minutes.

#include <Logger.h>

#include <memory>

namespace nwn2mod
{
    class MasterServer
    {
    public:
        /// Hooks bind and starts the check-in thread.
        static bool Install(std::shared_ptr<Logger> logger);
    };
}
