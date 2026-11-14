// CyDSP RIFF metadata bounds/preservation adaptation; third_party/licenses/choc.txt
// CyAudioCore changes: exact integer PCM conversion, write-failure reporting, tolerant
// (still bounded) chunk parsing, byte-exact legacy text chunks, RF64 ds64 details.
//
//    ██████ ██   ██  ██████   ██████
//   ██      ██   ██ ██    ██ ██            ** Classy Header-Only Classes **
//   ██      ███████ ██    ██ ██
//   ██      ██   ██ ██    ██ ██           https://github.com/Tracktion/choc
//    ██████ ██   ██  ██████   ██████
//
//   CHOC is (C)2022 Tracktion Corporation, and is offered under the terms of the ISC license:
//
//   Permission to use, copy, modify, and/or distribute this software for any purpose with or
//   without fee is hereby granted, provided that the above copyright notice and this permission
//   notice appear in all copies. THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
//   WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
//   AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
//   CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
//   WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
//   CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#ifndef CYDSP_PRIVATE_WAV_CODEC_INCLUDED
#define CYDSP_PRIVATE_WAV_CODEC_INCLUDED

#include <choc/platform/choc_Assert.h>
#include <choc/audio/choc_AudioFileFormat.h>
#include <choc/audio/choc_AudioSampleData.h>
#include <choc/memory/choc_Base64.h>

namespace choc::audio
{

//==============================================================================
/**
    An AudioFormat class which can read and write WAV files.

    The template parameter lets you choose whether the format can create
    writers or not - if you only need to read and not write, then using a
    CyDSPWavFormat<false> will avoid bloating your binary with unused
    code.
*/
template <bool supportWriting>
class CyDSPWavFormat  : public AudioFileFormat
{
public:
    CyDSPWavFormat() = default;
    ~CyDSPWavFormat() override {}

    std::string getFileSuffixes() override;
    uint32_t getMaximumNumChannels() override;
    std::vector<double> getSupportedSampleRates() override;
    std::vector<BitDepth> getSupportedBitDepths() override;
    std::vector<std::string> getQualityLevels() override;
    bool supportsWriting() const override;

    std::unique_ptr<AudioFileReader> createReader (std::shared_ptr<std::istream>) override;
    std::unique_ptr<AudioFileWriter> createWriter (std::shared_ptr<std::ostream>, AudioFileProperties) override;
    using AudioFileFormat::createReader;
    using AudioFileFormat::createWriter;

private:
    struct Implementation;
};


//==============================================================================
//        _        _           _  _
//     __| |  ___ | |_   __ _ (_)| | ___
//    / _` | / _ \| __| / _` || || |/ __|
//   | (_| ||  __/| |_ | (_| || || |\__ \ _  _  _
//    \__,_| \___| \__| \__,_||_||_||___/(_)(_)(_)
//
//   Code beyond this point is implementation detail...
//
//==============================================================================

template <bool supportWriting>
struct CyDSPWavFormat<supportWriting>::Implementation
{
    struct FormatException {};
    [[noreturn]] static void throwFormatException()    { throw FormatException(); }

    //==============================================================================
    struct WAVReader : public AudioFileReader
    {
        WAVReader (std::shared_ptr<std::istream> s)  : stream (std::move (s))
        {
            streamEndPosition = getStreamEndPosition();
        }

        bool initialise()
        {
            properties.formatName = "WAV";
            stream->exceptions (std::istream::failbit);

            try
            {
                auto chunk = readChunkType();

                if (chunk == "RIFF") { readMainChunk (readChunkRange(), false); return true; }
                if (chunk == "RF64") { readMainChunk (readChunkRange(), true);  return true; }
            }
            catch (...) {}

            return false;
        }

        const AudioFileProperties& getProperties() override   { return properties; }

        bool readFrames (uint64_t frameIndex, choc::buffer::ChannelArrayView<float> buffer) override
        {
            return readFramesForType (frameIndex, buffer);
        }

        bool readFrames (uint64_t frameIndex, choc::buffer::ChannelArrayView<double> buffer) override
        {
            return readFramesForType (frameIndex, buffer);
        }

        template <typename FloatType>
        bool readFramesForType (uint64_t frameIndex, choc::buffer::ChannelArrayView<FloatType> buffer)
        {
            auto numChannels = buffer.getNumChannels();

            if (numChannels != properties.numChannels)
                return false;

            if (auto numFrames = buffer.getNumFrames())
            {
                if (frameIndex + numFrames > properties.numFrames)
                {
                    if (frameIndex >= properties.numFrames)
                    {
                        buffer.clear();
                        return true;
                    }

                    auto trailingEmptyFrames = static_cast<choc::buffer::FrameCount> (frameIndex + numFrames - properties.numFrames);
                    buffer.getEnd (trailingEmptyFrames).clear();
                    numFrames -= trailingEmptyFrames;
                    buffer = buffer.getStart (numFrames);
                }

                seek (dataChunk.start + frameIndex * frameStride);

                // CyAudioCore: one retained block (many frames per stream call at any channel count).
                auto* const rawData = rawBuffer.data();
                const auto rawDataFrames = static_cast<uint32_t> (rawBuffer.size() / frameStride);

                while (numFrames != 0)
                {
                    auto framesToDo = std::min (rawDataFrames, numFrames);
                    stream->read (rawData, static_cast<std::streamsize> (framesToDo * frameStride));
                    auto dest = buffer.getStart (framesToDo);

                    switch (properties.bitDepth)
                    {
                        // CyAudioCore: exact x / 2^(bits-1) normalisation (see decodeIntegers).
                        case BitDepth::int8:
                        case BitDepth::int16:
                        case BitDepth::int24:
                        case BitDepth::int32:   decodeIntegers (dest, rawData, sampleStride); break;

                        case BitDepth::float32:
                        {
                            reverseEndiannessIfNeeded<uint32_t> (rawData, framesToDo, numChannels);
                            copy (dest, choc::buffer::createInterleavedView (reinterpret_cast<float*> (rawData), numChannels, framesToDo));
                            break;
                        }

                        case BitDepth::float64:
                        {
                            reverseEndiannessIfNeeded<uint64_t> (rawData, framesToDo, numChannels);
                            copy (dest, choc::buffer::createInterleavedView (reinterpret_cast<double*> (rawData), numChannels, framesToDo));
                            break;
                        }

                        case BitDepth::unknown:
                        default:                return false;
                    }

                    buffer = buffer.fromFrame (framesToDo);
                    numFrames -= framesToDo;
                }
            }

            return true;
        }

        struct ChunkRange
        {
            uint64_t start = 0, length = 0;
            uint64_t getEnd() const                { return start + length; }
            uint64_t getNextChunkStart() const     { return start + length + (length & 1u); }
        };

