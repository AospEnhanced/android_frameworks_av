/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_HARDWARE_STREAM_HAL_HIDL_H
#define ANDROID_HARDWARE_STREAM_HAL_HIDL_H

#include <atomic>
#include <mutex>

#include PATH(android/hardware/audio/CORE_TYPES_FILE_VERSION/IStream.h)
#include PATH(android/hardware/audio/CORE_TYPES_FILE_VERSION/IStreamIn.h)
#include PATH(android/hardware/audio/FILE_VERSION/IStreamOut.h)
#include <android-base/thread_annotations.h>
#include <fmq/EventFlag.h>
#include <fmq/MessageQueue.h>
#include <media/audiohal/EffectHalInterface.h>
#include <media/audiohal/StreamHalInterface.h>
#include <mediautils/Synchronization.h>

#include "CoreConversionHelperHidl.h"
#include "StreamPowerLog.h"

using ::android::hardware::audio::CORE_TYPES_CPP_VERSION::IStream;
using ::android::hardware::EventFlag;
using ::android::hardware::MessageQueue;
using ::android::hardware::Return;
using ReadParameters =
        ::android::hardware::audio::CORE_TYPES_CPP_VERSION::IStreamIn::ReadParameters;
using ReadStatus = ::android::hardware::audio::CORE_TYPES_CPP_VERSION::IStreamIn::ReadStatus;
using WriteCommand = ::android::hardware::audio::CPP_VERSION::IStreamOut::WriteCommand;
using WriteStatus = ::android::hardware::audio::CPP_VERSION::IStreamOut::WriteStatus;

namespace android {

class DeviceHalHidl;

class StreamHalHidl : public virtual StreamHalInterface, public CoreConversionHelperHidl
{
  public:
    // Return size of input/output buffer in bytes for this stream - eg. 4800.
    virtual status_t getBufferSize(size_t *size);

    // Return the base configuration of the stream:
    //   - channel mask;
    //   - format - e.g. AUDIO_FORMAT_PCM_16_BIT;
    //   - sampling rate in Hz - eg. 44100.
    virtual status_t getAudioProperties(audio_config_base_t *configBase);

    // Set audio stream parameters.
    virtual status_t setParameters(const String8& kvPairs);

    // Get audio stream parameters.
    virtual status_t getParameters(const String8& keys, String8 *values);

    // Add or remove the effect on the stream.
    virtual status_t addEffect(sp<EffectHalInterface> effect);
    virtual status_t removeEffect(sp<EffectHalInterface> effect);

    // Put the audio hardware input/output into standby mode.
    virtual status_t standby();

    virtual status_t dump(int fd, const Vector<String16>& args) override;

    // Start a stream operating in mmap mode.
    virtual status_t start();

    // Stop a stream operating in mmap mode.
    virtual status_t stop();

    // Retrieve information on the data buffer in mmap mode.
    virtual status_t createMmapBuffer(int32_t minSizeFrames,
                                      struct audio_mmap_buffer_info *info);

    // Get current read/write position in the mmap buffer
    virtual status_t getMmapPosition(struct audio_mmap_position *position);

    // Set the priority of the thread that interacts with the HAL
    // (must match the priority of the audioflinger's thread that calls 'read' / 'write')
    virtual status_t setHalThreadPriority(int priority);

    status_t legacyCreateAudioPatch(const struct audio_port_config& port,
                                            std::optional<audio_source_t> source,
                                            audio_devices_t type) override;

    status_t legacyReleaseAudioPatch() override;

  protected:
    // Subclasses can not be constructed directly by clients.
    StreamHalHidl(std::string_view className, IStream *stream);

    ~StreamHalHidl() override;

    status_t getCachedBufferSize(size_t *size);

    status_t getHalPid(pid_t *pid);

    bool requestHalThreadPriority(pid_t threadPid, pid_t threadId);

    // mStreamPowerLog is used for audio signal power logging.
    StreamPowerLog mStreamPowerLog;

  private:
    const int HAL_THREAD_PRIORITY_DEFAULT = -1;
    IStream * const mStream;
    int mHalThreadPriority;
    size_t mCachedBufferSize;
};

class StreamOutHalHidl : public StreamOutHalInterface, public StreamHalHidl {
  public:
    // Put the audio hardware input/output into standby mode (from StreamHalInterface).
    status_t standby() override;

