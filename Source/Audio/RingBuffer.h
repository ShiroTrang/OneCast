#pragma once
#include <JuceHeader.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

/// Lock-free, single-producer / single-consumer ring buffer for multi-channel audio.
///
/// One thread writes (e.g. WASAPI capture thread) while another reads (e.g. JUCE
/// audio callback). The implementation guarantees correct happens-before ordering
/// through acquire / release semantics on the head and tail indices.
///
/// Internally the frame capacity is always rounded up to the next power of two so
/// that index wrap-around can be performed with a simple bitmask instead of an
/// expensive modulo.
///
/// Both head and tail are monotonically-increasing 32-bit counters. The
/// effective position within the circular buffer is obtained by masking with
/// (capacity_ - 1). Because the counters can wrap past UINT32_MAX, the
/// difference (head - tail) always yields the correct number of outstanding
/// frames as long as the backlog never exceeds 2^32 frames (a practical
/// impossibility for any real-time audio ring buffer).
class RingBuffer
{
public:
    /// @param numChannels      Number of audio channels (typically 2 for stereo).
    /// @param requestedSize    Minimum buffer size in frames. Will be rounded up
    ///                         to the next power of two.
    RingBuffer (int numChannels, int requestedSize)
        : numChannels_ (numChannels),
          capacity_ (nextPowerOfTwo (requestedSize)),
          mask_ (static_cast<std::uint32_t> (capacity_ - 1)),
          buffer_ (static_cast<std::size_t> (numChannels_) * capacity_, 0.0f),
          head_ (0),   // written by producer, read by consumer
          tail_ (0)    // written by consumer, read by producer
    {
        jassert (numChannels > 0);
        jassert (requestedSize > 0);
    }

    RingBuffer (const RingBuffer&) = delete;
    RingBuffer& operator= (const RingBuffer&) = delete;

    // ------------------------------------------------------------------
    //  Queries
    // ------------------------------------------------------------------

    int getNumChannels() const noexcept { return numChannels_; }
    int getCapacity()    const noexcept { return capacity_; }

    /// Number of frames available for the producer to write.
    int getAvailableForWrite() const noexcept
    {
        const auto h = head_.load (std::memory_order_acquire);
        const auto t = tail_.load (std::memory_order_acquire);
        return static_cast<int> (capacity_ - 1 - (h - t));
    }

    /// Number of frames available for the consumer to read.
    int getAvailableForRead() const noexcept
    {
        const auto h = head_.load (std::memory_order_acquire);
        const auto t = tail_.load (std::memory_order_acquire);
        return static_cast<int> (h - t);
    }

    // ------------------------------------------------------------------
    //  Write (producer side -- WASAPI capture thread)
    // ------------------------------------------------------------------

    /// Write interleaved float data (as received from WASAPI) into the ring
    /// buffer. The data is deinterleaved into per-channel storage.
    ///
    /// If the source has more channels than the buffer, the extra channels are
    /// dropped. If the source has fewer channels, the remaining buffer channels
    /// are filled with silence.
    ///
    /// @param interleavedData   Pointer to interleaved sample data.
    /// @param numFrames         Number of frames to write.
    /// @param numSourceChannels  Channel count in @p interleavedData.
    /// @return                  Number of frames actually written.
    int writeInterleaved (const float* interleavedData, int numFrames,
                          int numSourceChannels) noexcept
    {
        jassert (interleavedData != nullptr);
        jassert (numFrames >= 0);
        jassert (numSourceChannels > 0);

        std::uint32_t h;
        const int available = producerAvailable (h);
        const int framesToWrite = std::min (numFrames, available);
        if (framesToWrite <= 0)
            return 0;

        const int channelsToCopy = std::min (numSourceChannels, numChannels_);

        for (int ch = 0; ch < channelsToCopy; ++ch)
        {
            float* const base = channelBase (ch);

            for (int i = 0; i < framesToWrite; ++i)
                base[(h + static_cast<std::uint32_t> (i)) & mask_] =
                    interleavedData[i * numSourceChannels + ch];
        }

        // Zero any extra buffer channels that the source does not provide.
        for (int ch = channelsToCopy; ch < numChannels_; ++ch)
        {
            float* const base = channelBase (ch);

            for (int i = 0; i < framesToWrite; ++i)
                base[(h + static_cast<std::uint32_t> (i)) & mask_] = 0.0f;
        }

        head_.store (h + static_cast<std::uint32_t> (framesToWrite),
                     std::memory_order_release);
        return framesToWrite;
    }

