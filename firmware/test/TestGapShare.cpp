#include "UnitTesting.h"
#include "Core.h"
#include "gap_share.h"

using namespace SimpleLib;

// Which task gets each fill gap, so only one writes PSRAM per gap

Fact("GapShare Nobody Wanting Gets Nothing")
{
    GapRotor r;
    bool wants[2] = { false, false };
    Assert(r.Next(wants, 2) == -1);
}

Fact("GapShare A Single Client Gets Every Gap")
{
    GapRotor r;
    bool wants[2] = { false, true };
    for (int i = 0; i < 5; i++)
        Assert(r.Next(wants, 2) == 1);
}

Fact("GapShare Two Clients Alternate")
{
    GapRotor r;
    bool wants[2] = { true, true };
    int a = r.Next(wants, 2);
    int b = r.Next(wants, 2);
    int c = r.Next(wants, 2);
    Assert(a != b && a == c);
    Assert(a >= 0 && b >= 0);
}

Fact("GapShare A Client That Stops Wanting Is Skipped")
{
    GapRotor r;
    bool wants[3] = { true, true, true };
    Assert(r.Next(wants, 3) == 0);
    wants[1] = false;
    Assert(r.Next(wants, 3) == 2);
    Assert(r.Next(wants, 3) == 0);
}