    // Return the frame size (number of bytes per sample) of a stream.
    virtual status_t getFrameSize(size_t *size);

    // Return the audio hardware driver estimated latency in milliseconds.
    virtual status_t getLatency(uint32_t *latency);

    // Use this method in situations where audio mixing is done in the hardware.
    virtual status_t setVolume(float left, float right);

    // Selects the audio presentation (if available).
    virtual status_t selectPresentation(int presentationId, int programId);

    // Write audio buffer to driver.
    virtual status_t write(const void *buffer, size_t bytes, size_t *written);

    // Return the number of audio frames written by the audio dsp to DAC since
    // the output has exited standby.
    virtual status_t getRenderPosition(uint64_t *dspFrames);

    // Set the callback for notifying completion of non-blocking write and drain.
    virtual status_t setCallback(wp<StreamOutHalInterfaceCallback> callback);

    // Returns whether pause and resume operations are supported.
    virtual status_t supportsPauseAndResume(bool *supportsPause, bool *supportsResume);

    // Notifies to the audio driver to resume playback following a pause.
    virtual status_t pause();

    // Notifies to the audio driver to resume playback following a pause.
    virtual status_t resume();

    // Returns whether drain operation is supported.
    virtual status_t supportsDrain(bool *supportsDrain);

    // Requests notification when data buffered by the driver/hardware has been played.
    virtual status_t drain(bool earlyNotify);

    // Notifies to the audio driver to flush (that is, drop) the queued data. Stream must
    // already be paused before calling 'flush'.
    virtual status_t flush();

    // Return a recent count of the number of audio frames presented to an external observer.
    // This excludes frames which have been written but are still in the pipeline. See the
    // table at the start of the 'StreamOutHalInterface' for the specification of the frame
    // count behavior w.r.t. 'flush', 'drain' and 'standby' operations.
    virtual status_t getPresentationPosition(uint64_t *frames, struct timespec *timestamp);

    // Notifies the HAL layer that the framework considers the current playback as completed.
    status_t presentationComplete() override;

    // Called when the metadata of the stream's source has been changed.
    status_t updateSourceMetadata(const SourceMetadata& sourceMetadata) override;

    // Methods used by StreamOutCallback (HIDL).
    void onWriteReady();
    void onDrainReady();
    void onError();

    // Returns the Dual Mono mode presentation setting.
    status_t getDualMonoMode(audio_dual_mono_mode_t* mode) override;

    // Sets the Dual Mono mode presentation on the output device.
    status_t setDualMonoMode(audio_dual_mono_mode_t mode) override;

    // Returns the Audio Description Mix level in dB.
    status_t getAudioDescriptionMixLevel(float* leveldB) override;

    // Sets the Audio Description Mix level in dB.
    status_t setAudioDescriptionMixLevel(float leveldB) override;

    // Retrieves current playback rate parameters.
    status_t getPlaybackRateParameters(audio_playback_rate_t* playbackRate) override;

    // Sets the playback rate parameters that control playback behavior.
    status_t setPlaybackRateParameters(const audio_playback_rate_t& playbackRate) override;

    status_t setEventCallback(const sp<StreamOutHalInterfaceEventCallback>& callback) override;

    // Methods used by StreamCodecFormatCallback (HIDL).
    void onCodecFormatChanged(const std::vector<uint8_t>& metadataBs);

    status_t setLatencyMode(audio_latency_mode_t mode) override;
    status_t getRecommendedLatencyModes(std::vector<audio_latency_mode_t> *modes) override;
    status_t setLatencyModeCallback(
            const sp<StreamOutHalInterfaceLatencyModeCallback>& callback) override;

    void onRecommendedLatencyModeChanged(const std::vector<audio_latency_mode_t>& modes);

    status_t exit() override;

  private:
    friend class DeviceHalHidl;
    typedef MessageQueue<WriteCommand, hardware::kSynchronizedReadWrite> CommandMQ;
    typedef MessageQueue<uint8_t, hardware::kSynchronizedReadWrite> DataMQ;
    typedef MessageQueue<WriteStatus, hardware::kSynchronizedReadWrite> StatusMQ;

