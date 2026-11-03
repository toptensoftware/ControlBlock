// Wi-Fi connection state machine and backoff (docs/wifi.md): events in,
// driver actions and state changes out

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "wifi_state.h"

using namespace SimpleLib;

namespace
{
    const uint8_t IP[4] = { 192, 168, 1, 50 };

    // A machine brought to the first attempt
    struct Fixture
    {
        WifiMachine m;

        WifiAction Connect() { return m.OnCredentials(true); }

        // Connect, then fail `n` attempts; returns the last action
        WifiAction FailAttempts(int n, uint8_t reason = 201)
        {
            WifiAction a = Connect();
            for (int i = 0; i < n; i++) {
                a = m.OnDisconnected(reason);
                if (i + 1 < n)
                    m.OnRetryTimer();
            }
            return a;
        }
    };

    bool Quiet(const WifiAction &a)
    {
        return !a.start && !a.stop && !a.disconnect && !a.connect && !a.cancel_retry && !a.changed && a.retry_ms < 0;
    }
}

Fact("Wifi State Starts With No Credentials")
{
    WifiMachine m;
    Assert(m.GetState() == WifiState::NoCredentials);
    Assert(m.Reason() == 0);
    Assert(m.Ip()[0] == 0 && m.Ip()[3] == 0);
}

Fact("Wifi State First Attempt Starts The Driver And Connects")
{
    Fixture f;
    WifiAction a = f.Connect();
    Assert(a.start && a.connect && a.changed && !a.stop && !a.disconnect);
    Assert(a.retry_ms < 0);
    Assert(f.m.GetState() == WifiState::Connecting);
}

Fact("Wifi State Success")
{
    Fixture f;
    f.Connect();
    WifiAction a = f.m.OnGotIp(IP);
    Assert(a.changed && a.cancel_retry && !a.connect && a.retry_ms < 0);
    Assert(f.m.GetState() == WifiState::Connected);
    Assert(!memcmp(f.m.Ip(), IP, 4));

    // The same address again is no change
    a = f.m.OnGotIp(IP);
    Assert(!a.changed);
    // A different one is
    const uint8_t other[4] = { 10, 0, 0, 7 };
    a = f.m.OnGotIp(other);
    Assert(a.changed && !memcmp(f.m.Ip(), other, 4));
}

Fact("Wifi State Failed Attempt Retries After A Second")
{
    Fixture f;
    f.Connect();
    WifiAction a = f.m.OnDisconnected(201);                 // AP not found
    Assert(a.changed && a.retry_ms == WIFI_RETRY_MIN_MS && !a.connect);
    Assert(f.m.GetState() == WifiState::Retrying);
    Assert(f.m.Reason() == 201);

    a = f.m.OnRetryTimer();
    Assert(a.connect && !a.changed && a.retry_ms < 0 && !a.start);
    Assert(f.m.GetState() == WifiState::Retrying);          // stays so across attempts: no event per retry
}

Fact("Wifi State Backoff Doubles To The Cap")
{
    Fixture f;
    f.Connect();
    int32_t expected[] = { 1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000 };
    for (int32_t e : expected) {
        WifiAction a = f.m.OnDisconnected(201);
        Assert(a.retry_ms == e);
        Assert(f.m.OnRetryTimer().connect);
    }
    Assert(WIFI_RETRY_MAX_MS == 30000 && WIFI_RETRY_MIN_MS == 1000);
}

Fact("Wifi State Retries Report Only The First State Change")
{
    Fixture f;
    f.Connect();
    Assert(f.m.OnDisconnected(201).changed);                // Connecting -> Retrying
    f.m.OnRetryTimer();
    WifiAction a = f.m.OnDisconnected(201);                 // Retrying -> Retrying
    Assert(!a.changed && a.retry_ms == 2000);
}

Fact("Wifi State Success Resets The Backoff")
{
    Fixture f;
    f.FailAttempts(4);
    f.m.OnRetryTimer();
    f.m.OnGotIp(IP);
    // The link drops: back to a second
    WifiAction a = f.m.OnDisconnected(200);
    Assert(a.changed && a.retry_ms == WIFI_RETRY_MIN_MS);
    Assert(f.m.GetState() == WifiState::Retrying);
    Assert(f.m.Ip()[0] == 0);                               // no address any more
    Assert(f.m.Reason() == 200);
}

Fact("Wifi State Authentication Failure Goes Straight To The Cap")
{
    static const uint8_t auth[] = { 14, 15, 23, 202, 204 };
    for (uint8_t reason : auth) {
        Fixture f;
        f.Connect();
        WifiAction a = f.m.OnDisconnected(reason);
        Assert(a.retry_ms == WIFI_RETRY_MAX_MS);
        Assert(f.m.Reason() == reason);
    }
    // Other failures don't
    Fixture f;
    f.Connect();
    Assert(f.m.OnDisconnected(201).retry_ms == WIFI_RETRY_MIN_MS);
}

Fact("Wifi State Authentication Failure On A Later Retry Also Goes To The Cap")
{
    Fixture f;
    f.Connect();
    f.m.OnDisconnected(201);
    f.m.OnRetryTimer();
    Assert(f.m.OnDisconnected(202).retry_ms == WIFI_RETRY_MAX_MS);
}

Fact("Wifi State New Credentials Retry At Once")
{
    Fixture f;
    f.FailAttempts(3);                                      // backed off to 4 s, waiting
    WifiAction a = f.m.OnCredentials(true);
    Assert(a.connect && a.cancel_retry && a.changed && !a.start);
    Assert(!a.disconnect);                                  // nothing was in flight
    Assert(f.m.GetState() == WifiState::Connecting);

    // The backoff starts again from a second
    Assert(f.m.OnDisconnected(201).retry_ms == WIFI_RETRY_MIN_MS);
}

