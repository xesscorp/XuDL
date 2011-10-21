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
//  This module manages the interface to the SD card.
//
//********************************************************************

#include <spi.h>
#include <delays.h>
#include "HardwareProfile.h"
#include "GenericTypeDefs.h"
#include "utils.h"
#include "sdcard.h"

// SD memory card commands.
#define GO_IDLE_STATE 0x00
#define SEND_CSD 0x09
#define SEND_CID 0x0a
#define READ_SINGLE_BLOCK 0x11
#define WRITE_SINGLE_BLOCK 0x18
#define APP_CMD 0x37
#define INIT_CARD 0x29

// SD card parameters.
DWORD numBlocks;  // Number of blocks in SD card.
WORD wrBlockSize; // Size of write block in bytes.
WORD rdBlockSize; // Size of read block in bytes.
BYTE eraseSize;   // Size of erase block in units of write blocks.

// Current SD card block status.
struct
{
    unsigned open  : 1; // TRUE when open.
    unsigned write : 1; // TRUE when block is open for writing.
    unsigned read  : 1; // TRUE when block is open for reading.
    DWORD    blockIdx; // Index of the currently open SD card block.
    WORD     ptr; // Index into the currently open SD card block.
}
currSdBlockStatus = { 0, };


// Send command to SD card and return the R1 reply.
BYTE SendSdCmd(
    BYTE  cmd,
    DWORD arg )
{
    SD_CS = 0;              // Select the SD card.
    WriteSPI( 0xFF );       // Wake it up.
    WriteSPI( cmd | 0x40 ); // Send command.
    WriteSPI( arg >> 24 );  // Send 32-bit argument, MSB first.
    WriteSPI( arg >> 16 );
    WriteSPI( arg >> 8 );
    WriteSPI( arg );  // Send last byte of argument.
    WriteSPI( 0x95 ); // Send hard-coded CRC (CRC not used after first command).
    WriteSPI( 0xFF ); // Send null so device gets a few clocks before responding.
    return ReadSPI(); // Return the R1 reply from the SD card.
}



// Wait for a token signaling the start of data from the SD card.
BYTE PrepSdForDataIo( BYTE cmd, DWORD arg )
{
    BYTE retries = 0;
    BYTE result = SendSdCmd( cmd, arg );
    while ( result != 0 )
    {
        if ( ++retries >= 0xFF )
            return 1;  // Timed out.

        result = ReadSPI(); // Try reading again.
    }
    for ( retries = 0; retries < 0xFF; retries++ )
    {
        BYTE token = ReadSPI();
        if ( token == 0xFE )
            return 0;  // Success!

        if ( token != 0xFF )
            return token;  // Some type of error occurred.
    }
    return 0b10000000; // Timed out - no token was received.
}



// Get SD card parameters.
BYTE GetSdParameters( void )
{
    BYTE i;
    BYTE result;
    BYTE retries;
    BYTE csd_buff[16];
    WORD c_size;

    // Prepare for reading card-specific data.
    if(PrepSdForDataIo(SEND_CSD,0) != 0)
        return 1; // Failure.

    // Read card-specific data.
    for ( i = 0; i < sizeof( csd_buff ); i++ )
        csd_buff[i] = ReadSPI();
    // Get #blocks, block write size, block read size.
    c_size        = csd_buff[6] & 0b11;
    c_size      <<= 8;
    c_size       |= csd_buff[7];
    c_size      <<= 2;
    c_size       |= ( csd_buff[8] >> 6 );
    numBlocks     = c_size + 1;
    numBlocks   <<= ( ( ( ( csd_buff[9] & 0b11 ) << 1 ) | ( csd_buff[10] >> 7 ) ) + 2 );
    wrBlockSize   = 1;
    wrBlockSize <<= ( ( ( csd_buff[12] & 0b11 ) << 2 ) | ( csd_buff[13] >> 6 ) );
    rdBlockSize   = 1;
    rdBlockSize <<= ( csd_buff[5] & 0b1111 );
    if(csd_buff[10] & 0b01000000)
    {
        eraseSize = 512 / wrBlockSize;
        eraseSize = 0x45;
    }
    else
    {
        eraseSize = (csd_buff[10] & 0b00111111) << 1;
        if(csd_buff[11] & 0b10000000)
            eraseSize++;
        eraseSize++;
        eraseSize = 0x12;
    }
    return 0;
} /* GetSdParameters */



// Initialize the interface to the SD card.
BYTE InitSd( void )
{
    BYTE retries;

    // Initialize the SPI port to the SD card.
    SD_CS_INIT(); // Initialize the SD card chip-select.
    SD_CS = 1;
    OpenSPI( SPI_FOSC_64, MODE_00, SMPMID ); // Use a slow clock (48 MHz / 64 = 750 KHz), CKE=1, CKP=0, SMP=0.

    // Wait for SD card to start by sending 0xFF with chip-select high.
    for ( retries = 0; retries < 10; retries++ )
        WriteSPI( 0xFF );

    // Force SD card into SPI mode.
    retries = 0;
    while ( SendSdCmd( GO_IDLE_STATE, 0 ) != 0x01 )
        if ( ++retries >= 0xFF )
            return 1;

    // Speed-up the SPI port after SD card is in SPI mode.
    OpenSPI( SPI_FOSC_4, MODE_00, SMPMID );

    // Initialize the SD card.
    retries = 0;
    if ( SendSdCmd( APP_CMD, 0 ) == 0xFF )
        return 3;

    while ( SendSdCmd( INIT_CARD, 0 ) != 0x00 )
    {
        if ( ++retries >= 0xFF )
            return 2;

        if ( SendSdCmd( APP_CMD, 0 ) == 0xFF )
            return 3;
    }

    // Read parameters from SD card.
    if ( GetSdParameters() != 0 )
        return 4;  // Error reading SD card parameters.

    return 0; // Success!
} /* InitSd */



// Open the SD card for reading or writing.
BYTE OpenSd(
    DWORD blockIdx,
    BYTE  mode )
{
    currSdBlockStatus.open  = FALSE;
    currSdBlockStatus.read  = FALSE;
    currSdBlockStatus.write = FALSE;
    currSdBlockStatus.ptr   = 0;
    if ( blockIdx < numBlocks )
    {
        currSdBlockStatus.blockIdx = blockIdx;
        if ( mode == READ_BLOCK )
        {
            if(PrepSdForDataIo(READ_SINGLE_BLOCK,blockIdx*rdBlockSize) != 0)
                return 1; // Failure.
            currSdBlockStatus.read = TRUE;
        }
        else if ( mode == WRITE_BLOCK )
        {
            if(PrepSdForDataIo(WRITE_SINGLE_BLOCK,blockIdx*wrBlockSize) != 0)
                return 2; // Failure.
            currSdBlockStatus.write = TRUE;
        }
        currSdBlockStatus.open     = TRUE;
        return 0; // Success.
    }
    return 3; // Failure.
} /* OpenSd */



// Write data to SD card block.
BYTE WriteSd(
    BYTE *wrBuff,
    WORD num )
{
}



// Read data from SD card block.
BYTE ReadSd(
    BYTE *rdBuff,
    WORD *num )
{
}



// Close the SD card.
BYTE CloseSd( void )
{
    currSdBlockStatus.open  = FALSE;
    currSdBlockStatus.read  = FALSE;
    currSdBlockStatus.write = FALSE;
    currSdBlockStatus.ptr   = 0;
}
