#include "Config.h"

std::expected<Config, std::string> Config::FromString(std::string_view src)
{
    return Yaml::FromString<Config>(src);
}

std::expected<Config, std::string> Config::FromFile(std::filesystem::path path)
{
    return Yaml::FromFile<Config>(path);
}
