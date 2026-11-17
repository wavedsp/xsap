/************************************************************************
 *
 *  XSap - Audio Playback Implementation
 *
 *  FILE:      AudioPlayer.cpp
 *
 *  PURPOSE:   PulseAudio and ALSA playback backends, selected at run time.
 *
 *             Both libraries are opened with dlopen and their entry points
 *             resolved by hand, so the program links against neither and
 *             still runs on a machine that has only one of them, or neither.
 *             PulseAudio is tried first because it is what a desktop session
 *             normally provides; ALSA is the fallback.
 *
 *  Update:    2026.1
 *
 ************************************************************************/

#include "AudioPlayer.h"

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <new>
#include <sstream>
#include <system_error>
#include <utility>

namespace xsap {

namespace {

constexpr std::size_t kBlockSamples = 4096;
constexpr int kPulseStreamPlayback = 1;
constexpr int kPulseSampleS16LE = 3;
constexpr int kAlsaStreamPlayback = 0;
constexpr int kAlsaFormatS16LE = 2;
constexpr int kAlsaAccessRwInterleaved = 3;
constexpr unsigned int kAlsaLatencyUs = 200000;

struct pa_simple;

struct pa_sample_spec {
    int format;
    std::uint32_t rate;
    std::uint8_t channels;
};

struct snd_pcm_t;

using snd_pcm_sframes_t = long;
using snd_pcm_uframes_t = unsigned long;

/*****************************************************************************
 * isLittleEndian
 *
 * Whether the host stores the low byte first. Both backends are configured
 * for S16LE and the samples are handed over as raw memory, so on a big endian
 * host every sample would be byte swapped; playback refuses rather than
 * producing noise.
 *
 *****************************************************************************/
bool isLittleEndian() noexcept
{
    const std::uint16_t probe = 0x0102;
    unsigned char bytes[sizeof(probe)] = {};
    std::memcpy(bytes, &probe, sizeof(probe));
    return bytes[0] == 0x02;
}

class DynamicLibrary {
public:
    /*************************************************************************
     * DynamicLibrary
     *
     * Opens a shared library, remembering why if it could not be opened
     * rather than throwing. A closed handle is a normal state here, since
     * neither audio library is guaranteed to be installed.
     *
     *************************************************************************/
    explicit DynamicLibrary(const char* path)
        : handle_(dlopen(path, RTLD_LAZY | RTLD_LOCAL))
    {
        if (handle_ == nullptr) {
            const char* error = dlerror();
            openError_ = error != nullptr ? error : "unknown dlopen failure";
        }
    }

    ~DynamicLibrary()
    {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    DynamicLibrary(DynamicLibrary&& other) noexcept
        : handle_(other.handle_), openError_(std::move(other.openError_))
    {
        other.handle_ = nullptr;
    }

    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept
    {
        if (this != &other) {
            if (handle_ != nullptr) {
                dlclose(handle_);
            }
            handle_ = other.handle_;
            openError_ = std::move(other.openError_);
            other.handle_ = nullptr;
        }
        return *this;
    }

    bool isOpen() const noexcept
    {
        return handle_ != nullptr;
    }

    const std::string& openError() const noexcept
    {
        return openError_;
    }

    /*************************************************************************
     * load
     *
     * Resolves one entry point and reports by name which symbol was missing.
     *
     * dlerror is cleared first and tested afterwards because a symbol may
     * legitimately resolve to a null address, so a null return alone does not
     * distinguish success from failure.
     *
     *************************************************************************/
    template <typename Symbol>
    bool load(const char* name, Symbol& symbol, std::string& error) const
    {
        if (handle_ == nullptr) {
            error = "library handle is not open";
            return false;
        }

        dlerror();
        void* rawSymbol = dlsym(handle_, name);
        const char* dlsymError = dlerror();
        if (dlsymError != nullptr || rawSymbol == nullptr) {
            std::ostringstream stream;
            stream << "missing symbol " << name;
            if (dlsymError != nullptr) {
                stream << ": " << dlsymError;
            }
            error = stream.str();
            return false;
        }

        symbol = reinterpret_cast<Symbol>(rawSymbol);
        return true;
    }

private:
    void* handle_ = nullptr;
    std::string openError_;
};

/*****************************************************************************
 * joinInitErrors
 *
 * Combines both backends' diagnoses into one message. Reporting only the
 * second would hide the usual cause, which is that one of the two libraries
 * is missing.
 *
 *****************************************************************************/
std::string joinInitErrors(const std::string& pulseError, const std::string& alsaError)
{
    std::ostringstream stream;
    stream << "PulseAudio initialization failed: " << pulseError
           << "; ALSA initialization failed: " << alsaError;
    return stream.str();
}

} // namespace

class AudioPlayerBackendBase {
public:
    virtual ~AudioPlayerBackendBase() = default;

