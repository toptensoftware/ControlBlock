#pragma once

// Platform layer for code that's shared between the firmware and the host
// unit tests (firmware/test). On the device it maps to ESP-IDF; on the host
// it's single threaded: the tests drive the "ISR" and the "task" in turn.

#include <stdint.h>

#ifdef ESP_PLATFORM

#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_cache.h"

namespace port
{
    // Spinlock between a task and an ISR (on either core)
    class SpinLock
    {
    public:
        SpinLock() { spinlock_initialize(&m_mux); }
        void Enter()    { portENTER_CRITICAL(&m_mux); }
        void Exit()     { portEXIT_CRITICAL(&m_mux); }
        void EnterISR() { portENTER_CRITICAL_ISR(&m_mux); }
        void ExitISR()  { portEXIT_CRITICAL_ISR(&m_mux); }
    private:
        portMUX_TYPE m_mux;
    };

    inline uint32_t Cycles()      { return esp_cpu_get_cycle_count(); }
    inline uint32_t CyclesPerUs() { return CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ; }

    // Write any dirty cache lines in [p, p + bytes) back to PSRAM now, so the
    // bus traffic happens here rather than when the cache evicts them later.
    inline void CacheWriteback(const void *p, size_t bytes)
    {
        esp_cache_msync((void *)p, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
}

#else   // host

#include <stddef.h>

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

namespace port
{
    class SpinLock
    {
    public:
        void Enter()    {}
        void Exit()     {}
        void EnterISR() {}
        void ExitISR()  {}
    };

    inline uint32_t Cycles()      { return 0; }
    inline uint32_t CyclesPerUs() { return 1; }
    inline void     CacheWriteback(const void *, size_t) {}
}

#endif

// Atomics on a plain uint32_t
#if defined(__GNUC__)

namespace port
{
    inline uint32_t AtomicLoad(const uint32_t *p)            { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
    // Release: everything written before is visible to whoever loads `v`
    inline void     AtomicStore(uint32_t *p, uint32_t v)     { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
    inline uint32_t AtomicExchange(uint32_t *p, uint32_t v)  { return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL); }
    inline uint32_t AtomicOr(uint32_t *p, uint32_t v)        { return __atomic_fetch_or(p, v, __ATOMIC_ACQ_REL); }
    inline uint32_t AtomicAnd(uint32_t *p, uint32_t v)       { return __atomic_fetch_and(p, v, __ATOMIC_ACQ_REL); }
    // If *p == expected, set it to desired and return true
    inline bool AtomicCompareExchange(uint32_t *p, uint32_t expected, uint32_t desired)
    {
        return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
}

#elif defined(_MSC_VER)

#include <intrin.h>

namespace port
{
    inline uint32_t AtomicLoad(const uint32_t *p)
    {
        uint32_t v = *(const volatile uint32_t *)p;
        _ReadWriteBarrier();
        return v;
    }
    inline void AtomicStore(uint32_t *p, uint32_t v)
    {
        _ReadWriteBarrier();
        *(volatile uint32_t *)p = v;
    }
    inline uint32_t AtomicExchange(uint32_t *p, uint32_t v) { return (uint32_t)_InterlockedExchange((volatile long *)p, (long)v); }
    inline uint32_t AtomicOr(uint32_t *p, uint32_t v)       { return (uint32_t)_InterlockedOr((volatile long *)p, (long)v); }
    inline uint32_t AtomicAnd(uint32_t *p, uint32_t v)      { return (uint32_t)_InterlockedAnd((volatile long *)p, (long)v); }
    inline bool AtomicCompareExchange(uint32_t *p, uint32_t expected, uint32_t desired)
    {
        return (uint32_t)_InterlockedCompareExchange((volatile long *)p, (long)desired, (long)expected) == expected;
    }
}

#else
#error "No atomics for this compiler"
#endif
