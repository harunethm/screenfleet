// Builds a 128-byte EDID 1.4 block for a virtual "ScreenFleet" monitor.
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>

namespace sst {

using EdidBlock = std::array<uint8_t, 128>;

inline EdidBlock MakeEdid(uint32_t width, uint32_t height, uint32_t refresh, uint32_t serial) {
    EdidBlock e{};
    const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    memcpy(e.data(), header, 8);

    // Manufacturer "SST": three 5-bit letters, big-endian.
    uint16_t mfg = uint16_t((('S' - '@') << 10) | (('S' - '@') << 5) | ('T' - '@'));
    e[8] = uint8_t(mfg >> 8);
    e[9] = uint8_t(mfg);
    e[10] = 0x01;  // product code (LE)
    e[11] = 0x00;
    e[12] = uint8_t(serial);
    e[13] = uint8_t(serial >> 8);
    e[14] = uint8_t(serial >> 16);
    e[15] = uint8_t(serial >> 24);
    e[16] = 0;            // week
    e[17] = 2026 - 1990;  // year
    e[18] = 1;            // EDID 1.4
    e[19] = 4;
    e[20] = 0x80;  // digital input
    e[21] = 0;     // size unknown: Windows defaults to 100% scaling
    e[22] = 0;
    e[23] = 120;   // gamma 2.2
    e[24] = 0x06;  // sRGB default, preferred timing is the first descriptor
    const uint8_t srgb[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54};
    memcpy(&e[25], srgb, 10);
    // 35-37: no established timings. 38-53: no standard timings.
    for (int i = 38; i < 54; ++i) e[i] = 0x01;

    // Descriptor 1: detailed timing (reduced blanking).
    uint32_t hFront = 48, hSync = 32, hBack = 80;
    uint32_t hBlank = hFront + hSync + hBack;
    uint32_t vFront = 3, vSync = 6;
    uint32_t vBlank = vFront + vSync + (height / 40 + 6);
    uint64_t clock10k = (uint64_t(width + hBlank) * (height + vBlank) * refresh + 5000) / 10000;
    if (clock10k > 0xFFFF) clock10k = 0xFFFF;
    uint8_t* d = &e[54];
    d[0] = uint8_t(clock10k);
    d[1] = uint8_t(clock10k >> 8);
    d[2] = uint8_t(width);
    d[3] = uint8_t(hBlank);
    d[4] = uint8_t(((width >> 8) & 0xF) << 4 | ((hBlank >> 8) & 0xF));
    d[5] = uint8_t(height);
    d[6] = uint8_t(vBlank);
    d[7] = uint8_t(((height >> 8) & 0xF) << 4 | ((vBlank >> 8) & 0xF));
    d[8] = uint8_t(hFront);
    d[9] = uint8_t(hSync);
    d[10] = uint8_t((vFront & 0xF) << 4 | (vSync & 0xF));
    d[11] = uint8_t(((hFront >> 8) & 3) << 6 | ((hSync >> 8) & 3) << 4 | ((vFront >> 4) & 3) << 2 | ((vSync >> 4) & 3));
    d[17] = 0x1E;  // digital separate sync, +hsync +vsync

    // Descriptor 2: monitor name, 13 bytes: ended with 0x0A, padded with spaces.
    const uint8_t nameTag[5] = {0x00, 0x00, 0x00, 0xFC, 0x00};
    memcpy(&e[72], nameTag, 5);
    memcpy(&e[77], "ScreenFleet\n ", 13);

    // Descriptor 3: range limits (wide open).
    const uint8_t range[18] = {0x00, 0x00, 0x00, 0xFD, 0x00, 0x18, 0xF0, 0x01, 0xFF, 0xFF,
                               0x00, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20};
    memcpy(&e[90], range, 18);

    // Descriptor 4: dummy.
    e[111] = 0x10;

    e[126] = 0;  // no extension blocks
    uint8_t sum = 0;
    for (int i = 0; i < 127; ++i) sum = uint8_t(sum + e[i]);
    e[127] = uint8_t(256 - sum);
    return e;
}

inline uint32_t EdidSerial(const uint8_t* edid) {
    return uint32_t(edid[12]) | uint32_t(edid[13]) << 8 | uint32_t(edid[14]) << 16 | uint32_t(edid[15]) << 24;
}

}  // namespace sst
