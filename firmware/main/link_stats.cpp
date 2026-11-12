#include <stdio.h>
#include "link_stats.h"

void FormatLinkSample(const LinkSample &s, uint32_t checks, uint32_t check_cycles, uint32_t cycles_per_us,
                      char *out, size_t size)
{
    if (!size)
        return;
    out[0] = 0;
    if (!s.transfers && !s.reads && !checks)
        return;
    snprintf(out, size, " | usb rx: %lu xfers, %lu KB, %lu wakes, %lu reads, fifo %lu/%lu/%lu, receive %lu us, "
                        "%lu checks, %lu us",
             (unsigned long)s.transfers, (unsigned long)(s.bytes / 1024), (unsigned long)s.wakes,
             (unsigned long)s.reads, (unsigned long)s.fifo_min, (unsigned long)s.fifo_avg, (unsigned long)s.fifo_max,
             (unsigned long)(s.receive_cycles / cycles_per_us), (unsigned long)checks,
             (unsigned long)(check_cycles / cycles_per_us));
}
