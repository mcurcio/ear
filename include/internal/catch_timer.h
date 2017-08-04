/*
 *  Created by Phil on 05/08/2013.
 *  Copyright 2013 Two Blue Cubes Ltd. All rights reserved.
 *
 *  Distributed under the Boost Software License, Version 1.0. (See accompanying
 *  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */
#ifndef TWOBLUECUBES_CATCH_TIMER_H_INCLUDED
#define TWOBLUECUBES_CATCH_TIMER_H_INCLUDED

#include <stdint.h>

namespace Catch {

    auto getCurrentNanosecondsSinceEpoch() -> uint64_t;

    class Timer {
        uint64_t m_nanoseconds = 0;
    public:
        void start();
        auto getElapsedNanoseconds() const -> unsigned int;
        auto getElapsedMicroseconds() const -> unsigned int;
        auto getElapsedMilliseconds() const -> unsigned int;
        auto getElapsedSeconds() const -> double;
    };

} // namespace Catch

#endif // TWOBLUECUBES_CATCH_TIMER_H_INCLUDED