        void readMainChunk (ChunkRange mainChunkRange, bool isRF64)
        {
            if (mainChunkRange.length < 4 || readChunkType() != "WAVE")
                throwFormatException();

            if (isRF64)
            {
                if (readChunkType() != "ds64")
                    throwFormatException();

                auto chunkRange = readChunkRange();

                if (chunkRange.length < 28)
                    throwFormatException();

                mainChunkRange.length = readInt<uint64_t>();
                ds64DataSize = readInt<uint64_t>();
                seek (chunkRange.getNextChunkStart());
            }

            // CyAudioCore tolerance for benign real-world deviations. Every range stays
            // bounded by the stream: a declared RIFF size beyond the end (truncated or
            // still-growing file) is clamped, never trusted.
            if (mainChunkRange.start > streamEndPosition)
                throwFormatException();
            mainChunkRange.length = std::min (mainChunkRange.length, streamEndPosition - mainChunkRange.start);
            const auto mainEnd = mainChunkRange.getEnd();
            bool foundFormat = false, foundData = false;
            uint64_t metadataBytes = 0;
            uint32_t chunks = 0;
            while (getPosition() < mainEnd)
            {
                if (mainEnd - getPosition() < 8) break; // Trailing bytes that cannot hold a header.
                if (++chunks > 65536) throwFormatException();
                auto type = readChunkType();
                auto chunkRange = readChunkRange();
                // EBU 3306: the ds64 size replaces only a 0xffffffff placeholder.
                if (isRF64 && type == "data" && chunkRange.length == 0xffffffffu)
                    chunkRange.length = ds64DataSize;
                const bool truncated = chunkRange.length > mainEnd - chunkRange.start;

                if (type == "fmt ")
                {
                    if (foundFormat || truncated || chunkRange.length < 16) throwFormatException();
                    foundFormat = true;
                    readChunk_fmt (chunkRange);
                    if (getPosition() > chunkRange.getEnd()) throwFormatException();
                }
                else if (type == "data")
                {
                    if (foundData) throwFormatException();
                    foundData = true;
                    if (truncated) chunkRange.length = mainEnd - chunkRange.start; // Truncated recording.
                    dataChunk = chunkRange;
                }
                else if (type != "JUNK" && ! truncated)
                {
                    readMetadataChunk (type, chunkRange, metadataBytes);
                }

                if (truncated) break;
                const auto next = chunkRange.getNextChunkStart();
                if (next >= mainEnd) break; // Also accepts a missing final pad byte.
                seek (next);
            }
            if (!foundFormat || !foundData || !frameStride)
                throwFormatException();
            dataChunk.length -= dataChunk.length % frameStride; // A partial last frame is dropped.
            properties.numFrames = dataChunk.length / frameStride;
            rawBuffer.resize (std::max<size_t> (frameStride, (65536u / frameStride) * frameStride));
        }

        // Optional chunks never make the audio unreadable: a damaged, undersized or
        // oversized one is dropped (not clamped to unrelated data) and parsing continues.
        void readMetadataChunk (const std::string& type, ChunkRange chunkRange, uint64_t& metadataBytes)
        {
            const auto minimum = type == "bext" ? 602u : type == "smpl" ? 36u :
                type == "cue " || type == "LIST" ? 4u :
                type == "inst" || type == "INST" ? 7u : type == "acid" ? 24u : 0u;
            if (chunkRange.length < minimum || chunkRange.length > 16777216u ||
                chunkRange.length > 33554432u - metadataBytes)
                return;

            try
            {
                if (type == "bext")                          readMetadata_BWAV    (type, chunkRange);
                else if (type == "cue ")                     readMetadata_Cue     (type, chunkRange);
                else if (type == "smpl")                     readMetadata_SMPL    (type, chunkRange);
                else if (type == "LIST")                     readMetadata_LIST    (type, chunkRange);
                else if (type == "Trkn" || type == "axml")   readMetadata_Text    (type, chunkRange);
                else if (type == "inst" || type == "INST")   readMetadata_INST    (type);
                else if (type == "acid")                     readMetadata_ACID    (type);
                else                                         readMetadata_Unknown (type, chunkRange);
                metadataBytes += chunkRange.length;
            }
            catch (const FormatException&) {}
        }

        template <typename Buffer>
        static void decodeIntegers (Buffer& dest, const char* raw, uint32_t bytes)
        {
            // Little-endian PCM (8-bit is unsigned) to exactly value / 2^(bits-1).
            using FloatType = typename Buffer::Sample;
            const auto channels = dest.getNumChannels();
            const auto frames = dest.getNumFrames();
            const auto stride = static_cast<size_t> (channels) * bytes;
            const auto scale = static_cast<FloatType> (std::ldexp (1.0, 1 - static_cast<int> (bytes * 8u)));

            for (uint32_t chan = 0; chan < channels; ++chan)
            {
                auto s = reinterpret_cast<const uint8_t*> (raw) + static_cast<size_t> (chan) * bytes;
                auto out = dest.getIterator (chan);

                for (uint32_t frame = 0; frame < frames; ++frame, s += stride, ++out)
                {
                    int32_t v;
                    if (bytes == 1)      v = static_cast<int32_t> (s[0]) - 128;
                    else if (bytes == 2) v = static_cast<int16_t> (static_cast<uint16_t> (s[0] | (s[1] << 8)));
                    else if (bytes == 3) v = static_cast<int32_t> ((static_cast<uint32_t> (s[0]) << 8)
                                                                   | (static_cast<uint32_t> (s[1]) << 16)
                                                                   | (static_cast<uint32_t> (s[2]) << 24)) >> 8;
                    else                 v = static_cast<int32_t> (static_cast<uint32_t> (s[0])
                                                                   | (static_cast<uint32_t> (s[1]) << 8)
                                                                   | (static_cast<uint32_t> (s[2]) << 16)
                                                                   | (static_cast<uint32_t> (s[3]) << 24));
                    *out = scale * static_cast<FloatType> (v);
                }
            }
        }

        void readChunk_fmt (ChunkRange chunkRange)
        {
            auto format = readInt<uint16_t>();
            properties.numChannels = readInt<uint16_t>();
            auto sampleRate = readInt<uint32_t>();
            properties.sampleRate = sampleRate;
            auto bytesPerSecond = readInt<uint32_t>(); (void) bytesPerSecond;
            frameStride = readInt<uint16_t>();
            auto bitsPerSample = readInt<uint16_t>();

            if (frameStride == 0)
                frameStride = properties.numChannels * (bitsPerSample / 8u);

            sampleStride = bitsPerSample / 8u;

            switch (format)
            {
                case 1:   properties.bitDepth = getIntegerBitDepth (bitsPerSample); break;
                case 3:   properties.bitDepth = getFloatBitDepth (bitsPerSample); break;

                case 0xfffe: // WAVE_FORMAT_EXTENSIBLE
                {
                    if (chunkRange.length < 40 || readInt<uint16_t>() < 22) // extension size
                        throwFormatException();

                    const auto validBits = readInt<uint16_t>();
                    if (validBits == 0 || validBits > bitsPerSample) throwFormatException();
                    properties.speakers = getSpeakers (readInt<uint32_t>());

                    char subFormatGUID[16] = {};
                    stream->read (subFormatGUID, sizeof (subFormatGUID));

                    if (GUIDs::isEqual (subFormatGUID, GUIDs::IEEEFloatFormat))
                        properties.bitDepth = getFloatBitDepth (bitsPerSample);
                    else if (GUIDs::isEqual (subFormatGUID, GUIDs::pcmFormat) || GUIDs::isEqual (subFormatGUID, GUIDs::ambisonicFormat))
                        properties.bitDepth = getIntegerBitDepth (bitsPerSample);

                    break;
                }

                case 0x674f:  // WAVE_FORMAT_OGG_VORBIS_MODE_1
                case 0x6750:  // WAVE_FORMAT_OGG_VORBIS_MODE_2
                case 0x6751:  // WAVE_FORMAT_OGG_VORBIS_MODE_3
                case 0x676f:  // WAVE_FORMAT_OGG_VORBIS_MODE_1_PLUS
                case 0x6770:  // WAVE_FORMAT_OGG_VORBIS_MODE_2_PLUS
                case 0x6771:  // WAVE_FORMAT_OGG_VORBIS_MODE_3_PLUS
                    containsOggVorbisData = true; // TODO: might want to pick this up somewhere
                    break;

                default:
                    break;
            }

            if (properties.bitDepth == BitDepth::unknown || !properties.numChannels ||
                properties.numChannels > 256 || !sampleStride ||
                frameStride != properties.numChannels * sampleStride || frameStride > 2048)
                throwFormatException();
        }

