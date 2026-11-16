#ifndef SORTERHUNTER_TIMEOUT_H
#define SORTERHUNTER_TIMEOUT_H

#include <chrono>

// Cooperative wall-clock deadline. Checking periodically avoids a clock syscall
// on every search iteration while allowing normal C++ unwinding on timeout.
struct TimeoutExpired {};
extern bool timeoutEnabled;
extern unsigned timeoutCheckCounter;
extern std::chrono::steady_clock::time_point timeoutDeadline;

inline void checkTimeout()
{
    if (timeoutEnabled && ++timeoutCheckCounter >= 1024) {
        timeoutCheckCounter = 0;
        if (std::chrono::steady_clock::now() >= timeoutDeadline)
            throw TimeoutExpired();
    }
}

#endif