    virtual const char* name() const noexcept = 0;
    virtual bool writeFrames(const std::int16_t* data,
                             std::size_t frameCount,
                             std::size_t& writtenFrames,
                             std::string& error) = 0;
    virtual bool drain(std::string& error) = 0;
    virtual void requestAbort() noexcept = 0;
};

struct AudioPlayer::PlaybackState {
    std::vector<std::int16_t> samples; // Interleaved when channels is 2
    unsigned int sampleRate = 0;
    unsigned int channels = 1;
    PlaybackCallback callback;
    std::atomic<bool> stopRequested{false};
    std::mutex backendMutex;
    std::shared_ptr<AudioPlayerBackendBase> backend;
};

namespace {

class PulseBackend final : public AudioPlayerBackendBase {
public:
    using pa_simple_new_t = pa_simple* (*)(const char*,
                                           const char*,
                                           int,
                                           const char*,
                                           const char*,
                                           const pa_sample_spec*,
                                           const void*,
                                           const void*,
                                           int*);
    using pa_simple_write_t = int (*)(pa_simple*, const void*, std::size_t, int*);
    using pa_simple_drain_t = int (*)(pa_simple*, int*);
    using pa_simple_free_t = void (*)(pa_simple*);
    using pa_strerror_t = const char* (*)(int);

    /*************************************************************************
     * create
     *
     * Builds a backend only if libpulse-simple is present and the stream
     * opens, so a non-null result is always ready to play. The constructor is
     * private and does nothing that can fail, which keeps the failure paths
     * here.
     *
     *************************************************************************/
    static std::shared_ptr<PulseBackend> create(unsigned int sampleRate,
                                                unsigned int channels,
                                                std::string& error)
    {
        auto backend = std::shared_ptr<PulseBackend>(new PulseBackend());
        if (!backend->loadSymbols(error)) {
            return {};
        }
        if (!backend->openStream(sampleRate, channels, error)) {
            return {};
        }
        return backend;
    }

    /*************************************************************************
     * ~PulseBackend
     *
     * Frees the stream if one was opened. Guarded on the symbol as well as
     * the handle, because a backend can be destroyed after a failed symbol
     * load.
     *
     *************************************************************************/
    ~PulseBackend() override
    {
        if (stream_ != nullptr && paSimpleFree_ != nullptr) {
            paSimpleFree_(stream_);
            stream_ = nullptr;
        }
    }

    const char* name() const noexcept override
    {
        return "PulseAudio";
    }

    /*************************************************************************
     * writeFrames
     *
     * Writes a block, blocking until the server has taken all of it. The
     * simple API is all or nothing, so unlike the ALSA backend this never
     * reports a partial count.
     *
     *************************************************************************/
    bool writeFrames(const std::int16_t* data,
                     std::size_t frameCount,
                     std::size_t& writtenFrames,
                     std::string& error) override
    {
        writtenFrames = 0;
        int pulseError = 0;
        const std::size_t bytes = frameCount * channels_ * sizeof(std::int16_t);
        if (paSimpleWrite_(stream_, data, bytes, &pulseError) != 0) {
            error = formatError("write", pulseError);
            return false;
        }
        writtenFrames = frameCount;
        return true;
    }

    /*************************************************************************
     * drain
     *
     * Blocks until the queued samples have actually been played, so the
     * completion callback does not arrive while sound is still coming out.
     *
     *************************************************************************/
    bool drain(std::string& error) override
    {
        int pulseError = 0;
        if (paSimpleDrain_(stream_, &pulseError) != 0) {
            error = formatError("drain", pulseError);
            return false;
        }
        return true;
    }

