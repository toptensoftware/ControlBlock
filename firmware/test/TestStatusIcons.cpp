#include "UnitTesting.h"
#include "Core.h"
#include "status_icons.h"

using namespace SimpleLib;

// Which status bar icons show (Wi-Fi, busy, error), with an injected clock

Fact("StatusIcons Wifi Connected Or Not")
{
    StatusIcons s;
    Assert(s.Visible(0) == STATUS_ICON_WIFI_OFF);           // before any state: not connected
    s.SetWifi(WifiState::Connected);
    Assert(s.Visible(0) == STATUS_ICON_WIFI_CONNECTED);
    WifiState others[] = { WifiState::NoCredentials, WifiState::Connecting, WifiState::Retrying };
    for (WifiState w : others) {
        s.SetWifi(w);
        Assert(s.Visible(0) == STATUS_ICON_WIFI_OFF);
    }
}

Fact("StatusIcons Busy Shows At Once And Stays The Minimum")
{
    StatusIcons s;
    s.SetBusy(true, 1000);
    Assert(s.Visible(1000) & STATUS_ICON_BUSY);
    s.SetBusy(false, 1040);                                 // a quick HID tap
    Assert(s.Visible(1040) & STATUS_ICON_BUSY);
    Assert(s.Visible(1000 + STATUS_BUSY_MIN_MS - 1) & STATUS_ICON_BUSY);
    Assert(!(s.Visible(1000 + STATUS_BUSY_MIN_MS) & STATUS_ICON_BUSY));
}

Fact("StatusIcons Busy Longer Than The Minimum Ends With The Queue")
{
    StatusIcons s;
    s.SetBusy(true, 0);
    Assert(s.Visible(5000) & STATUS_ICON_BUSY);
    Assert(s.NextDeadlineMs(5000) == STATUS_NO_DEADLINE);   // nothing timed while busy
    s.SetBusy(false, 5000);
    Assert(!(s.Visible(5000) & STATUS_ICON_BUSY));
}

// A fire-and-forget request outlives its op: busy stays while it's pending
Fact("StatusIcons Busy While Requests Are Pending")
{
    StatusIcons s;
    s.SetBusy(true, 1000);
    s.SetNetBusy(true, 1010);
    s.SetBusy(false, 1020);                                 // the op handed it over and ended
    Assert(s.Visible(1000 + STATUS_BUSY_MIN_MS + 3000) & STATUS_ICON_BUSY);
    Assert(s.NextDeadlineMs(1000 + STATUS_BUSY_MIN_MS + 3000) == STATUS_NO_DEADLINE);
    s.SetNetBusy(false, 6000);
    Assert(!(s.Visible(6000) & STATUS_ICON_BUSY));
}

Fact("StatusIcons Pending Requests Alone Show Busy, With The Minimum")
{
    StatusIcons s;
    s.SetNetBusy(true, 0);
    Assert(s.Visible(0) & STATUS_ICON_BUSY);
    s.SetNetBusy(false, 50);
    Assert(s.Visible(STATUS_BUSY_MIN_MS - 1) & STATUS_ICON_BUSY);
    Assert(!(s.Visible(STATUS_BUSY_MIN_MS) & STATUS_ICON_BUSY));

    // The queue ending first doesn't end it
    s.SetNetBusy(true, 5000);
    s.SetBusy(true, 5000);
    s.SetBusy(false, 6000);
    Assert(s.Visible(7000) & STATUS_ICON_BUSY);
}

Fact("StatusIcons Pending Requests Are Ignored While Hosted")
{
    StatusIcons s;
    s.SetHosted(true);
    s.SetNetBusy(true, 0);
    Assert(!(s.Visible(0) & STATUS_ICON_BUSY));
    s.SetNetBusy(true, 0);
    s.SetHosted(false);                                     // release clears it too
    Assert(!(s.Visible(1000) & STATUS_ICON_BUSY));
}

