// SPDX-License-Identifier: AGPL-3.0-or-later
//
// kick-mcu-sim: the control surface firmware running on the desktop, headless, in place of the
// emulator Surface. It serves the same pty or TCP line, so Kick connects to it unchanged, and a
// script drives the fake pads, buttons, knobs and touch strip.
//
//   kick-mcu-sim [--root DIR] [--tcp PORT] [--script FILE] [--host-log FILE]
//                [--curve linear|soft|hard] [--tick-us N] [--dump-leds FILE]
//   kick-mcu-sim --replay TRACE [--dump-leds FILE]
//
//   --root       emulated device root; the pty is linked at DIR/dev/ttymxc0 (default $KICK_EMU_ROOT or ~/KickEmu)
//   --tcp        listen on this port instead of opening a pty; DIR/surface.port is written
//   --script     play this script (see sim/script_player.h) and exit with its result; without one, run until killed
//   --host-log   Kick's log file, read by `expect host` in a script
//   --curve      pad velocity curve (default linear, which is what `pad N velocity` in a script assumes)
//   --tick-us    main loop period in microseconds (default 250)
//   --dump-leds  write the final LED table to FILE as one line of JSON ("-" is stdout); see sim/led_json.h
//   --replay     play the RX lines of an emulator trace (KICK_EMU_TRACE) into the firmware with no serial line,
//                write the LED table with --dump-leds, and exit. The emulator Surface has the same two options.
//
// Frames go to the host one whole frame at a time, at the moment a UART running at 230400 baud
// would have finished sending them. The host cannot resynchronise (it throws away a read that
// ends mid-frame), so splitting a frame across writes would be a bug in the simulator, not a
// feature of the line.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "kick_mcu/app/firmware.h"
#include "led_json.h"
#include "paced_uart.h"
#include "script_player.h"
#include "serial_transport.h"
#include "sim_board.h"
#include "sim_leds.h"
#include "trace_replay.h"

using namespace kick_mcu;

namespace
{
std::atomic<bool> gStop{ false };

void onSignal( int ) { gStop = true; }

/// Real time as the firmware's clock.
class SteadyClock : public bsp::IClock
{
public:
    SteadyClock() { start_ = std::chrono::steady_clock::now(); }

    uint64_t elapsedUs() const { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::steady_clock::now() - start_ ).count(); }
    uint32_t nowUs() const override { return (uint32_t)elapsedUs(); }
    uint32_t nowMs() const override { return (uint32_t)( elapsedUs() / 1000 ); }

private:
    std::chrono::steady_clock::time_point start_;
};

/// Collects the bytes the paced UART releases and writes each frame to the line in one piece.
/// MCU-to-host frames are 'M' 'T' type dir cmd size payload crc trailer: size + 7 bytes in all.
class FrameWriter
{
public:
    explicit FrameWriter( sim::SerialTransport& line )
        : line_( line )
    {
    }

    void onByte( uint8_t byte )
    {
        pending_.push_back( byte );
        if( pending_.size() >= 6 && pending_[0] == 'M' && pending_[1] == 'T' && pending_.size() == (size_t)pending_[5] + 7 )
            emit();
        else if( pending_.size() >= 6 && !( pending_[0] == 'M' && pending_[1] == 'T' ) )
            emit(); // not a frame the firmware built; pass it through rather than sit on it
    }

    uint32_t framesWritten() const { return frames_; }
    uint32_t framesLost() const { return lost_; }
    void     discard() { pending_.clear(); }

private:
    void emit()
    {
        if( line_.write( pending_.data(), pending_.size() ) )
            ++frames_;
        else
            ++lost_;
        pending_.clear();
    }

    sim::SerialTransport& line_;
    std::vector<uint8_t>  pending_;
    uint32_t              frames_{ 0 };
    uint32_t              lost_{ 0 };
};

std::string readFile( const std::string& path )
{
    std::ifstream in( path, std::ios::binary );
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void usage()
{
    std::fputs( "usage: kick-mcu-sim [--root DIR] [--tcp PORT] [--script FILE] [--host-log FILE] [--curve linear|soft|hard] [--tick-us N] [--dump-leds FILE]\n"
                "       kick-mcu-sim --replay TRACE [--dump-leds FILE]\n",
                stderr );
}

bool writeLedDump( const std::string& path, const output::LedFrame& table )
{
    const std::string json = sim::ledTableJson( table );
    if( path == "-" )
        return std::fwrite( json.data(), 1, json.size(), stdout ) == json.size();
    std::ofstream out( path, std::ios::binary );
    out << json;
    return bool( out );
}
} // namespace

