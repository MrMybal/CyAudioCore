#pragma once
#include <cydsp/CyDSP.h>
#include <cydsp/core/AudioBuffer.h>
#include <cydsp/core/AudioLayout.h>
#include <cydsp/core/Cpu.h>
#include <cydsp/core/SpscQueue.h>
#include <cydsp/math/VectorOps.h>
#include <cydsp/processors/Generators.h>
#include <cydsp/processors/ChannelMatrix.h>
#include <cydsp/processors/SincInterpolator.h>
#include <cydsp/processors/TimeStretch.h>
#include <cydsp/synthesis/Wavetable.h>
#include <cydsp/synthesis/SampleData.h>
// A source checkout bypasses the package version check, so state the requirement here too.
static_assert(CYDSP_VERSION_MAJOR > 0 || CYDSP_VERSION_MINOR >= 3,
              "CyAudioCore requires CyDSP 0.3 or later (Adsr::process, SampleData::render)");
// Identical CyDSP types; no duplicate storage, conversion or numerical kernel.
namespace cyaudio {
using cydsp::Sample;
using cydsp::Status;
using cydsp::ProcessSpec;
using cydsp::CpuBackend;
using cydsp::Architecture;
using cydsp::CpuFeatures;
using cydsp::AudioBlock;
using cydsp::AudioBuffer;
using cydsp::VectorOps;
using cydsp::SpscQueue;
using cydsp::Adsr;
using cydsp::AdsrParameters;
using cydsp::Waveform;
using cydsp::Oscillator;
using cydsp::Wavetable;
using cydsp::WavetableOscillator;
using cydsp::SampleData;
using cydsp::SampleInterpolation;
using cydsp::ChannelMatrix;
using cydsp::defaultChannelMatrix;
using cydsp::SincInterpolator;
using cydsp::TimeStretch;
using cydsp::isFinite;
using cydsp::isPowerOfTwo;
using cydsp::validateProcessBlocks;
using cydsp::validateDisjointBlocks;
using cydsp::interleave;
using cydsp::deinterleave;
using cydsp::detectCpuFeatures;
using cydsp::backendName;
}
