// Where the PDM application (v1.91U) finds its flash data, and the RAM it gives the compiled logic.
#pragma once

#include <cstddef>
#include <cstdint>

namespace pdm::layout {

// ------------------------------------------------------------------ the configuration block
inline constexpr uint16_t kConfig = 0x1E20;  // [0]=0 [1]=9 (format) [2..3]=length, CRC16 in the last 2 bytes
inline constexpr uint8_t kConfigFormat = 9;
inline constexpr uint16_t kCanRxDefs = 0x1E24;  // user CAN receive definitions
inline constexpr uint16_t kCanRxTimeouts = 0x1E40;
inline constexpr uint16_t kCanRxTimeoutEnable = 0x1E47;
inline constexpr uint16_t kUserTxMsg0 = 0x1E48;    // 4 x 45 bytes; byte 0 = rate (0: off)
inline constexpr uint16_t kStreamIdrs = 0x1EFC;    // 6 MSCAN IDR quads: 0x500..0x505
inline constexpr uint16_t kStreamRate = 0x1F14;    // x 10 ms
inline constexpr uint16_t kStreamEnable = 0x1F15;  // bit n: message group n
inline constexpr uint16_t kOutputConfig = 0x1F16;  // 32 x 13 bytes
inline constexpr size_t kOutputConfigBytes = 13;
inline constexpr uint16_t kLogicCodeLengths = 0x20B6;  // 200: pointer increments before each channel's call
inline constexpr uint16_t kLogicInvert = 0x217E;       // 200
inline constexpr uint16_t kLogicCount = 0x2246;
inline constexpr uint16_t kInputStateChannels = 0x2247;  // 23 x 8-bit channel index (0x500 input bits)
inline constexpr uint16_t kFaultResetChannel = 0x2260;   // index hi, lo, mask
inline constexpr uint16_t kOutputShedChannel = 0x2263;   // index hi, lo, mask
inline constexpr uint16_t kOutput9Interlock = 0x2266;
inline constexpr uint16_t kCanDevices = 0x2267;
inline constexpr uint16_t kCanTxEnableChannel = 0x22BB;  // index hi, lo, mask (every 100 ms)
inline constexpr uint16_t kSkipModelCheck = 0x22BE;
inline constexpr uint16_t kCompiledLogic = 0x22BF;  // JSR ,X per channel
inline constexpr uint16_t kFlash2End = 0x8000;

// output config fields (offsets in a 13-byte record)
inline constexpr size_t kOutChannelHi = 0;
inline constexpr size_t kOutChannelLo = 1;
inline constexpr size_t kOutMask = 2;
inline constexpr size_t kOutFailsafe = 3;
inline constexpr size_t kOutTimeoutMask = 4;
inline constexpr size_t kOutRetryDelayHi = 7;  // x 20 ms
inline constexpr size_t kOutRetryDelayLo = 8;
inline constexpr size_t kOutRetries = 9;     // 0xFF: unlimited
inline constexpr size_t kOutI2tScaleHi = 5;  // s = min((I * scale) >> 8, 0xFFF); 0: no I^2t
inline constexpr size_t kOutI2tScaleLo = 6;
inline constexpr size_t kOutI2tDivisor = 11;  // level = (acc >> 4) / divisor: time constant ~1.6 s per unit
inline constexpr size_t kOutFlags = 12;

// ------------------------------------------------------------------ RAM: the channel table and the logic ABI
inline constexpr uint16_t kChannelTable = 0x0114;   // a channel index is an offset from here
inline constexpr uint16_t kLogicState = 0x011C;     // 200 logic channels: channel index 8 + i
inline constexpr uint16_t kInputVoltage = 0x01E4;   // 23 inputs, 0.2 V/bit
inline constexpr uint16_t kOutputVoltage = 0x01FB;  // 32 outputs, 0.2 V/bit
inline constexpr uint16_t kOutputCurrent = 0x021B;  // 32 outputs, 0.5 A/bit (1-8), 0.2 A/bit (9-32)
inline constexpr uint16_t kOutputLoad = 0x023B;
inline constexpr uint16_t kOutputStatus =
    0x0293;                                   // bit0 on, bit1 fault (low voltage), bit2 overcurrent, bit3 retries done
inline constexpr uint16_t kRunMode = 0x02FA;  // 0 normal, 1 error/no config, 2 factory test, 3 sleep
inline constexpr uint16_t kErrorCode = 0x0108;
inline constexpr uint8_t kZpChannelTable = 0x40;  // the logic routines' pointer to the channel table
inline constexpr uint8_t kZpDebounceHi = 0x42;    // the routine's debounce limit (20 ms evaluations)
inline constexpr uint8_t kZpDebounceLo = 0x43;

inline constexpr size_t kInputs = 23;
inline constexpr size_t kOutputs = 32;
inline constexpr size_t kLogicChannels = 200;

// ------------------------------------------------------------------ the current calibration
inline constexpr uint16_t kCurrentCal = 0x7B00;  // [0]=0 [1]=1 [2..3]=length, 32 x {lut[32], gain16, offset16}, CRC16
inline constexpr size_t kCalEntryBytes = 36;

// ------------------------------------------------------------------ factory data
inline constexpr uint16_t kFactory = 0x0462;       // to the end of the block
inline constexpr uint16_t kFactoryModel = 0x0467;  // 0 PDM32, 1 PDM16, 2 PDM30, 3 PDM15
inline constexpr uint16_t kFactorySerial = 0x0464;

// ------------------------------------------------------------------ the application image
inline constexpr uint16_t kAppEntry = 0x8006;         // the application entry
inline constexpr uint16_t kVectorRedirects = 0xC33B;  // 23 x JMP, one per vector
inline constexpr uint16_t kBootHardwareRevision = 0xFD80;

}  // namespace pdm::layout
