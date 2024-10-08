/*
 * Copyright (C) 2023 The Android Open Source Project
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

#define LOG_TAG "VirtualCameraRenderThread"
#include "VirtualCameraRenderThread.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "Exif.h"
#include "GLES/gl.h"
#include "VirtualCameraDevice.h"
#include "VirtualCameraSessionContext.h"
#include "aidl/android/hardware/camera/common/Status.h"
#include "aidl/android/hardware/camera/device/BufferStatus.h"
#include "aidl/android/hardware/camera/device/CameraBlob.h"
#include "aidl/android/hardware/camera/device/CameraBlobId.h"
#include "aidl/android/hardware/camera/device/CameraMetadata.h"
#include "aidl/android/hardware/camera/device/CaptureResult.h"
#include "aidl/android/hardware/camera/device/ErrorCode.h"
#include "aidl/android/hardware/camera/device/ICameraDeviceCallback.h"
#include "aidl/android/hardware/camera/device/NotifyMsg.h"
#include "aidl/android/hardware/camera/device/ShutterMsg.h"
#include "aidl/android/hardware/camera/device/StreamBuffer.h"
#include "android-base/thread_annotations.h"
#include "android/binder_auto_utils.h"
#include "android/hardware_buffer.h"
#include "hardware/gralloc.h"
#include "system/camera_metadata.h"
#include "ui/GraphicBuffer.h"
#include "ui/Rect.h"
#include "util/EglFramebuffer.h"
#include "util/JpegUtil.h"
#include "util/MetadataUtil.h"
#include "util/Util.h"
#include "utils/Errors.h"

namespace android {
namespace companion {
namespace virtualcamera {

using ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::camera::device::BufferStatus;
using ::aidl::android::hardware::camera::device::CameraBlob;
using ::aidl::android::hardware::camera::device::CameraBlobId;
using ::aidl::android::hardware::camera::device::CameraMetadata;
using ::aidl::android::hardware::camera::device::CaptureResult;
using ::aidl::android::hardware::camera::device::ErrorCode;
using ::aidl::android::hardware::camera::device::ErrorMsg;
using ::aidl::android::hardware::camera::device::ICameraDeviceCallback;
using ::aidl::android::hardware::camera::device::NotifyMsg;
using ::aidl::android::hardware::camera::device::ShutterMsg;
using ::aidl::android::hardware::camera::device::Stream;
using ::aidl::android::hardware::camera::device::StreamBuffer;
using ::aidl::android::hardware::graphics::common::PixelFormat;
using ::android::base::ScopedLockAssertion;

using ::android::hardware::camera::common::helper::ExifUtils;

namespace {

// helper type for the visitor
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
// explicit deduction guide (not needed as of C++20)
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

using namespace std::chrono_literals;

static constexpr std::chrono::milliseconds kAcquireFenceTimeout = 500ms;

// See REQUEST_PIPELINE_DEPTH in CaptureResult.java.
// This roughly corresponds to frame latency, we set to
// documented minimum of 2.
static constexpr uint8_t kPipelineDepth = 2;

static constexpr size_t kJpegThumbnailBufferSize = 32 * 1024;  // 32 KiB

static constexpr UpdateTextureTask kUpdateTextureTask;

CameraMetadata createCaptureResultMetadata(
    const std::chrono::nanoseconds timestamp,
    const RequestSettings& requestSettings,
    const Resolution reportedSensorSize) {
  // All of the keys used in the response needs to be referenced in
  // availableResultKeys in CameraCharacteristics (see initCameraCharacteristics
  // in VirtualCameraDevice.cc).
  MetadataBuilder builder =
      MetadataBuilder()
          .setAberrationCorrectionMode(
              ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF)
          .setControlAeAvailableAntibandingModes(
              {ANDROID_CONTROL_AE_ANTIBANDING_MODE_OFF})
          .setControlAeAntibandingMode(ANDROID_CONTROL_AE_ANTIBANDING_MODE_OFF)
          .setControlAeExposureCompensation(0)
          .setControlAeLockAvailable(false)
          .setControlAeLock(ANDROID_CONTROL_AE_LOCK_OFF)
          .setControlAeMode(ANDROID_CONTROL_AE_MODE_ON)
          .setControlAePrecaptureTrigger(
              // Limited devices are expected to have precapture ae enabled and
              // respond to cancellation request. Since we don't actuall support
              // AE at all, let's just respect the cancellation expectation in
              // case it's requested
              requestSettings.aePrecaptureTrigger ==
                      ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL
                  ? ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL
                  : ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE)
          .setControlAeState(ANDROID_CONTROL_AE_STATE_INACTIVE)
          .setControlAfMode(ANDROID_CONTROL_AF_MODE_OFF)
          .setControlAfTrigger(ANDROID_CONTROL_AF_TRIGGER_IDLE)
          .setControlAfState(ANDROID_CONTROL_AF_STATE_INACTIVE)
          .setControlAwbMode(ANDROID_CONTROL_AWB_MODE_AUTO)
          .setControlAwbLock(ANDROID_CONTROL_AWB_LOCK_OFF)
          .setControlAwbState(ANDROID_CONTROL_AWB_STATE_INACTIVE)
          .setControlCaptureIntent(requestSettings.captureIntent)
          .setControlEffectMode(ANDROID_CONTROL_EFFECT_MODE_OFF)
          .setControlMode(ANDROID_CONTROL_MODE_AUTO)
          .setControlSceneMode(ANDROID_CONTROL_SCENE_MODE_DISABLED)
          .setControlVideoStabilizationMode(
              ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF)
          .setCropRegion(0, 0, reportedSensorSize.width,
                         reportedSensorSize.height)
          .setFaceDetectMode(ANDROID_STATISTICS_FACE_DETECT_MODE_OFF)
          .setFlashState(ANDROID_FLASH_STATE_UNAVAILABLE)
          .setFlashMode(ANDROID_FLASH_MODE_OFF)
          .setFocalLength(VirtualCameraDevice::kFocalLength)
          .setJpegQuality(requestSettings.jpegQuality)
          .setJpegOrientation(requestSettings.jpegOrientation)
          .setJpegThumbnailSize(requestSettings.thumbnailResolution.width,
                                requestSettings.thumbnailResolution.height)
          .setJpegThumbnailQuality(requestSettings.thumbnailJpegQuality)
          .setLensOpticalStabilizationMode(
              ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF)
          .setNoiseReductionMode(ANDROID_NOISE_REDUCTION_MODE_OFF)
          .setPipelineDepth(kPipelineDepth)
          .setSensorTimestamp(timestamp)
          .setStatisticsHotPixelMapMode(
              ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF)
          .setStatisticsLensShadingMapMode(
              ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF)
          .setStatisticsSceneFlicker(ANDROID_STATISTICS_SCENE_FLICKER_NONE);

  if (requestSettings.fpsRange.has_value()) {
    builder.setControlAeTargetFpsRange(requestSettings.fpsRange.value());
  }

  if (requestSettings.gpsCoordinates.has_value()) {
    const GpsCoordinates& coordinates = requestSettings.gpsCoordinates.value();
    builder.setJpegGpsCoordinates(coordinates);
  }

  std::unique_ptr<CameraMetadata> metadata = builder.build();

  if (metadata == nullptr) {
    ALOGE("%s: Failed to build capture result metadata", __func__);
    return CameraMetadata();
  }
  return std::move(*metadata);
}

NotifyMsg createShutterNotifyMsg(int frameNumber,
                                 std::chrono::nanoseconds timestamp) {
  NotifyMsg msg;
  msg.set<NotifyMsg::Tag::shutter>(ShutterMsg{
      .frameNumber = frameNumber,
      .timestamp = timestamp.count(),
  });
  return msg;
}

NotifyMsg createBufferErrorNotifyMsg(int frameNumber, int streamId) {
  NotifyMsg msg;
  msg.set<NotifyMsg::Tag::error>(ErrorMsg{.frameNumber = frameNumber,
                                          .errorStreamId = streamId,
                                          .errorCode = ErrorCode::ERROR_BUFFER});
  return msg;
}

NotifyMsg createRequestErrorNotifyMsg(int frameNumber) {
  NotifyMsg msg;
  msg.set<NotifyMsg::Tag::error>(ErrorMsg{
      .frameNumber = frameNumber,
      // errorStreamId needs to be set to -1 for ERROR_REQUEST
      // (not tied to specific stream).
      .errorStreamId = -1,
      .errorCode = ErrorCode::ERROR_REQUEST});
  return msg;
}

std::shared_ptr<EglFrameBuffer> allocateTemporaryFramebuffer(
    EGLDisplay eglDisplay, const uint width, const int height) {
  const AHardwareBuffer_Desc desc{
      .width = static_cast<uint32_t>(width),
      .height = static_cast<uint32_t>(height),
      .layers = 1,
      .format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420,
      .usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
               AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
      .rfu0 = 0,
      .rfu1 = 0};

  AHardwareBuffer* hwBufferPtr;
  int status = AHardwareBuffer_allocate(&desc, &hwBufferPtr);
  if (status != NO_ERROR) {
    ALOGE(
        "%s: Failed to allocate hardware buffer for temporary framebuffer: %d",
        __func__, status);
    return nullptr;
  }

  return std::make_shared<EglFrameBuffer>(
      eglDisplay,
      std::shared_ptr<AHardwareBuffer>(hwBufferPtr, AHardwareBuffer_release));
}

bool isYuvFormat(const PixelFormat pixelFormat) {
  switch (static_cast<android_pixel_format_t>(pixelFormat)) {
    case HAL_PIXEL_FORMAT_YCBCR_422_I:
    case HAL_PIXEL_FORMAT_YCBCR_422_SP:
    case HAL_PIXEL_FORMAT_Y16:
    case HAL_PIXEL_FORMAT_YV12:
    case HAL_PIXEL_FORMAT_YCBCR_420_888:
      return true;
    default:
      return false;
  }
}

std::vector<uint8_t> createExif(
    Resolution imageSize, const CameraMetadata resultMetadata,
    const std::vector<uint8_t>& compressedThumbnail = {}) {
  std::unique_ptr<ExifUtils> exifUtils(ExifUtils::create());
  exifUtils->initialize();

  // Make a copy of the metadata in order to converting it the HAL metadata
  // format (as opposed to the AIDL class) and use the setFromMetadata method
  // from ExifUtil
  camera_metadata_t* rawSettings =
      clone_camera_metadata((camera_metadata_t*)resultMetadata.metadata.data());
  if (rawSettings != nullptr) {
    android::hardware::camera::common::helper::CameraMetadata halMetadata(
        rawSettings);
    exifUtils->setFromMetadata(halMetadata, imageSize.width, imageSize.height);
  }
  exifUtils->setMake(VirtualCameraDevice::kDefaultMakeAndModel);
  exifUtils->setModel(VirtualCameraDevice::kDefaultMakeAndModel);
  exifUtils->setFlash(0);

  std::vector<uint8_t> app1Data;

  size_t thumbnailDataSize = compressedThumbnail.size();
  const void* thumbnailData =
      thumbnailDataSize > 0
          ? reinterpret_cast<const void*>(compressedThumbnail.data())
          : nullptr;

  if (!exifUtils->generateApp1(thumbnailData, thumbnailDataSize)) {
    ALOGE("%s: Failed to generate APP1 segment for EXIF metadata", __func__);
    return app1Data;
  }

  const uint8_t* data = exifUtils->getApp1Buffer();
  const size_t size = exifUtils->getApp1Length();

  app1Data.insert(app1Data.end(), data, data + size);
  return app1Data;
}

std::chrono::nanoseconds getMaxFrameDuration(
    const RequestSettings& requestSettings) {
  if (requestSettings.fpsRange.has_value()) {
    return std::chrono::nanoseconds(static_cast<uint64_t>(
        1e9 / std::max(1, requestSettings.fpsRange->minFps)));
  }
  return std::chrono::nanoseconds(
      static_cast<uint64_t>(1e9 / VirtualCameraDevice::kMinFps));
}

class FrameAvailableListenerProxy : public ConsumerBase::FrameAvailableListener {
 public:
  FrameAvailableListenerProxy(std::function<void()> callback)
      : mOnFrameAvailableCallback(callback) {
  }

  virtual void onFrameAvailable(const BufferItem&) override {
    ALOGV("%s: onFrameAvailable", __func__);
    mOnFrameAvailableCallback();
  }

 private:
  std::function<void()> mOnFrameAvailableCallback;
};

}  // namespace

CaptureRequestBuffer::CaptureRequestBuffer(int streamId, int bufferId,
                                           sp<Fence> fence)
    : mStreamId(streamId), mBufferId(bufferId), mFence(fence) {
}

int CaptureRequestBuffer::getStreamId() const {
  return mStreamId;
}

int CaptureRequestBuffer::getBufferId() const {
  return mBufferId;
}

sp<Fence> CaptureRequestBuffer::getFence() const {
  return mFence;
}

VirtualCameraRenderThread::VirtualCameraRenderThread(
    VirtualCameraSessionContext& sessionContext,
    const Resolution inputSurfaceSize, const Resolution reportedSensorSize,
    std::shared_ptr<ICameraDeviceCallback> cameraDeviceCallback)
    : mCameraDeviceCallback(cameraDeviceCallback),
      mInputSurfaceSize(inputSurfaceSize),
      mReportedSensorSize(reportedSensorSize),
      mSessionContext(sessionContext),
      mInputSurfaceFuture(mInputSurfacePromise.get_future()) {
}

VirtualCameraRenderThread::~VirtualCameraRenderThread() {
  stop();
  if (mThread.joinable()) {
    mThread.join();
  }
}

ProcessCaptureRequestTask::ProcessCaptureRequestTask(
    int frameNumber, const std::vector<CaptureRequestBuffer>& requestBuffers,
    const RequestSettings& requestSettings)
    : mFrameNumber(frameNumber),
      mBuffers(requestBuffers),
      mRequestSettings(requestSettings) {
}

int ProcessCaptureRequestTask::getFrameNumber() const {
  return mFrameNumber;
}

const std::vector<CaptureRequestBuffer>& ProcessCaptureRequestTask::getBuffers()
    const {
  return mBuffers;
}

const RequestSettings& ProcessCaptureRequestTask::getRequestSettings() const {
  return mRequestSettings;
}

void VirtualCameraRenderThread::requestTextureUpdate() {
  std::lock_guard<std::mutex> lock(mLock);
  // If queue is not empty, we don't need to set the mTextureUpdateRequested
  // flag, since the texture will be updated during ProcessCaptureRequestTask
  // processing anyway.
  if (mQueue.empty()) {
    mTextureUpdateRequested = true;
    mCondVar.notify_one();
  }
}

void VirtualCameraRenderThread::enqueueTask(
    std::unique_ptr<ProcessCaptureRequestTask> task) {
  std::lock_guard<std::mutex> lock(mLock);
  // When enqueving process capture request task, clear the
  // mTextureUpdateRequested flag. If this flag is set, the texture was not yet
  // updated and it will be updated when processing ProcessCaptureRequestTask
  // anyway.
  mTextureUpdateRequested = false;
  mQueue.emplace_back(std::move(task));
  mCondVar.notify_one();
}

void VirtualCameraRenderThread::flush() {
  std::lock_guard<std::mutex> lock(mLock);
  while (!mQueue.empty()) {
    std::unique_ptr<ProcessCaptureRequestTask> task = std::move(mQueue.front());
    mQueue.pop_front();
    flushCaptureRequest(*task);
  }
}

void VirtualCameraRenderThread::start() {
  mThread = std::thread(&VirtualCameraRenderThread::threadLoop, this);
}

void VirtualCameraRenderThread::stop() {
  {
    std::lock_guard<std::mutex> lock(mLock);
    mPendingExit = true;
    mCondVar.notify_one();
  }
}

sp<Surface> VirtualCameraRenderThread::getInputSurface() {
  return mInputSurfaceFuture.get();
}

RenderThreadTask VirtualCameraRenderThread::dequeueTask() {
  std::unique_lock<std::mutex> lock(mLock);
  // Clang's thread safety analysis doesn't perform alias analysis,
  // so it doesn't support moveable std::unique_lock.
  //
  // Lock assertion below is basically explicit declaration that
  // the lock is held in this scope, which is true, since it's only
  // released during waiting inside mCondVar.wait calls.
  ScopedLockAssertion lockAssertion(mLock);

  mCondVar.wait(lock, [this]() REQUIRES(mLock) {
    return mPendingExit || mTextureUpdateRequested || !mQueue.empty();
  });
  if (mPendingExit) {
    // Render thread task with null task signals render thread to terminate.
    return RenderThreadTask(nullptr);
  }
  if (mTextureUpdateRequested) {
    // If mTextureUpdateRequested, it's guaranteed the queue is empty, return
    // kUpdateTextureTask to signal we want render thread to update the texture
    // (consume buffer from the queue).
    mTextureUpdateRequested = false;
    return RenderThreadTask(kUpdateTextureTask);
  }
  RenderThreadTask task(std::move(mQueue.front()));
  mQueue.pop_front();
  return task;
}

void VirtualCameraRenderThread::threadLoop() {
  ALOGV("Render thread starting");

  mEglDisplayContext = std::make_unique<EglDisplayContext>();
  mEglTextureYuvProgram =
      std::make_unique<EglTextureProgram>(EglTextureProgram::TextureFormat::YUV);
  mEglTextureRgbProgram = std::make_unique<EglTextureProgram>(
      EglTextureProgram::TextureFormat::RGBA);
  mEglSurfaceTexture = std::make_unique<EglSurfaceTexture>(
      mInputSurfaceSize.width, mInputSurfaceSize.height);
  sp<FrameAvailableListenerProxy> frameAvailableListener =
      sp<FrameAvailableListenerProxy>::make(
          [this]() { requestTextureUpdate(); });
  mEglSurfaceTexture->setFrameAvailableListener(frameAvailableListener);

  mInputSurfacePromise.set_value(mEglSurfaceTexture->getSurface());

  while (RenderThreadTask task = dequeueTask()) {
    std::visit(
        overloaded{[this](const std::unique_ptr<ProcessCaptureRequestTask>& t) {
                     processTask(*t);
                   },
                   [this](const UpdateTextureTask&) {
                     ALOGV("Idle update of the texture");
                     mEglSurfaceTexture->updateTexture();
                   }},
        task);
  }

  // Destroy EGL utilities still on the render thread.
  mEglSurfaceTexture.reset();
  mEglTextureRgbProgram.reset();
  mEglTextureYuvProgram.reset();
  mEglDisplayContext.reset();

  ALOGV("Render thread exiting");
}

void VirtualCameraRenderThread::processTask(
    const ProcessCaptureRequestTask& request) {
  std::chrono::nanoseconds timestamp =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch());
  const std::chrono::nanoseconds lastAcquisitionTimestamp(
      mLastAcquisitionTimestampNanoseconds.exchange(timestamp.count(),
                                                    std::memory_order_relaxed));

  if (request.getRequestSettings().fpsRange) {
    const int maxFps =
        std::max(1, request.getRequestSettings().fpsRange->maxFps);
    const std::chrono::nanoseconds minFrameDuration(
        static_cast<uint64_t>(1e9 / maxFps));
    const std::chrono::nanoseconds frameDuration =
        timestamp - lastAcquisitionTimestamp;
    if (frameDuration < minFrameDuration) {
      // We're too fast for the configured maxFps, let's wait a bit.
      const std::chrono::nanoseconds sleepTime =
          minFrameDuration - frameDuration;
      ALOGV("Current frame duration would  be %" PRIu64
            " ns corresponding to, "
            "sleeping for %" PRIu64
            " ns before updating texture to match maxFps %d",
            static_cast<uint64_t>(frameDuration.count()),
            static_cast<uint64_t>(sleepTime.count()), maxFps);

      std::this_thread::sleep_for(sleepTime);
      timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch());
      mLastAcquisitionTimestampNanoseconds.store(timestamp.count(),
                                                 std::memory_order_relaxed);
    }
  }

  // Calculate the maximal amount of time we can afford to wait for next frame.
  const std::chrono::nanoseconds maxFrameDuration =
      getMaxFrameDuration(request.getRequestSettings());
  const std::chrono::nanoseconds elapsedDuration =
      timestamp - lastAcquisitionTimestamp;
  if (elapsedDuration < maxFrameDuration) {
    // We can afford to wait for next frame.
    // Note that if there's already new frame in the input Surface, the call
    // below returns immediatelly.
    bool gotNewFrame = mEglSurfaceTexture->waitForNextFrame(maxFrameDuration -
                                                            elapsedDuration);
    timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    if (!gotNewFrame) {
      ALOGV(
          "%s: No new frame received on input surface after waiting for "
          "%" PRIu64 "ns, repeating last frame.",
          __func__,
          static_cast<uint64_t>((timestamp - lastAcquisitionTimestamp).count()));
    }
    mLastAcquisitionTimestampNanoseconds.store(timestamp.count(),
                                               std::memory_order_relaxed);
  }
  // Acquire new (most recent) image from the Surface.
  mEglSurfaceTexture->updateTexture();

  CaptureResult captureResult;
  captureResult.fmqResultSize = 0;
  captureResult.frameNumber = request.getFrameNumber();
  // Partial result needs to be set to 1 when metadata are present.
  captureResult.partialResult = 1;
  captureResult.inputBuffer.streamId = -1;
  captureResult.physicalCameraMetadata.resize(0);
  captureResult.result = createCaptureResultMetadata(
      timestamp, request.getRequestSettings(), mReportedSensorSize);

  const std::vector<CaptureRequestBuffer>& buffers = request.getBuffers();
  captureResult.outputBuffers.resize(buffers.size());

  for (int i = 0; i < buffers.size(); ++i) {
    const CaptureRequestBuffer& reqBuffer = buffers[i];
    StreamBuffer& resBuffer = captureResult.outputBuffers[i];
    resBuffer.streamId = reqBuffer.getStreamId();
    resBuffer.bufferId = reqBuffer.getBufferId();
    resBuffer.status = BufferStatus::OK;

    const std::optional<Stream> streamConfig =
        mSessionContext.getStreamConfig(reqBuffer.getStreamId());

    if (!streamConfig.has_value()) {
      resBuffer.status = BufferStatus::ERROR;
      continue;
    }

    auto status = streamConfig->format == PixelFormat::BLOB
                      ? renderIntoBlobStreamBuffer(
                            reqBuffer.getStreamId(), reqBuffer.getBufferId(),
                            captureResult.result, request.getRequestSettings(),
                            reqBuffer.getFence())
                      : renderIntoImageStreamBuffer(reqBuffer.getStreamId(),
                                                    reqBuffer.getBufferId(),
                                                    reqBuffer.getFence());
    if (!status.isOk()) {
      resBuffer.status = BufferStatus::ERROR;
    }
  }

  std::vector<NotifyMsg> notifyMsg{
      createShutterNotifyMsg(request.getFrameNumber(), timestamp)};
  for (const StreamBuffer& resBuffer : captureResult.outputBuffers) {
    if (resBuffer.status != BufferStatus::OK) {
      notifyMsg.push_back(createBufferErrorNotifyMsg(request.getFrameNumber(),
                                                     resBuffer.streamId));
    }
  }

  auto status = mCameraDeviceCallback->notify(notifyMsg);
  if (!status.isOk()) {
    ALOGE("%s: notify call failed: %s", __func__,
          status.getDescription().c_str());
    return;
  }

  std::vector<::aidl::android::hardware::camera::device::CaptureResult>
      captureResults(1);
  captureResults[0] = std::move(captureResult);

  status = mCameraDeviceCallback->processCaptureResult(captureResults);
  if (!status.isOk()) {
    ALOGE("%s: processCaptureResult call failed: %s", __func__,
          status.getDescription().c_str());
    return;
  }

  ALOGV("%s: Successfully called processCaptureResult", __func__);
}

void VirtualCameraRenderThread::flushCaptureRequest(
    const ProcessCaptureRequestTask& request) {
  CaptureResult captureResult;
  captureResult.fmqResultSize = 0;
  captureResult.frameNumber = request.getFrameNumber();
  captureResult.inputBuffer.streamId = -1;

  const std::vector<CaptureRequestBuffer>& buffers = request.getBuffers();
  captureResult.outputBuffers.resize(buffers.size());

  for (int i = 0; i < buffers.size(); ++i) {
    const CaptureRequestBuffer& reqBuffer = buffers[i];
    StreamBuffer& resBuffer = captureResult.outputBuffers[i];
    resBuffer.streamId = reqBuffer.getStreamId();
    resBuffer.bufferId = reqBuffer.getBufferId();
    resBuffer.status = BufferStatus::ERROR;
    sp<Fence> fence = reqBuffer.getFence();
    if (fence != nullptr && fence->isValid()) {
      resBuffer.releaseFence.fds.emplace_back(fence->dup());
    }
  }

  auto status = mCameraDeviceCallback->notify(
      {createRequestErrorNotifyMsg(request.getFrameNumber())});
  if (!status.isOk()) {
    ALOGE("%s: notify call failed: %s", __func__,
          status.getDescription().c_str());
    return;
  }

  std::vector<::aidl::android::hardware::camera::device::CaptureResult>
      captureResults(1);
  captureResults[0] = std::move(captureResult);

  status = mCameraDeviceCallback->processCaptureResult(captureResults);
  if (!status.isOk()) {
    ALOGE("%s: processCaptureResult call failed: %s", __func__,
          status.getDescription().c_str());
  }
}

std::vector<uint8_t> VirtualCameraRenderThread::createThumbnail(
    const Resolution resolution, const int quality) {
  if (resolution.width == 0 || resolution.height == 0) {
    ALOGV("%s: Skipping thumbnail creation, zero size requested", __func__);
    return {};
  }

  ALOGV("%s: Creating thumbnail with size %d x %d, quality %d", __func__,
        resolution.width, resolution.height, quality);
  Resolution bufferSize = roundTo2DctSize(resolution);
  std::shared_ptr<EglFrameBuffer> framebuffer = allocateTemporaryFramebuffer(
      mEglDisplayContext->getEglDisplay(), bufferSize.width, bufferSize.height);
  if (framebuffer == nullptr) {
    ALOGE(
        "Failed to allocate temporary framebuffer for JPEG thumbnail "
        "compression");
    return {};
  }

  // TODO(b/324383963) Add support for letterboxing if the thumbnail size
  // doesn't correspond
  //  to input texture aspect ratio.
  if (!renderIntoEglFramebuffer(*framebuffer, /*fence=*/nullptr,
                                Rect(resolution.width, resolution.height))
           .isOk()) {
    ALOGE(
        "Failed to render input texture into temporary framebuffer for JPEG "
        "thumbnail");
    return {};
  }

  std::vector<uint8_t> compressedThumbnail;
  compressedThumbnail.resize(kJpegThumbnailBufferSize);
  ALOGE("%s: Compressing thumbnail %d x %d", __func__, resolution.width,
        resolution.height);
  std::optional<size_t> compressedSize =
      compressJpeg(resolution.width, resolution.height, quality,
                   framebuffer->getHardwareBuffer(), {},
                   compressedThumbnail.size(), compressedThumbnail.data());
  if (!compressedSize.has_value()) {
    ALOGE("%s: Failed to compress jpeg thumbnail", __func__);
    return {};
  }
  compressedThumbnail.resize(compressedSize.value());
  return compressedThumbnail;
}

