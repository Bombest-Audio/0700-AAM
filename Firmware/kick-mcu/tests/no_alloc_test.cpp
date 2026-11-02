// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The receive path must not allocate. Two checks: the types involved are plain storage, and a
// run through the whole path with a counting operator new sees nothing.
#include <gtest/gtest.h>

#include <type_traits>

#include "alloc_hook.h"
#include "kick_mcu/link/link.h"
#include "sim_clock.h"
#include "sim_uart.h"
#include "test_support.h"

using namespace kick_mcu;
using namespace kick_mcu::link;
using namespace testing_support;

// Fixed-buffer types: trivially destructible (nothing owns heap memory) and with no virtual
// members. A std::vector or std::function member would break the first assertion.
static_assert( std::is_trivially_destructible<RxParser>::value, "RxParser must not own heap memory" );
static_assert( std::is_trivially_destructible<Frame>::value, "Frame must not own heap memory" );
static_assert( std::is_trivially_destructible<TxQueue>::value, "TxQueue must not own heap memory" );
static_assert( std::is_trivially_destructible<Session>::value, "Session must not own heap memory" );
static_assert( std::is_trivially_destructible<SpscByteRing<256>>::value, "the receive ring must not own heap memory" );
static_assert( !std::is_polymorphic<RxParser>::value, "RxParser has no virtual functions" );
static_assert( sizeof( RxParser ) <= contract::kRxFrameBufferBytes + 64, "RxParser holds one frame buffer and a few counters" );

TEST( NoAlloc, ParsingFramesDoesNotAllocate )
{
    // Build the traffic first: the vectors here are the test's, not the firmware's.
    Bytes stream;
    for( int i = 0; i < 500; ++i )
    {
        const Bytes f = fromHost( contract::PKT_PAD, contract::PAD_SET_ALL_MULTI_COLOUR, Bytes( 1 + i % 33, (uint8_t)i ) );
        stream.insert( stream.end(), f.begin(), f.end() );
        stream.push_back( 0xEE ); // garbage
        stream.push_back( 'M' );  // a false start
    }

    RxParser parser;
    struct Count
    {
        size_t n{ 0 };
        static void on( void* c, const Frame& ) { ++static_cast<Count*>( c )->n; }
    } count;
    parser.setHandler( &Count::on, &count );

    alloc_hook::arm();
    for( uint8_t b : stream )
        parser.feed( b );
    parser.expire();
    const size_t allocations = alloc_hook::disarm();

    EXPECT_EQ( 0u, allocations );
    EXPECT_EQ( 500u, count.n );
}

namespace
{
// A UART that keeps no log, so the test counts only allocations made by firmware code.
class NullUart : public bsp::IUart
{
public:
    size_t write( bsp::ByteSpan bytes ) override
    {
        written += bytes.size;
        return bytes.size;
    }
    size_t writableBytes() const override { return 1024; }
    void   onByte( bsp::ByteHandler handler, void* context ) override
    {
        handler_ = handler;
        context_ = context;
    }
    void inject( const Bytes& b )
    {
        for( uint8_t x : b )
            handler_( context_, x );
    }

    size_t           written{ 0 };
    bsp::ByteHandler handler_{ nullptr };
    void*            context_{ nullptr };
};
} // namespace

TEST( NoAlloc, TheWholeLinkPathDoesNotAllocate )
{
    NullUart      uart;
    sim::SimClock clock;
    Link          link( uart, clock );

    const Bytes init1 = fromHost( contract::PKT_SYSTEM, contract::SYS_COMM_INIT, { 1 } );
    const Bytes init2 = fromHost( contract::PKT_SYSTEM, contract::SYS_COMM_INIT, { 2 } );
    const Bytes echo  = fromHost( contract::PKT_SYSTEM, contract::SYS_CHECK_COMM_STATUS, { 2 } );
    const Bytes led   = fromHost( contract::PKT_LED, contract::LED_SET_SINGLE_LED, { 4, 1 } );

    alloc_hook::arm();
    uart.inject( init1 );
    link.poll();
    uart.inject( init2 );
    link.poll();
    uart.inject( echo );
    link.poll();
    for( int i = 0; i < 2000; ++i )
    {
        uart.inject( led );
        const uint8_t p[2] = { (uint8_t)( i % 53 ), (uint8_t)( i & 1 ) };
        link.send( Lane::Button, contract::PKT_BUTTON, contract::BTN_STATUS, p, 2 );
        clock.advanceMs( 3 );
        link.poll();
    }
    const size_t allocations = alloc_hook::disarm();

    EXPECT_EQ( 0u, allocations );
    EXPECT_TRUE( link.session().streaming() );
    EXPECT_GT( uart.written, 2000u * 8 ) << "the loop really did transmit";
}