        void readMetadata_Unknown (std::string type, ChunkRange chunkRange)
        {
            addMetadata (choc::json::create ("type", type,
                                             "content", readIntoBase64 (chunkRange.getEnd() - getPosition())));
        }

        // Text in a legacy 8-bit encoding cannot be carried by the JSON strings without
        // being altered: such chunks keep their exact bytes under "data" instead.
        void readMetadata_Raw (std::string type, ChunkRange chunkRange)
        {
            seek (chunkRange.start);
            addMetadata (choc::json::create ("type", type, "data", readIntoBase64 (chunkRange.length)));
        }

        static bool isUTF8 (const std::string& s)
        {
            return choc::text::findInvalidUTF8Data (s.data(), s.size()) == nullptr;
        }

        void readMetadata_BWAV (std::string type, ChunkRange chunkRange)
        {
            {
                std::string text;
                text.resize (338); // description, originator, reference, date and time
                stream->read (text.data(), 338);
                seek (chunkRange.start + 602);
                auto history = readStringToEndOfChunk (chunkRange);
                if (! isUTF8 (text) || ! isUTF8 (history))
                    return readMetadata_Raw (type, chunkRange);
                seek (chunkRange.start);
            }

            auto readUMID = [this]
            {
                char umid[64];
                stream->read (umid, 64);
                std::string result;

                for (auto b : umid)
                    result += choc::text::createHexString (static_cast<uint8_t> (b), 2);

                return result;
            };

            auto bwav = choc::json::create ("type", type);

            bwav.setMember ("description",          readString (256));
            bwav.setMember ("originator",           readString (32));
            bwav.setMember ("originatorRef",        readString (32));
            bwav.setMember ("originationDate",      readString (10));
            bwav.setMember ("originationTime",      readString (8));
            bwav.setMember ("timeRef",              static_cast<int64_t> (readInt<uint64_t>()));
            bwav.setMember ("version",              static_cast<int32_t> (readInt<uint16_t>()));
            bwav.setMember ("umid",                 readUMID());
            bwav.setMember ("loudnessValue",        static_cast<int32_t> (readInt<uint16_t>()));
            bwav.setMember ("loudnessRange",        static_cast<int32_t> (readInt<uint16_t>()));
            bwav.setMember ("maxTruePeakLevel",     static_cast<int32_t> (readInt<uint16_t>()));
            bwav.setMember ("maxMomentaryLoudness", static_cast<int32_t> (readInt<uint16_t>()));
            bwav.setMember ("maxShortTermLoudness", static_cast<int32_t> (readInt<uint16_t>()));
            stream->ignore (180);
            bwav.setMember ("codingHistory",        readStringToEndOfChunk (chunkRange));

            addMetadata (std::move (bwav));
        }

        void readMetadata_SMPL (std::string type, ChunkRange chunkRange)
        {
            auto smpl = choc::json::create ("type", type);

            smpl.setMember ("manufacturerCode",   static_cast<int64_t> (readInt<uint32_t>()));
            smpl.setMember ("productID",          static_cast<int64_t> (readInt<uint32_t>()));
            smpl.setMember ("samplePeriod",       static_cast<int64_t> (readInt<uint32_t>()));
            smpl.setMember ("midiUnityNote",      static_cast<int64_t> (readInt<uint32_t>()));
            smpl.setMember ("midiPitchFraction",  static_cast<int64_t> (readInt<uint32_t>()));
            smpl.setMember ("smpteFormat",        static_cast<int64_t> (readInt<uint32_t>()));
            smpl.setMember ("smpteOffset",        static_cast<int64_t> (readInt<uint32_t>()));
            auto numLoops           = readInt<uint32_t>();
            auto samplerDataSize    = readInt<uint32_t>();

            if (numLoops > 65536 || numLoops > (chunkRange.getEnd() - getPosition()) / 24)
                throwFormatException();

            auto loops = choc::value::createEmptyArray();

            for (uint32_t i = 0; i < numLoops; ++i)
            {
                auto loop = choc::value::createObject ({});

                loop.setMember ("ID",                static_cast<int64_t> (readInt<uint32_t>()));
                loop.setMember ("loopType",          static_cast<int64_t> (readInt<uint32_t>()));
                loop.setMember ("startByte",         static_cast<int64_t> (readInt<uint32_t>()));
                loop.setMember ("endByte",           static_cast<int64_t> (readInt<uint32_t>()));
                loop.setMember ("fractionalOffset",  static_cast<int64_t> (readInt<uint32_t>()));
                loop.setMember ("playCount",         static_cast<int64_t> (readInt<uint32_t>()));

                loops.addArrayElement (std::move (loop));
            }

            smpl.setMember ("loops", std::move (loops));

            // A sampler-data size beyond the chunk is a common writer defect: keep the loops.
            samplerDataSize = static_cast<uint32_t> (std::min<uint64_t> (samplerDataSize, chunkRange.getEnd() - getPosition()));

            smpl.setMember ("samplerData", readIntoBase64 (samplerDataSize));
            addMetadata (std::move (smpl));
        }

        void readMetadata_INST (std::string type)
        {
            auto inst = choc::json::create ("type", type);

            inst.setMember ("baseNote",      static_cast<int32_t> (readInt<uint8_t>()));
            inst.setMember ("fineTuning",    static_cast<int32_t> (readInt<int8_t>()));
            inst.setMember ("gainDecibels",  static_cast<int32_t> (readInt<int8_t>()));
            inst.setMember ("lowNote",       static_cast<int32_t> (readInt<uint8_t>()));
            inst.setMember ("highNote",      static_cast<int32_t> (readInt<uint8_t>()));
            inst.setMember ("lowVelocity",   static_cast<int32_t> (readInt<uint8_t>()));
            inst.setMember ("highVelocity",  static_cast<int32_t> (readInt<uint8_t>()));

            addMetadata (std::move (inst));
        }

