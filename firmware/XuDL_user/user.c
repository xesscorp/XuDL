//*********************************************************************
// Copyright (C) 2011 Dave Vanden Bout / XESS Corp. / www.xess.com
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or (at
// your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St, Fifth Floor, Boston, MA 02110, USA
//
//====================================================================
//
// Module Description:
//  This module manages the USB interface.
//
//********************************************************************

#include "USB/usb.h"
#include "USB/usb_function_generic.h"
#include "HardwareProfile.h"
#include "GenericTypeDefs.h"
#include "version.h"
#include "user.h"
#include "usbcmd.h"
#include "eeprom_flags.h"
#include "utils.h"
#include "blinker.h"
#include "sdcard.h"

// Information structure for device.
typedef struct DEVICE_INFO
{
    CHAR8 product_id[2];
    CHAR8 version_id[2];
    struct
    {
        // description string is size of max. USB packet minus storage for
        // product ID, device ID, checksum and command.
        CHAR8 str[USBGEN_EP_SIZE - 2 - 2 - 1 - 1];
    }     desc;
    CHAR8 checksum;
} DEVICE_INFO;

// USB data packet definitions
typedef union DATA_PACKET
{
    BYTE _byte[USBGEN_EP_SIZE];     //For byte access.
    WORD _word[USBGEN_EP_SIZE / 2];   //For word access.
    DWORD _dword[USBGEN_EP_SIZE / 4]; // For double-word access.
    struct
    {
        USBCMD cmd;
        BYTE   len;
    };
    struct
    {
        USBCMD      cmd;
        DEVICE_INFO device_info;
    };
    struct // EEPROM read/write structure
    {
        USBCMD cmd;
        BYTE len;
        union
        {
            rom far char *pAdr; // Address pointer
            struct
            {
                BYTE low;   // Little-endian order
                BYTE high;
                BYTE upper;
            };
        } ADR;
        BYTE data[USBGEN_EP_SIZE - 5];
    };
} DATA_PACKET;

#define NUM_ACTIVITY_BLINKS 10          // Indicate activity by blinking the LED this many times.

#pragma romdata
static const rom DEVICE_INFO device_info
    = {
    PRODUCT_ID,
    MAJOR_VERSION, MINOR_VERSION,         // Version.
    { "XuDL" },         // Description string.
    0x00                // Checksum (filled in later).
    }; // Change version in usb_descriptors.c as well!!

#pragma udata access my_access
static near DWORD lcntr;                    // Large counter for fast loops.
static near BYTE buffer_cntr;               // Holds the number of bytes left to process in the USB packet.
static near WORD save_FSR0, save_FSR1;      // Used for saving the contents of PIC hardware registers.

#pragma udata
static USB_HANDLE OutHandle[2] = {0,0}; // Handles to endpoint buffers that are receiving packets from the host.
static BYTE OutIndex           = 0;     // Index of endpoint buffer has received a complete packet from the host.
static DATA_PACKET *OutPacket;          // Pointer to the buffer with the most-recently received packet.
static BYTE OutPacketLength    = 0;     // Length (in bytes) of most-recently received packet.
static USB_HANDLE InHandle[2]  = {0,0}; // Handles to ping-pong endpoint buffers that are sending packets to the host.
static BYTE InIndex            = 0;     // Index of the endpoint buffer that is currently being filled before being sent to the host.
static DATA_PACKET *InPacket;           // Pointer to the buffer that is currently being filled.

#pragma udata usbram2
static DATA_PACKET InBuffer[2];     // Ping-pong buffers in USB RAM for sending packets to host.
static DATA_PACKET OutBuffer[2];    // Ping-pong buffers in USB RAM for receiving packets from host.


#pragma code

BYTE ReadEeprom(BYTE address)
{
    EECON1 = 0x00;
    EEADR = address;
    EECON1bits.RD = 1;
    return EEDATA;
}

void WriteEeprom(BYTE address, BYTE data)
{
    EEADR = address;
    EEDATA = data;
    EECON1 = 0b00000100;    //Setup writes: EEPGD=0,WREN=1
    EECON2 = 0x55;
    EECON2 = 0xAA;
    EECON1bits.WR = 1;
    while(EECON1bits.WR);       //Wait till WR bit is clear
}

void UserInit( void )
{
    BYTE i;

    // Enable high slew-rate for the I/O pins.
    SLRCON = 0;

	// Turn off analog input mode on I/O pins.
    ANSEL = 0;
    ANSELH = 0;

    // Initialize the I/O pins.
    INIT_GPIO0();
    INIT_GPIO1();
    INIT_GPIO2();
    INIT_GPIO3();

    #if defined( USE_USB_BUS_SENSE_IO )
    tris_usb_bus_sense = INPUT_PIN;
    #endif

    InitBlinker();  // Initialize LED status blinker.
    InitSd(); // Initialize SD card.
#if 0
	INIT_TX();
	INIT_RX();
	while(1)
		TX = !TX;
#endif

    // Initialize interrupts.
    RCONbits.IPEN     = 1;      // Enable prioritized interrupts.
    INTERRUPTS_ON();            // Enable high and low-priority interrupts.
}



// This function is called when the device becomes initialized, which occurs after the host sends a
// SET_CONFIGURATION (wValue not = 0) request.  This callback function should initialize the endpoints
// for the device's usage according to the current configuration.
void USBCBInitEP( void )
{
    // Enable the endpoint.
    USBEnableEndpoint( USBGEN_EP_NUM, USB_OUT_ENABLED | USB_IN_ENABLED | USB_HANDSHAKE_ENABLED | USB_DISALLOW_SETUP );
    // Now begin waiting for the first packets to be received from the host via this endpoint.
    OutIndex = 0;
    OutHandle[0] = USBGenRead( USBGEN_EP_NUM, (BYTE *)&OutBuffer[0], USBGEN_EP_SIZE );
    OutHandle[1] = USBGenRead( USBGEN_EP_NUM, (BYTE *)&OutBuffer[1], USBGEN_EP_SIZE );
    // Initialize the pointer to the buffer which will return data to the host via this endpoint.
    InIndex = 0;
    InPacket  = &InBuffer[0];
}



void ProcessIO( void )
{
    if ( ( USBGetDeviceState() < CONFIGURED_STATE ) || USBIsDeviceSuspended() )
        return;

    ServiceRequests();
}



void ServiceRequests( void )
{
    BYTE num_return_bytes;          // Number of bytes to return in response to received command.
    BYTE cmd;                     // Store the command in the received packet.

    // Process packets received through the primary endpoint.
    if ( !USBHandleBusy( OutHandle[OutIndex] ) )
    {
        num_return_bytes = 0;   // Initially, assume nothing needs to be returned.

        // Got a packet, so start getting another packet while we process this one.
        OutPacket        = &OutBuffer[OutIndex]; // Store pointer to just-received packet.
        OutPacketLength  = USBHandleGetLength( OutHandle[OutIndex] );   // Store length of received packet.
        cmd              = OutPacket->cmd;

        blink_counter    = NUM_ACTIVITY_BLINKS; // Blink the LED whenever a USB transaction occurs.

        switch ( cmd )  // Process the contents of the packet based on the command byte.
        {
            case ID_BOARD_CMD:
                // Blink the LED in order to identify the board.
                blink_counter                  = 50;
                InPacket->cmd                  = cmd;
                num_return_bytes               = 1;
                break;

            case INFO_CMD:
//                memcpy( ( void * )( (BYTE *)InPacket ), (void *)&sdBuf, sizeof( sdBuf ) );
                InPacket->_dword[0] = numBlocks;
                InPacket->_word[2] = rdBlockSize;
                InPacket->_word[3] = wrBlockSize;
                InPacket->_word[4] = eraseSize;
                num_return_bytes = 16;
                break;
                // Return a packet with information about this USB interface device.
                InPacket->cmd                  = cmd;
                memcpypgm2ram( ( void * )( (BYTE *)InPacket + 1 ), (const rom void *)&device_info, sizeof( DEVICE_INFO ) );
//                InPacket->device_info.checksum = calc_checksum( (CHAR8 *)InPacket, sizeof( DEVICE_INFO ) );
                num_return_bytes               = sizeof( DEVICE_INFO ) + 1; // Return information stored in packet.
                break;

            case READ_EEDATA_CMD:
                InPacket->cmd = OutPacket->cmd;
                for(buffer_cntr=0; buffer_cntr < OutPacket->len; buffer_cntr++)
                {
                    InPacket->data[buffer_cntr] = ReadEeprom((BYTE)OutPacket->ADR.pAdr + buffer_cntr);
                }
                num_return_bytes = buffer_cntr + 5;
                break;

            case WRITE_EEDATA_CMD:
                InPacket->cmd = OutPacket->cmd;
                for(buffer_cntr=0; buffer_cntr < OutPacket->len; buffer_cntr++)
                {
                    WriteEeprom((BYTE)OutPacket->ADR.pAdr + buffer_cntr, OutPacket->data[buffer_cntr]);
                }
                num_return_bytes = 1;
                break;

            case RESET_CMD:
                // When resetting, make sure to drop the device off the bus
                // for a period of time. Helps when the device is suspended.
                UCONbits.USBEN = 0;
                lcntr = 0xFFFF;
                for(lcntr = 0xFFFF; lcntr; lcntr--)
                    ;
                Reset();
                break;

            default:
                num_return_bytes = 0;
                break;
        } /* switch */

        // This command packet has been handled, so get another.
        OutHandle[OutIndex] = USBGenRead( USBGEN_EP_NUM, (BYTE *)&OutBuffer[OutIndex], USBGEN_EP_SIZE );
        OutIndex ^= 1; // Point to next ping-pong buffer.

        // Packets of data are returned to the PC here.
        // The counter indicates the number of data bytes in the outgoing packet.
        if ( num_return_bytes != 0U )
        {
            InHandle[InIndex] = USBGenWrite( USBGEN_EP_NUM, (BYTE *)InPacket, num_return_bytes ); // Now send the packet.
            InIndex ^= 1;
            while ( USBHandleBusy( InHandle[InIndex] ) )
            {
                ;                           // Wait until transmitter is not busy.
            }
            InPacket = &InBuffer[InIndex];
        }
    }
} /* ServiceRequests */
