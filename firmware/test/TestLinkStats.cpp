#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "link_stats.h"

using namespace SimpleLib;

// The host link's receive counters for the stats line (wip/wip-upload-throughput.md)

Fact("LinkStats Take Sums Counts And Resets")
{
    LinkCounters c;
    c.Transfer(512);
    c.Transfer(100);
    c.Wake();
    c.Wake();
    c.Wake();
    c.ReceiveCycles(1000);
    c.ReceiveCycles(500);

    LinkSample s = c.Take();
    Assert(s.transfers == 2 && s.bytes == 612);
    Assert(s.wakes == 3);
    Assert(s.receive_cycles == 1500);

    s = c.Take();
    Assert(s.transfers == 0 && s.bytes == 0 && s.wakes == 0 && s.receive_cycles == 0);
}

Fact("LinkStats Fifo Levels Give Min Avg And Max Per Take")
{
    LinkCounters c;
    LinkSample s = c.Take();
    Assert(s.reads == 0 && s.fifo_min == 0 && s.fifo_max == 0 && s.fifo_avg == 0);

    c.Read(512);
    c.Read(4096);
    c.Read(64);
    s = c.Take();
    Assert(s.reads == 3);
    Assert(s.fifo_min == 64 && s.fifo_max == 4096);
    Assert(s.fifo_avg == (512 + 4096 + 64) / 3);

    c.Read(1000);
    s = c.Take();
    Assert(s.fifo_min == 1000 && s.fifo_max == 1000 && s.fifo_avg == 1000);
}

Fact("LinkStats Format Shows Kilobytes And Microseconds")
{
    LinkSample s = {};
    s.transfers = 500;
    s.bytes = 256000;
    s.wakes = 250;
    s.reads = 400;
    s.fifo_min = 0;
    s.fifo_avg = 600;
    s.fifo_max = 4096;
    s.receive_cycles = 2400000;     // 10 ms at 240 MHz
    char text[160];
    FormatLinkSample(s, 30, 48000000, 240, text, sizeof(text));
    Assert(strcmp(text, " | usb rx: 500 xfers, 250 KB, 250 wakes, 400 reads, fifo 0/600/4096, receive 10000 us, "
                        "30 checks, 200000 us") == 0);
}

Fact("LinkStats Format Is Empty For An Idle Second")
{
    LinkSample s = {};
    char text[160] = "junk";
    FormatLinkSample(s, 0, 0, 240, text, sizeof(text));
    Assert(text[0] == 0);
}

Fact("LinkStats Format Truncates To Fit")
{
    LinkSample s = {};
    s.transfers = 1;
    s.bytes = 512;
    char text[12];
    FormatLinkSample(s, 0, 0, 240, text, sizeof(text));
    Assert(strlen(text) == sizeof(text) - 1);
}