        void readMetadata_Cue (std::string type, ChunkRange chunkRange)
        {
            auto cue = choc::json::create ("type", type);
            auto numCues = readInt<uint32_t>();

            if (numCues > 65536 || numCues > (chunkRange.getEnd() - getPosition()) / 24)
                throwFormatException();

            auto cues = choc::value::createEmptyArray();

            for (uint32_t i = 0; i < numCues; ++i)
            {
                auto c = choc::value::createObject ({});
                c.setMember ("ID",            static_cast<int64_t> (readInt<uint32_t>()));
                c.setMember ("position",      static_cast<int64_t> (readInt<uint32_t>()));
                c.setMember ("dataChunkID",   static_cast<int64_t> (readInt<uint32_t>()));
                c.setMember ("chunkStart",    static_cast<int64_t> (readInt<uint32_t>()));
                c.setMember ("blockStart",    static_cast<int64_t> (readInt<uint32_t>()));
                c.setMember ("sampleStart",   static_cast<int64_t> (readInt<uint32_t>()));

                cues.addArrayElement (std::move (c));
            }

            cue.setMember ("cues", std::move (cues));
            addMetadata (std::move (cue));
        }

        void readMetadata_LIST (std::string type, ChunkRange chunkRange)
        {
            if (chunkRange.length < 4 || chunkRange.length > 16777216u)
                throwFormatException();
            auto subType = readChunkType();
            auto payloadSize = chunkRange.length - 4;
            // Preserve the raw payload even for INFO: terminators, unknown chunks,
            // duplicated tags and padding are meaningful for lossless rewriting.
            auto list = choc::json::create ("type", type, "listType", subType,
                                           "data", readIntoBase64 (payloadSize));
            // Validate bounded nested chunks for standardized RIFF lists.
            if (subType == "adtl" || subType == "INFO" || subType == "info")
            {
                seek (chunkRange.start + 4);
                while (getPosition() < chunkRange.getEnd())
                {
                    if (chunkRange.getEnd() - getPosition() < 8)
                        throwFormatException();
                    auto childType = readChunkType();
                    const auto size = readInt<uint32_t>();
                    const auto start = getPosition();
                    if (size > chunkRange.getEnd() - start ||
                        (size & 1u) > chunkRange.getEnd() - start - size)
                        throwFormatException();
                    seek (start + size + (size & 1u));
                    (void)childType;
                }
            }
            addMetadata (std::move (list));
        }

        void readMetadata_ACID (std::string type)
        {
            auto acid = choc::json::create ("type", type);

            auto flags = readInt<uint32_t>();
            acid.setMember ("isOneShot",     (flags & 1) != 0);
            acid.setMember ("isRootNoteSet", (flags & 2) != 0);
            acid.setMember ("isStretchOn",   (flags & 4) != 0);
            acid.setMember ("isDiskBased",   (flags & 8) != 0);
            acid.setMember ("acidizerFlag",  (flags & 16) != 0);
            acid.setMember ("rootNote",      static_cast<int32_t> (readInt<uint16_t>()));
            stream->ignore (6);
            acid.setMember ("numBeats",          static_cast<int32_t> (readInt<uint32_t>()));
            acid.setMember ("meterDenominator",  static_cast<int32_t> (readInt<uint16_t>()));
            acid.setMember ("meterNumerator",    static_cast<int32_t> (readInt<uint16_t>()));
            acid.setMember ("tempo",             choc::memory::bit_cast<float> (readInt<uint32_t>()));

            addMetadata (std::move (acid));
        }

        void readMetadata_Text (std::string type, ChunkRange chunkRange) // Trkn, axml
        {
            auto content = readStringToEndOfChunk (chunkRange);
            if (! isUTF8 (content))
                return readMetadata_Raw (type, chunkRange);
            addMetadata (choc::json::create ("type", type, "content", content));
        }

        std::shared_ptr<std::istream> stream;
        AudioFileProperties properties;
        ChunkRange dataChunk;
        uint64_t streamEndPosition = 0, ds64DataSize = 0;
        uint32_t frameStride = 0, sampleStride = 0;
        bool containsOggVorbisData = false;
        std::vector<char> rawBuffer;

        template <typename Type>
        void seek (Type pos)            { stream->seekg (static_cast<std::streamoff> (pos)); }

        uint64_t getPosition() const    { return static_cast<uint64_t> (stream->tellg()); }

        uint64_t getStreamEndPosition()
        {
            auto currentPos = stream->tellg();
            stream->seekg (0, std::ios::end);
            auto end = stream->tellg();
            stream->seekg (currentPos);
            return static_cast<uint64_t> (end);
        }

        std::string readChunkType()
        {
            char chunk[4];
            stream->read (chunk, 4);
            return std::string (chunk, 4u);
        }

        // The declared length is validated (or clamped) against the enclosing range by the caller.
        ChunkRange readChunkRange()
        {
            const auto len = readInt<uint32_t>();
            const auto pos = getPosition();
            if (pos > streamEndPosition)
                throwFormatException();
            return { pos, len };
        }

        template <typename IntType>
        IntType readInt()
        {
            char n[sizeof (IntType)] = {};
            stream->read (n, sizeof (IntType));
            return choc::memory::readLittleEndian<IntType> (n);
        }

        std::string readString (uint32_t size)
        {
            if (size > 16777216u) throwFormatException();
            std::string s;
            s.resize (size);
            stream->read (s.data(), static_cast<std::streamsize> (size));
            auto nullChar = s.find ('\0');
            return nullChar != std::string::npos ? s.substr (0, nullChar) : s;
        }

        std::string readStringToEndOfChunk (ChunkRange range)
        {
            if (getPosition() > range.getEnd() || range.getEnd() - getPosition() > 16777216u)
                throwFormatException();
            return readString (static_cast<uint32_t> (range.getEnd() - getPosition()));
        }

        template <typename Size>
        std::string readIntoBase64 (Size size)
        {
            std::vector<char> data;
            if (size > 16777216u) throwFormatException();
            data.resize (static_cast<size_t> (size));
            stream->read (data.data(), static_cast<std::streamsize> (size));
            return choc::base64::encodeToString (data);
        }

        void addMetadata (choc::value::Value&& v)
        {
            if (! properties.metadata.isArray())
                properties.metadata = choc::value::createEmptyArray();

            properties.metadata.addArrayElement (std::move (v));
        }
    };

    //==============================================================================
    //==============================================================================
    struct WAVWriter   : public AudioFileWriter
    {
        WAVWriter (std::shared_ptr<std::ostream> s, AudioFileProperties&& p)
            : stream (std::move (s)), properties (std::move (p))
        {
            CHOC_ASSERT (stream != nullptr && ! stream->fail());
            CHOC_ASSERT (properties.sampleRate > 0 && properties.numChannels != 0 && properties.bitDepth != BitDepth::unknown);
            // CyAudioCore: a failed write only sets badbit; without it in the mask a full
            // disk went unreported with standard libraries that never promote it to failbit.
            stream->exceptions (std::ostream::failbit | std::ostream::badbit);

            headerPosition = stream->tellp();
            writeHeaderChunk();
            writeMetadataChunks();
            dataChunkPosition = stream->tellp();
            writeDataChunkSize();

            tempBuffer.resize (tempBufferFrames * bytesPerFrame);
        }

        ~WAVWriter() override
        {
            flush();
            stream.reset();
        }

        const AudioFileProperties& getProperties() override    { return properties; }

        bool appendFrames (choc::buffer::ChannelArrayView<const float>  source) override { return appendFramesForType (source); }
        bool appendFrames (choc::buffer::ChannelArrayView<const double> source) override { return appendFramesForType (source); }

