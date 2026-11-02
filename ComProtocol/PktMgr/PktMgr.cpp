#include "PktMgr.hpp"

#ifdef KICK_MCU
    #include <Hardware.hpp>
    #include <TaskHdl.hpp>
    #include <ComHdl.hpp>
#endif

void CPktMgr::Init( void )
{
    m_ui8FrameSize       = 0;
    m_ui8RxCommand       = 0;
    m_ui8TxCommand       = 0;
    m_ui8RxFifoWritePos  = 0;
    m_ui8RxFifoReadPos   = 0;
    m_boFrameRcvComplete = FALSE;
    m_enPktDir           = EN_PKT_REQUEST;

#ifdef PKT_HEADER_AVAILABLE
    m_enFrmState = EN_FRM_STATE_INIT_1;
#else
    m_enFrmState = EN_FRM_STATE_TYPE;
#endif
}

void CPktMgr::ClearReceiver( void )
{
    m_ui8RxFifoWritePos  = 0;
    m_ui8RxFifoReadPos   = 0;
    m_enPktDir           = EN_PKT_REQUEST;
    m_boFrameRcvComplete = FALSE;
    m_enFrmState         = EN_FRM_STATE_INIT_1;
    m_rxFrame.clear();
}

void CPktMgr::PushByteToRxFifo( uint8_t f_ui8Data )
{
    bool boRcvdCompleteMsg = FALSE;

    switch( m_enFrmState )
    {
        case EN_FRM_STATE_INIT_1:
            // Hunt for the 'M' that starts a frame. Anything else is line noise and must not reach the CRC.
            m_rxFrame.clear();
            if( f_ui8Data == 'M' )
            {
                m_rxFrame.push_back( f_ui8Data );
                m_enFrmState = EN_FRM_STATE_INIT_2;
            }

            break;
        case EN_FRM_STATE_INIT_2:
            if( f_ui8Data == 'T' )
            {
                m_rxFrame.push_back( f_ui8Data );
                m_enFrmState = EN_FRM_STATE_TYPE;
            }
            else if( f_ui8Data == 'M' )
            {
                // 'M' 'M' 'T': the first 'M' was noise, this one starts the frame.
                m_rxFrame.assign( 1, (char)f_ui8Data );
            }
            else
            {
                m_rxFrame.clear();
                m_enFrmState = EN_FRM_STATE_INIT_1;
            }

            break;
        case EN_FRM_STATE_TYPE:

            m_enPktType  = (enumPacketType)f_ui8Data;
            m_enFrmState = EN_FRM_STATE_DIR;
            m_rxFrame.push_back( f_ui8Data );
            break;
        case EN_FRM_STATE_DIR:
            m_rxFrame.push_back( f_ui8Data );
            m_enPktDir   = (enumPacketDir)f_ui8Data;
            m_enFrmState = EN_FRM_STATE_ID;
            break;
        case EN_FRM_STATE_ID:
            m_rxFrame.push_back( f_ui8Data );
            m_ui8RxCommand = f_ui8Data;
            m_enFrmState   = EN_FRM_STATE_SIZE;
            break;
        case EN_FRM_STATE_SIZE:
            m_rxFrame.push_back( f_ui8Data );
            m_ui8FrameSize = ( f_ui8Data );
            m_enFrmState   = EN_FRM_STATE_DATA;

            // The size byte counts the payload plus the CRC, so the payload is size - 1 bytes. With no
            // payload (size 0 or 1) there is nothing to collect and the CRC follows directly.
            if( m_ui8FrameSize <= 1 )
            {
                m_enFrmState = EN_FRM_STATE_CRC;
            }
            else if( (int)m_ui8FrameSize - 1 > (int)mc_ui8RxFifoSize )
            {
                // Would overrun the RX fifo. Drop the frame and hunt for the next one.
                ClearReceiver();
            }
            break;

        case EN_FRM_STATE_DATA:
            m_rxFrame.push_back( f_ui8Data );
            if( m_ui8RxFifoWritePos < ( m_ui8FrameSize - 1 ) )
            {
                m_ptrui8RxFifo[m_ui8RxFifoWritePos] = f_ui8Data;
                m_ui8RxFifoWritePos++;

                if( m_ui8RxFifoWritePos == ( m_ui8FrameSize - 1 ) )
                {
                    m_enFrmState = EN_FRM_STATE_CRC;
                }
            }
            break;
        case EN_FRM_STATE_CRC:
        {
            uint8_t ui8CrcTemp = CrcGen( m_rxFrame );
            if( ui8CrcTemp == f_ui8Data )
            {
                boRcvdCompleteMsg = TRUE;
                m_enFrmState      = EN_FRM_STATE_INIT_1;
            }
            else
            {
                // Bad CRC: forget the partial payload and resynchronise on the next 'M' 'T'.
                ClearReceiver();
            }
            m_rxFrame.clear(); // also on success: the next frame starts empty
            break;
        }
        default:
            ClearReceiver();
            break;
    }

    if( boRcvdCompleteMsg )
    {
        m_boFrameRcvComplete = TRUE;
    }
}

uint8_t CPktMgr::PopUint8FromRxFifo( void )
{
    return m_ptrui8RxFifo[m_ui8RxFifoReadPos++];
}

uint8_t CPktMgr::PopUint8FromTxFifo( void )
{
    return m_ptrui8TxFifo[m_ui8TxFifoReadPos++];
}

void CPktMgr::PushUint8ToTxFifo( uint8_t f_ui8Data )
{
    m_ptrui8TxFifo[m_ui8TxFifoWritePos++] = f_ui8Data;
}

uint8_t CPktMgr::CrcGen( std::vector<char>& f_txData )
{
    uint8_t ui8CrcValue = 0xff;
    uint8_t j;
    size_t  len = f_txData.size();

    for( size_t i = 0; i < len; i++ )
    {
        ui8CrcValue ^= f_txData[i];
        for( j = 0; j < 8; j++ )
        {
            if( ( ui8CrcValue & 0x80 ) != 0 )
                ui8CrcValue = (uint8_t)( ( ui8CrcValue << 1 ) ^ 0x31 );
            else
                ui8CrcValue <<= 1;
        }
    }
    return ui8CrcValue;
}

void CPktMgr::Reset( CPktMgr::HWMsg_t& f_HwMsg )
{
    f_HwMsg.pktType = (enumPacketType)0;
    f_HwMsg.pktDir  = (enumPacketDir)0;
    f_HwMsg.pktCmd  = 0;
    f_HwMsg.data.clear();
}

void CPktMgr::BuildAndTransmitPacket( CPktMgr::HWMsg_t& f_HwMsg, std::vector<char>& f_txData )
{
    uint8_t ui8TxBufferIndex = 0;
    int     sizeValue        = (int)f_HwMsg.data.size();
    int     txIndex          = 0;

    f_txData.push_back( 'M' );
    f_txData.push_back( 'T' );
    f_txData.push_back( f_HwMsg.pktType );
    f_txData.push_back( f_HwMsg.pktDir );
    f_txData.push_back( f_HwMsg.pktCmd );
    f_txData.push_back( sizeValue + PKTMGR_CRC_BYTE );

    while( sizeValue )
    {
        f_txData.push_back( f_HwMsg.data[txIndex] );
        sizeValue--;
        txIndex++;
    }

    f_txData.push_back( CrcGen( f_txData ) );
    //f_txData.push_back('\n');

    m_ui8TxCommand = m_ui8ArrTransmitBuffer[EN_FRM_STATE_ID];
}
