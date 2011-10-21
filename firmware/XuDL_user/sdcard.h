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
//  This module manages the LED blinker.
//
//********************************************************************

#ifndef SDCARD_H
#define SDCARD_H

#include "GenericTypeDefs.h"

#define READ_BLOCK 1
#define WRITE_BLOCK 2

extern DWORD numBlocks;
extern WORD wrBlockSize;
extern WORD rdBlockSize;
extern BYTE eraseSize;

BYTE SendSdCmd(
    BYTE  cmd,
    DWORD arg );
void ReadSdReg( BYTE cmd );
BYTE GetSdParameters( void );
BYTE InitSd( void );

#endif