Fact("Wifi State New Credentials While Connected Drop And Reconnect")
{
    Fixture f;
    f.Connect();
    f.m.OnGotIp(IP);
    WifiAction a = f.m.OnCredentials(true);
    Assert(a.disconnect && a.connect && a.changed && !a.start && !a.stop);
    Assert(f.m.GetState() == WifiState::Connecting);
    Assert(f.m.Ip()[0] == 0);

    // The disconnect we asked for comes back as an event: not a failure
    a = f.m.OnDisconnected(CB_WIFI_REASON_LEAVE);
    Assert(Quiet(a));
    Assert(f.m.GetState() == WifiState::Connecting);
    // A real failure afterwards still is
    Assert(f.m.OnDisconnected(CB_WIFI_REASON_LEAVE).retry_ms == WIFI_RETRY_MIN_MS);
}

Fact("Wifi State New Credentials While Connecting Drop And Reconnect")
{
    Fixture f;
    f.Connect();
    WifiAction a = f.m.OnCredentials(true);
    Assert(a.disconnect && a.connect && !a.changed);        // still Connecting: nothing to announce
    Assert(Quiet(f.m.OnDisconnected(CB_WIFI_REASON_LEAVE)));
}

Fact("Wifi State A Leave That Was Not Asked For Is A Failure")
{
    Fixture f;
    f.Connect();
    f.m.OnGotIp(IP);
    WifiAction a = f.m.OnDisconnected(CB_WIFI_REASON_LEAVE);
    Assert(a.changed && a.retry_ms == WIFI_RETRY_MIN_MS);
}

Fact("Wifi State Clear Stops And Goes To No Credentials")
{
    Fixture f;
    f.Connect();
    f.m.OnGotIp(IP);
    WifiAction a = f.m.OnCredentials(false);
    Assert(a.stop && a.changed && a.cancel_retry && !a.connect && !a.start);
    Assert(f.m.GetState() == WifiState::NoCredentials);
    Assert(f.m.Ip()[0] == 0 && f.m.Reason() == 0);
}

Fact("Wifi State Clear While Waiting To Retry Cancels The Retry")
{
    Fixture f;
    f.FailAttempts(2);
    WifiAction a = f.m.OnCredentials(false);
    Assert(a.stop && a.cancel_retry && a.changed);
    Assert(f.m.GetState() == WifiState::NoCredentials);
}

Fact("Wifi State Clearing Nothing Does Nothing")
{
    WifiMachine m;
    Assert(Quiet(m.OnCredentials(false)));
    Assert(m.GetState() == WifiState::NoCredentials);
}

Fact("Wifi State Stale Events After A Clear Are Ignored")
{
    Fixture f;
    f.Connect();
    f.m.OnGotIp(IP);
    f.m.OnCredentials(false);
    Assert(Quiet(f.m.OnDisconnected(CB_WIFI_REASON_LEAVE)));
    Assert(Quiet(f.m.OnDisconnected(201)));
    Assert(Quiet(f.m.OnGotIp(IP)));
    Assert(Quiet(f.m.OnLostIp()));
    Assert(Quiet(f.m.OnRetryTimer()));
    Assert(f.m.GetState() == WifiState::NoCredentials);
    Assert(f.m.Ip()[0] == 0);
}

Fact("Wifi State Set After A Clear Starts Again")
{
    Fixture f;
    f.Connect();
    f.m.OnGotIp(IP);
    f.m.OnCredentials(false);
    WifiAction a = f.m.OnCredentials(true);
    Assert(a.start && a.connect && a.changed);
    Assert(f.m.GetState() == WifiState::Connecting);
    // The stop's own disconnect event is still on its way: not a failure
    Assert(Quiet(f.m.OnDisconnected(CB_WIFI_REASON_LEAVE)));
}

Fact("Wifi State Stale Retry Timer Is Ignored")
{
    Fixture f;
    f.FailAttempts(1);
    f.m.OnCredentials(true);                                // a new set connects at once
    Assert(Quiet(f.m.OnRetryTimer()));                      // the old timer fires anyway
    f.m.OnGotIp(IP);
    Assert(Quiet(f.m.OnRetryTimer()));                      // and once connected
    Assert(f.m.GetState() == WifiState::Connected);
}

Fact("Wifi State Disconnect Event Without An Attempt Is Ignored")
{
    Fixture f;
    f.FailAttempts(1);                                      // waiting for the retry timer
    WifiAction a = f.m.OnDisconnected(201);
    Assert(Quiet(a));                                       // no attempt is in flight: not a second failure
    Assert(f.m.OnRetryTimer().connect);
}

Fact("Wifi State Lost IP Waits For Another")
{
    Fixture f;
    f.Connect();
    f.m.OnGotIp(IP);
    WifiAction a = f.m.OnLostIp();
    Assert(a.changed && f.m.GetState() == WifiState::Connecting && f.m.Ip()[0] == 0);
    a = f.m.OnGotIp(IP);
    Assert(a.changed && f.m.GetState() == WifiState::Connected);

    // Not connected: nothing to lose
    Fixture g;
    g.Connect();
    Assert(Quiet(g.m.OnLostIp()));
}

Fact("Wifi State Got IP While Retrying Connects")
{
    Fixture f;
    f.FailAttempts(2);
    f.m.OnRetryTimer();
    WifiAction a = f.m.OnGotIp(IP);
    Assert(a.changed && a.cancel_retry);
    Assert(f.m.GetState() == WifiState::Connected);
}
