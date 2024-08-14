//+--------------------------------------------------------------------------
//
// File:        LEDBuffer.h
//
// NDPi - (c) 2024 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of the NightDriver software project.
//
//    NightDriver is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    NightDriver is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with Nightdriver.  It is normally found in copying.txt
//    If not, see <https://www.gnu.org/licenses/>.
//
//
// Description:
//
//   Provides a timestamped buffer of colordata.  The LEDBufferManager keeps
//   N of these buffers in a circular queue, and each has a timestamp on it
//   indicating when it becomes valid.
//
// History:     Aug-14-2024         Davepl      Created from NightDriverStrip
//
//---------------------------------------------------------------------------

#pragma once

#include <memory>
#include <iostream>
#include <vector>
#include "values.h"
#include "globals.h"
#include "pixeltypes.h"
#include "apptime.h"

class LEDBuffer
{
  public:

  private:

    std::vector<CRGB>        _leds;
    const uint64_t           _timeStampMicroseconds;
    const uint64_t           _timeStampSeconds;

  public:

    explicit LEDBuffer(const CRGB * pData, size_t count, uint64_t seconds, uint64_t micros) :
        _timeStampMicroseconds(micros),
        _timeStampSeconds(seconds)
    {   
        _leds.assign(pData, pData+count);
    }

    ~LEDBuffer()
    {
    }

    uint64_t Seconds()      const  { return _timeStampSeconds;      }
    uint64_t MicroSeconds() const  { return _timeStampMicroseconds; }
    uint32_t Length()       const  { return _leds.size();           }
    
    bool IsBufferOlderThan(const timeval & tv) const
    {
        if (Seconds() < (uint64_t) tv.tv_sec)
            return true;

        if (Seconds() == (uint64_t) tv.tv_sec)
            if (MicroSeconds() < (uint64_t) tv.tv_usec)
                return true;

        return false;
    }

    // CreateFromWire
    //
    // Parse a frame from the WiFi data and return it as a constructed LEDBuffer object
    
    std::unique_ptr<LEDBuffer> CreateFromWire(std::unique_ptr<const uint8_t []> & payloadData, size_t payloadLength)   
    {
        constexpr auto minimumPayloadLength = 24;
    
        if (payloadLength < minimumPayloadLength)                 // Our header size
        {
            printf("Not enough data received to process");
            return nullptr;
        }

        uint16_t command16 = WORDFromMemory(&payloadData[0]);
        uint16_t channel16 = WORDFromMemory(&payloadData[2]);
        uint32_t length32  = DWORDFromMemory(&payloadData[4]);
        uint64_t seconds   = ULONGFromMemory(&payloadData[8]);
        uint64_t micros    = ULONGFromMemory(&payloadData[16]);

        //printf("UpdateFromWire -- Command: %u, Channel: %d, Length: %u, Seconds: %u, Micros: %u\n", command16, channel16, length32, seconds, micros);

        const size_t cbHeader = sizeof(command16) + sizeof(channel16) + sizeof(length32) + sizeof(seconds) + sizeof(micros);

        if (payloadLength < length32 * sizeof(CRGB) + cbHeader)
        {
            printf("command16: %d   length32: %d,  payloadLength: %lu\n", command16, length32, payloadLength);
            printf("Data size mismatch");
            return nullptr;
        }

        const CRGB * pData = reinterpret_cast<const CRGB *>(&payloadData[cbHeader]);
        return std::make_unique<LEDBuffer>(pData, length32, seconds, micros);    
    }
};

// LEDBufferManager
//
// Maintains a circular array of LEDBuffer objects, and provides methods to push new buffers
// and pop the oldest buffers.  The buffers are timestamped, and the manager can provide the
// age of the oldest and newest buffers in seconds.

class LEDBufferManager
{
    std::vector<std::unique_ptr<LEDBuffer>> _aptrBuffers;  // The circular array of buffer ptrs
    size_t _iNextBuffer;                                 // Head pointer index
    size_t _iLastBuffer;                                 // Tail pointer index
    size_t _cMaxBuffers;                                    // Number of buffers

    const LEDBuffer * PeekOldestBuffer() const
    {
        if (IsEmpty())
            return nullptr;

        return _aptrBuffers[_iLastBuffer].get();
    }

public:

    explicit LEDBufferManager(size_t cBuffers)
        : _aptrBuffers(cBuffers), _iNextBuffer(0), _iLastBuffer(0), _cMaxBuffers(cBuffers)
    {}

    double AgeOfOldestBuffer() const
    {
        if (!IsEmpty())
        {
            const auto & pOldest = _aptrBuffers[_iLastBuffer].get();
            return (pOldest->Seconds() + pOldest->MicroSeconds() / MICROS_PER_SECOND) - CAppTime::CurrentTime();
        }
        return 0.0;
    }

    double AgeOfNewestBuffer() const
    {
        if (!IsEmpty())
        {
            // Find the index of the newest buffer
            size_t newestIndex = (_iNextBuffer == 0) ? _cMaxBuffers - 1 : _iNextBuffer - 1;
            const auto & pNewest = _aptrBuffers[newestIndex].get();
            return (pNewest->Seconds() + pNewest->MicroSeconds() / MICROS_PER_SECOND) - CAppTime::CurrentTime();
        }
        return 0.0;
    }

    size_t Capacity() const
    {
        return _cMaxBuffers;
    }

    size_t Size() const
    {
        if (_iNextBuffer >= _iLastBuffer)
            return _iNextBuffer - _iLastBuffer;
        else
            return _cMaxBuffers - (_iLastBuffer - _iNextBuffer);
    }

    bool IsFull() const 
    {
        return Size() == Capacity();
    }

    inline bool IsEmpty() const
    {
        return Size() == 0;
    }

    void PushNewBuffer(std::unique_ptr<LEDBuffer> pBuffer)
    {
        if (IsFull())
            _iLastBuffer = (_iLastBuffer + 1) % _cMaxBuffers;  // Move the tail pointer to discard the oldest buffer

        _aptrBuffers[_iNextBuffer] = std::move(pBuffer);
        _iNextBuffer = (_iNextBuffer + 1) % _cMaxBuffers;
    }

    std::unique_ptr<LEDBuffer> PopOldestBuffer()
    {
        if (IsEmpty())
            return nullptr;

        auto pResult = std::move(_aptrBuffers[_iLastBuffer]);
        _iLastBuffer = (_iLastBuffer + 1) % _cMaxBuffers;

        return pResult;
    }
};
