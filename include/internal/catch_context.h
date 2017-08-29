/*
 *  Created by Phil on 31/12/2010.
 *  Copyright 2010 Two Blue Cubes Ltd. All rights reserved.
 *
 *  Distributed under the Boost Software License, Version 1.0. (See accompanying
 *  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */
#ifndef TWOBLUECUBES_CATCH_CONTEXT_H_INCLUDED
#define TWOBLUECUBES_CATCH_CONTEXT_H_INCLUDED

#include <memory>

namespace Catch {

    class TestCase;
    class Stream;
    struct IResultCapture;
    struct IRunner;
    struct IConfig;

    using IConfigPtr = std::shared_ptr<IConfig const>;

    struct IContext
    {
        virtual ~IContext() = default;

        virtual IResultCapture* getResultCapture() = 0;
        virtual IRunner* getRunner() = 0;
        virtual IConfigPtr getConfig() const = 0;
    };

    struct IMutableContext : IContext
    {
        virtual ~IMutableContext() = default;
        virtual void setResultCapture( IResultCapture* resultCapture ) = 0;
        virtual void setRunner( IRunner* runner ) = 0;
        virtual void setConfig( IConfigPtr const& config ) = 0;
    };

    IContext& getCurrentContext();
    IMutableContext& getCurrentMutableContext();
    void cleanUpContext();
}

#endif // TWOBLUECUBES_CATCH_CONTEXT_H_INCLUDED
