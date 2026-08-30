#pragma once

#define ADDON_VERSION "1.5.13"

// The R&D runner injects a source/build identity at configure time. Keep a
// deterministic fallback so ordinary upstream-style builds remain valid.
#ifndef TAPIR_BUILD_ID
#define TAPIR_BUILD_ID "unconfigured"
#endif
