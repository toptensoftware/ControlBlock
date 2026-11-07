// The standalone layout in flash. Spec: docs/protocol.md, docs/standalone.md.

#include <string.h>
#include "layout_store.h"
#include "crc32_table.h"

namespace
{
    void put32(uint8_t *p, uint32_t v)
    {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
    }

    uint32_t get32(const uint8_t *p)
    {
        return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    }
}

void LayoutStore::Init(const LayoutFlash &flash)
{
    m_flash = flash;
    m_flash.capacity -= m_flash.capacity % LAYOUT_SECTOR;
    m_active = false;
}

bool LayoutStore::Stored(LayoutInfo *info)
{
    if (!MaxSize())
        return false;
    uint8_t h[LAYOUT_HEADER_SIZE];
    if (!m_flash.read(0, h, sizeof(h), m_flash.ctx))
        return false;
    if (get32(h) != LAYOUT_MAGIC || get32(h + 4) != LAYOUT_HEADER_VERSION)
        return false;
    if (get32(h + 16) != crc32(0, h, 16))
        return false;
    uint32_t size = get32(h + 8);
    if (size == 0 || size > MaxSize())
        return false;
    info->size = size;
    info->crc  = get32(h + 12);
    return true;
}

bool LayoutStore::Verify(const LayoutInfo &info, void *scratch, uint32_t scratch_len)
{
    if (!scratch_len || info.size > MaxSize())
        return false;
    uint32_t crc = 0;
    for (uint32_t off = 0; off < info.size; ) {
        uint32_t n = info.size - off < scratch_len ? info.size - off : scratch_len;
        if (!m_flash.read(LAYOUT_DATA_OFFSET + off, scratch, n, m_flash.ctx))
            return false;
        crc = crc32(crc, scratch, n);
        off += n;
    }
    return crc == info.crc;
}

bool LayoutStore::Read(const LayoutInfo &info, void *dst)
{
    if (info.size == 0 || info.size > MaxSize())
        return false;
    if (!m_flash.read(LAYOUT_DATA_OFFSET, dst, info.size, m_flash.ctx))
        return false;
    return crc32(0, dst, info.size) == info.crc;
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

// An error ends the upload and gives the screen back
LayoutStoreResult LayoutStore::Fail(LayoutStoreResult r)
{
    Abort();
    return r;
}

void LayoutStore::Abort()
{
    if (!m_active)
        return;
    m_active = false;
    m_flash.busy(false, m_flash.ctx);
}

LayoutStoreResult LayoutStore::Begin(uint32_t size, uint32_t crc)
{
    if (size == 0)
        return LayoutStoreResult::BadArg;
    if (size > MaxSize())
        return LayoutStoreResult::NoSpace;

    // Blank the screen before the first erase, which is what stalls it
    if (!m_active)
        m_flash.busy(true, m_flash.ctx);
    m_active   = true;
    m_size     = size;
    m_crc      = crc;
    m_received = 0;
    m_running  = 0;
    m_erased   = 0;

    // The header goes first: from here nothing is stored
    if (!m_flash.erase(0, LAYOUT_SECTOR, m_flash.ctx))
        return Fail(LayoutStoreResult::IoError);
    m_erased = LAYOUT_SECTOR;
    return LayoutStoreResult::Ok;
}

// Erase so that everything below `end` is erased: up to the next
// LAYOUT_ERASE_AHEAD boundary, so the flash driver can use block erases and
// most chunks erase nothing
bool LayoutStore::EraseTo(uint32_t end)
{
    if (end <= m_erased)
        return true;
    uint32_t to = (end + LAYOUT_ERASE_AHEAD - 1) / LAYOUT_ERASE_AHEAD * LAYOUT_ERASE_AHEAD;
    if (to > m_flash.capacity)
        to = m_flash.capacity;
    if (!m_flash.erase(m_erased, to - m_erased, m_flash.ctx))
        return false;
    m_erased = to;
    return true;
}

LayoutStoreResult LayoutStore::Data(uint32_t offset, const void *data, uint32_t len)
{
    if (!m_active || offset != m_received)
        return Fail(LayoutStoreResult::BadSequence);
    if (len > m_size - m_received)
        return Fail(LayoutStoreResult::BadArg);
    if (!len)
        return LayoutStoreResult::Ok;

    if (!EraseTo(LAYOUT_DATA_OFFSET + offset + len))
        return Fail(LayoutStoreResult::IoError);
    if (!m_flash.write(LAYOUT_DATA_OFFSET + offset, data, len, m_flash.ctx))
        return Fail(LayoutStoreResult::IoError);
    m_running   = crc32(m_running, data, len);
    m_received += len;
    return LayoutStoreResult::Ok;
}

LayoutStoreResult LayoutStore::Commit(void *scratch, uint32_t scratch_len)
{
    if (!m_active || m_received != m_size)
        return LayoutStoreResult::BadSequence;           // an early commit leaves the upload going
    if (m_running != m_crc)
        return Fail(LayoutStoreResult::BadCrc);

    // What the flash holds, not what we sent
    LayoutInfo info = { m_size, m_crc };
    if (!Verify(info, scratch, scratch_len))
        return Fail(LayoutStoreResult::IoError);

    uint8_t h[LAYOUT_HEADER_SIZE];
    put32(h, LAYOUT_MAGIC);
    put32(h + 4, LAYOUT_HEADER_VERSION);
    put32(h + 8, m_size);
    put32(h + 12, m_crc);
    put32(h + 16, crc32(0, h, 16));
    if (!m_flash.write(0, h, sizeof(h), m_flash.ctx))
        return Fail(LayoutStoreResult::IoError);

    // Done. The screen stays dark: the device restarts now.
    m_active = false;
    return LayoutStoreResult::Ok;
}
