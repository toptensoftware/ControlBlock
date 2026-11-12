#pragma once

// Test environment for the layout blob: the sample blob baked by host/src/bake
// (data/sample_layout.json -> data/sample_layout.bin), helpers to read and
// rebuild it, and a buffer that faults on any read past its end.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Core.h"
#include "layout_blob.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// File offsets in the sample, from the generated structs (layout_types.h)
#define CB_OFF(type, field) ((uint32_t)offsetof(layoutdata::type, field))      // within a struct
#define CB_ROOT             BPAK_HEADER_SIZE
#define CB_ROOT_LONGPRESS   (CB_ROOT + CB_OFF(layoutRoot, longPressMs))
#define CB_HOME_KEYS        (CB_ROOT + CB_OFF(layoutRoot, homePage) + CB_OFF(home, keys_count))       // count
#define CB_HOME_KEYS_PTR    (CB_ROOT + CB_OFF(layoutRoot, homePage) + CB_OFF(home, keys))
#define CB_KEY_COUNT(k, s)  ((k) + LayoutKeyImageOffset(s))             // image `s`'s (count, ptr) pair (LAYOUT_KEY_*)
#define CB_KEY_PTR(k, s)    ((k) + LayoutKeyImagePtrOffset(s))
#define CB_KEY_ID(k)        ((k) + CB_OFF(key, id))
#define CB_KEY_RECT(k)      ((k) + CB_OFF(key, rect))
#define CB_HOME_BINDINGS    (CB_ROOT + CB_OFF(layoutRoot, homePage) + CB_OFF(home, bindings_count))     // count
#define CB_HOME_BINDINGS_PTR (CB_ROOT + CB_OFF(layoutRoot, homePage) + CB_OFF(home, bindings))
#define CB_ROOT_DEVICES     (CB_ROOT + CB_OFF(layoutRoot, devices_count))
#define CB_ROOT_DEVICES_PTR (CB_ROOT + CB_OFF(layoutRoot, devices))
#define CB_ROOT_ACTIVITIES  (CB_ROOT + CB_OFF(layoutRoot, activities_count))
#define CB_ROOT_ACTIVITIES_PTR (CB_ROOT + CB_OFF(layoutRoot, activities))

namespace layout_env
{
    inline uint32_t U32(const SimpleLib::List<uint8_t> &b, uint32_t off)
    {
        uint32_t v;
        memcpy(&v, b.GetBuffer() + off, 4);
        return v;
    }
    inline void Put32(SimpleLib::List<uint8_t> &b, uint32_t off, uint32_t v)
    {
        memcpy(b.GetBuffer() + off, &v, 4);
    }

    // data/sample_layout.bin (run from firmware/test or the repository root)
    inline SimpleLib::List<uint8_t> LoadSample()
    {
        static const char *dirs[] = { "data/", "firmware/test/data/", "../test/data/" };
        FILE *f = nullptr;
        char path[256];
        for (const char *d : dirs) {
            snprintf(path, sizeof(path), "%ssample_layout.bin", d);
            if ((f = fopen(path, "rb")) != nullptr)
                break;
        }
        if (!f) {
            fprintf(stderr, "Can't find firmware/test/data/sample_layout.bin (run from firmware/test)\n");
            exit(2);
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        SimpleLib::List<uint8_t> bytes;
        bytes.SetCount((int)size, 0);
        if (fread(bytes.GetBuffer(), 1, size, f) != (size_t)size)
            exit(2);
        fclose(f);
        return bytes;
    }

    // Relocate `b` in place to its own address, as the device does after validating:
    // the pointers in it are real (32-bit) addresses. Once per blob.
    inline const layoutdata::layoutRoot *Relocate(SimpleLib::List<uint8_t> &b)
    {
        LayoutRelocate(b.GetBuffer(), (uint32_t)b.GetCount(), (uint32_t)(uintptr_t)b.GetBuffer());
        return LayoutRoot(b.GetBuffer());
    }

    // File offset of Home's key `i` in a blob as baked (not relocated)
    inline uint32_t HomeKey(const SimpleLib::List<uint8_t> &b, uint32_t i)
    {
        return U32(b, CB_HOME_KEYS_PTR) + i * (uint32_t)sizeof(layoutdata::key);
    }

    inline uint32_t TableOffset(const SimpleLib::List<uint8_t> &b) { return U32(b, 12); }
    inline uint32_t RelocCount(const SimpleLib::List<uint8_t> &b)  { return U32(b, 8); }
    inline uint32_t Reloc(const SimpleLib::List<uint8_t> &b, uint32_t i) { return U32(b, TableOffset(b) + i * 4); }

    // The same data with a different relocation table
    inline SimpleLib::List<uint8_t> WithRelocs(const SimpleLib::List<uint8_t> &src, const SimpleLib::List<uint32_t> &relocs)
    {
        uint32_t table = TableOffset(src);
        SimpleLib::List<uint8_t> out;
        out.SetCount((int)(table + relocs.GetCount() * 4), 0);
        memcpy(out.GetBuffer(), src.GetBuffer(), table);
        Put32(out, 8, (uint32_t)relocs.GetCount());
        for (int i = 0; i < relocs.GetCount(); i++)
            Put32(out, table + i * 4, relocs[i]);
        return out;
    }

    inline SimpleLib::List<uint32_t> Relocs(const SimpleLib::List<uint8_t> &b)
    {
        SimpleLib::List<uint32_t> r;
        for (uint32_t i = 0; i < RelocCount(b); i++)
            r.Add(Reloc(b, i));
        return r;
    }

    // A copy of `bytes` placed so its last byte is the last accessible byte
    // before an inaccessible page: any read past the end faults. (Elsewhere
    // an ordinary 4-byte aligned allocation.)
    class Guarded
    {
    public:
        explicit Guarded(const SimpleLib::List<uint8_t> &bytes) { Set(bytes.GetBuffer(), (uint32_t)bytes.GetCount()); }
        Guarded(const void *data, uint32_t size) { Set(data, size); }
        ~Guarded()
        {
#ifdef _WIN32
            if (m_base) VirtualFree(m_base, 0, MEM_RELEASE);
#else
            free(m_base);
#endif
        }
        Guarded(const Guarded &) = delete;
        Guarded &operator=(const Guarded &) = delete;

        uint8_t *Data() { return m_data; }
        uint32_t Size() const { return m_size; }

    private:
        void Set(const void *data, uint32_t size)
        {
            m_size = size;
#ifdef _WIN32
            // The data ends at the guard page, 4-aligned
            size_t page = 4096;
            size_t pages = (size + page - 1) / page;
            m_base = (uint8_t *)VirtualAlloc(nullptr, (pages + 1) * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            DWORD old;
            VirtualProtect(m_base + pages * page, page, PAGE_NOACCESS, &old);
            size_t start = (pages * page - size) & ~(size_t)3;
            m_data = m_base + start;
#else
            m_base = (uint8_t *)malloc(size + 4);
            m_data = m_base;
#endif
            memcpy(m_data, data, size);
        }

        uint8_t *m_base = nullptr;
        uint8_t *m_data = nullptr;
        uint32_t m_size = 0;
    };
}