    /*************************************************************************
     * requestAbort
     *
     * Deliberately does nothing. The simple API offers no way to discard what
     * is already queued, so stopping waits for the current block instead;
     * with the block size used here that is short enough not to be felt.
     *
     *************************************************************************/
    void requestAbort() noexcept override
    {
    }

private:
    PulseBackend()
        : pulseSimpleLibrary_("libpulse-simple.so.0"), pulseLibrary_("libpulse.so.0")
    {
    }

    /*************************************************************************
     * loadSymbols
     *
     * Opens libpulse-simple and resolves the entry points.
     *
     * Only the four simple-API symbols are required. pa_strerror comes from
     * libpulse, which is looked up separately and allowed to fail, so a
     * machine carrying only libpulse-simple still plays sound and merely
     * reports errors by number.
     *
     *************************************************************************/
    bool loadSymbols(std::string& error)
    {
        if (!pulseSimpleLibrary_.isOpen()) {
            error = "dlopen(libpulse-simple.so.0) failed: " + pulseSimpleLibrary_.openError();
            return false;
        }

        if (!pulseSimpleLibrary_.load("pa_simple_new", paSimpleNew_, error) ||
            !pulseSimpleLibrary_.load("pa_simple_write", paSimpleWrite_, error) ||
            !pulseSimpleLibrary_.load("pa_simple_drain", paSimpleDrain_, error) ||
            !pulseSimpleLibrary_.load("pa_simple_free", paSimpleFree_, error)) {
            error = "PulseAudio symbol load failed: " + error;
            return false;
        }

        std::string strerrorError;
        if (pulseLibrary_.isOpen()) {
            pulseLibrary_.load("pa_strerror", paStrError_, strerrorError);
        }
        return true;
    }

    /*************************************************************************
     * openStream
     *
     * Opens a mono or stereo S16LE playback stream at the file's rate,
     * letting the server choose the buffering.
     *
     *************************************************************************/
    bool openStream(unsigned int sampleRate, unsigned int channels, std::string& error)
    {
        pa_sample_spec sampleSpec{};
        sampleSpec.format = kPulseSampleS16LE;
        sampleSpec.rate = sampleRate;
        sampleSpec.channels = static_cast<std::uint8_t>(channels);
        channels_ = channels;

        int pulseError = 0;
        stream_ = paSimpleNew_(nullptr,
                               "xsap",
                               kPulseStreamPlayback,
                               nullptr,
                               "playback",
                               &sampleSpec,
                               nullptr,
                               nullptr,
                               &pulseError);
        if (stream_ == nullptr) {
            error = formatError("stream initialization", pulseError);
            return false;
        }
        return true;
    }

    /*************************************************************************
     * formatError
     *
     * Turns a PulseAudio error code into a readable message, naming the
     * action that failed. pa_strerror lives in libpulse rather than
     * libpulse-simple and is optional, so the bare code is used if it could
     * not be resolved.
     *
     *************************************************************************/
    std::string formatError(const char* action, int errorCode) const
    {
        std::ostringstream stream;
        stream << action << " failed";
        if (paStrError_ != nullptr) {
            stream << ": " << paStrError_(errorCode);
        } else {
            stream << " with error code " << errorCode;
        }
        return stream.str();
    }

    DynamicLibrary pulseSimpleLibrary_;
    DynamicLibrary pulseLibrary_;
    pa_simple_new_t paSimpleNew_ = nullptr;
    pa_simple_write_t paSimpleWrite_ = nullptr;
    pa_simple_drain_t paSimpleDrain_ = nullptr;
    pa_simple_free_t paSimpleFree_ = nullptr;
    pa_strerror_t paStrError_ = nullptr;
    pa_simple* stream_ = nullptr;
    unsigned int channels_ = 1;
};

class AlsaBackend final : public AudioPlayerBackendBase {
public:
    using snd_pcm_open_t = int (*)(snd_pcm_t**, const char*, int, int);
    using snd_pcm_set_params_t = int (*)(snd_pcm_t*, int, int, unsigned int, unsigned int, int, unsigned int);
    using snd_pcm_writei_t = snd_pcm_sframes_t (*)(snd_pcm_t*, const void*, snd_pcm_uframes_t);
    using snd_pcm_recover_t = int (*)(snd_pcm_t*, int, int);
    using snd_pcm_drain_t = int (*)(snd_pcm_t*);
    using snd_pcm_drop_t = int (*)(snd_pcm_t*);
    using snd_pcm_close_t = int (*)(snd_pcm_t*);
    using snd_strerror_t = const char* (*)(int);

