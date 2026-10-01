#pragma once
// The two logging keys every ported plugin understands, applied the same way everywhere.
//
// NWNX4 gave each plugin `log_level` and `log_max_file_size` in its ini, and administrators relied
// on both: one to turn a chatty plugin down, the other to stop a log filling a disk. Keeping the
// key names and the accepted values means that knowledge carries over unchanged.
//
// The logger has to exist before the config is read - reading it may itself need to report a
// problem - so verbosity and rotation are applied afterwards rather than passed to a constructor.
#include <Logger.h>

namespace nwn2ports
{
    /// Applies `log_level` and `log_max_file_size` from any config struct that has them.
    /// Anything unrecognised is reported and ignored rather than silently dropping the setting.
    template <typename TConfig>
    void ApplyLogConfig(Logger& logger, const TConfig& config)
    {
        if (config.log_level)
        {
            if (auto level = Logger::ParseLevel(*config.log_level))
            {
                logger.SetMinimumLevel(*level);
            }
            else
            {
                logger(Logger::Level::Warning,
                       "log_level '{}' is not one of none, error, warning, info, debug, trace; "
                       "keeping info.", *config.log_level);
            }
        }

        if (config.log_max_file_size)
        {
            if (auto bytes = Logger::ParseSize(*config.log_max_file_size))
            {
                logger.SetMaxFileSize(*bytes);

                if (*bytes == 0)
                {
                    logger(Logger::Level::Warning,
                           "log_max_file_size is 0, so this log will grow without bound.");
                }
            }
            else
            {
                logger(Logger::Level::Warning,
                       "log_max_file_size '{}' is not a size such as 10M; keeping the default.",
                       *config.log_max_file_size);
            }
        }
    }
}
