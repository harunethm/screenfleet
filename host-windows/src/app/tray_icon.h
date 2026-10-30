#pragma once

#include <windows.h>

namespace sst::app {

// The product mark (a monitor with the tablet beside it) as an icon of the
// given pixel size; white for dark backgrounds, near-black for light ones.
HICON CreateMarkIcon(int size, bool white);

}  // namespace sst::app