        template <typename FloatType>
        bool appendFramesForType (choc::buffer::ChannelArrayView<const FloatType> source)
        {
            try
            {
                auto numChannels = source.getNumChannels();

                if (numChannels != properties.numChannels)
                    return false;

                auto framesToDo = source.getNumFrames();

                while (framesToDo != 0)
                {
                    auto rawTempBuffer = tempBuffer.data();
                    auto framesThisTime = std::min (framesToDo, tempBufferFrames);
                    auto sourceThisTime = source.getStart (framesThisTime);
                    uint32_t sampleStride = 0;

                    switch (properties.bitDepth)
                    {
                        // CyAudioCore: round to nearest and clamp (see encodeIntegers).
                        case BitDepth::int8:    encodeIntegers (rawTempBuffer, 1, sourceThisTime); sampleStride = 1; break;
                        case BitDepth::int16:   encodeIntegers (rawTempBuffer, 2, sourceThisTime); sampleStride = 2; break;
                        case BitDepth::int24:   encodeIntegers (rawTempBuffer, 3, sourceThisTime); sampleStride = 3; break;
                        case BitDepth::int32:   encodeIntegers (rawTempBuffer, 4, sourceThisTime); sampleStride = 4; break;

                        case BitDepth::float32:
                        {
                            copy (choc::buffer::createInterleavedView (reinterpret_cast<float*> (rawTempBuffer), numChannels, framesThisTime), sourceThisTime);
                            sampleStride = 4;
                            reverseEndiannessIfNeeded<uint32_t> (rawTempBuffer, framesThisTime, numChannels);
                            break;
                        }

                        case BitDepth::float64:
                        {
                            copy (choc::buffer::createInterleavedView (reinterpret_cast<double*> (rawTempBuffer), numChannels, framesThisTime), sourceThisTime);
                            sampleStride = 8;
                            reverseEndiannessIfNeeded<uint64_t> (rawTempBuffer, framesThisTime, numChannels);
                            break;
                        }

                        case BitDepth::unknown:
                        default:                return false;
                    }

                    stream->write (rawTempBuffer, static_cast<std::streamsize> (sampleStride * numChannels * framesThisTime));
                    properties.numFrames += framesThisTime; // Only frames that reached the stream are declared.
                    framesToDo -= framesThisTime;
                    source = source.fromFrame (framesThisTime);
                }

                return true;
            }
            catch (...) {}

            return false;
        }

        // Round half away from zero, clamped to [-2^(bits-1), 2^(bits-1) - 1]; NaN writes silence.
        static int32_t quantise (double v, double scale) noexcept
        {
            if (! (v == v)) return 0;
            const auto x = v * scale;
            if (x >= scale - 1) return static_cast<int32_t> (scale - 1);
            if (x <= -scale)    return static_cast<int32_t> (-scale);
            auto i = static_cast<int64_t> (x);
            const auto fraction = x - static_cast<double> (i);
            if (fraction >= 0.5) ++i; else if (fraction <= -0.5) --i;
            return static_cast<int32_t> (i);
        }

        template <typename Buffer>
        static void encodeIntegers (char* raw, uint32_t bytes, const Buffer& source)
        {
            // Samples are value * 2^(bits-1): the exact inverse of the reader. Little-endian; 8-bit unsigned.
            const auto size = source.getSize();
            const auto stride = static_cast<size_t> (size.numChannels) * bytes;
            const auto scale = std::ldexp (1.0, static_cast<int> (bytes * 8u) - 1);

            for (decltype (size.numChannels) chan = 0; chan < size.numChannels; ++chan)
            {
                auto d = reinterpret_cast<uint8_t*> (raw) + static_cast<size_t> (chan) * bytes;
                auto in = source.getIterator (chan);

                for (decltype (size.numFrames) frame = 0; frame < size.numFrames; ++frame, d += stride, ++in)
                {
                    const auto value = quantise (static_cast<double> (in.get()), scale);
                    const auto bits = static_cast<uint32_t> (bytes == 1 ? value + 128 : value);
                    for (uint32_t b = 0; b < bytes; ++b)
                        d[b] = static_cast<uint8_t> (bits >> (8u * b));
                }
            }
        }

        bool flush() override
        {
            if (properties.numFrames == lastNumFramesWritten)
                return true;

            try
            {
                auto writePos = stream->tellp();
                writeZeros (writePos & 1);
                stream->seekp (headerPosition);
                writeHeaderChunk();
                stream->seekp (dataChunkPosition);
                writeDataChunkSize();
                stream->seekp (writePos);
                stream->flush();
                return true;
            }
            catch (...) {}

            return false;
        }

        bool isFloat() const                     { return choc::audio::isFloat (properties.bitDepth); }
        uint64_t getTotalAudioDataSize() const   { return properties.numFrames * bytesPerFrame; }

        void writeHeaderChunk()
        {
            auto bytesPerSample = getBytesPerSample (properties.bitDepth);
            bytesPerFrame = properties.numChannels * bytesPerSample;

            if (bytesPerFrame == 0)
                throwFormatException();

            lastNumFramesWritten = properties.numFrames;
            auto totalAudioDataSize = getTotalAudioDataSize();

            auto mainChunkSize = 4 // WAVE
                                + 8 + 28 // ds64 or JUNK
                                + 8 + 40 // fmt
                                + 8 + totalAudioDataSize + (totalAudioDataSize & 1) // data chunk
                                + getTotalMetadataChunkSizes();

            mainChunkSize += (mainChunkSize & 1);

            isRF64 = (mainChunkSize >> 32) != 0;

            if (isRF64)
                writeChunkNameAndSize ("RF64", 0xffffffff);
            else
                writeChunkNameAndSize ("RIFF", static_cast<uint32_t> (mainChunkSize));

            writeChunkName ("WAVE");

            if (isRF64)
            {
                writeChunkNameAndSize ("ds64", 28);
                writeInt<uint64_t> (mainChunkSize);
                writeInt<uint64_t> (totalAudioDataSize);
                writeInt<uint64_t> (properties.numFrames); // sample count, as common readers expect
                writeZeros (4);                            // empty chunk-size table
            }
            else
            {
                writeChunkNameAndSize ("JUNK", 28);
                writeZeros (28);
            }

            writeChunkNameAndSize ("fmt ", 40);
            writeInt<uint16_t> (0xfffe); // WAVE_FORMAT_EXTENSIBLE
            writeInt<uint16_t> (static_cast<uint16_t> (properties.numChannels));
            writeInt<uint32_t> (static_cast<uint32_t> (properties.sampleRate + 0.5));
            // The 32-bit field saturates instead of converting an out-of-range double.
            writeInt<uint32_t> (static_cast<uint32_t> (std::min (properties.sampleRate * bytesPerFrame + 0.5, 4294967295.0)));
            writeInt<uint16_t> (static_cast<uint16_t> (bytesPerFrame));
            writeInt<uint16_t> (static_cast<uint16_t> (bytesPerSample * 8)); // bits per sample
            writeInt<uint16_t> (22); // size of the extension
            writeInt<uint16_t> (static_cast<uint16_t> (bytesPerSample * 8)); // valid bits per sample
            writeInt<uint32_t> (getSpeakerBits (properties.speakers));

            stream->write (reinterpret_cast<const char*> (isFloat() ? GUIDs::IEEEFloatFormat
                                                                    : GUIDs::pcmFormat),
                           sizeof (GUIDs::IEEEFloatFormat));
        }