    /*************************************************************************
     * create
     *
     * Builds a backend only if libasound is present and the device opens, so
     * a non-null result is always ready to play. The constructor is private
     * and does nothing that can fail, which keeps the failure paths here.
     *
     *************************************************************************/
    static std::shared_ptr<AlsaBackend> create(unsigned int sampleRate,
                                               unsigned int channels,
                                               std::string& error)
    {
        auto backend = std::shared_ptr<AlsaBackend>(new AlsaBackend());
        if (!backend->loadSymbols(error)) {
            return {};
        }
        if (!backend->openDevice(sampleRate, channels, error)) {
            return {};
        }
        return backend;
    }

    /*************************************************************************
     * ~AlsaBackend
     *
     * Closes the device if one was opened. Guarded on the symbol as well as
     * the handle, because a backend can be destroyed after a failed symbol
     * load.
     *
     *************************************************************************/
    ~AlsaBackend() override
    {
        if (pcm_ != nullptr && sndPcmClose_ != nullptr) {
            sndPcmClose_(pcm_);
            pcm_ = nullptr;
        }
    }

    const char* name() const noexcept override
    {
        return "ALSA";
    }

    /*************************************************************************
     * writeFrames
     *
     * Writes as much as the device will take and reports how much went, which
     * may be less than asked for.
     *
     * An underrun is normal rather than fatal, so a negative result is passed
     * to snd_pcm_recover and the write retried; only a failure to recover is
     * treated as an error. Unlike the PulseAudio backend this can return a
     * partial count, which is why the caller loops.
     *
     *************************************************************************/
    bool writeFrames(const std::int16_t* data,
                     std::size_t frameCount,
                     std::size_t& writtenFrames,
                     std::string& error) override
    {
        writtenFrames = 0;

        while (true) {
            const auto requestedFrames = static_cast<snd_pcm_uframes_t>(frameCount);
            const snd_pcm_sframes_t result = sndPcmWritei_(pcm_, data, requestedFrames);
            if (result > 0) {
                writtenFrames = static_cast<std::size_t>(result);
                return true;
            }
            if (result == 0) {
                error = "write failed: snd_pcm_writei returned 0 frames";
                return false;
            }

            const int recovered = sndPcmRecover_(pcm_, static_cast<int>(result), 0);
            if (recovered < 0) {
                error = formatError("write", recovered);
                return false;
            }
        }
    }

    /*************************************************************************
     * drain
     *
     * Blocks until the queued samples have actually been played, so the
     * completion callback does not arrive while sound is still coming out.
     *
     *************************************************************************/
    bool drain(std::string& error) override
    {
        const int result = sndPcmDrain_(pcm_);
        if (result < 0) {
            error = formatError("drain", result);
            return false;
        }
        return true;
    }

    /*************************************************************************
     * requestAbort
     *
     * Drops whatever is still queued in the device so that a blocked write
     * returns at once. This is what makes stop() responsive rather than
     * waiting out the buffer.
     *
     *************************************************************************/
    void requestAbort() noexcept override
    {
        if (pcm_ != nullptr && sndPcmDrop_ != nullptr) {
            sndPcmDrop_(pcm_);
        }
    }

private:
    AlsaBackend()
        : alsaLibrary_("libasound.so.2")
    {
    }

    /*************************************************************************
     * loadSymbols
     *
     * Opens libasound and resolves the entry points. Every symbol here is
     * required, so a single missing one fails the backend and lets the caller
     * report why rather than crashing later on a null pointer.
     *
     *************************************************************************/
    bool loadSymbols(std::string& error)
    {
        if (!alsaLibrary_.isOpen()) {
            error = "dlopen(libasound.so.2) failed: " + alsaLibrary_.openError();
            return false;
        }

        if (!alsaLibrary_.load("snd_pcm_open", sndPcmOpen_, error) ||
            !alsaLibrary_.load("snd_pcm_set_params", sndPcmSetParams_, error) ||
            !alsaLibrary_.load("snd_pcm_writei", sndPcmWritei_, error) ||
            !alsaLibrary_.load("snd_pcm_recover", sndPcmRecover_, error) ||
            !alsaLibrary_.load("snd_pcm_drain", sndPcmDrain_, error) ||
            !alsaLibrary_.load("snd_pcm_drop", sndPcmDrop_, error) ||
            !alsaLibrary_.load("snd_pcm_close", sndPcmClose_, error) ||
            !alsaLibrary_.load("snd_strerror", sndStrError_, error)) {
            error = "ALSA symbol load failed: " + error;
            return false;
        }

        return true;
    }