int main( int argc, char** argv )
{
    std::string root;
    if( const char* env = std::getenv( "KICK_EMU_ROOT" ) )
        root = env;
    std::string scriptPath, hostLog, curve = "linear", dumpLeds, replayPath;
    int         tcpPort = 0;
    long        tickUs  = 250;

    for( int i = 1; i < argc; ++i )
    {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if( a == "--root" && ( v = next() ) )
            root = v;
        else if( a == "--tcp" && ( v = next() ) )
            tcpPort = std::atoi( v );
        else if( a == "--script" && ( v = next() ) )
            scriptPath = v;
        else if( a == "--host-log" && ( v = next() ) )
            hostLog = v;
        else if( a == "--curve" && ( v = next() ) )
            curve = v;
        else if( a == "--tick-us" && ( v = next() ) )
            tickUs = std::atol( v );
        else if( a == "--dump-leds" && ( v = next() ) )
            dumpLeds = v;
        else if( a == "--replay" && ( v = next() ) )
            replayPath = v;
        else
        {
            usage();
            return 2;
        }
    }
    if( !replayPath.empty() )
    {
        if( !scriptPath.empty() || tcpPort > 0 )
        {
            usage();
            return 2;
        }
        std::ifstream in( replayPath, std::ios::binary );
        if( !in )
        {
            std::cerr << "kick-mcu-sim: trace not found: " << replayPath << std::endl;
            return 2;
        }
        std::ostringstream text;
        text << in.rdbuf();
        sim::TraceReplayer replayer;
        if( !replayer.run( text.str() ) )
        {
            std::cerr << "kick-mcu-sim: " << replayer.error() << std::endl;
            return 2;
        }
        const auto& stats = replayer.firmware().commands().stats();
        std::fprintf( stderr, "kick-mcu-sim: replayed %zu RX lines, %zu bytes; LED commands applied %u, malformed %u, out of range %u, unknown %u\n", replayer.rxLines(), replayer.rxBytes(), stats.applied,
                      stats.malformed, stats.outOfRange, stats.unknown );
        if( !dumpLeds.empty() && !writeLedDump( dumpLeds, replayer.table() ) )
        {
            std::cerr << "kick-mcu-sim: cannot write " << dumpLeds << std::endl;
            return 2;
        }
        return 0;
    }

    if( root.empty() )
    {
        const char* home = std::getenv( "HOME" );
        root             = std::string( home ? home : "." ) + "/KickEmu";
    }
    if( tickUs < 50 || tickUs > 5000 || ( curve != "linear" && curve != "soft" && curve != "hard" ) )
    {
        usage();
        return 2;
    }

    std::string                              error;
    std::unique_ptr<sim::SerialTransport>    line = tcpPort > 0 ? sim::SerialTransport::listenTcp( root, tcpPort, error ) : sim::SerialTransport::openPty( root, error );
    if( !line )
    {
        std::cerr << "kick-mcu-sim: " << error << std::endl;
        return 2;
    }

    SteadyClock    clock;
    sim::PacedUart uart( clock, 24 );
    sim::SimBoard  board;
    sim::SimLedDriver ledDriver;
    FrameWriter    writer( *line );
    uart.setSink( [&writer]( uint8_t byte, double ) { writer.onByte( byte ); } );

    app::FirmwareConfig config;
    app::Firmware       firmware( uart, clock, app::InputHardware{ board.matrix, board.pads, board.encoders, board.strip }, app::OutputHardware{ ledDriver }, config );
    const auto preset = curve == "soft" ? input::VelocityCurve::Preset::Soft : curve == "hard" ? input::VelocityCurve::Preset::Hard : input::VelocityCurve::Preset::Linear;
    firmware.scan().pads().curve().load( preset );

    std::unique_ptr<sim::ScriptPlayer> player;
    if( !scriptPath.empty() )
    {
        std::ifstream in( scriptPath );
        if( !in )
        {
            std::cerr << "kick-mcu-sim: script not found: " << scriptPath << std::endl;
            return 2;
        }
        std::ostringstream text;
        text << in.rdbuf();

        sim::ScriptHooks hooks;
        hooks.streaming     = [&firmware] { return firmware.link().session().streaming(); };
        hooks.setAftertouch = [&firmware]( bool on ) { firmware.scan().pads().setAftertouchEnabled( on ); };
        hooks.ledTable      = [&firmware]() -> const output::LedFrame& { return firmware.leds().front(); };
        if( !hostLog.empty() )
            hooks.hostLog = [hostLog] { return readFile( hostLog ); };
        player = std::make_unique<sim::ScriptPlayer>( board, config.scan, hooks );
        if( !player->load( text.str() ) )
        {
            std::cerr << "kick-mcu-sim: " << player->error() << std::endl;
            return 2;
        }
    }

    std::signal( SIGINT, onSignal );
    std::signal( SIGTERM, onSignal );

    int  exitCode = 0;
    auto next     = std::chrono::steady_clock::now();
    while( !gStop )
    {
        if( line->service() )
            writer.discard(); // a new client: nothing from before it connected is for it

        uint8_t buffer[256];
        for( size_t n; ( n = line->read( buffer, sizeof buffer ) ) > 0; )
            uart.injectRx( buffer, n );

        if( player )
        {
            const auto state = player->advance( clock.elapsedUs() );
            if( state != sim::ScriptPlayer::State::Running )
            {
                if( state == sim::ScriptPlayer::State::Failed )
                    std::cerr << player->failure() << std::endl;
                exitCode = player->exitCode();
                // Let the last frames leave before the line closes.
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds( 50 );
                while( std::chrono::steady_clock::now() < until )
                {
                    firmware.step();
                    uart.service();
                    line->service();
                    std::this_thread::sleep_for( std::chrono::microseconds( tickUs ) );
                }
                break;
            }
        }

        firmware.step();
        uart.service();

        next += std::chrono::microseconds( tickUs );
        const auto now = std::chrono::steady_clock::now();
        if( next < now )
            next = now; // fell behind (a stalled host or a busy machine): do not try to catch up
        std::this_thread::sleep_until( next );
    }

    if( !dumpLeds.empty() && !writeLedDump( dumpLeds, firmware.leds().front() ) )
    {
        std::cerr << "kick-mcu-sim: cannot write " << dumpLeds << std::endl;
        exitCode = exitCode != 0 ? exitCode : 2;
    }

    const auto& tx = firmware.link().tx().stats();
    std::fprintf( stderr, "kick-mcu-sim: frames written %u, lost %u; queue dropped button %u pad/knob %u aftertouch %u; rx crc errors %u\n", writer.framesWritten(), writer.framesLost(),
                  tx.dropped[(size_t)link::Lane::Button], tx.dropped[(size_t)link::Lane::PadKnob], tx.dropped[(size_t)link::Lane::Aftertouch], firmware.link().parser().stats().crcErrors );
    return exitCode;
}