        void writeDataChunkSize()
        {
            writeChunkNameAndSize ("data", isRF64 ? 0xffffffffu : static_cast<uint32_t> (getTotalAudioDataSize()));
        }

        uint32_t getMetadataChunkSize (const choc::value::ValueView& chunk) const
        {
            auto type = chunk["type"].toString();

            if (type.size() != 4 || type == "fmt " || type == "data" ||
                type == "RIFF" || type == "RF64" || type == "ds64")
                throwFormatException();

            if (isRawText (type, chunk))            return getDecodedBase64Size (chunk, "data");
            if (type == "bext")                     return getMetadataSize_BWAV (chunk);
            if (type == "smpl")                     return getMetadataSize_SMPL (chunk);
            if (type == "inst" || type == "INST")   return getMetadataSize_INST();
            if (type == "cue ")                     return getMetadataSize_Cue  (chunk);
            if (type == "LIST")                     return getMetadataSize_LIST (chunk);
            if (type == "acid")                     return getMetadataSize_ACID();
            if (type == "Trkn")                     return getMetadataSize_Trkn (chunk);
            if (type == "axml")                     return getMetadataSize_AXML (chunk);

            return getMetadataSize_Unknown  (chunk);
        }

        void writeMetadataPayload (const choc::value::ValueView& chunk)
        {
            auto size = getMetadataChunkSize (chunk);
            auto type = chunk["type"].toString();
            writeChunkNameAndSize (type, size);

            if (isRawText (type, chunk))            return writeData (getDecodedBase64 (chunk, "data"));
            if (type == "bext")                     return writeMetadata_BWAV (chunk);
            if (type == "smpl")                     return writeMetadata_SMPL (chunk);
            if (type == "inst" || type == "INST")   return writeMetadata_INST (chunk);
            if (type == "cue ")                     return writeMetadata_Cue  (chunk);
            if (type == "LIST")                     return writeMetadata_LIST (chunk);
            if (type == "acid")                     return writeMetadata_ACID (chunk);
            if (type == "Trkn")                     return writeMetadata_Trkn (chunk);
            if (type == "axml")                     return writeMetadata_AXML (chunk);

            return writeMetadata_Unknown (chunk);
        }

        // Byte-exact text chunk kept by the reader (legacy 8-bit encoding).
        static bool isRawText (const std::string& type, const choc::value::ValueView& chunk)
        {
            return (type == "bext" || type == "Trkn" || type == "axml")
                    && chunk.isObject() && chunk.hasObjectMember ("data") && chunk["data"].isString();
        }

        void writeMetadataChunk (const choc::value::ValueView& chunk)
        {
            writeMetadataPayload(chunk);
            writeZeros(getMetadataChunkSize(chunk) & 1u);
        }

        uint64_t getTotalMetadataChunkSizes() const
        {
            uint64_t total = 0;

            if (properties.metadata.isArray())
                for (const auto& meta : properties.metadata)
                {
                    const auto n = getMetadataChunkSize (meta);
                    total += 8ull + n + (n & 1u);
                }

            return total;
        }

        void writeMetadataChunks()
        {
            if (properties.metadata.isArray())
                for (const auto& meta : properties.metadata)
                    writeMetadataChunk (meta);
        }

        static uint32_t getMetadataSize_BWAV (const choc::value::ValueView& bwav)
        {
            return 256 + 32 + 32 + 10 + 8 + 8 + 2 + 64 + 2 + 2 + 2 + 2 + 2 + 180
                    + static_cast<uint32_t> (bwav["codingHistory"].toString().length());
        }

        void writeMetadata_BWAV (const choc::value::ValueView& bwav)
        {
            auto writeUMID = [this] (std::string_view umid)
            {
                uint8_t bytes[64] = {};

                if (umid.length() == 128)
                    for (size_t i = 0; i < 64; ++i)
                        bytes[i] = static_cast<uint8_t> (choc::text::hexDigitToInt (static_cast<uint32_t> (umid[i * 2])) * 16
                                                       + choc::text::hexDigitToInt (static_cast<uint32_t> (umid[i * 2 + 1])));

                stream->write (reinterpret_cast<char*> (bytes), 64);
            };

            writePaddedString (bwav, "description", 256);
            writePaddedString (bwav, "originator", 32);
            writePaddedString (bwav, "originatorRef", 32);
            writePaddedString (bwav, "originationDate", 10);
            writePaddedString (bwav, "originationTime", 8);
            writeInt<uint64_t> (static_cast<uint64_t> (bwav["timeRef"].getWithDefault<int64_t> (0)));
            writeInt<uint16_t> (static_cast<uint16_t> (bwav["version"].getWithDefault<int64_t> (0)));
            writeUMID (bwav["umid"].toString());
            writeInt<uint16_t> (static_cast<uint16_t> (bwav["loudnessValue"].getWithDefault<int64_t> (0)));
            writeInt<uint16_t> (static_cast<uint16_t> (bwav["loudnessRange"].getWithDefault<int64_t> (0)));
            writeInt<uint16_t> (static_cast<uint16_t> (bwav["maxTruePeakLevel"].getWithDefault<int64_t> (0)));
            writeInt<uint16_t> (static_cast<uint16_t> (bwav["maxMomentaryLoudness"].getWithDefault<int64_t> (0)));
            writeInt<uint16_t> (static_cast<uint16_t> (bwav["maxShortTermLoudness"].getWithDefault<int64_t> (0)));
            writeZeros (180);
            writeData (bwav["codingHistory"].toString());
        }

        static uint32_t getMetadataSize_SMPL (const choc::value::ValueView& smpl)
        {
            const auto& loops = smpl["loops"];

            return 4 * 9
                    + 4 * 6 * static_cast<uint32_t> (loops.isArray() ? loops.size() : 0)
                    + getDecodedBase64Size (smpl, "samplerData");
        }

        void writeMetadata_SMPL (const choc::value::ValueView& smpl)
        {
            auto samplerData = getDecodedBase64 (smpl, "samplerData");
            const auto& loops = smpl["loops"];

            writeInt<uint32_t> (static_cast<uint32_t> (smpl["manufacturerCode"].getWithDefault<int64_t> (0)));
            writeInt<uint32_t> (static_cast<uint32_t> (smpl["productID"].getWithDefault<int64_t> (0)));
            writeInt<uint32_t> (static_cast<uint32_t> (smpl["samplePeriod"].getWithDefault<int64_t> (0)));
            writeInt<uint32_t> (static_cast<uint32_t> (smpl["midiUnityNote"].getWithDefault<int64_t> (0)));
            writeInt<uint32_t> (static_cast<uint32_t> (smpl["midiPitchFraction"].getWithDefault<int64_t> (0)));
            writeInt<uint32_t> (static_cast<uint32_t> (smpl["smpteFormat"].getWithDefault<int64_t> (0)));
            writeInt<uint32_t> (static_cast<uint32_t> (smpl["smpteOffset"].getWithDefault<int64_t> (0)));
            writeInt<uint32_t> (static_cast<uint32_t> (loops.isArray() ? loops.size() : 0));
            writeInt<uint32_t> (static_cast<uint32_t> (samplerData.size()));

            if (loops.isArray())
            {
                for (const auto& loop : loops)
                {
                    writeInt<uint32_t> (static_cast<uint32_t> (loop["ID"].getWithDefault<int64_t> (0)));
                    writeInt<uint32_t> (static_cast<uint32_t> (loop["loopType"].getWithDefault<int64_t> (0)));
                    writeInt<uint32_t> (static_cast<uint32_t> (loop["startByte"].getWithDefault<int64_t> (0)));
                    writeInt<uint32_t> (static_cast<uint32_t> (loop["endByte"].getWithDefault<int64_t> (0)));
                    writeInt<uint32_t> (static_cast<uint32_t> (loop["fractionalOffset"].getWithDefault<int64_t> (0)));
                    writeInt<uint32_t> (static_cast<uint32_t> (loop["playCount"].getWithDefault<int64_t> (0)));
                }
            }

            writeData (samplerData);
        }