    /// Write per-channel (non-interleaved) data into the ring buffer.
    ///
    /// @param channelData      Array of per-channel float pointers (one per source
    ///                         channel). May be null for channels you wish to skip.
    /// @param numFrames        Number of frames to write.
    /// @param numSourceChannels  Channel count in @p channelData.
    /// @return                 Number of frames actually written.
    int write (const float* const* channelData, int numFrames,
               int numSourceChannels) noexcept
    {
        jassert (channelData != nullptr);
        jassert (numFrames >= 0);
        jassert (numSourceChannels > 0);

        std::uint32_t h;
        const int available = producerAvailable (h);
        const int framesToWrite = std::min (numFrames, available);
        if (framesToWrite <= 0)
            return 0;

        const int channelsToCopy = std::min (numSourceChannels, numChannels_);

        for (int ch = 0; ch < channelsToCopy; ++ch)
        {
            jassert (channelData[ch] != nullptr);
            const float* const src = channelData[ch];
            float* const base = channelBase (ch);

            for (int i = 0; i < framesToWrite; ++i)
                base[(h + static_cast<std::uint32_t> (i)) & mask_] = src[i];
        }

        // Silence extra buffer channels.
        for (int ch = channelsToCopy; ch < numChannels_; ++ch)
        {
            float* const base = channelBase (ch);

            for (int i = 0; i < framesToWrite; ++i)
                base[(h + static_cast<std::uint32_t> (i)) & mask_] = 0.0f;
        }

        head_.store (h + static_cast<std::uint32_t> (framesToWrite),
                     std::memory_order_release);
        return framesToWrite;
    }

    // ------------------------------------------------------------------
    //  Read (consumer side -- JUCE audio callback)
    // ------------------------------------------------------------------

    /// Read from the ring buffer into a JUCE AudioBuffer.
    ///
    /// @param dest       Destination AudioBuffer. Will be written starting at
    ///                   channel 0. If the AudioBuffer has more channels than
    ///                   the ring buffer, the extra channels are filled with
    ///                   silence. If the AudioBuffer has fewer channels, only
    ///                   those channels are populated.
    /// @param numFrames  Maximum number of frames to read.
    /// @return           Number of frames actually read.
    int read (juce::AudioBuffer<float>& dest, int numFrames) noexcept
    {
        jassert (numFrames >= 0);

        const int destChannels = dest.getNumChannels();
        return readRaw (dest.getArrayOfWritePointers(), numFrames, destChannels);
    }

    /// Read from the ring buffer into raw per-channel float pointers.
    ///
    /// @param channelData     Array of per-channel float pointers. Pointers for
    ///                        channels beyond the ring buffer's channel count
    ///                        will have silence written to them.
    /// @param numFrames       Maximum number of frames to read.
    /// @param numDestChannels  Number of channels in @p channelData.
    /// @return                Number of frames actually read.
    int read (float* const* channelData, int numFrames,
              int numDestChannels) noexcept
    {
        jassert (channelData != nullptr);
        jassert (numFrames >= 0);
        jassert (numDestChannels > 0);

        return readRaw (channelData, numFrames, numDestChannels);
    }

    /// Copy up to @p numFrames frames into @p channelData WITHOUT consuming
    /// them. Consumer thread only. Pair with discard() to consume exactly as
    /// many frames as were actually used (e.g. by a resampler).
    ///
    /// @return Number of frames copied.
    int peek (float* const* channelData, int numFrames,
              int numDestChannels) const noexcept
    {
        jassert (channelData != nullptr);
        jassert (numFrames >= 0);

        std::uint32_t t;
        const int available = consumerAvailable (t);
        const int framesToRead = std::min (numFrames, available);
        if (framesToRead <= 0)
            return 0;

        const int channelsToCopy = std::min (numDestChannels, numChannels_);

        for (int ch = 0; ch < channelsToCopy; ++ch)
        {
            float* const dst = channelData[ch];
            const float* const base = channelBase (ch);

            for (int i = 0; i < framesToRead; ++i)
                dst[i] = base[(t + static_cast<std::uint32_t> (i)) & mask_];
        }

        for (int ch = channelsToCopy; ch < numDestChannels; ++ch)
            std::fill (channelData[ch], channelData[ch] + framesToRead, 0.0f);

        return framesToRead;
    }

    /// Consume up to @p numFrames frames without copying them. Consumer
    /// thread only, so it is safe while the producer keeps writing.
    ///
    /// @return Number of frames discarded.
    int discard (int numFrames) noexcept
    {
        jassert (numFrames >= 0);

        std::uint32_t t;
        const int framesToDrop = std::min (numFrames, consumerAvailable (t));
        if (framesToDrop <= 0)
            return 0;

        tail_.store (t + static_cast<std::uint32_t> (framesToDrop),
                     std::memory_order_release);
        return framesToDrop;
    }

