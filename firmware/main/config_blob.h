#pragma once

// The standalone config blob (BPAK, written by tools/bake): validation and
// relocation. Platform independent and pure: bytes in, a summary out. Spec:
// docs/standalone.md ("Loading", "Schema").
//
// File layout (binpack's BPAK, 32-bit pointers):
//
//     u32 signature "BPAK"  u32 version  u32 relocation count  u32 table offset
//     16 reserved bytes
//     data: the packed structs (root at offset 32), strings, rle565 images
//     relocation table: u32 offsets of the pointer slots, ascending
//
// Every pointer in the data is a file offset (0 = null). Relocating adds the
// blob's address to each slot in the table.
//
// Validate walks the whole structure by offsets (the layout comes from the
// generated structs, config_types.h) and checks everything the firmware will
// later trust: every pointer, count, string, op and image. It also requires the
// relocation table to be exactly the set of non-null pointer slots in the
// structure, so relocating cannot touch anything else. Relocate is then safe to
// run, and the relocated blob is read through the structs (ConfigRoot).

#include <stddef.h>
#include <stdint.h>
#include "config_types.h"
#include "key_layout.h"
#include "page_compositor.h"
#include "screen_layout.h"

#define BPAK_SIGNATURE      0x4B415042u     // "BPAK" as bytes 42 50 41 4B
#define BPAK_FORMAT_VERSION 1
#define BPAK_HEADER_SIZE    32
#define CONFIG_BLOB_VERSION 1               // the root's `version`; tools/bake BLOB_VERSION

// Limits (a config over them is rejected)
#define CONFIG_MAX_DEVICES      32
#define CONFIG_MAX_ACTIVITIES   32
#define CONFIG_MAX_BINDINGS     256         // per list
#define CONFIG_MAX_OPS          64          // per op list
#define CONFIG_MAX_ACTIVITY_PAGES (SCREEN_MAX_PAGES - 1)    // Home is page 0
#define CONFIG_MAX_OP_DATA      4096        // bytes of an http body or a UDP payload
#define CONFIG_MAX_SCRIPT       16384       // bytes of a script's source (MAX_SCRIPT_BYTES in tools/bake/src/consts.js)

// An image in the blob: its file offset (the image store ID) and byte size.
// offset 0 = no image.
struct ConfigImage
{
    uint32_t offset;
    uint32_t size;
};

// What the boot code needs from a validated blob
struct ConfigSummary
{
    uint32_t    version;
    uint32_t    long_press_ms;                  // 0 = firmware default
    uint32_t    device_count;
    uint32_t    activity_count;
    ConfigImage home[COMP_KEYS][KEY_SLOTS];     // KEY_SLOT_* order: bg, fg, pressed bg, pressed fg
};

enum class ConfigError
{
    Ok,
    Truncated,          // shorter than the header or the root
    BadSignature,
    BadVersion,         // BPAK format or root version
    BadHeader,          // relocation table doesn't fit exactly
    BadRelocation,      // an entry out of range, unaligned, out of order, or not a pointer slot
    BadPointer,         // a pointer, count or string outside the data
    BadAlignment,       // the blob or an image isn't 4-byte aligned
    BadLimit,           // too many devices, activities, pages, bindings or ops
    BadOp,              // unknown op code or field
    BadIndex,           // an activity or device index out of range
    BadImage,           // failed check_image, or not 128x128
};

const char *ConfigErrorName(ConfigError e);

// Check `blob` (4-byte aligned, `size` bytes) and fill `summary`. Reads the
// blob only; leaves `summary` untouched on an error.
ConfigError ConfigValidate(const void *blob, uint32_t size, ConfigSummary *summary);

// Add `base` to every pointer slot in the relocation table. Only after
// ConfigValidate() returned Ok for the same bytes, and only once. On the
// device `base` is the blob's address. After this the offsets in the data
// are absolute addresses: the summary's offsets still index the blob.
void ConfigRelocate(void *blob, uint32_t size, uint32_t base);

// The root of a blob (validated and relocated to its own address, or just
// validated if only counts and plain values are read)
inline const cfgdata::configRoot *ConfigRoot(const void *blob)
{
    return (const cfgdata::configRoot *)((const uint8_t *)blob + BPAK_HEADER_SIZE);
}
