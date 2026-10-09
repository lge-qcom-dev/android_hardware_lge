// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lge::dualscreen {

constexpr size_t kHeaderSize = 7;
constexpr size_t kPayloadSize = 4096 - kHeaderSize;

// All HID reports use little-endian fields, including the unaligned header.
uint16_t read16(const uint8_t* bytes);
bool validDimensions(int width, int height);
std::vector<uint8_t> packPixels(const std::vector<uint8_t>& pixels);
std::vector<uint8_t> pixelReport(uint32_t offset, const uint8_t* pixels, size_t size);

}  // namespace lge::dualscreen