    // ------------------------------------------------------------------
    //  Reset (only when no readers or writers are active)
    // ------------------------------------------------------------------

    /// Reset the buffer to empty and zero all samples.
    /// NOT thread-safe -- call only when neither the producer nor the consumer
    /// is accessing the buffer.
    void reset() noexcept
    {
        head_.store (0, std::memory_order_relaxed);
        tail_.store (0, std::memory_order_relaxed);

        std::fill (buffer_.begin(), buffer_.end(), 0.0f);
    }

private:
    // ------------------------------------------------------------------
    //  Internal helpers
    // ------------------------------------------------------------------

    /// Return a pointer to the beginning of @p channel's flat storage.
    /// The caller may index into it using (index & mask_) for wrap-around.
    const float* channelBase (int channel) const noexcept
    {
        jassert (channel >= 0 && channel < numChannels_);
        return buffer_.data()
               + static_cast<std::size_t> (channel) * capacity_;
    }

    float* channelBase (int channel) noexcept
    {
        jassert (channel >= 0 && channel < numChannels_);
        return buffer_.data()
               + static_cast<std::size_t> (channel) * capacity_;
    }

    /// Compute how many frames the producer can write right now.
    /// Only the producer thread may call this.
    /// head_ is owned by the producer and can be loaded relaxed.
    /// tail_ is written by the consumer and must be loaded with acquire.
    int producerAvailable (std::uint32_t& outHead) const noexcept
    {
        outHead = head_.load (std::memory_order_relaxed);
        const auto t = tail_.load (std::memory_order_acquire);
        return static_cast<int> (static_cast<std::uint32_t> (capacity_ - 1) - (outHead - t));
    }

    /// Compute how many frames the consumer can read right now.
    /// Only the consumer thread may call this.
    /// tail_ is owned by the consumer and can be loaded relaxed.
    /// head_ is written by the producer and must be loaded with acquire.
    int consumerAvailable (std::uint32_t& outTail) const noexcept
    {
        outTail = tail_.load (std::memory_order_relaxed);
        const auto h = head_.load (std::memory_order_acquire);
        return static_cast<int> (h - outTail);
    }

    /// Core read implementation shared by both read() overloads.
    int readRaw (float* const* channelData, int numFrames,
                 int numDestChannels) noexcept
    {
        std::uint32_t t;
        const int available = consumerAvailable (t);
        const int framesToRead = std::min (numFrames, available);
        if (framesToRead <= 0)
            return 0;

        const int channelsToCopy = std::min (numDestChannels, numChannels_);

        for (int ch = 0; ch < channelsToCopy; ++ch)
        {
            jassert (channelData[ch] != nullptr);
            float* const dst = channelData[ch];
            const float* const base = channelBase (ch);

            for (int i = 0; i < framesToRead; ++i)
                dst[i] = base[(t + static_cast<std::uint32_t> (i)) & mask_];
        }

        // Zero any extra destination channels that the ring buffer does not have.
        for (int ch = channelsToCopy; ch < numDestChannels; ++ch)
        {
            jassert (channelData[ch] != nullptr);
            float* const dst = channelData[ch];

            std::fill (dst, dst + framesToRead, 0.0f);
        }

        tail_.store (t + static_cast<std::uint32_t> (framesToRead),
                     std::memory_order_release);
        return framesToRead;
    }

    /// Round up to the next power of two. Returns @p value if it is already
    /// a power of two.
    static int nextPowerOfTwo (int value) noexcept
    {
        jassert (value > 0);
        std::uint32_t v = static_cast<std::uint32_t> (value);
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v++;
        return static_cast<int> (v);
    }

    // ------------------------------------------------------------------
    //  Data members
    // ------------------------------------------------------------------

    const int numChannels_;
    const int capacity_;     // always a power of two
    const std::uint32_t mask_; // capacity_ - 1

    // Interleaved storage: samples are arranged as consecutive per-channel
    // blocks, each of length capacity_.  buffer_ has
    // numChannels_ * capacity_ elements.
    std::vector<float> buffer_;

    // head: monotonically-increasing count of frames written so far.
    //       The producer advances this; the consumer reads it.
    //
    // tail: monotonically-increasing count of frames consumed so far.
    //       The consumer advances this; the producer reads it.
    //
    // Both wrap at UINT32_MAX as they increment.  The actual position
    // within the circular buffer is (counter & mask_).
    std::atomic<std::uint32_t> head_;
    std::atomic<std::uint32_t> tail_;
};
