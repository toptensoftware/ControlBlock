// The layout upload: CRC-32, the partition's header, and the upload state
// machine over a fake NOR flash (docs/protocol.md "Layout upload",
// docs/standalone.md "Layout storage")

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "layout_store.h"
#include "crc32_table.h"
#include "flash_env.h"

using namespace SimpleLib;

namespace
{
    // Deterministic bytes that don't compress into a pattern
    void Fill(uint8_t *p, uint32_t n, uint32_t seed = 1)
    {
        uint32_t x = seed * 2654435761u + 12345;
        for (uint32_t i = 0; i < n; i++) {
            x = x * 1664525u + 1013904223u;
            p[i] = (uint8_t)(x >> 24);
        }
    }

    uint32_t Get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

    struct Fixture
    {
        FakeFlash   flash;
        LayoutStore store;
        uint8_t     scratch[1024];

        explicit Fixture(uint32_t capacity = 24 * LAYOUT_SECTOR) : flash(capacity)
        {
            store.Init(flash.Host());
        }

        // The whole upload: begin, chunks, commit
        LayoutStoreResult Upload(const uint8_t *blob, uint32_t size, uint32_t chunk = 1000, uint32_t crc = 0)
        {
            if (!crc)
                crc = crc32(0, blob, size);
            LayoutStoreResult r = store.Begin(size, crc);
            if (r != LayoutStoreResult::Ok)
                return r;
            for (uint32_t off = 0; off < size; off += chunk) {
                uint32_t n = size - off < chunk ? size - off : chunk;
                r = store.Data(off, blob + off, n);
                if (r != LayoutStoreResult::Ok)
                    return r;
            }
            return store.Commit(scratch, sizeof(scratch));
        }
    };
}

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------

Fact("Crc32 Matches The Zlib Check Value")
{
    Assert(crc32(0, "123456789", 9) == 0xCBF43926u);
    Assert(crc32(0, "", 0) == 0);
    Assert(crc32(0, "a", 1) == 0xE8B7BE43u);
}

Fact("Crc32 Chains Across Pieces")
{
    uint8_t data[5000];
    Fill(data, sizeof(data));
    uint32_t whole = crc32(0, data, sizeof(data));
    uint32_t a = crc32(0, data, 1);
    a = crc32(a, data + 1, 2047);
    a = crc32(a, data + 2048, sizeof(data) - 2048);
    Assert(a == whole);
}

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

Fact("Layout Store Nothing Stored In Blank Flash")
{
    Fixture f;
    LayoutInfo info;
    Assert(!f.store.Stored(&info));
    Assert(f.store.MaxSize() == f.flash.Capacity() - LAYOUT_DATA_OFFSET);
    Assert(!f.store.Active());
}

Fact("Layout Store Upload Stores The Blob And Header")
{
    Fixture f;
    uint8_t blob[10000];
    Fill(blob, sizeof(blob));
    Assert(f.Upload(blob, sizeof(blob)) == LayoutStoreResult::Ok);

    LayoutInfo info;
    Assert(f.store.Stored(&info));
    Assert(info.size == sizeof(blob) && info.crc == crc32(0, blob, sizeof(blob)));
    Assert(memcmp(f.flash.mem + LAYOUT_DATA_OFFSET, blob, sizeof(blob)) == 0);

    // The header's layout is the spec's
    const uint8_t *h = f.flash.mem;
    Assert(h[0] == 0x43 && h[1] == 0x42 && h[2] == 0x43 && h[3] == 0x46);
    Assert(Get32(h + 4) == 1);
    Assert(Get32(h + 8) == sizeof(blob) && Get32(h + 12) == info.crc);
    Assert(Get32(h + 16) == crc32(0, h, 16));
    Assert(!f.store.Active());
}

Fact("Layout Store Header Must Be Whole To Count")
{
    Fixture f;
    uint8_t blob[3000];
    Fill(blob, sizeof(blob));
    Assert(f.Upload(blob, sizeof(blob)) == LayoutStoreResult::Ok);
    LayoutInfo info;
    Assert(f.store.Stored(&info));

    // Each damaged header is not a layout
    for (int byte = 0; byte < 20; byte++) {
        f.flash.mem[byte] ^= 0x10;
        Assert(!f.store.Stored(&info));
        f.flash.mem[byte] ^= 0x10;
        Assert(f.store.Stored(&info));
    }

    // A size that can't be: right CRC, wrong content
    uint8_t h[20];
    memcpy(h, f.flash.mem, 20);
    uint32_t big = f.store.MaxSize() + 1;
    memcpy(h + 8, &big, 4);                         // little endian host
    uint32_t c = crc32(0, h, 16);
    memcpy(h + 16, &c, 4);
    memcpy(f.flash.mem, h, 20);                     // (writing the test's own bytes)
    Assert(!f.store.Stored(&info));
}

