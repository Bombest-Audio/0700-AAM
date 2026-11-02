// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The firmware's main loop body: transport plus input scanning.
#pragma once

#include "kick_mcu/app/scan_loop.h"
#include "kick_mcu/bsp/bsp.h"
#include "kick_mcu/link/link.h"

namespace kick_mcu
{
namespace app
{
    struct FirmwareConfig
    {
        link::LinkConfig link{};
        ScanConfig       scan{};
    };

    /// Connects the input layer's sink to a Link: events go into the transmit queue, and are
    /// refused until the host handshake is done.
    class LinkSink : public input::IEventSink
    {
    public:
        explicit LinkSink( link::Link& link )
            : link_( link )
        {
        }

        bool accepting() const override { return link_.session().streaming(); }
        bool send( link::Lane lane, uint8_t type, uint8_t cmd, const uint8_t* payload, size_t length ) override { return link_.send( lane, type, cmd, payload, length ); }
        void discardQueued( link::Lane lane, uint8_t type, uint8_t firstByte ) override { link_.tx().discard( lane, type, firstByte ); }

    private:
        link::Link& link_;
    };

    /// What runs on the MCU. step() is one pass of the main loop: scan, then let the transport
    /// receive, run the heartbeat and send. The caller decides how often to call it; the latency
    /// figures in the README assume a pass every 250 microseconds.
    ///
    /// Host commands the firmware acts on so far: PAD_SET_AFTER_TOUCH (turns pad aftertouch on
    /// or off). The LED, BPM, shutdown and update commands arrive in later changes; until then
    /// they are counted in hostFramesIgnored().
    class Firmware
    {
    public:
        Firmware( bsp::IUart& uart, const bsp::IClock& clock, InputHardware hardware, const FirmwareConfig& config = {} );

        void step();

        link::Link&   link() { return link_; }
        ScanLoop&     scan() { return scan_; }
        uint32_t      hostFramesIgnored() const { return hostFramesIgnored_; }

    private:
        static void onHostFrame( void* context, const link::Frame& frame );

        link::Link link_;
        LinkSink   sink_;
        ScanLoop   scan_;
        uint32_t   hostFramesIgnored_{ 0 };
    };

} // namespace app
} // namespace kick_mcu