    /*************************************************************************
     * openDevice
     *
     * Opens the default device for mono or stereo S16LE at the file's rate and asks for
     * a 200 ms buffer, which is long enough to survive scheduling jitter and
     * short enough that stopping feels immediate.
     *
     * The device is closed again if the parameter setup fails, so a failed
     * create leaves nothing open behind it.
     *
     *************************************************************************/
    bool openDevice(unsigned int sampleRate, unsigned int channels, std::string& error)
    {
        const int openResult = sndPcmOpen_(&pcm_, "default", kAlsaStreamPlayback, 0);
        if (openResult < 0) {
            error = formatError("device open", openResult);
            return false;
        }

        const int paramResult = sndPcmSetParams_(pcm_,
                                                 kAlsaFormatS16LE,
                                                 kAlsaAccessRwInterleaved,
                                                 channels,
                                                 sampleRate,
                                                 1,
                                                 kAlsaLatencyUs);
        if (paramResult < 0) {
            error = formatError("parameter setup", paramResult);
            sndPcmClose_(pcm_);
            pcm_ = nullptr;
            return false;
        }

        return true;
    }

    /*************************************************************************
     * formatError
     *
     * Turns an ALSA error code into a readable message, naming the action
     * that failed. Falls back to the bare code if snd_strerror could not be
     * resolved.
     *
     *************************************************************************/
    std::string formatError(const char* action, int errorCode) const
    {
        std::ostringstream stream;
        stream << action << " failed";
        if (sndStrError_ != nullptr) {
            stream << ": " << sndStrError_(errorCode);
        } else {
            stream << " with error code " << errorCode;
        }
        return stream.str();
    }