Fact("Layout Store Reports A Read Failure As Nothing Stored")
{
    Fixture f;
    f.flash.fail_read_in = 1;
    LayoutInfo info;
    Assert(!f.store.Stored(&info));
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

Fact("Layout Store Begin Destroys The Stored Layout And Blanks The Screen")
{
    Fixture f;
    uint8_t blob[2000];
    Fill(blob, sizeof(blob));
    Assert(f.Upload(blob, sizeof(blob)) == LayoutStoreResult::Ok);
    Assert(f.flash.busy);                           // commit leaves it dark: a restart follows

    LayoutInfo info;
    uint32_t erased = f.flash.erased_bytes;
    Assert(f.store.Begin(100, 1234) == LayoutStoreResult::Ok);
    Assert(!f.store.Stored(&info));                 // gone at once
    Assert(f.store.Active() && f.flash.busy);
    // Nothing but the header sector was touched
    Assert(f.flash.erased_bytes == erased + LAYOUT_SECTOR);
}

Fact("Layout Store Bad Begin Changes Nothing")
{
    Fixture f;
    uint8_t blob[2000];
    Fill(blob, sizeof(blob));
    Assert(f.Upload(blob, sizeof(blob)) == LayoutStoreResult::Ok);
    f.flash.busy = false;
    int erases = f.flash.erase_calls, changes = f.flash.busy_changes;

    Assert(f.store.Begin(0, 1) == LayoutStoreResult::BadArg);
    Assert(f.store.Begin(f.store.MaxSize() + 1, 1) == LayoutStoreResult::NoSpace);
    LayoutInfo info;
    Assert(f.store.Stored(&info));
    Assert(f.flash.erase_calls == erases && f.flash.busy_changes == changes && !f.flash.busy);
    Assert(!f.store.Active());
}

Fact("Layout Store Fills The Partition To The Last Byte")
{
    Fixture f;
    uint32_t size = f.store.MaxSize();
    uint8_t *blob = (uint8_t *)malloc(size);
    Fill(blob, size);
    Assert(f.Upload(blob, size, 4096) == LayoutStoreResult::Ok);     // the erase-ahead is capped at the end
    LayoutInfo info;
    Assert(f.store.Stored(&info) && info.size == size);
    Assert(memcmp(f.flash.mem + LAYOUT_DATA_OFFSET, blob, size) == 0);
    Assert(f.flash.highest_erased == f.flash.Capacity());
    free(blob);
}

Fact("Layout Store Erases Ahead A Block At A Time Not The Whole Partition")
{
    Fixture f(256 * LAYOUT_SECTOR);                 // 1 MB
    uint8_t chunk[8192];
    Fill(chunk, sizeof(chunk));
    Assert(f.store.Begin(500000, 1) == LayoutStoreResult::Ok);
    Assert(f.flash.erased_bytes == LAYOUT_SECTOR);

    Assert(f.store.Data(0, chunk, sizeof(chunk)) == LayoutStoreResult::Ok);
    Assert(f.flash.highest_erased == LAYOUT_ERASE_AHEAD);   // up to the first 64 KB boundary
    Assert(f.flash.erased_bytes == LAYOUT_ERASE_AHEAD);       // the header sector plus the rest of the block

    // The next chunks are inside what's erased: no more erases
    int erases = f.flash.erase_calls;
    for (uint32_t off = sizeof(chunk); off + sizeof(chunk) + LAYOUT_DATA_OFFSET <= LAYOUT_ERASE_AHEAD; off += sizeof(chunk))
        Assert(f.store.Data(off, chunk, sizeof(chunk)) == LayoutStoreResult::Ok);
    Assert(f.flash.erase_calls == erases);

    // The chunk that crosses the boundary erases the next block
    Assert(f.store.Data(7 * sizeof(chunk), chunk, sizeof(chunk)) == LayoutStoreResult::Ok);
    Assert(f.flash.erase_calls == erases + 1);
    Assert(f.flash.highest_erased == 2 * LAYOUT_ERASE_AHEAD);
    f.store.Abort();
}

Fact("Layout Store Erases Only What The Data Needs")
{
    Fixture f(256 * LAYOUT_SECTOR);
    uint8_t chunk[1000];
    Fill(chunk, sizeof(chunk));
    uint32_t size = 100000;
    Assert(f.store.Begin(size, 1) == LayoutStoreResult::Ok);
    for (uint32_t off = 0; off < size; off += sizeof(chunk))
        Assert(f.store.Data(off, chunk, size - off < sizeof(chunk) ? size - off : sizeof(chunk)) == LayoutStoreResult::Ok);
    // Everything written is erased, and not much more: to the 64 KB boundary past the end
    uint32_t end = LAYOUT_DATA_OFFSET + size;
    Assert(f.flash.highest_erased >= end);
    Assert(f.flash.highest_erased == (end + LAYOUT_ERASE_AHEAD - 1) / LAYOUT_ERASE_AHEAD * LAYOUT_ERASE_AHEAD);
}

Fact("Layout Store Out Of Order Chunk Ends The Upload")
{
    Fixture f;
    uint8_t blob[4000];
    Fill(blob, sizeof(blob));
    uint32_t crc = crc32(0, blob, sizeof(blob));
    Assert(f.store.Begin(sizeof(blob), crc) == LayoutStoreResult::Ok);
    Assert(f.store.Data(0, blob, 1000) == LayoutStoreResult::Ok);
    Assert(f.store.Data(2000, blob + 2000, 1000) == LayoutStoreResult::BadSequence);     // a gap
    Assert(!f.store.Active() && !f.flash.busy);
    // The rest is refused until a new begin
    Assert(f.store.Data(1000, blob + 1000, 1000) == LayoutStoreResult::BadSequence);
    Assert(f.store.Commit(f.scratch, sizeof(f.scratch)) == LayoutStoreResult::BadSequence);
    LayoutInfo info;
    Assert(!f.store.Stored(&info));
}

Fact("Layout Store Repeated Chunk Is Out Of Order Too")
{
    Fixture f;
    uint8_t blob[2000];
    Fill(blob, sizeof(blob));
    Assert(f.store.Begin(sizeof(blob), 1) == LayoutStoreResult::Ok);
    Assert(f.store.Data(0, blob, 1000) == LayoutStoreResult::Ok);
    Assert(f.store.Data(0, blob, 1000) == LayoutStoreResult::BadSequence);
}

Fact("Layout Store Chunk Past The End Ends The Upload")
{
    Fixture f;
    uint8_t blob[2000];
    Fill(blob, sizeof(blob));
    Assert(f.store.Begin(1500, 1) == LayoutStoreResult::Ok);
    Assert(f.store.Data(0, blob, 1000) == LayoutStoreResult::Ok);
    Assert(f.store.Data(1000, blob + 1000, 501) == LayoutStoreResult::BadArg);
    Assert(!f.store.Active() && !f.flash.busy);
}

Fact("Layout Store Data Or Commit Without A Begin")
{
    Fixture f;
    uint8_t b[10] = {};
    Assert(f.store.Data(0, b, sizeof(b)) == LayoutStoreResult::BadSequence);
    Assert(f.store.Commit(f.scratch, sizeof(f.scratch)) == LayoutStoreResult::BadSequence);
    Assert(f.flash.write_calls == 0 && f.flash.erase_calls == 0 && f.flash.busy_changes == 0);
}

Fact("Layout Store Early Commit Lets The Upload Carry On")
{
    Fixture f;
    uint8_t blob[3000];
    Fill(blob, sizeof(blob));
    Assert(f.store.Begin(sizeof(blob), crc32(0, blob, sizeof(blob))) == LayoutStoreResult::Ok);
    Assert(f.store.Data(0, blob, 1000) == LayoutStoreResult::Ok);
    Assert(f.store.Commit(f.scratch, sizeof(f.scratch)) == LayoutStoreResult::BadSequence);
    Assert(f.store.Active() && f.flash.busy);
    Assert(f.store.Data(1000, blob + 1000, 2000) == LayoutStoreResult::Ok);
    Assert(f.store.Commit(f.scratch, sizeof(f.scratch)) == LayoutStoreResult::Ok);
    LayoutInfo info;
    Assert(f.store.Stored(&info) && info.size == sizeof(blob));
}

Fact("Layout Store Wrong Crc Is Refused And Nothing Is Stored")
{
    Fixture f;
    uint8_t blob[3000];
    Fill(blob, sizeof(blob));
    Assert(f.Upload(blob, sizeof(blob), 1000, crc32(0, blob, sizeof(blob)) ^ 1) == LayoutStoreResult::BadCrc);
    LayoutInfo info;
    Assert(!f.store.Stored(&info));
    Assert(!f.store.Active() && !f.flash.busy);
}

Fact("Layout Store Catches A Bad Write By Reading Back")
{
    Fixture f;
    uint8_t blob[3000];
    Fill(blob, sizeof(blob));
    Assert(f.store.Begin(sizeof(blob), crc32(0, blob, sizeof(blob))) == LayoutStoreResult::Ok);
    Assert(f.store.Data(0, blob, 1000) == LayoutStoreResult::Ok);
    f.flash.corrupt_next_write = true;                  // the flash says OK but stores a flipped bit
    Assert(f.store.Data(1000, blob + 1000, 2000) == LayoutStoreResult::Ok);
    Assert(f.store.Commit(f.scratch, sizeof(f.scratch)) == LayoutStoreResult::IoError);
    LayoutInfo info;
    Assert(!f.store.Stored(&info));
    Assert(!f.store.Active() && !f.flash.busy);
}

Fact("Layout Store Flash Errors End The Upload")
{
    uint8_t blob[3000];
    Fill(blob, sizeof(blob));
    uint32_t crc = crc32(0, blob, sizeof(blob));

    {   // The header can't be erased: nothing started
        Fixture f;
        f.flash.fail_erase_in = 1;
        Assert(f.store.Begin(sizeof(blob), crc) == LayoutStoreResult::IoError);
        Assert(!f.store.Active() && !f.flash.busy);
    }
    {   // The erase ahead fails
        Fixture f;
        Assert(f.store.Begin(sizeof(blob), crc) == LayoutStoreResult::Ok);
        f.flash.fail_erase_in = 1;
        Assert(f.store.Data(0, blob, 1000) == LayoutStoreResult::IoError);
        Assert(!f.store.Active() && !f.flash.busy);
        Assert(f.store.Data(0, blob, 1000) == LayoutStoreResult::BadSequence);
    }
    {   // A data write fails
        Fixture f;
        Assert(f.store.Begin(sizeof(blob), crc) == LayoutStoreResult::Ok);
        f.flash.fail_write_in = 1;
        Assert(f.store.Data(0, blob, 1000) == LayoutStoreResult::IoError);
        Assert(!f.store.Active() && !f.flash.busy);
    }
    {   // The header write fails at commit
        Fixture f;
        Assert(f.store.Begin(sizeof(blob), crc) == LayoutStoreResult::Ok);
        Assert(f.store.Data(0, blob, sizeof(blob)) == LayoutStoreResult::Ok);
        f.flash.fail_write_in = 1;
        Assert(f.store.Commit(f.scratch, sizeof(f.scratch)) == LayoutStoreResult::IoError);
        LayoutInfo info;
        Assert(!f.store.Stored(&info) && !f.flash.busy);
    }
    {   // The read-back fails at commit
        Fixture f;
        Assert(f.store.Begin(sizeof(blob), crc) == LayoutStoreResult::Ok);
        Assert(f.store.Data(0, blob, sizeof(blob)) == LayoutStoreResult::Ok);
        f.flash.fail_read_in = 2;
        Assert(f.store.Commit(f.scratch, sizeof(f.scratch)) == LayoutStoreResult::IoError);
        LayoutInfo info;
        Assert(!f.store.Stored(&info) && !f.flash.busy);
    }
}

Fact("Layout Store Abort Drops The Upload And Is Harmless When Idle")
{
    Fixture f;
    f.store.Abort();
    Assert(f.flash.busy_changes == 0);

    uint8_t blob[100] = {};
    Assert(f.store.Begin(sizeof(blob), 1) == LayoutStoreResult::Ok);
    Assert(f.store.Data(0, blob, 50) == LayoutStoreResult::Ok);
    f.store.Abort();
    Assert(!f.store.Active() && !f.flash.busy);
    Assert(f.store.Data(50, blob, 50) == LayoutStoreResult::BadSequence);
    LayoutInfo info;
    Assert(!f.store.Stored(&info));
}

Fact("Layout Store A New Begin Restarts The Upload")
{
    Fixture f;
    uint8_t a[3000], b[2000];
    Fill(a, sizeof(a), 1);
    Fill(b, sizeof(b), 2);
    Assert(f.store.Begin(sizeof(a), crc32(0, a, sizeof(a))) == LayoutStoreResult::Ok);
    Assert(f.store.Data(0, a, 1500) == LayoutStoreResult::Ok);
    // The first upload is abandoned; the second starts from offset 0 on clean flash
    Assert(f.Upload(b, sizeof(b)) == LayoutStoreResult::Ok);
    LayoutInfo info;
    Assert(f.store.Stored(&info) && info.size == sizeof(b) && info.crc == crc32(0, b, sizeof(b)));
    Assert(memcmp(f.flash.mem + LAYOUT_DATA_OFFSET, b, sizeof(b)) == 0);
}

Fact("Layout Store Replaces A Stored Layout")
{
    Fixture f;
    uint8_t a[5000], b[2500];
    Fill(a, sizeof(a), 1);
    Fill(b, sizeof(b), 2);
    Assert(f.Upload(a, sizeof(a)) == LayoutStoreResult::Ok);
    Assert(f.Upload(b, sizeof(b)) == LayoutStoreResult::Ok);
    LayoutInfo info;
    Assert(f.store.Stored(&info) && info.size == sizeof(b));
    Assert(memcmp(f.flash.mem + LAYOUT_DATA_OFFSET, b, sizeof(b)) == 0);
}

Fact("Layout Store Verify Rereads The Flash")
{
    Fixture f;
    uint8_t blob[5000];
    Fill(blob, sizeof(blob));
    Assert(f.Upload(blob, sizeof(blob)) == LayoutStoreResult::Ok);
    LayoutInfo info;
    Assert(f.store.Stored(&info));
    Assert(f.store.Verify(info, f.scratch, sizeof(f.scratch)));
    // A scratch size that doesn't divide the blob
    Assert(f.store.Verify(info, f.scratch, 777));

    f.flash.mem[LAYOUT_DATA_OFFSET + 4321] ^= 0x80;
    Assert(!f.store.Verify(info, f.scratch, sizeof(f.scratch)));
    f.flash.mem[LAYOUT_DATA_OFFSET + 4321] ^= 0x80;
    f.flash.fail_read_in = 3;
    Assert(!f.store.Verify(info, f.scratch, sizeof(f.scratch)));
}

Fact("Layout Store Uploads Whatever The Chunk Size")
{
    uint8_t blob[7777];
    Fill(blob, sizeof(blob));
    static const uint32_t chunks[] = { 1, 7, 4095, 4096, 4097, 8188, 7777 };
    for (uint32_t chunk : chunks) {
        Fixture f;
        Assert(f.Upload(blob, sizeof(blob), chunk) == LayoutStoreResult::Ok);
        LayoutInfo info;
        Assert(f.store.Stored(&info));
        Assert(memcmp(f.flash.mem + LAYOUT_DATA_OFFSET, blob, sizeof(blob)) == 0);
    }
}

Fact("Layout Store Without A Partition Has No Room")
{
    Fixture f(0);                                   // an old partition table: no layout partition
    Assert(f.store.MaxSize() == 0);
    LayoutInfo info;
    Assert(!f.store.Stored(&info));                 // doesn't touch the flash
    Assert(f.store.Begin(100, 1) == LayoutStoreResult::NoSpace);
    Assert(!f.store.Active() && f.flash.busy_changes == 0);

    Fixture g(LAYOUT_SECTOR);                       // just a header sector: still no room for data
    Assert(g.store.MaxSize() == 0);
    Assert(g.store.Begin(1, 1) == LayoutStoreResult::NoSpace);
}

Fact("Layout Store Read Loads The Blob And Checks Its Crc")
{
    Fixture f;
    uint8_t blob[5000], out[5000];
    Fill(blob, sizeof(blob));
    Assert(f.Upload(blob, sizeof(blob)) == LayoutStoreResult::Ok);
    LayoutInfo info;
    Assert(f.store.Stored(&info));

    Assert(f.store.Read(info, out));
    Assert(memcmp(out, blob, sizeof(blob)) == 0);

    // Damaged flash, a failing read, and a header from elsewhere all fail
    f.flash.mem[LAYOUT_DATA_OFFSET + 4321] ^= 0x80;
    Assert(!f.store.Read(info, out));
    f.flash.mem[LAYOUT_DATA_OFFSET + 4321] ^= 0x80;
    f.flash.fail_read_in = 1;
    Assert(!f.store.Read(info, out));
    LayoutInfo too_big = { f.store.MaxSize() + 1, info.crc };
    Assert(!f.store.Read(too_big, out));
    LayoutInfo none = { 0, 0 };
    Assert(!f.store.Read(none, out));
    Assert(f.store.Read(info, out));
}
