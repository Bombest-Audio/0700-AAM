// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The wire contract with the Kick host, restated for firmware that cannot include JUCE.
//
// Nothing here is authoritative. The authoritative definitions are
// Kick/modules/contracts/dcl/dcl_contracts.h and ComProtocol/PktMgr/PktMgr.hpp, and
// tests/contract_test.cpp compiles against both and static_asserts every value below equal to
// them, so a drift on either side fails the build instead of the link.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kick_mcu
{
namespace contract
{
    // ---- framing -------------------------------------------------------------------------
    //
    //   'M' 'T' type dir cmd size payload[size-1] crc8 [trailer]
    //
    // size is the payload length plus one (the CRC byte). crc8 is polynomial 0x31, initial value
    // 0xFF, over every byte from 'M' through the last payload byte.

    constexpr uint8_t kStartByte1 = 'M';
    constexpr uint8_t kStartByte2 = 'T';
    constexpr uint8_t kCrcPoly    = 0x31;
    constexpr uint8_t kCrcInit    = 0xFF;

    /// Bytes before the payload: 'M' 'T' type dir cmd size.
    constexpr size_t kHeaderBytes = 6;
    constexpr size_t kCrcBytes    = 1;

    /// The byte the Surface appends after the CRC on every frame it sends to the host. The host
    /// reads size + 7 bytes per frame (input_events.cpp, `packetLength`) and hands the first
    /// size + 6 to PktMgr, so the last one is dropped unread. MCU-to-host frames need it;
    /// host-to-MCU frames do not carry one.
    constexpr uint8_t kMcuToHostTrailer = '\n';

    /// Largest MCU-to-host payload. The host's PktMgr RX buffer is BUFFER_RX_SIZE = 20 bytes
    /// (Kick/modules/input_events/ComConfig.hpp) and PushByteToRxFifo writes size - 1 bytes into
    /// it with no check against its length, so 21 bytes would overrun it. 20 would fit; this
    /// stops at 19 to keep a byte of margin, and nothing the MCU sends is longer than a few bytes.
    constexpr size_t kMaxPayloadToHost = 19;

    /// Largest host-to-MCU payload the host's own senders produce (EN_PAD_SET_STEP_ALL_PAD).
    constexpr size_t kLargestHostPayload = 33;

    /// MCU receive buffer, in bytes of a whole frame (header, payload and CRC).
    constexpr size_t kRxFrameBufferBytes = 64;
    constexpr size_t kMaxPayloadFromHost = kRxFrameBufferBytes - kHeaderBytes - kCrcBytes;

    static_assert(kLargestHostPayload <= kMaxPayloadFromHost, "receive buffer too small for the host's largest frame");

    // ---- enums, values from the host --------------------------------------------------------

    /// CPktMgr::enumPacketType
    enum PacketType : uint8_t
    {
        PKT_SYSTEM = 0,
        PKT_BUTTON,
        PKT_LED,
        PKT_PAD,
        PKT_KNOBS,
        PKT_MIDI,
        PKT_CV,
        PKT_SLIDER,
    };

    /// CPktMgr::enumPacketDir. The host only ever sends requests and ignores responses.
    enum PacketDir : uint8_t
    {
        DIR_REQUEST = 0,
        DIR_RESPONSE,
    };

    /// DeviceControlContracts::SysCmd
    enum SysCmd : uint8_t
    {
        SYS_RESET_GD = 0,
        SYS_GET_SW_VER,
        SYS_COMM_INIT,
        SYS_SW_UPDATE,
        SYS_CHECK_COMM_STATUS,
        SYS_BPM,
        SYS_RECONNECT,
        SYS_SHUTDOWN,
    };

    /// DeviceControlContracts::SysCommCmd
    enum SysCommCmd : uint8_t
    {
        COMM_NOT_CONNECTED = 0,
        COMM_CONNECTED,
    };

    /// The second handshake stage reuses COMM_INIT and CHECK_COMM_STATUS with payload [2]. The
    /// value is not in dcl_contracts.h; it is what the host sends and expects (see the handshake
    /// section of the README).
    constexpr uint8_t kCommStageLive = 2;

    /// DeviceControlContracts::BtnCmd
    enum BtnCmd : uint8_t
    {
        BTN_STATUS = 0,
    };

    /// DeviceControlContracts::BtnState
    enum BtnState : uint8_t
    {
        BTN_RELEASED = 0,
        BTN_PRESSED,
        BTN_LONGPRESSED,
        BTN_DOUBLECLICK,
    };

    /// DeviceControlContracts::LedCmd
    enum LedCmd : uint8_t
    {
        LED_SET_SINGLE_LED = 0,
        LED_BLINK_SINGLE,
        LED_SET_ALL,
    };

    /// DeviceControlContracts::PadCmd
    enum PadCmd : uint8_t
    {
        PAD_STATE = 0,
        PAD_SET_SINGLE_PAD,
        PAD_SET_ALL_SINGLE_COLOUR,
        PAD_SET_ALL_MULTI_COLOUR,
        PAD_SETTINGS,
        PAD_COLOR_MODE,
        PAD_SET_COLOUR_STATE,
        PAD_SET_AFTER_TOUCH,
        PAD_SET_BLINKING,
        PAD_SET_STEP_SINGLE_PAD,
        PAD_SET_STEP_ALL_PAD,
    };

    /// DeviceControlContracts::KnobCmd
    enum KnobCmd : uint8_t
    {
        KNOB_ROTARY = 0,
        KNOB_QLINK,
    };

    /// DeviceControlContracts::KnobDirection
    enum KnobDirection : uint8_t
    {
        KNOB_ANTICLOCKWISE = 0,
        KNOB_CLOCKWISE,
    };

    /// DeviceControlContracts::SliderCmd
    enum SliderCmd : uint8_t
    {
        SLIDER_TOUCH_INPUT = 0,
        SLIDER_BAR_LED,
        SLIDER_LEVEL_LED,
        SLIDER_INDIVIDUAL_LED,
    };

} // namespace contract
} // namespace kick_mcu