    mediautils::atomic_wp<StreamOutHalInterfaceCallback> mCallback;
    mediautils::atomic_wp<StreamOutHalInterfaceEventCallback> mEventCallback;
    mediautils::atomic_wp<StreamOutHalInterfaceLatencyModeCallback> mLatencyModeCallback;

    const sp<::android::hardware::audio::CPP_VERSION::IStreamOut> mStream;
    std::unique_ptr<CommandMQ> mCommandMQ;
    std::unique_ptr<DataMQ> mDataMQ;
    std::unique_ptr<StatusMQ> mStatusMQ;
    std::atomic<pid_t> mWriterClient;
    EventFlag* mEfGroup;
    std::mutex mPositionMutex;
    // Used to expand correctly the 32-bit position from the HAL.
    uint64_t mRenderPosition GUARDED_BY(mPositionMutex) = 0;
    bool mExpectRetrograde GUARDED_BY(mPositionMutex) = false; // See 'presentationComplete'.

    // Can not be constructed directly by clients.
    StreamOutHalHidl(const sp<::android::hardware::audio::CPP_VERSION::IStreamOut>& stream);

    virtual ~StreamOutHalHidl();

    using WriterCallback = std::function<void(const WriteStatus& writeStatus)>;
    status_t callWriterThread(
            WriteCommand cmd, const char* cmdName,
            const uint8_t* data, size_t dataSize, WriterCallback callback);
    status_t prepareForWriting(size_t bufferSize);
};

class StreamInHalHidl : public StreamInHalInterface, public StreamHalHidl {
  public:
    // Return the frame size (number of bytes per sample) of a stream.
    virtual status_t getFrameSize(size_t *size);

    // Set the input gain for the audio driver.
    virtual status_t setGain(float gain);

    // Read audio buffer in from driver.
    virtual status_t read(void *buffer, size_t bytes, size_t *read);

    // Return the amount of input frames lost in the audio driver.
    virtual status_t getInputFramesLost(uint32_t *framesLost);

    // Return a recent count of the number of audio frames received and
    // the clock time associated with that frame count.
    // The count must not reset to zero when a PCM input enters standby.
    virtual status_t getCapturePosition(int64_t *frames, int64_t *time);

    // Get active microphones
    status_t getActiveMicrophones(std::vector<media::MicrophoneInfoFw> *microphones) override;

    // Set microphone direction (for processing)
    virtual status_t setPreferredMicrophoneDirection(
                            audio_microphone_direction_t direction) override;

    // Set microphone zoom (for processing)
    virtual status_t setPreferredMicrophoneFieldDimension(float zoom) override;

    // Called when the metadata of the stream's sink has been changed.
    status_t updateSinkMetadata(const SinkMetadata& sinkMetadata) override;

  private:
    friend class DeviceHalHidl;
    typedef MessageQueue<ReadParameters, hardware::kSynchronizedReadWrite> CommandMQ;
    typedef MessageQueue<uint8_t, hardware::kSynchronizedReadWrite> DataMQ;
    typedef MessageQueue<ReadStatus, hardware::kSynchronizedReadWrite> StatusMQ;

    const sp<::android::hardware::audio::CORE_TYPES_CPP_VERSION::IStreamIn> mStream;
    std::unique_ptr<CommandMQ> mCommandMQ;
    std::unique_ptr<DataMQ> mDataMQ;
    std::unique_ptr<StatusMQ> mStatusMQ;
    std::atomic<pid_t> mReaderClient;
    EventFlag* mEfGroup;

    // Can not be constructed directly by clients.
    StreamInHalHidl(
            const sp<::android::hardware::audio::CORE_TYPES_CPP_VERSION::IStreamIn>& stream);

    virtual ~StreamInHalHidl();

    using ReaderCallback = std::function<void(const ReadStatus& readStatus)>;
    status_t callReaderThread(
            const ReadParameters& params, const char* cmdName, ReaderCallback callback);
    status_t prepareForReading(size_t bufferSize);
};

} // namespace android

#endif // ANDROID_HARDWARE_STREAM_HAL_HIDL_H
