// SPDX-License-Identifier: GPL-3.0-or-later
#define RECOMP_QUIET_LOG_TEST_SINK testLogSink
#define RECOMP_QUIET_LOG_TEST_FILE testQuietFile
#define RECOMP_PLAY_LOG_TEST_FILE testLoudFile
#include "diag_log_sample.h"
#include <cassert>
#include <cstring>

namespace recomp::diag {
unsigned emitted=0;
unsigned lastQuietSummary=99;
bool quietFile=false,loudFile=true;
bool testQuietFile() noexcept {return quietFile;}
bool testLoudFile() noexcept {return loudFile;}
void testLogSink(int,const char*,const char* format,va_list arguments) {
    ++emitted;
    if(std::strcmp(format,"RENDER_GENERIC_V210Z quiet_log=%u")==0)
        lastQuietSummary=va_arg(arguments,unsigned);
}
}

int main() {
    using namespace recomp::diag;
#if defined(__APPLE__)
    const char* option=std::getenv("RECOMP_VERBOSE_DIAGNOSTICS");
    const bool platformQuiet=!(option && std::strcmp(option,"1")==0);
#else
    const bool platformQuiet=false;
#endif
    assert(quietSkip()==(platformQuiet || RECOMP_PLAY_QUIET_AT_START));
    const auto before=emitted;
    quietLog(4,"test","ordinary");
    assert(emitted==before+unsigned(!quietSkip()));
    quietLogAlways(6,"test","failure");
    quietLogSampled(1,6,"test","refusal");
    assert(emitted==before+unsigned(!quietSkip())+2);

    quietLogWindow();
    assert(quietSkip()==platformQuiet);
    assert(lastQuietSummary==unsigned(platformQuiet));
    quietFile=true;
    quietLogWindow();
    assert(quietSkip());
    quietFile=false;
    quietLogWindow();
    assert(quietSkip()==platformQuiet);

#if defined(RECOMP_PLAY_BUILD) && RECOMP_PLAY_BUILD
    loudFile=false;
    quietLogWindow();
    assert(quietSkip());
    loudFile=true;
    quietLogWindow();
    assert(quietSkip()==platformQuiet);
#endif
    const auto final=emitted;
    quietLogAlways(6,"test","failure after poll");
    quietLogSampled(1,6,"test","refusal after poll");
    assert(emitted==final+2);
}
