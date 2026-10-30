#pragma once

#include <string>

namespace sst {

// Version and date of the installed ScreenFleet display driver, as Windows
// records them ("1.0.0.0", "30 Oct 2026"). False if it isn't installed.
bool QueryDriverInfo(std::string& version, std::string& date);

}  // namespace sst