        static uint32_t getMetadataSize_INST()
        {
            return 7;
        }

        void writeMetadata_INST (const choc::value::ValueView& inst)
        {
            writeInt<uint8_t> (static_cast<uint8_t> (inst["baseNote"].getWithDefault<int64_t> (0)));
            writeInt<int8_t>  (static_cast<int8_t>  (inst["fineTuning"].getWithDefault<int64_t> (0)));
            writeInt<int8_t>  (static_cast<int8_t>  (inst["gainDecibels"].getWithDefault<int64_t> (0)));
            writeInt<uint8_t> (static_cast<uint8_t> (inst["lowNote"].getWithDefault<int64_t> (0)));
            writeInt<uint8_t> (static_cast<uint8_t> (inst["highNote"].getWithDefault<int64_t> (0)));
            writeInt<uint8_t> (static_cast<uint8_t> (inst["lowVelocity"].getWithDefault<int64_t> (0)));
            writeInt<uint8_t> (static_cast<uint8_t> (inst["highVelocity"].getWithDefault<int64_t> (0)));
        }

        static uint32_t getMetadataSize_Cue (const choc::value::ValueView& cue)
        {
            const auto& cues = cue["cues"];
            return 4 + static_cast<uint32_t> (cues.isArray() ? cues.size() : 0) * 4 * 6;
        }

        void writeMetadata_Cue (const choc::value::ValueView& cue)
        {
            const auto& cues = cue["cues"];
            writeInt<uint32_t> (static_cast<uint32_t> (cues.size()));

            for (const auto& c : cues)
            {
                writeInt<uint32_t> (static_cast<uint32_t> (c["ID"].getWithDefault<int64_t>(0)));
                writeInt<uint32_t> (static_cast<uint32_t> (c["position"].getWithDefault<int64_t>(0)));
                writeInt<uint32_t> (static_cast<uint32_t> (c["dataChunkID"].getWithDefault<int64_t>(0)));
                writeInt<uint32_t> (static_cast<uint32_t> (c["chunkStart"].getWithDefault<int64_t>(0)));
                writeInt<uint32_t> (static_cast<uint32_t> (c["blockStart"].getWithDefault<int64_t>(0)));
                writeInt<uint32_t> (static_cast<uint32_t> (c["sampleStart"].getWithDefault<int64_t>(0)));
            }
        }

        static uint32_t getMetadataSize_LIST (const choc::value::ValueView& list)
        {
            if (list["data"].isString())
            {
                if (!list["listType"].isString() || list["listType"].getString().size() != 4)
                    throwFormatException(); // A legacy lost subtype cannot be guessed.
                const auto n = getDecodedBase64Size (list, "data");
                if (n > 16777212u) throwFormatException();
                return 4 + n;
            }
            const auto& items = list["items"];
            if (!items.isArray()) throwFormatException();
            uint64_t total = 4;
            for (const auto& item : items)
            {
                if (item["type"].toString().size() != 4) throwFormatException();
                const auto n = item["value"].toString().size() + 1; // INFO includes NUL.
                total += 8 + n + (n & 1u);
                if (total > 16777216u) throwFormatException();
            }
            return static_cast<uint32_t>(total);
        }

        void writeMetadata_LIST (const choc::value::ValueView& list)
        {
            if (list["data"].isString())
            {
                writeChunkName (list["listType"].toString());
                writeData (getDecodedBase64 (list, "data"));
                return;
            }
            writeChunkName (list["listType"].isString() ? list["listType"].toString() : "INFO");
            for (const auto& item : list["items"])
            {
                const auto value = item["value"].toString();
                const auto n = value.size() + 1;
                writeChunkNameAndSize (item["type"].toString(), static_cast<uint32_t>(n));
                writeData (value); writeZeros(1); writeZeros(n & 1u);
            }
        }

        static uint32_t getMetadataSize_ACID()
        {
            return 4 + 2 + 6 + 4 + 2 + 2 + 4;
        }

        void writeMetadata_ACID (const choc::value::ValueView& acid)
        {
            writeInt<uint32_t> ((acid["isOneShot"].getWithDefault<bool> (false)     ? 1u  : 0u)
                              | (acid["isRootNoteSet"].getWithDefault<bool> (false) ? 2u  : 0u)
                              | (acid["isStretchOn"].getWithDefault<bool> (false)   ? 4u  : 0u)
                              | (acid["isDiskBased"].getWithDefault<bool> (false)   ? 8u  : 0u)
                              | (acid["acidizerFlag"].getWithDefault<bool> (false)  ? 16u : 0u));
            writeInt<uint16_t> (static_cast<uint16_t> (acid["rootNote"].getWithDefault<int64_t> (0)));
            writeZeros (6);
            writeInt<uint32_t> (static_cast<uint32_t> (acid["numBeats"].getWithDefault<int64_t> (0)));
            writeInt<uint16_t> (static_cast<uint16_t> (acid["meterDenominator"].getWithDefault<int64_t> (0)));
            writeInt<uint16_t> (static_cast<uint16_t> (acid["meterNumerator"].getWithDefault<int64_t> (0)));
            writeInt<uint32_t> (choc::memory::bit_cast<uint32_t> (acid["tempo"].getWithDefault<float> (0)));
        }

        static uint32_t getMetadataSize_Unknown (const choc::value::ValueView& u)  { return getDecodedBase64Size (u, "content"); }
        void writeMetadata_Unknown (const choc::value::ValueView& u)               { writeData (getDecodedBase64 (u, "content")); }

        static uint32_t getMetadataSize_Trkn (const choc::value::ValueView& trkn)  { return static_cast<uint32_t> (trkn["content"].toString().size()); }
        void writeMetadata_Trkn (const choc::value::ValueView& trkn)               { writeData (trkn["content"].toString()); }

        static uint32_t getMetadataSize_AXML (const choc::value::ValueView& axml)  { return static_cast<uint32_t> (axml["content"].toString().size()); }
        void writeMetadata_AXML (const choc::value::ValueView& axml)               { writeData (axml["content"].toString()); }

        void writeChunkName (std::string_view name)
        {
            if (name.length() != 4)
                throwFormatException();

            stream->write (name.data(), 4);
        }

        void writeChunkNameAndSize (std::string_view name, uint32_t size)
        {
            writeChunkName (name);
            writeInt (size);
        }

        template <typename IntType>
        void writeInt (IntType i)
        {
            char n[sizeof (IntType)];
            choc::memory::writeLittleEndian (n, i);
            stream->write (n, sizeof (IntType));
        }

