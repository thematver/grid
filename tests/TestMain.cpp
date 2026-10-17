#include "Test.h"
#include <cstring>

int main (int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (auto& c : wtest::registry())
    {
        if (filter && std::strstr (c.name, filter) == nullptr) continue;
        wtest::currentName() = c.name;
        const int before = wtest::failures();
        c.fn();
        ++run;
        std::printf ("%s %s\n", wtest::failures() == before ? "ok  " : "FAIL", c.name);
    }
    std::printf ("\n%d tests, %d failures\n", run, wtest::failures());
    return wtest::failures() == 0 ? 0 : 1;
}