ndk::ScopedAStatus VirtualCameraRenderThread::renderIntoBlobStreamBuffer(
    const int streamId, const int bufferId, const CameraMetadata& resultMetadata,
    const RequestSettings& requestSettings, sp<Fence> fence) {
  std::shared_ptr<AHardwareBuffer> hwBuffer =
      mSessionContext.fetchHardwareBuffer(streamId, bufferId);
  if (hwBuffer == nullptr) {
    ALOGE("%s: Failed to fetch hardware buffer %d for streamId %d", __func__,
          bufferId, streamId);
    return cameraStatus(Status::INTERNAL_ERROR);
  }

  std::optional<Stream> stream = mSessionContext.getStreamConfig(streamId);
  if (!stream.has_value()) {
    ALOGE("%s, failed to fetch information about stream %d", __func__, streamId);
    return cameraStatus(Status::INTERNAL_ERROR);
  }

  ALOGV("%s: Rendering JPEG with size %d x %d, quality %d", __func__,
        stream->width, stream->height, requestSettings.jpegQuality);

  // Let's create YUV framebuffer and render the surface into this.
  // This will take care about rescaling as well as potential format conversion.
  // The buffer dimensions need to be rounded to nearest multiple of JPEG DCT
  // size, however we pass the viewport corresponding to size of the stream so
  // the image will be only rendered to the area corresponding to the stream
  // size.
  Resolution bufferSize =
      roundTo2DctSize(Resolution(stream->width, stream->height));
  std::shared_ptr<EglFrameBuffer> framebuffer = allocateTemporaryFramebuffer(
      mEglDisplayContext->getEglDisplay(), bufferSize.width, bufferSize.height);
  if (framebuffer == nullptr) {
    ALOGE("Failed to allocate temporary framebuffer for JPEG compression");
    return cameraStatus(Status::INTERNAL_ERROR);
  }

  // Render into temporary framebuffer.
  ndk::ScopedAStatus status = renderIntoEglFramebuffer(
      *framebuffer, /*fence=*/nullptr, Rect(stream->width, stream->height));
  if (!status.isOk()) {
    ALOGE("Failed to render input texture into temporary framebuffer");
    return status;
  }

  PlanesLockGuard planesLock(hwBuffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                             fence);
  if (planesLock.getStatus() != OK) {
    return cameraStatus(Status::INTERNAL_ERROR);
  }

  std::vector<uint8_t> app1ExifData =
      createExif(Resolution(stream->width, stream->height), resultMetadata,
                 createThumbnail(requestSettings.thumbnailResolution,
                                 requestSettings.thumbnailJpegQuality));
  std::optional<size_t> compressedSize = compressJpeg(
      stream->width, stream->height, requestSettings.jpegQuality,
      framebuffer->getHardwareBuffer(), app1ExifData,
      stream->bufferSize - sizeof(CameraBlob), (*planesLock).planes[0].data);

  if (!compressedSize.has_value()) {
    ALOGE("%s: Failed to compress JPEG image", __func__);
    return cameraStatus(Status::INTERNAL_ERROR);
  }

  CameraBlob cameraBlob{
      .blobId = CameraBlobId::JPEG,
      .blobSizeBytes = static_cast<int32_t>(compressedSize.value())};

  memcpy(reinterpret_cast<uint8_t*>((*planesLock).planes[0].data) +
             (stream->bufferSize - sizeof(cameraBlob)),
         &cameraBlob, sizeof(cameraBlob));

  ALOGV("%s: Successfully compressed JPEG image, resulting size %zu B",
        __func__, compressedSize.value());

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraRenderThread::renderIntoImageStreamBuffer(
    int streamId, int bufferId, sp<Fence> fence) {
  ALOGV("%s", __func__);

  const std::chrono::nanoseconds before =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch());

  // Render test pattern using EGL.
  std::shared_ptr<EglFrameBuffer> framebuffer =
      mSessionContext.fetchOrCreateEglFramebuffer(
          mEglDisplayContext->getEglDisplay(), streamId, bufferId);
  if (framebuffer == nullptr) {
    ALOGE(
        "%s: Failed to get EGL framebuffer corresponding to buffer id "
        "%d for streamId %d",
        __func__, bufferId, streamId);
    return cameraStatus(Status::ILLEGAL_ARGUMENT);
  }

  ndk::ScopedAStatus status = renderIntoEglFramebuffer(*framebuffer, fence);

  const std::chrono::nanoseconds after =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch());

  ALOGV("Rendering to buffer %d, stream %d took %lld ns", bufferId, streamId,
        after.count() - before.count());

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraRenderThread::renderIntoEglFramebuffer(
    EglFrameBuffer& framebuffer, sp<Fence> fence, std::optional<Rect> viewport) {
  ALOGV("%s", __func__);
  // Wait for fence to clear.
  if (fence != nullptr && fence->isValid()) {
    status_t ret = fence->wait(kAcquireFenceTimeout.count());
    if (ret != 0) {
      ALOGE("Timeout while waiting for the acquire fence for buffer");
      return cameraStatus(Status::INTERNAL_ERROR);
    }
  }

  mEglDisplayContext->makeCurrent();
  framebuffer.beforeDraw();

  Rect viewportRect =
      viewport.value_or(Rect(framebuffer.getWidth(), framebuffer.getHeight()));
  glViewport(viewportRect.left, viewportRect.top, viewportRect.getWidth(),
             viewportRect.getHeight());

  sp<GraphicBuffer> textureBuffer = mEglSurfaceTexture->getCurrentBuffer();
  if (textureBuffer == nullptr) {
    // If there's no current buffer, nothing was written to the surface and
    // texture is not initialized yet. Let's render the framebuffer black
    // instead of rendering the texture.
    glClearColor(0.0f, 0.5f, 0.5f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
  } else {
    const bool renderSuccess =
        isYuvFormat(static_cast<PixelFormat>(textureBuffer->getPixelFormat()))
            ? mEglTextureYuvProgram->draw(
                  mEglSurfaceTexture->getTextureId(),
                  mEglSurfaceTexture->getTransformMatrix())
            : mEglTextureRgbProgram->draw(
                  mEglSurfaceTexture->getTextureId(),
                  mEglSurfaceTexture->getTransformMatrix());
    if (!renderSuccess) {
      ALOGE("%s: Failed to render texture", __func__);
      return cameraStatus(Status::INTERNAL_ERROR);
    }
  }
  framebuffer.afterDraw();

  return ndk::ScopedAStatus::ok();
}

}  // namespace virtualcamera
}  // namespace companion
}  // namespace android
