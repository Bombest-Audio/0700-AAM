// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The host's own PktMgr and the firmware codec, run against each other.
#include <gtest/gtest.h>

#include <vector>

#include "kick_mcu/link/frame_codec.h"
#include "kick_mcu/link/rx_parser.h"
#include "test_support.h"

#include "Defines.hpp"
#include "PktMgr.hpp"

using namespace kick_mcu;
using namespace kick_mcu::link;
using namespace testing_support;

namespace
{
// PktMgr as input_events.cpp builds it, with the same 20-byte buffers.
struct HostPkt
{
    uint8_t rx[20]{};
    uint8_t tx[20]{};
    CPktMgr mgr{ CPktMgr::EN_COM_CHANNEL_USB, rx, 20, tx, 20, 60 };

    HostPkt() { mgr.Init(); }
};
} // namespace

TEST( PktMgrCrossCheck, HostCrcEqualsFirmwareCrc )
{
    Rng rng( 99 );
    for( int i = 0; i < 500; ++i )
    {
        std::vector<char> bytes;
        const size_t      n = rng.below( 30 );
        for( size_t k = 0; k < n; ++k )
            bytes.push_back( (char)rng.next() );

        Bytes same( bytes.begin(), bytes.end() );
        EXPECT_EQ( CPktMgr::CrcGen( bytes ), crc8( same.data(), same.size() ) ) << "length " << n;
    }
}

TEST( PktMgrCrossCheck, HostEncoderMatchesFirmwareEncoderWithoutTheTrailer )
{
    HostPkt host;
    for( int type = 0; type <= 7; ++type )
        for( size_t len = 0; len <= 19; ++len )
        {
            CPktMgr::HWMsg_t msg;
            msg.pktType = (CPktMgr::enumPacketType)type;
            msg.pktDir  = CPktMgr::EN_PKT_REQUEST;
            msg.pktCmd  = type + 1;
            Bytes payload;
            for( size_t i = 0; i < len; ++i )
            {
                msg.data.push_back( (int)( ( i * 7 + type ) & 0xFF ) );
                payload.push_back( (uint8_t)msg.data.back() );
            }

            std::vector<char> hostBytes;
            host.mgr.BuildAndTransmitPacket( msg, hostBytes );

            const Bytes mine = fromHost( (uint8_t)type, (uint8_t)msg.pktCmd, payload );
            EXPECT_EQ( Bytes( hostBytes.begin(), hostBytes.end() ), mine ) << "type " << type << " len " << len;
        }
}

TEST( PktMgrCrossCheck, FirmwareParserReadsFramesTheHostBuilds )
{
    HostPkt host;
    RxParser  parser;
    Collector got;
    got.attach( parser );

    std::vector<Decoded> expected;
    for( int i = 0; i < 100; ++i )
    {
        CPktMgr::HWMsg_t msg;
        msg.pktType = (CPktMgr::enumPacketType)( i % 8 );
        msg.pktDir  = CPktMgr::EN_PKT_REQUEST;
        msg.pktCmd  = i % 11;
        Bytes payload;
        for( int k = 0; k < i % 20; ++k )
        {
            msg.data.push_back( ( i * 13 + k ) & 0xFF );
            payload.push_back( (uint8_t)msg.data.back() );
        }
        std::vector<char> bytes;
        host.mgr.BuildAndTransmitPacket( msg, bytes );
        for( char b : bytes )
            parser.feed( (uint8_t)b );
        expected.push_back( { (uint8_t)msg.pktType, 0, (uint8_t)msg.pktCmd, payload } );
    }

    ASSERT_EQ( expected.size(), got.frames.size() );
    for( size_t i = 0; i < expected.size(); ++i )
        EXPECT_EQ( expected[i], got.frames[i] ) << i;
}

TEST( PktMgrCrossCheck, HostPktMgrReadsFramesTheFirmwareEncodes )
{
    // The MCU-to-host direction: the host drops the trailer itself (input_events.cpp passes
    // size + 6 bytes to PktMgr), so feed it everything but the last byte.
    for( size_t len = 1; len <= 19; ++len )
    {
        HostPkt host;
        Bytes   payload;
        for( size_t i = 0; i < len; ++i )
            payload.push_back( (uint8_t)( 0x40 + i ) );
        const Bytes frame = toHost( contract::PKT_PAD, contract::PAD_STATE, payload );

        for( size_t i = 0; i + 1 < frame.size(); ++i )
            host.mgr.PushByteToRxFifo( frame[i] );

        ASSERT_TRUE( host.mgr.IsFrameRcvComplete() ) << "len " << len;
        EXPECT_EQ( CPktMgr::EN_PKT_TYPE_PAD, host.mgr.GetRxPktType() );
        EXPECT_EQ( contract::PAD_STATE, host.mgr.GetRxCommand() );
        EXPECT_EQ( len + 1, (size_t)host.mgr.GetRxFrameSize() );
        for( size_t i = 0; i < len; ++i )
            EXPECT_EQ( payload[i], host.mgr.PopUint8FromRxFifo() ) << "len " << len << " byte " << i;
    }
}
