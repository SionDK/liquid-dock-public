#pragma once
#include <cstdint>

// Shared memory written by LiquidDockSensors.exe (elevated) and read by the dock.
#define LD_SENSOR_MAP   L"Local\\LiquidDockSensors"
#define LD_SENSOR_TASK  L"LiquidDockSensors"
// Exists while the dock wants the helper's sensor readings (CPU temperature on): the helper also runs
// for the windows of programs run as administrator (admin_protocol.h) and samples only when asked.
#define LD_SENSOR_WANT  L"Local\\LiquidDock.WantSensors"
#define LD_SENSOR_MAGIC 0x4C445332u

enum : uint32_t {
    LD_HAS_CPU_TEMP = 1u << 0,
    LD_HAS_GPU      = 1u << 1,
    LD_HAS_SSD_TEMP = 1u << 2,
    LD_HAS_GPU_LOAD = 1u << 3,
    // set by the dock's own sampler only
    LD_HWINFO           = 1u << 4,   // HWiNFO is running and reporting to its Gadget key
    LD_CPU_TEMP_HWINFO  = 1u << 5,   // cpuTemp comes from HWiNFO
};

struct LDSensorBlock {
    uint32_t magic;
    uint32_t size;
    volatile uint32_t seq;     // seqlock: odd while writing
    uint32_t flags;
    uint64_t tick;             // GetTickCount64() of the last write
    float cpuTemp, cpuLoad;                         // °C, %
    float gpuTemp, gpuLoad, gpuMemUsed, gpuMemTotal; // °C, %, GB, GB
    float ramUsed, ramTotal, ramLoad;               // GB, GB, %
    float ssdTemp, ssdActive, ssdUsed, ssdTotal;    // °C, %, GB, GB (system drive)
    wchar_t cpuName[64];
    wchar_t gpuName[64];
    wchar_t ssdName[64];
    wchar_t status[128];       // human readable problem, empty when fine
};
