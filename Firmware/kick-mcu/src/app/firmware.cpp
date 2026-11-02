// SPDX-License-Identifier: AGPL-3.0-or-later
#include "kick_mcu/app/firmware.h"

#include "kick_mcu/contract.h"

namespace kick_mcu
{
namespace app
{
    Firmware::Firmware( bsp::IUart& uart, const bsp::IClock& clock, InputHardware hardware, OutputHardware outputs, const FirmwareConfig& config )
        : Firmware( uart, clock, hardware, outputs.leds, config )
    {
    }

    Firmware::Firmware( bsp::IUart& uart, const bsp::IClock& clock, InputHardware hardware, const FirmwareConfig& config )
        : Firmware( uart, clock, hardware, static_cast<bsp::ILedDriver&>( nullLeds_ ), config )
    {
    }

    Firmware::Firmware( bsp::IUart& uart, const bsp::IClock& clock, InputHardware hardware, bsp::ILedDriver& leds, const FirmwareConfig& config )
        : link_( uart, clock, config.link )
        , sink_( link_ )
        , scan_( hardware, sink_, clock, config.scan )
        , commands_( ledState_ )
        , blink_( clock, config.blinkPeriodMs )
        , refresh_( leds, ledState_, blink_, clock, config.render )
    {
        link_.setPassThrough( &Firmware::onHostFrame, this );
    }

    void Firmware::onHostFrame( void* context, const link::Frame& frame )
    {
        Firmware* self = static_cast<Firmware*>( context );
        if( frame.type == contract::PKT_PAD && frame.cmd == contract::PAD_SET_AFTER_TOUCH && frame.length >= 1 )
        {
            self->scan_.pads().setAftertouchEnabled( frame.payload[0] != 0 );
            return;
        }
        if( self->commands_.handle( frame ) != output::CommandResult::NotMine )
            return; // applied, or rejected and counted in commands().stats()
        ++self->hostFramesIgnored_;
    }

    void Firmware::step()
    {
        scan_.tick();
        link_.poll();
        refresh_.service();
    }

} // namespace app
} // namespace kick_mcu
