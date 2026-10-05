// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#include <compositionengine/UdfpsExtension.h>

#include <limits>

uint32_t getUdfpsDimZOrder(uint32_t z) {
    return z;
}

uint32_t getUdfpsZOrder(uint32_t z, bool touched) {
    // Keep the independently composed illumination surface above the framework dim window.
    return touched ? std::numeric_limits<uint32_t>::max() : z;
}

uint64_t getUdfpsUsageBits(uint64_t usageBits, bool /*touched*/) {
    return usageBits;
}
