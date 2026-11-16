#pragma once

#include <switch.h>
#include <drako3d.h>

// The libounce framebuffer console opens its own GPU session which cannot coexist with drako3d's.
// This is annoying
ConsoleRenderer* drakoConsoleRenderer(DkDevice device);
