#pragma once

// The standalone config in flash, and its upload (docs/protocol.md
// "Configuration upload"; partition layout in docs/standalone.md). Platform independent:
// the device supplies the flash partition through ConfigFlash.
//
// Partition layout: sector 0 is a header; the blob starts at
// CONFIG_DATA_OFFSET (4-aligned, so it could be flash-mapped).
//
//     u32 magic  u32 version  u32 blob size  u32 blob CRC-32  u32 CRC-32 of the first 16 bytes
//
// The header is written last. Begin() erases it, so an interrupted upload
// leaves no valid header and nothing counts as stored.
//
// Not thread safe: the protocol task calls everything.

#include <stddef.h>
#include <stdint.h>

#define CONFIG_SECTOR        4096           // flash erase unit
#define CONFIG_DATA_OFFSET   4096           // where the blob starts
#define CONFIG_ERASE_AHEAD   65536          // data is erased in blocks up to this boundary
#define CONFIG_MAGIC         0x46434243u    // "CBCF" as bytes 43 42 43 46
#define CONFIG_HEADER_VERSION 1
#define CONFIG_HEADER_SIZE   20

// What the config store needs from the device
struct ConfigFlash
{
    // The partition's size in bytes (a multiple of CONFIG_SECTOR)
    uint32_t capacity;
    // Erase [offset, offset + len): sector aligned. True on success.
    bool (*erase)(uint32_t offset, uint32_t len, void *ctx);
    // Write to erased bytes. True on success.
    bool (*write)(uint32_t offset, const void *data, uint32_t len, void *ctx);
    bool (*read)(uint32_t offset, void *data, uint32_t len, void *ctx);
    // An upload started (true) or ended (false): flash work stalls the
    // display, so the device blanks it. A successful commit leaves it
    // blanked, because a restart follows.
    void (*busy)(bool busy, void *ctx);
    void *ctx;
};

struct ConfigInfo
{
    uint32_t size, crc;
};

enum class ConfigResult { Ok, BadArg, NoSpace, BadSequence, BadCrc, IoError };

class ConfigStore
{
public:
    void Init(const ConfigFlash &flash);

    // The largest blob that fits
    // (0 with no partition, or one too small to hold any data)
    uint32_t MaxSize() const { return m_flash.capacity > CONFIG_DATA_OFFSET ? m_flash.capacity - CONFIG_DATA_OFFSET : 0; }

    // Is there a whole header? (The blob's CRC is not re-checked: see Verify.)
    bool Stored(ConfigInfo *info);

    // Read the blob back and compare its CRC and size. `scratch` is any
    // buffer; bigger is faster.
    bool Verify(const ConfigInfo &info, void *scratch, uint32_t scratch_len);

    // Read the stored blob (`info` from Stored) into `dst`, `info.size` bytes,
    // and check its CRC. False if the read fails or the CRC doesn't match.
    // One flash read, so `dst` can be any memory (PSRAM at boot).
    bool Read(const ConfigInfo &info, void *dst);

    // --- Upload ---
    // Begin destroys the stored config (and abandons an upload in progress).
    // BadArg for a size of 0, NoSpace for one over MaxSize (nothing
    // changes), IoError if the flash fails.
    ConfigResult Begin(uint32_t size, uint32_t crc);
    // The next chunk, in order. Any error ends the upload: later calls get
    // BadSequence until Begin.
    ConfigResult Data(uint32_t offset, const void *data, uint32_t len);
    // Check, read back and write the header. BadSequence (and the upload
    // carries on) if not all of it has arrived; any other error ends it.
    ConfigResult Commit(void *scratch, uint32_t scratch_len);
    // Drop an upload (RESET_STREAM); nothing is stored
    void Abort();
    bool Active() const { return m_active; }

private:
    ConfigResult Fail(ConfigResult r);
    bool EraseTo(uint32_t end);

    ConfigFlash m_flash = {};
    bool        m_active = false;
    uint32_t    m_size = 0, m_crc = 0;         // the upload's: given at Begin
    uint32_t    m_received = 0, m_running = 0; // bytes so far and their CRC
    uint32_t    m_erased = 0;                  // flash is erased up to here (partition offset)
};
