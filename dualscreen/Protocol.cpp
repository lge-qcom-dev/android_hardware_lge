// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#include "Protocol.h"

#include <cstddef>

namespace lge::dualscreen {

uint16_t read16(const uint8_t* bytes) {
    return bytes[0] | (uint16_t(bytes[1]) << 8);
}

bool validDimensions(int width, int height) {
    return width > 0 && height > 0 && int64_t(width) * height <= 65534 &&
           (int64_t(width) * height % 2) == 0;
}

std::vector<uint8_t> packPixels(const std::vector<uint8_t>& pixels) {
    if (pixels.size() % 2) return {};
    std::vector<uint8_t> packed(pixels.size() / 2);
    for (size_t i = 0; i < pixels.size(); i += 2) {
        // Stock drawSubDisplay reverses the byte order, preserving each pixel pair.
        packed[packed.size() - 1 - i / 2] = (pixels[i] & 0xf0) | (pixels[i + 1] >> 4);
    }
    return packed;
}

std::vector<uint8_t> pixelReport(uint32_t offset, const uint8_t* pixels, size_t size) {
    if (size > kPayloadSize) return {};
    std::vector<uint8_t> report(kHeaderSize + size);
    report[0] = 2;
    report[1] = size & 0xff;
    report[2] = (size >> 8) & 0xff;
    for (size_t i = 0; i < 4; ++i) report[3 + i] = (offset >> (8 * i)) & 0xff;
    for (size_t i = 0; i < size; ++i) report[kHeaderSize + i] = pixels[i];
    return report;
}

}  // namespace lge::dualscreen
