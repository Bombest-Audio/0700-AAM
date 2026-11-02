// SPDX-License-Identifier: AGPL-3.0-or-later
#include "kick_mcu/app/firmware.h"

#include "kick_mcu/contract.h"

namespace kick_mcu
{
namespace app
{
    Firmware::Firmware( bsp::IUart& uart, const bsp::IClock& clock, InputHardware hardware, const FirmwareConfig& config )
        : link_( uart, clock, config.link )
        , sink_( link_ )
        , scan_( hardware, sink_, clock, config.scan )
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
        ++self->hostFramesIgnored_;
    }

    void Firmware::step()
    {
        scan_.tick();
        link_.poll();
    }

} // namespace app
} // namespace kick_mcu