    DynamicLibrary alsaLibrary_;
    snd_pcm_open_t sndPcmOpen_ = nullptr;
    snd_pcm_set_params_t sndPcmSetParams_ = nullptr;
    snd_pcm_writei_t sndPcmWritei_ = nullptr;
    snd_pcm_recover_t sndPcmRecover_ = nullptr;
    snd_pcm_drain_t sndPcmDrain_ = nullptr;
    snd_pcm_drop_t sndPcmDrop_ = nullptr;
    snd_pcm_close_t sndPcmClose_ = nullptr;
    snd_strerror_t sndStrError_ = nullptr;
    snd_pcm_t* pcm_ = nullptr;
};

/*****************************************************************************
 * createBackend
 *
 * Opens the first backend that works, preferring PulseAudio because that is
 * what a desktop session normally provides, and falling back to ALSA.
 *
 * If neither opens, both diagnoses are returned together: the usual failure
 * is a machine with one of the two libraries missing, and reporting only the
 * second error hides that.
 *
 *****************************************************************************/
std::shared_ptr<AudioPlayerBackendBase> createBackend(unsigned int sampleRate,
                                                      unsigned int channels,
                                                      std::string& combinedError)
{
    std::string pulseError;
    if (auto pulse = PulseBackend::create(sampleRate, channels, pulseError)) {
        combinedError.clear();
        return pulse;
    }

    std::string alsaError;
    if (auto alsa = AlsaBackend::create(sampleRate, channels, alsaError)) {
        combinedError.clear();
        return alsa;
    }

    combinedError = joinInitErrors(pulseError, alsaError);
    return {};
}

} // namespace

AudioPlayer::AudioPlayer() = default;

/*****************************************************************************
 * ~AudioPlayer
 *
 * Stops playback and joins the worker, so that the thread can never outlive
 * the object it refers back to.
 *
 *****************************************************************************/
AudioPlayer::~AudioPlayer()
{
    stop();
}

/*****************************************************************************
 * play
 *
 * Convenience overload taking a whole vector; forwards to the pointer and
 * count form, which does the real work.
 *
 *****************************************************************************/
bool AudioPlayer::play(const std::vector<std::int16_t>& samples,
                       unsigned int sampleRate,
                       PlaybackCallback callback,
                       const std::vector<std::int16_t>* rightSamples)
{
    if (rightSamples != nullptr && rightSamples->size() != samples.size()) {
        setLastError("play() requires both stereo channels to have the same length");
        return false;
    }
    return play(samples.data(), samples.size(), sampleRate, std::move(callback),
                rightSamples != nullptr ? rightSamples->data() : nullptr);
}

/*****************************************************************************
 * play
 *
 * Starts playing a span of samples, returning as soon as the worker thread is
 * running rather than when the sound has finished.
 *
 * Any sound already playing is stopped first, so a second click interrupts
 * the first rather than overlapping it. The samples are copied into the
 * playback state, which the worker holds by shared pointer, so the caller's
 * buffer may go away immediately afterwards. Failures before the thread
 * starts are reported through the return value; failures after it starts are
 * reported through the callback and lastError().
 *
 *****************************************************************************/
bool AudioPlayer::play(const std::int16_t* samples,
                       std::size_t sampleCount,
                       unsigned int sampleRate,
                       PlaybackCallback callback,
                       const std::int16_t* rightSamples)
{
    stop();

    if (sampleCount == 0) {
        setLastError("play() rejected empty PCM sample data");
        return false;
    }

    if (samples == nullptr) {
        setLastError("play() received a null PCM sample pointer");
        return false;
    }

    if (sampleRate == 0U) {
        setLastError("play() rejected a zero sample rate");
        return false;
    }

    auto state = std::make_shared<PlaybackState>();
    try {
        if (rightSamples != nullptr) {
            state->channels = 2;
            state->samples.resize(sampleCount * 2U);
            for (std::size_t i = 0; i < sampleCount; ++i) {
                state->samples[2U * i] = samples[i];
                state->samples[2U * i + 1U] = rightSamples[i];
            }
        } else {
            state->samples.assign(samples, samples + sampleCount);
        }
    } catch (const std::bad_alloc&) {
        setLastError("play() ran out of memory copying the PCM samples");
        return false;
    }
    state->sampleRate = sampleRate;
    state->callback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_.clear();
        state_ = state;
        playing_.store(true, std::memory_order_release);
    }

    try {
        worker_ = std::thread(&AudioPlayer::playbackThread, this, state);
    } catch (const std::system_error& error) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.reset();
            playing_.store(false, std::memory_order_release);
        }
        setLastError(std::string("failed to start playback thread: ") + error.what());
        return false;
    }

    return true;
}

/*****************************************************************************
 * stop
 *
 * Stops playback and waits for the worker to finish, so that it is safe to
 * destroy the player or start a new sound as soon as this returns.
 *
 * Setting the stop flag alone is not enough: the worker is usually blocked
 * inside a device write, and would not look at the flag until the device
 * drained. The backend is therefore also told to abort, which unblocks the
 * write. The thread and the backend handle are copied out under the lock and
 * used outside it, so that joining never happens while holding it.
 *
 *****************************************************************************/
void AudioPlayer::stop()
{
    std::shared_ptr<PlaybackState> state;
    std::thread worker;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
        if (state != nullptr) {
            state->stopRequested.store(true, std::memory_order_release);
        }
        worker = std::move(worker_);
    }

    std::shared_ptr<AudioPlayerBackendBase> backend;
    if (state != nullptr) {
        std::lock_guard<std::mutex> lock(state->backendMutex);
        backend = state->backend;
    }

    if (backend != nullptr) {
        backend->requestAbort();
    }

    if (worker.joinable()) {
        worker.join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == state) {
            state_.reset();
        }
    }

    playing_.store(false, std::memory_order_release);
}

bool AudioPlayer::isPlaying() const noexcept
{
    return playing_.load(std::memory_order_acquire);
}

/*****************************************************************************
 * lastError
 *
 * The reason the last play() or playback attempt failed, or empty if it did
 * not. Worth reading after play() returns false, since the real diagnosis
 * often comes from the backend rather than from the arguments.
 *
 *****************************************************************************/
std::string AudioPlayer::lastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

