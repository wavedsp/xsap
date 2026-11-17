/************************************************************************
 *
 *  XSap - Audio Playback Header
 *
 *  FILE:      AudioPlayer.h
 *
 *  PURPOSE:   Plays a span of samples on a background thread and reports
 *             progress back to the UI. Counterpart of WinSap's AudioPlayer,
 *             which uses waveOut; here the backend is chosen at run time.
 *
 *  Update:    2026.1
 *
 ************************************************************************/

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xsap {

class AudioPlayerBackendBase;

class AudioPlayer {
public:
    enum class PlaybackEvent {
        Progress,
        Completed,
        Cancelled,
        Failed
    };

    // Callbacks run on the internal playback worker thread. They must return
    // quickly, must not throw, and must not call play(), stop(), or destroy
    // this AudioPlayer instance. playedSamples is a count of PCM samples per
    // channel successfully written so far. Progress may be reported multiple
    // times; Completed, Cancelled, or Failed is reported at most once per play().
    using PlaybackCallback = std::function<void(std::size_t playedSamples, PlaybackEvent event)>;

    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;
    AudioPlayer(AudioPlayer&&) = delete;
    AudioPlayer& operator=(AudioPlayer&&) = delete;

    // rightSamples, when given, holds a second channel of the same length and
    // the two are played as one stereo stream.
    bool play(const std::vector<std::int16_t>& samples,
              unsigned int sampleRate,
              PlaybackCallback callback = {},
              const std::vector<std::int16_t>* rightSamples = nullptr);
    bool play(const std::int16_t* samples,
              std::size_t sampleCount,
              unsigned int sampleRate,
              PlaybackCallback callback = {},
              const std::int16_t* rightSamples = nullptr);

    void stop();

    bool isPlaying() const noexcept;
    std::string lastError() const;

private:
    struct PlaybackState;

    void playbackThread(std::shared_ptr<PlaybackState> state);
    void setLastError(std::string error);
    static void invokeCallback(const PlaybackCallback& callback,
                               std::size_t playedSamples,
                               PlaybackEvent event);

    std::atomic<bool> playing_{false};
    mutable std::mutex mutex_;
    std::thread worker_;
    std::shared_ptr<PlaybackState> state_;
    std::string lastError_;
};

} // namespace xsap
