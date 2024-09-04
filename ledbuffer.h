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
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include "values.h"
#include "globals.h"
#include "pixeltypes.h"
#include "apptime.h"

// A custom exception that is thrown if data can't be parsed from the wire

class LEDBufferException : public std::runtime_error 
{
  public:
    explicit LEDBufferException(const std::string& message) : std::runtime_error(message) {}
};

// LEDBuffer
//
// Represents a frame of LED data with a timestamp.  The data is a vector of CRGB objects.
// The timestamp is in seconds and microseconds since the epoch.

class LEDBuffer
{
  public:

  private:

    std::deque<CRGB> _leds;
    const uint64_t    _timeStampMicroseconds;
    const uint64_t    _timeStampSeconds;

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

    constexpr uint64_t Seconds()      const  { return _timeStampSeconds;      }
    constexpr uint64_t MicroSeconds() const  { return _timeStampMicroseconds; }
    
    const std::deque<CRGB> & ColorData() const
    {
        return _leds;
    }

    // CreateFromWire
    //
    // Parse a frame from the WiFi data and return it as a constructed LEDBuffer object
    
    static std::unique_ptr<LEDBuffer> CreateFromWire(std::span<const uint8_t> payload)   
    {
        const     auto payloadData = payload.data();
        const     auto payloadLength = payload.size();
        constexpr auto minimumPayloadLength = 24;

        if (payloadLength < minimumPayloadLength) // Our header size
        {
            throw LEDBufferException("Not enough data received to process");
        }

        uint16_t command16 = WORDFromMemory(&payloadData[0]);
        uint16_t channel16 = WORDFromMemory(&payloadData[2]);
        uint32_t length32  = DWORDFromMemory(&payloadData[4]);
        uint64_t seconds   = ULONGFromMemory(&payloadData[8]);
        uint64_t micros    = ULONGFromMemory(&payloadData[16]);

        constexpr size_t cbHeader = sizeof(command16) + sizeof(channel16) + sizeof(length32) + sizeof(seconds) + sizeof(micros);

        if (payloadLength < length32 * sizeof(CRGB) + cbHeader)
            throw LEDBufferException("Data size mismatch: insufficient data for expected length");

        // Create and return a new LEDBuffer object by passing in the CRGB color data, length, and timestamp
        return std::make_unique<LEDBuffer>(reinterpret_cast<const CRGB *>(&payloadData[cbHeader]), length32, seconds, micros);
    }
};

// LEDBufferManager
//
// Maintains a circular array of LEDBuffer objects and provides methods to push new buffers
// and pop the oldest buffers.  The buffers are timestamped, and the manager can provide the
// age of the oldest and newest buffers in seconds.

class LEDBufferManager
{
    std::deque<std::unique_ptr<LEDBuffer>> _buffers;  // The deque of buffer pointers
    size_t _cMaxBuffers;                              // Maximum number of buffers
    mutable std::mutex _mutex;              // Recursive mutex to protect the buffer

public:
    explicit LEDBufferManager(size_t cBuffers)
        : _cMaxBuffers(cBuffers)
    {}

    double AgeOfOldestBuffer() const
    {
        std::lock_guard<std::mutex> lock(_mutex);  // Lock the mutex before accessing the buffer

        if (!_buffers.empty())
        {
            const auto & pOldest = _buffers.front().get();
            return (pOldest->Seconds() + pOldest->MicroSeconds() / (double)MICROS_PER_SECOND) - CAppTime::CurrentTime();
        }
        return MAXDOUBLE;
    }

    double AgeOfNewestBuffer() const
    {
        std::lock_guard<std::mutex> lock(_mutex);  // Lock the mutex before accessing the buffer

        if (!_buffers.empty())
        {
            const auto & pNewest = _buffers.back();
            return (pNewest->Seconds() + pNewest->MicroSeconds() / (double)MICROS_PER_SECOND) - CAppTime::CurrentTime();
        }
        return MAXDOUBLE;
    }

    constexpr size_t Capacity() const
    {
        return _cMaxBuffers;
    }

    size_t Size() const
    {
        return _buffers.size();
    }


    // PopOldestBuffer
    //
    // Uses move semantics to return ownership of the oldest buffer
    
    std::optional<std::unique_ptr<LEDBuffer>> PopOldestBuffer()
    {
        std::lock_guard<std::mutex> lock(_mutex);  // Lock the mutex before modifying the buffer

        if (_buffers.empty()) 
            return std::nullopt;  // Return empty optional if the buffer is empty

        auto pResult = std::move(_buffers.front());
        _buffers.pop_front();  // Remove the oldest buffer from the front

        return std::optional<std::unique_ptr<LEDBuffer>>(std::move(pResult));
    }

    // PushNewBuffer
    //
    // Uses move semantics to take ownership of the incoming buffer

    void PushNewBuffer(std::unique_ptr<LEDBuffer> pBuffer)
    {
        std::lock_guard<std::mutex> lock(_mutex);  // Lock the mutex before modifying the buffer

        // If the queue is full, pop the oldest buffer to make space
        if (_buffers.size() == _cMaxBuffers)
            _buffers.pop_front();

        // Add the new buffer to the back of the deque
        _buffers.push_back(std::move(pBuffer));
    }
};