/*****************************************************************************
 * playbackThread
 *
 * The worker. Opens a backend, then writes the samples a block at a time,
 * reporting progress after every write so the playback cursor keeps up.
 *
 * The stop flag is checked before each block and again before each partial
 * write, because a backend write blocks until the device has room and a long
 * buffer would otherwise delay stopping by the length of that buffer. Every
 * exit path goes through `finish`, so the backend is released and the
 * callback is delivered exactly once however playback ends.
 *
 *****************************************************************************/
void AudioPlayer::playbackThread(std::shared_ptr<PlaybackState> state)
{
    auto finish = [this, &state](PlaybackEvent event, std::size_t playedSamples, std::string error) {
        if (event == PlaybackEvent::Failed) {
            setLastError(std::move(error));
        }

        {
            std::lock_guard<std::mutex> lock(state->backendMutex);
            state->backend.reset();
        }

        playing_.store(false, std::memory_order_release);
        invokeCallback(state->callback, playedSamples, event);
    };

    if (!isLittleEndian()) {
        finish(PlaybackEvent::Failed,
               0,
               "AudioPlayer requires little-endian hosts for S16LE playback");
        return;
    }

    std::string initError;
    std::shared_ptr<AudioPlayerBackendBase> backend =
        createBackend(state->sampleRate, state->channels, initError);
    if (backend == nullptr) {
        finish(PlaybackEvent::Failed, 0, std::move(initError));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->backendMutex);
        state->backend = backend;
    }

    if (state->stopRequested.load(std::memory_order_acquire)) {
        backend->requestAbort();
        finish(PlaybackEvent::Cancelled, 0, {});
        return;
    }

    // Counted in frames: one sample per channel
    const std::size_t totalFrames = state->samples.size() / state->channels;
    std::size_t playedSamples = 0;
    while (playedSamples < totalFrames) {
        if (state->stopRequested.load(std::memory_order_acquire)) {
            backend->requestAbort();
            finish(PlaybackEvent::Cancelled, playedSamples, {});
            return;
        }

        const std::size_t requestedSamples =
            std::min(kBlockSamples, totalFrames - playedSamples);
        std::size_t blockWritten = 0;

        while (blockWritten < requestedSamples) {
            if (state->stopRequested.load(std::memory_order_acquire)) {
                backend->requestAbort();
                finish(PlaybackEvent::Cancelled, playedSamples, {});
                return;
            }

            std::size_t writtenSamples = 0;
            std::string writeError;
            if (!backend->writeFrames(state->samples.data() + playedSamples * state->channels,
                                      requestedSamples - blockWritten,
                                      writtenSamples,
                                      writeError)) {
                std::ostringstream stream;
                stream << backend->name() << ' ' << writeError
                       << " after " << playedSamples << " samples";
                finish(PlaybackEvent::Failed, playedSamples, stream.str());
                return;
            }

            if (writtenSamples == 0) {
                std::ostringstream stream;
                stream << backend->name() << " write returned 0 samples";
                finish(PlaybackEvent::Failed, playedSamples, stream.str());
                return;
            }

            blockWritten += writtenSamples;
            playedSamples += writtenSamples;
            invokeCallback(state->callback, playedSamples, PlaybackEvent::Progress);
        }
    }

    if (state->stopRequested.load(std::memory_order_acquire)) {
        backend->requestAbort();
        finish(PlaybackEvent::Cancelled, playedSamples, {});
        return;
    }

    std::string drainError;
    if (!backend->drain(drainError)) {
        std::ostringstream stream;
        stream << backend->name() << ' ' << drainError
               << " after " << playedSamples << " samples";
        finish(PlaybackEvent::Failed, playedSamples, stream.str());
        return;
    }

    finish(PlaybackEvent::Completed, playedSamples, {});
}

/*****************************************************************************
 * setLastError
 *
 * Records the message lastError() will report. Takes the lock because the
 * worker thread writes it while the UI thread reads it.
 *
 *****************************************************************************/
void AudioPlayer::setLastError(std::string error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = std::move(error);
}

/*****************************************************************************
 * invokeCallback
 *
 * Calls the progress callback if one was supplied. Static, because it is
 * called from the worker thread after the player's own state has already been
 * torn down.
 *
 *****************************************************************************/
void AudioPlayer::invokeCallback(const PlaybackCallback& callback,
                                 std::size_t playedSamples,
                                 PlaybackEvent event)
{
    if (callback) {
        callback(playedSamples, event);
    }
}

} // namespace xsap