Fact("StatusIcons A New Start During The Minimum Extends It")
{
    StatusIcons s;
    s.SetBusy(true, 0);
    s.SetBusy(false, 100);
    s.SetBusy(true, 300);
    s.SetBusy(false, 350);
    Assert(s.Visible(600) & STATUS_ICON_BUSY);              // past the first minimum
    Assert(s.NextDeadlineMs(600) == 300 + STATUS_BUSY_MIN_MS - 600);
    Assert(!(s.Visible(300 + STATUS_BUSY_MIN_MS) & STATUS_ICON_BUSY));
}

Fact("StatusIcons Error Shows For Five Seconds")
{
    StatusIcons s;
    s.ReportError(2000);
    Assert(s.Visible(2000) & STATUS_ICON_ERROR);
    Assert(s.NextDeadlineMs(2000) == STATUS_ERROR_SHOW_MS);
    Assert(s.Visible(2000 + STATUS_ERROR_SHOW_MS - 1) & STATUS_ICON_ERROR);
    Assert(!(s.Visible(2000 + STATUS_ERROR_SHOW_MS) & STATUS_ICON_ERROR));
    Assert(s.NextDeadlineMs(2000 + STATUS_ERROR_SHOW_MS) == STATUS_NO_DEADLINE);
}

// Busy and error share one place: the error wins while it shows
Fact("StatusIcons Error Overrides Busy")
{
    StatusIcons s;
    s.SetBusy(true, 1000);
    s.ReportError(2000);
    uint8_t v = s.Visible(2000);
    Assert((v & STATUS_ICON_ERROR) && !(v & STATUS_ICON_BUSY));
    v = s.Visible(2000 + STATUS_ERROR_SHOW_MS);              // error over, queue still busy
    Assert((v & STATUS_ICON_BUSY) && !(v & STATUS_ICON_ERROR));
}

Fact("StatusIcons Another Error Restarts The Five Seconds")
{
    StatusIcons s;
    s.ReportError(0);
    s.ReportError(4000);
    Assert(s.Visible(6000) & STATUS_ICON_ERROR);
    Assert(s.NextDeadlineMs(6000) == 4000 + STATUS_ERROR_SHOW_MS - 6000);
    Assert(!(s.Visible(4000 + STATUS_ERROR_SHOW_MS) & STATUS_ICON_ERROR));
}

Fact("StatusIcons Next Deadline Is The Nearest")
{
    StatusIcons s;
    Assert(s.NextDeadlineMs(0) == STATUS_NO_DEADLINE);
    s.ReportError(0);
    s.SetBusy(true, 100);
    s.SetBusy(false, 200);
    Assert(s.NextDeadlineMs(200) == 100 + STATUS_BUSY_MIN_MS - 200);
    Assert(s.NextDeadlineMs(100 + STATUS_BUSY_MIN_MS) == STATUS_ERROR_SHOW_MS - 100 - STATUS_BUSY_MIN_MS);
}

Fact("StatusIcons Hosted Shows Only Wifi")
{
    StatusIcons s;
    s.SetWifi(WifiState::Connected);
    s.SetBusy(true, 0);
    s.ReportError(0);
    s.SetHosted(true);
    Assert(s.Visible(0) == STATUS_ICON_WIFI_CONNECTED);
    Assert(s.NextDeadlineMs(0) == STATUS_NO_DEADLINE);

    // Ignored while hosted
    s.SetBusy(true, 10);
    s.ReportError(10);
    Assert(s.Visible(10) == STATUS_ICON_WIFI_CONNECTED);

    // Cleared, not resumed, on release
    s.SetHosted(false);
    Assert(s.Visible(20) == STATUS_ICON_WIFI_CONNECTED);
    s.SetBusy(true, 30);
    Assert(s.Visible(30) & STATUS_ICON_BUSY);
}

Fact("StatusIcons Wait Is The Nearer Of Clock And Icons")
{
    Assert(status_wait_ms(STATUS_NO_DEADLINE, STATUS_NO_DEADLINE) == STATUS_NO_DEADLINE);
    Assert(status_wait_ms(60000, STATUS_NO_DEADLINE) == 60000);
    Assert(status_wait_ms(STATUS_NO_DEADLINE, 500) == 500);
    Assert(status_wait_ms(60000, 500) == 500);
    Assert(status_wait_ms(300, 500) == 300);
    Assert(status_wait_ms(0, 500) == 0);
}