        void writeZeros (uint32_t numBytes)
        {
            for (uint32_t i = 0; i < numBytes; ++i)
                writeInt<uint8_t> (0);
        }

        template <typename Type>
        void writeData (const Type& s)
        {
            stream->write (s.data(), static_cast<std::streamsize> (s.size()));
        }

        void writePaddedString (const choc::value::ValueView& chunk, const char* name, uint32_t totalSize)
        {
            auto s = chunk[name].toString();
            auto sizeToWrite = std::min (totalSize, static_cast<uint32_t> (s.length()));
            stream->write (s.data(), static_cast<std::streamsize> (sizeToWrite));

            if (sizeToWrite < totalSize)
                writeZeros (totalSize - sizeToWrite);
        }

        static std::vector<char> getDecodedBase64 (const choc::value::ValueView& v, const char* name)
        {
            auto asBase64 = v[name].toString();
            std::vector<char> data;

            if (choc::base64::decodeToContainer (data, asBase64))
                return data;

            return {};
        }

        static uint32_t getDecodedBase64Size (const choc::value::ValueView& v, const char* name)
        {
            return static_cast<uint32_t> (getDecodedBase64 (v, name).size());
        }

        std::shared_ptr<std::ostream> stream;
        AudioFileProperties properties;
        std::ostream::pos_type headerPosition = 0, dataChunkPosition = 0;
        bool isRF64 = false;
        uint32_t bytesPerFrame = 0;
        uint64_t lastNumFramesWritten = 0;
        std::vector<char> tempBuffer;
        static constexpr uint32_t tempBufferFrames = 512;
    };

    //==============================================================================
    static constexpr Speaker getSpeaker (uint32_t flag)
    {
        switch (flag)
        {
            case 0x1:       return Speaker::front_left;
            case 0x2:       return Speaker::front_right;
            case 0x4:       return Speaker::front_center;
            case 0x8:       return Speaker::low_frequency;
            case 0x10:      return Speaker::back_left;
            case 0x20:      return Speaker::back_right;
            case 0x40:      return Speaker::front_left_of_center;
            case 0x80:      return Speaker::front_right_of_center;
            case 0x100:     return Speaker::back_center;
            case 0x200:     return Speaker::side_left;
            case 0x400:     return Speaker::side_right;
            case 0x800:     return Speaker::top_center;
            case 0x1000:    return Speaker::top_front_left;
            case 0x2000:    return Speaker::top_front_center;
            case 0x4000:    return Speaker::top_front_right;
            case 0x8000:    return Speaker::top_back_left;
            case 0x10000:   return Speaker::top_back_center;
            case 0x20000:   return Speaker::top_back_right;
            default:        return Speaker::unknown;
        }
    }

    static constexpr uint32_t getSpeakerBit (Speaker s)
    {
        switch (s)
        {
            case Speaker::front_left:             return 0x1;
            case Speaker::front_right:            return 0x2;
            case Speaker::front_center:           return 0x4;
            case Speaker::low_frequency:          return 0x8;
            case Speaker::back_left:              return 0x10;
            case Speaker::back_right:             return 0x20;
            case Speaker::front_left_of_center:   return 0x40;
            case Speaker::front_right_of_center:  return 0x80;
            case Speaker::back_center:            return 0x100;
            case Speaker::side_left:              return 0x200;
            case Speaker::side_right:             return 0x400;
            case Speaker::top_center:             return 0x800;
            case Speaker::top_front_left:         return 0x1000;
            case Speaker::top_front_center:       return 0x2000;
            case Speaker::top_front_right:        return 0x4000;
            case Speaker::top_back_left:          return 0x8000;
            case Speaker::top_back_center:        return 0x10000;
            case Speaker::top_back_right:         return 0x20000;
            case Speaker::unknown:
            default:                              return 0;
        }
    }

    static std::vector<Speaker> getSpeakers (uint32_t bits)
    {
        std::vector<Speaker> results;

        for (uint32_t i = 1; i <= 0x20000; i <<= 1)
            if ((bits & i) != 0)
                results.push_back (getSpeaker (i));

        return results;
    }

    static uint32_t getSpeakerBits (const std::vector<Speaker>& speakers)
    {
        uint32_t flags = 0;

        for (auto s : speakers)
            flags |= getSpeakerBit (s);

        return flags;
    }

    template <typename IntType>
    static void reverseEndiannessIfNeeded (void* data, uint32_t numFrames, uint32_t numChannels)
    {
        (void) data; (void) numFrames; (void) numChannels;

        if constexpr (CHOC_BIG_ENDIAN)
        {
            auto ints = static_cast<IntType*> (data);

            for (uint32_t i = 0; i < numFrames * numChannels; ++i)
                ints[i] = choc::memory::swapByteOrder (ints[i]);
        }
    }

    struct GUIDs
    {
        static constexpr const uint8_t pcmFormat[]       = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
        static constexpr const uint8_t IEEEFloatFormat[] = { 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
        static constexpr const uint8_t ambisonicFormat[] = { 0x01, 0x00, 0x00, 0x00, 0x21, 0x07, 0xd3, 0x11, 0x86, 0x44, 0xc8, 0xc1, 0xca, 0x00, 0x00, 0x00 };

        static bool isEqual (const void* a, const void* b)    { return std::memcmp (a, b, 16) == 0; }
    };
};

//==============================================================================
template <bool supportWriting>
std::string CyDSPWavFormat<supportWriting>::getFileSuffixes()
{
    return "wav bwf";
}

template <bool supportWriting>
std::vector<double> CyDSPWavFormat<supportWriting>::getSupportedSampleRates()
{
    return { 8000,  11025, 12000, 16000,  22050,  32000,  44100,
             48000, 88200, 96000, 176400, 192000, 352800, 384000 };
}

template <bool supportWriting>
uint32_t CyDSPWavFormat<supportWriting>::getMaximumNumChannels()
{
    return 65535;
}

template <bool supportWriting>
std::vector<BitDepth> CyDSPWavFormat<supportWriting>::getSupportedBitDepths()
{
    return { BitDepth::int8,
             BitDepth::int16,
             BitDepth::int24,
             BitDepth::int32,
             BitDepth::float32,
             BitDepth::float64 };
}

template <bool supportWriting>
std::vector<std::string> CyDSPWavFormat<supportWriting>::getQualityLevels()  { return {}; }

template <bool supportWriting>
bool CyDSPWavFormat<supportWriting>::supportsWriting() const        { return true; }

template <bool supportWriting>
std::unique_ptr<AudioFileReader> CyDSPWavFormat<supportWriting>::createReader (std::shared_ptr<std::istream> s)
{
    if (s == nullptr || s->fail())
        return {};

    auto r = std::make_unique<typename Implementation::WAVReader> (std::move (s));

    if (r->initialise())
        return r;

    return {};
}

template <bool supportWriting>
std::unique_ptr<AudioFileWriter> CyDSPWavFormat<supportWriting>::createWriter (std::shared_ptr<std::ostream> s, AudioFileProperties p)
{
    if constexpr (supportWriting)
    {
        try
        {
            return std::make_unique<typename Implementation::WAVWriter> (std::move (s), std::move (p));
        }
        catch (...) {}
    }

    return {};
}

} // namespace choc::audio

#endif // CYDSP_PRIVATE_WAV_CODEC_INCLUDED
