/*
 *  Created by Phil on 28/04/2011.
 *  Copyright 2010 Two Blue Cubes Ltd. All rights reserved.
 *
 *  Distributed under the Boost Software License, Version 1.0. (See accompanying
 *  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */
#ifndef TWOBLUECUBES_CATCH_APPROX_HPP_INCLUDED
#define TWOBLUECUBES_CATCH_APPROX_HPP_INCLUDED

#include "catch_tostring.h"

#include <cmath>
#include <limits>

#if defined(CATCH_CPP11_OR_GREATER)
#include <type_traits>
#endif

namespace Catch {
namespace Detail {

    class Approx {
    public:
        explicit Approx ( double value )
        :   m_epsilon( std::numeric_limits<float>::epsilon()*100 ),
            m_scale( 1.0 ),
            m_value( value )
        {}

        Approx( Approx const& other )
        :   m_epsilon( other.m_epsilon ),
            m_scale( other.m_scale ),
            m_value( other.m_value )
        {}

        static Approx custom() {
            return Approx( 0 );
        }

        Approx operator()( double value ) {
            Approx approx( value );
            approx.epsilon( m_epsilon );
            approx.scale( m_scale );
            return approx;
        }

#if defined(CATCH_CPP11_OR_GREATER)
        template <typename T, typename = typename std::enable_if<std::is_constructible<double, T>::value>::type>
        friend bool operator == ( const T& lhs, Approx const& rhs ) {
            // Thanks to Richard Harris for his help refining this formula
            auto lhs_v = double(lhs);
            return fabs( lhs_v - rhs.m_value ) < rhs.m_epsilon * (rhs.m_scale + (std::max)( fabs(lhs_v), fabs(rhs.m_value) ) );
        }

        template <typename T, typename = typename std::enable_if<std::is_constructible<double, T>::value>::type>
        friend bool operator == ( Approx const& lhs, const T& rhs ) {
            return operator==( rhs, lhs );
        }

        template <typename T, typename = typename std::enable_if<std::is_constructible<double, T>::value>::type>
        friend bool operator != ( T lhs, Approx const& rhs ) {
            return !operator==( lhs, rhs );
        }

        template <typename T, typename = typename std::enable_if<std::is_constructible<double, T>::value>::type>
        friend bool operator != ( Approx const& lhs, T rhs ) {
            return !operator==( rhs, lhs );
        }

        template <typename T, typename = typename std::enable_if<std::is_constructible<double, T>::value>::type>
        friend bool operator <= ( T lhs, Approx const& rhs )
        {
          return double(lhs) < rhs.m_value || lhs == rhs;
        }

        template <typename T, typename = typename std::enable_if<std::is_constructible<double, T>::value>::type>
        friend bool operator <= ( Approx const& lhs, T rhs )
        {
          return lhs.m_value < double(rhs) || lhs == rhs;
        }

        template <typename T, typename = typename std::enable_if<std::is_constructible<double, T>::value>::type>
        friend bool operator >= ( T lhs, Approx const& rhs )
        {
          return double(lhs) > rhs.m_value || lhs == rhs;
        }

        template <typename T, typename = typename std::enable_if<std::is_constructible<double, T>::value>::type>
        friend bool operator >= ( Approx const& lhs, T rhs )
        {
          return lhs.m_value > double(rhs) || lhs == rhs;
        }
#else
        friend bool operator == ( double lhs, Approx const& rhs ) {
            // Thanks to Richard Harris for his help refining this formula
            return fabs( lhs - rhs.m_value ) < rhs.m_epsilon * (rhs.m_scale + (std::max)( fabs(lhs), fabs(rhs.m_value) ) );
        }

        friend bool operator == ( Approx const& lhs, double rhs ) {
            return operator==( rhs, lhs );
        }

        friend bool operator != ( double lhs, Approx const& rhs ) {
            return !operator==( lhs, rhs );
        }

        friend bool operator != ( Approx const& lhs, double rhs ) {
            return !operator==( rhs, lhs );
        }

        friend bool operator <= ( double lhs, Approx const& rhs )
        {
          return lhs < rhs.m_value || lhs == rhs;
        }

        friend bool operator <= ( Approx const& lhs, double rhs )
        {
          return lhs.m_value < rhs || lhs == rhs;
        }

        friend bool operator >= ( double lhs, Approx const& rhs )
        {
          return lhs > rhs.m_value || lhs == rhs;
        }

        friend bool operator >= ( Approx const& lhs, double rhs )
        {
          return lhs.m_value > rhs || lhs == rhs;
        }
#endif

        Approx& epsilon( double newEpsilon ) {
            m_epsilon = newEpsilon;
            return *this;
        }

        Approx& scale( double newScale ) {
            m_scale = newScale;
            return *this;
        }

        std::string toString() const {
            std::ostringstream oss;
            oss << "Approx( " << Catch::toString( m_value ) << " )";
            return oss.str();
        }

    private:
        double m_epsilon;
        double m_scale;
        double m_value;
    };
}

template<>
inline std::string toString<Detail::Approx>( Detail::Approx const& value ) {
    return value.toString();
}

} // end namespace Catch

#endif // TWOBLUECUBES_CATCH_APPROX_HPP_INCLUDED
