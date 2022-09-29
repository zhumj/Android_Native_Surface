/*
 * Copyright 2013 The Android Open Source Project
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

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <termios.h>
#include <unistd.h>

#define LOG_TAG "ScreenRecord"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <binder/IPCThreadState.h>
#include <utils/Errors.h>
#include <utils/SystemClock.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>
#include <media/MediaCodecBuffer.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormatPriv.h>
#include <media/NdkMediaMuxer.h>
#include <media/openmax/OMX_IVCommon.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecConstants.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/PersistentSurface.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <mediadrm/ICrypto.h>
#include <ui/DisplayMode.h>
#include <ui/DisplayState.h>

#include "screenrecord.h"
#include "Overlay.h"

using android::ABuffer;
using android::ALooper;
using android::AMessage;
using android::AString;
using android::ui::DisplayMode;
using android::IBinder;
using android::IGraphicBufferProducer;
using android::ISurfaceComposer;
using android::MediaCodec;
using android::MediaCodecBuffer;
using android::Overlay;
using android::PersistentSurface;
using android::PhysicalDisplayId;
using android::ProcessState;
using android::Rect;
using android::String8;
using android::SurfaceComposerClient;
using android::Vector;
using android::sp;
using android::status_t;

using android::INVALID_OPERATION;
using android::NAME_NOT_FOUND;
using android::NO_ERROR;
using android::UNKNOWN_ERROR;

namespace ui = android::ui;

static const uint32_t kMinBitRate = 100000;         // 0.1Mbps
static const uint32_t kMaxBitRate = 200 * 1000000;  // 200Mbps
static const uint32_t kMaxTimeLimitSec = 180;       // 3 minutes
static const uint32_t kFallbackWidth = 1280;        // 720p
static const uint32_t kFallbackHeight = 720;
static const char *kMimeTypeAvc = "video/avc";
static const char *kMimeTypeApplicationOctetstream = "application/octet-stream";
static const char *kWinscopeMagicString = "#VV1NSC0PET1ME!#";

// Command-line parameters.
static bool gVerbose = false;           // chatty on stdout
static bool gRotate = false;            // rotate 90 degrees
static bool gMonotonicTime = false;     // use system monotonic time for timestamps
static bool gPersistentSurface = false; // use persistent surface
static enum {
    FORMAT_MP4, FORMAT_H264, FORMAT_WEBM, FORMAT_3GPP, FORMAT_FRAMES, FORMAT_RAW_FRAMES
} gOutputFormat = FORMAT_MP4;           // data format for output
static AString gCodecName = "";         // codec name override
static bool gSizeSpecified = false;     // was size explicitly requested?
static bool gWantInfoScreen = false;    // do we want initial info screen?
static bool gWantFrameTime = false;     // do we want times on each frame?
static uint32_t gVideoWidth = 0;        // default width+height
static uint32_t gVideoHeight = 0;
static uint32_t gBitRate = 20000000;     // 20Mbps
static uint32_t gTimeLimitSec = kMaxTimeLimitSec;
static uint32_t gBframes = 0;
static PhysicalDisplayId gPhysicalDisplayId;
// Set by signal handler to stop recording.
// static volatile bool gStopRequested = false;

// Previous signal handler state, restored after first hit.
static struct sigaction gOrigSigactionINT;
static struct sigaction gOrigSigactionHUP;
bool gStopRequested = false;


/*
 * Catch keyboard interrupt signals.  On receipt, the "stop requested"
 * flag is raised, and the original handler is restored (so that, if
 * we get stuck finishing, a second Ctrl-C will kill the process).
 */
static void signalCatcher(int signum) {
    gStopRequested = true;
    switch (signum) {
        case SIGINT:
        case SIGHUP:
            sigaction(SIGINT, &gOrigSigactionINT, NULL);
            sigaction(SIGHUP, &gOrigSigactionHUP, NULL);
            break;
        default:
            abort();
            break;
    }
}

/*
 * Configures signal handlers.  The previous handlers are saved.
 *
 * If the command is run from an interactive adb shell, we get SIGINT
 * when Ctrl-C is hit.  If we're run from the host, the local adb process
 * gets the signal, and we get a SIGHUP when the terminal disconnects.
 */
static status_t configureSignals() {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = signalCatcher;
    if (sigaction(SIGINT, &act, &gOrigSigactionINT) != 0) {
        status_t err = -errno;
        fprintf(stderr, "Unable to configure SIGINT handler: %s\n",
                strerror(errno));
        return err;
    }
    if (sigaction(SIGHUP, &act, &gOrigSigactionHUP) != 0) {
        status_t err = -errno;
        fprintf(stderr, "Unable to configure SIGHUP handler: %s\n",
                strerror(errno));
        return err;
    }
    signal(SIGPIPE, SIG_IGN);
    return NO_ERROR;
}

/*
 * Configures and starts the MediaCodec encoder.  Obtains an input surface
 * from the codec.
 */
status_t prepareEncoder(float displayFps, sp<MediaCodec> *pCodec,
                        sp<IGraphicBufferProducer> *pBufferProducer) {
    status_t err;

    if (gVerbose) {
        printf("Configuring recorder for %dx%d %s at %.2fMbps\n",
               gVideoWidth, gVideoHeight, kMimeTypeAvc, gBitRate / 1000000.0);
        fflush(stdout);
    }

    sp<AMessage> format = new AMessage;
    format->setInt32(KEY_WIDTH, gVideoWidth);
    format->setInt32(KEY_HEIGHT, gVideoHeight);
    format->setString(KEY_MIME, kMimeTypeAvc);
    format->setInt32(KEY_COLOR_FORMAT, OMX_COLOR_FormatAndroidOpaque);
    format->setInt32(KEY_BIT_RATE, gBitRate);
    format->setFloat(KEY_FRAME_RATE, displayFps);
    format->setInt32(KEY_I_FRAME_INTERVAL, 10);
    format->setInt32(KEY_MAX_B_FRAMES, gBframes);
    if (gBframes > 0) {
        format->setInt32(KEY_PROFILE, AVCProfileMain);
        format->setInt32(KEY_LEVEL, AVCLevel41);
    }

    sp<android::ALooper> looper = new android::ALooper;
    looper->setName("screenrecord_looper");
    looper->start();
    ALOGV("Creating codec");
    sp<MediaCodec> codec;
    if (gCodecName.empty()) {
        codec = MediaCodec::CreateByType(looper, kMimeTypeAvc, true);
        if (codec == NULL) {
            fprintf(stderr, "ERROR: unable to create %s codec instance\n",
                    kMimeTypeAvc);
            return UNKNOWN_ERROR;
        }
    } else {
        codec = MediaCodec::CreateByComponentName(looper, gCodecName);
        if (codec == NULL) {
            fprintf(stderr, "ERROR: unable to create %s codec instance\n",
                    gCodecName.c_str());
            return UNKNOWN_ERROR;
        }
    }

    err = codec->configure(format, NULL, NULL,
                           MediaCodec::CONFIGURE_FLAG_ENCODE);
    if (err != NO_ERROR) {
        fprintf(stderr, "ERROR: unable to configure %s codec at %dx%d (err=%d)\n",
                kMimeTypeAvc, gVideoWidth, gVideoHeight, err);
        codec->release();
        return err;
    }

    ALOGV("Creating encoder input surface");
    sp<IGraphicBufferProducer> bufferProducer;
    if (gPersistentSurface) {
        sp<PersistentSurface> surface = MediaCodec::CreatePersistentInputSurface();
        bufferProducer = surface->getBufferProducer();
        err = codec->setInputSurface(surface);
    } else {
        err = codec->createInputSurface(&bufferProducer);
    }
    if (err != NO_ERROR) {
        fprintf(stderr,
                "ERROR: unable to %s encoder input surface (err=%d)\n",
                gPersistentSurface ? "set" : "create",
                err);
        codec->release();
        return err;
    }

    ALOGV("Starting codec");
    err = codec->start();
    if (err != NO_ERROR) {
        fprintf(stderr, "ERROR: unable to start codec (err=%d)\n", err);
        codec->release();
        return err;
    }

    ALOGV("Codec prepared");
    *pCodec = codec;
    *pBufferProducer = bufferProducer;
    return 0;
}

/*
 * Sets the display projection, based on the display dimensions, video size,
 * and device orientation.
 */
static status_t setDisplayProjection(
        SurfaceComposerClient::Transaction &t,
        const sp<IBinder> &dpy,
        const ui::DisplayState &displayState) {
    // Set the region of the layer stack we're interested in, which in our case is "all of it".
    Rect layerStackRect(displayState.layerStackSpaceRect);

    // We need to preserve the aspect ratio of the display.
    float displayAspect = layerStackRect.getHeight() / static_cast<float>(layerStackRect.getWidth());


    // Set the way we map the output onto the display surface (which will
    // be e.g. 1280x720 for a 720p video).  The rect is interpreted
    // post-rotation, so if the display is rotated 90 degrees we need to
    // "pre-rotate" it by flipping width/height, so that the orientation
    // adjustment changes it back.
    //
    // We might want to encode a portrait display as landscape to use more
    // of the screen real estate.  (If players respect a 90-degree rotation
    // hint, we can essentially get a 720x1280 video instead of 1280x720.)
    // In that case, we swap the configured video width/height and then
    // supply a rotation value to the display projection.
    uint32_t videoWidth, videoHeight;
    uint32_t outWidth, outHeight;
    if (!gRotate) {
        videoWidth = gVideoWidth;
        videoHeight = gVideoHeight;
    } else {
        videoWidth = gVideoHeight;
        videoHeight = gVideoWidth;
    }
    if (videoHeight > (uint32_t) (videoWidth * displayAspect)) {
        // limited by narrow width; reduce height
        outWidth = videoWidth;
        outHeight = (uint32_t) (videoWidth * displayAspect);
    } else {
        // limited by short height; restrict width
        outHeight = videoHeight;
        outWidth = (uint32_t) (videoHeight / displayAspect);
    }
    uint32_t offX, offY;
    offX = (videoWidth - outWidth) / 2;
    offY = (videoHeight - outHeight) / 2;
    Rect displayRect(offX, offY, offX + outWidth, offY + outHeight);

    if (gVerbose) {
        if (gRotate) {
            printf("Rotated content area is %ux%u at offset x=%d y=%d\n",
                   outHeight, outWidth, offY, offX);
            fflush(stdout);
        } else {
            printf("Content area is %ux%u at offset x=%d y=%d\n",
                   outWidth, outHeight, offX, offY);
            fflush(stdout);
        }
    }

    t.setDisplayProjection(dpy,
                           gRotate ? ui::ROTATION_90 : ui::ROTATION_0,
                           layerStackRect, displayRect);
    return NO_ERROR;
}

/*
 * Configures the virtual display.  When this completes, virtual display
 * frames will start arriving from the buffer producer.
 */
static status_t prepareVirtualDisplay(
        const ui::DisplayState &displayState,
        const sp<IGraphicBufferProducer> &bufferProducer,
        sp<IBinder> *pDisplayHandle) {
    sp<IBinder> dpy = SurfaceComposerClient::createDisplay(
            String8("ScreenRecorder"), false /*secure*/);
    SurfaceComposerClient::Transaction t;
    t.setDisplaySurface(dpy, bufferProducer);
    setDisplayProjection(t, dpy, displayState);
    t.setDisplayLayerStack(dpy, displayState.layerStack);
    t.apply();

    *pDisplayHandle = dpy;

    return NO_ERROR;
}

/*
 * Writes an unsigned integer byte-by-byte in little endian order regardless
 * of the platform endianness.
 */
template<typename UINT>
static void writeValueLE(UINT value, uint8_t *buffer) {
    for (int i = 0; i < sizeof(UINT); ++i) {
        buffer[i] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

/*
 * Saves frames presentation time relative to the elapsed realtime clock in microseconds
 * preceded by a Winscope magic string and frame count to a metadata track.
 * This metadata is used by the Winscope tool to sync video with SurfaceFlinger
 * and WindowManager traces.
 *
 * The metadata is written as a binary array as follows:
 * - winscope magic string (kWinscopeMagicString constant), without trailing null char,
 * - the number of recorded frames (as little endian uint32),
 * - for every frame its presentation time relative to the elapsed realtime clock in microseconds
 *   (as little endian uint64).
 */
static status_t writeWinscopeMetadata(const Vector<int64_t> &timestamps,
                                      const ssize_t metaTrackIdx, AMediaMuxer *muxer) {
    ALOGV("Writing metadata");
    int64_t systemTimeToElapsedTimeOffsetMicros = (android::elapsedRealtimeNano()
                                                   - systemTime(SYSTEM_TIME_MONOTONIC)) / 1000;
    sp<ABuffer> buffer = new ABuffer(timestamps.size() * sizeof(int64_t)
                                     + sizeof(uint32_t) + strlen(kWinscopeMagicString));
    uint8_t *pos = buffer->data();
    strcpy(reinterpret_cast<char *>(pos), kWinscopeMagicString);
    pos += strlen(kWinscopeMagicString);
    writeValueLE<uint32_t>(timestamps.size(), pos);
    pos += sizeof(uint32_t);
    for (size_t idx = 0; idx < timestamps.size(); ++idx) {
        writeValueLE<uint64_t>(static_cast<uint64_t>(timestamps[idx]
                                                     + systemTimeToElapsedTimeOffsetMicros), pos);
        pos += sizeof(uint64_t);
    }
    AMediaCodecBufferInfo bufferInfo = {
            0,
            static_cast<int32_t>(buffer->size()),
            timestamps[0],
            0
    };
    return AMediaMuxer_writeSampleData(muxer, metaTrackIdx, buffer->data(), &bufferInfo);
}

/*
 * Runs the MediaCodec encoder, sending the output to the MediaMuxer.  The
 * input frames are coming from the virtual display as fast as SurfaceFlinger
 * wants to send them.
 *
 * Exactly one of muxer or rawFp must be non-null.
 *
 * The muxer must *not* have been started before calling.
 */
/*
* 运行MediaCodec编码器，将输出发送到MediaMuxer。这个
* 输入帧来自虚拟显示器的速度与SurfaceFlinger一样快
* 希望发送它们。
* muxer或rawFp中必须有一个是非空的。
* 在调用之前，muxer必须“未”启动。
*/
sp<MediaCodec> encoder;
AMediaMuxer *muxer = nullptr;
FILE *rawFp = NULL;
sp<IBinder> display;
sp<IBinder> virtualDpy;
ui::Rotation orientation;
sp<IGraphicBufferProducer> encoderInputSurface;
sp<IGraphicBufferProducer> bufferProducer;
sp<Overlay> overlay;

status_t runEncoder(bool *runFlag, void callback(uint8_t *, size_t)) {
    static int kTimeout = 250000;   // be responsive on signal 响应信号
    status_t err;
    ssize_t trackIdx = -1;
    ssize_t metaTrackIdx = -1;
    uint32_t debugNumFrames = 0;
    int64_t startWhenNsec = systemTime(CLOCK_MONOTONIC);
    int64_t endWhenNsec = startWhenNsec + seconds_to_nanoseconds(gTimeLimitSec);
    Vector<int64_t> timestamps;
    bool firstFrame = true;

    assert((rawFp == NULL && muxer != NULL) || (rawFp != NULL && muxer == NULL));

    Vector<sp<MediaCodecBuffer> > buffers;
    err = encoder->getOutputBuffers(&buffers);
    if (err != NO_ERROR) {
        fprintf(stderr, "Unable to get output buffers (err=%d)\n", err);
        return err;
    }

    // Run until we're signaled.
    while ((*runFlag) && !gStopRequested) {
        size_t bufIndex, offset, size;
        int64_t ptsUsec;
        uint32_t flags;

        if (firstFrame) {
            ATRACE_NAME("first_frame");
            firstFrame = false;
        }

        if (systemTime(CLOCK_MONOTONIC) > endWhenNsec) {
            if (gVerbose) {
                printf("Time limit reached\n");
                fflush(stdout);
            }
            break;
        }

        ALOGV("Calling dequeueOutputBuffer");
        err = encoder->dequeueOutputBuffer(&bufIndex, &offset, &size, &ptsUsec,
                                           &flags, kTimeout);
        ALOGV("dequeueOutputBuffer returned %d", err);
        switch (err) {
            case NO_ERROR:
                // got a buffer
                if ((flags & MediaCodec::BUFFER_FLAG_CODECCONFIG) != 0) {
                    ALOGV("Got codec config buffer (%zu bytes)", size);
                    if (muxer != NULL) {
                        // ignore this -- we passed the CSD into MediaMuxer when
                        // we got the format change notification
                        size = 0;
                    }
                }
                if (size != 0) {
                    ALOGV("Got data in buffer %zu, size=%zu, pts=%" PRId64,
                          bufIndex, size, ptsUsec);

                    { // scope
                        ATRACE_NAME("orientation");
                        // Check orientation, update if it has changed.
                        //
                        // Polling for changes is inefficient and wrong, but the
                        // useful stuff is hard to get at without a Dalvik VM.
                        ui::DisplayState displayState;
                        err = SurfaceComposerClient::getDisplayState(display, &displayState);
                        if (err != NO_ERROR) {
                            ALOGW("getDisplayState() failed: %d", err);
                        } else if (orientation != displayState.orientation) {
                            ALOGD("orientation changed, now %s", toCString(displayState.orientation));
                            SurfaceComposerClient::Transaction t;
                            setDisplayProjection(t, virtualDpy, displayState);
                            t.apply();
                            orientation = displayState.orientation;
                        }
                    }

                    // If the virtual display isn't providing us with timestamps,
                    // use the current time.  This isn't great -- we could get
                    // decoded data in clusters -- but we're not expecting
                    // to hit this anyway.

                    // 如果虚拟显示器没有为我们提供时间戳，
                    // 使用当前时间。这不太好，我们可以
                    // 集群中的解码数据--但我们并不期望
                    // 无论如何都要打这个。
                    if (ptsUsec == 0) {
                        ptsUsec = systemTime(SYSTEM_TIME_MONOTONIC) / 1000;
                    }

                    if (muxer == NULL) {
                        // fseek(rawFp,0,1);
                        //   fseek( rawFp, 1, SEEK_SET );
                        // fwrite(buffers[bufIndex]->data(), 1, size, rawFp);
                        // Flush the data immediately in case we're streaming.
                        // We don't want to do this if all we've written is
                        // the SPS/PPS data because mplayer gets confused.
                        // if ((flags & MediaCodec::BUFFER_FLAG_CODECCONFIG) == 0) {
                        //     fflush(rawFp);
                        // }
                        if (callback) {
                            callback(buffers[bufIndex]->data(), size);
                        }
                    } else {
                        // The MediaMuxer docs are unclear, but it appears that we
                        // need to pass either the full set of BufferInfo flags, or
                        // (flags & BUFFER_FLAG_SYNCFRAME).
                        //
                        // If this blocks for too long we could drop frames.  We may
                        // want to queue these up and do them on a different thread.
                        ATRACE_NAME("write sample");
                        // printf("write sample\n");
                        assert(trackIdx != -1);
                        // TODO
                        sp<ABuffer> buffer = new ABuffer(buffers[bufIndex]->data(), buffers[bufIndex]->size());
                        AMediaCodecBufferInfo bufferInfo = {
                                0,
                                static_cast<int32_t>(buffer->size()),
                                ptsUsec,
                                flags
                        };
                        err = AMediaMuxer_writeSampleData(muxer, trackIdx, buffer->data(), &bufferInfo);
                        if (err != NO_ERROR) {
                            fprintf(stderr,
                                    "Failed writing data to muxer (err=%d)\n", err);
                            return err;
                        }
                        if (gOutputFormat == FORMAT_MP4) {
                            timestamps.add(ptsUsec);
                        }
                    }
                    debugNumFrames++;
                }
                err = encoder->releaseOutputBuffer(bufIndex);
                if (err != NO_ERROR) {
                    fprintf(stderr, "Unable to release output buffer (err=%d)\n",
                            err);
                    return err;
                }
                if ((flags & MediaCodec::BUFFER_FLAG_EOS) != 0) {
                    // Not expecting EOS from SurfaceFlinger.  Go with it.
                    ALOGI("Received end-of-stream");
                    gStopRequested = true;
                    *runFlag = false;
                }
                break;
            case -EAGAIN:                       // INFO_TRY_AGAIN_LATER
                ALOGV("Got -EAGAIN, looping");
                break;
            case android::INFO_FORMAT_CHANGED:    // INFO_OUTPUT_FORMAT_CHANGED
            {
                // Format includes CSD, which we must provide to muxer.
                ALOGV("Encoder format changed");
                sp<AMessage> newFormat;
                encoder->getOutputFormat(&newFormat);
                // TODO remove when MediaCodec has been replaced with AMediaCodec
                AMediaFormat *ndkFormat = AMediaFormat_fromMsg(&newFormat);
                if (muxer != NULL) {
                    trackIdx = AMediaMuxer_addTrack(muxer, ndkFormat);
                    if (gOutputFormat == FORMAT_MP4) {
                        AMediaFormat *metaFormat = AMediaFormat_new();
                        AMediaFormat_setString(metaFormat, AMEDIAFORMAT_KEY_MIME, kMimeTypeApplicationOctetstream);
                        metaTrackIdx = AMediaMuxer_addTrack(muxer, metaFormat);
                        AMediaFormat_delete(metaFormat);
                    }
                    ALOGV("Starting muxer");
                    err = AMediaMuxer_start(muxer);
                    if (err != NO_ERROR) {
                        fprintf(stderr, "Unable to start muxer (err=%d)\n", err);
                        return err;
                    }
                }
            }
                break;
            case android::INFO_OUTPUT_BUFFERS_CHANGED:   // INFO_OUTPUT_BUFFERS_CHANGED
                // Not expected for an encoder; handle it anyway.
                ALOGV("Encoder buffers changed");
                err = encoder->getOutputBuffers(&buffers);
                if (err != NO_ERROR) {
                    fprintf(stderr,
                            "Unable to get new output buffers (err=%d)\n", err);
                    return err;
                }
                break;
            case INVALID_OPERATION:
                ALOGW("dequeueOutputBuffer returned INVALID_OPERATION");
                return err;
            default:
                fprintf(stderr,
                        "Got weird result %d from dequeueOutputBuffer\n", err);
                return err;
        }
    }

    ALOGV("Encoder stopping (req=%d)", gStopRequested);
    if (gVerbose) {
        printf("Encoder stopping; recorded %u frames in %" PRId64 " seconds\n",
               debugNumFrames, nanoseconds_to_seconds(
                        systemTime(CLOCK_MONOTONIC) - startWhenNsec));
        fflush(stdout);
    }
    if (metaTrackIdx >= 0 && !timestamps.isEmpty()) {
        err = writeWinscopeMetadata(timestamps, metaTrackIdx, muxer);
        if (err != NO_ERROR) {
            fprintf(stderr, "Failed writing metadata to muxer (err=%d)\n", err);
            return err;
        }
    }
    return NO_ERROR;
}

/*
 * Raw H.264 byte stream output requested.  Send the output to stdout
 * if desired.  If the output is a tty, reconfigure it to avoid the
 * CRLF line termination that we see with "adb shell" commands.
 */
static FILE *prepareRawOutput(const char *fileName) {
    FILE *rawFp = NULL;

    if (strcmp(fileName, "-") == 0) {
        if (gVerbose) {
            fprintf(stderr, "ERROR: verbose output and '-' not compatible");
            return NULL;
        }
        rawFp = stdout;
    } else {
        rawFp = fopen(fileName, "w");
        if (rawFp == NULL) {
            fprintf(stderr, "fopen raw failed: %s\n", strerror(errno));
            return NULL;
        }
    }

    int fd = fileno(rawFp);
    if (isatty(fd)) {
        // best effort -- reconfigure tty for "raw"
        ALOGD("raw video output to tty (fd=%d)", fd);
        struct termios term;
        if (tcgetattr(fd, &term) == 0) {
            cfmakeraw(&term);
            if (tcsetattr(fd, TCSANOW, &term) == 0) {
                ALOGD("tty successfully configured for raw");
            }
        }
    }

    return rawFp;
}

static inline uint32_t floorToEven(uint32_t num) {
    return num & ~1;
}

/*
 * Main "do work" start point.
 *
 * Configures codec, muxer, and virtual display, then starts moving bits
 * around.
 */
static status_t recordScreen(const char *fileName) {
    status_t err;

    // Configure signal handler.
    // 配置信号处理
    err = configureSignals();
    if (err != NO_ERROR) return err;

    // Start Binder thread pool.  MediaCodec needs to be able to receive
    // messages from mediaserver.
    // 启动活页夹线程池,MediaCodec需要接收来自mediaserver的消息。

    sp<ProcessState> self = ProcessState::self();
    self->startThreadPool();

    // Get main display parameters.
    // 获取主屏幕参数
    display = SurfaceComposerClient::getPhysicalDisplayToken(
            gPhysicalDisplayId);
    if (display == nullptr) {
        fprintf(stderr, "ERROR: no display\n");
        return NAME_NOT_FOUND;
    }

    ui::DisplayState displayState;
    err = SurfaceComposerClient::getDisplayState(display, &displayState);
    if (err != NO_ERROR) {
        fprintf(stderr, "ERROR: unable to get display state\n");
        return err;
    }

    DisplayMode displayMode;
    err = SurfaceComposerClient::getActiveDisplayMode(display, &displayMode);
    if (err != NO_ERROR) {
        fprintf(stderr, "ERROR: unable to get display config\n");
        return err;
    }

    const ui::Size &layerStackSpaceRect = displayState.layerStackSpaceRect;
    if (gVerbose) {
        printf("Display is %dx%d @%.2ffps (orientation=%s), layerStack=%u\n",
               layerStackSpaceRect.getWidth(), layerStackSpaceRect.getHeight(),
               displayMode.refreshRate, toCString(displayState.orientation),
               displayState.layerStack);
        fflush(stdout);
    }

    // Encoder can't take odd number as config
    // 编码器无法将奇数作为配置 
    if (gVideoWidth == 0) {
        gVideoWidth = floorToEven(layerStackSpaceRect.getWidth());
    }
    if (gVideoHeight == 0) {
        gVideoHeight = floorToEven(layerStackSpaceRect.getHeight());
    }

    // Configure and start the encoder.
    // 配置并启动编码器。
    // sp<MediaCodec> encoder;


    err = prepareEncoder(displayMode.refreshRate, &encoder, &encoderInputSurface);
    if (err != NO_ERROR && !gSizeSpecified) {
        // fallback is defined for landscape; swap if we're in portrait
        bool needSwap = gVideoWidth < gVideoHeight;
        uint32_t newWidth = needSwap ? kFallbackHeight : kFallbackWidth;
        uint32_t newHeight = needSwap ? kFallbackWidth : kFallbackHeight;
        if (gVideoWidth != newWidth && gVideoHeight != newHeight) {
            ALOGV("Retrying with 720p");
            fprintf(stderr, "WARNING: failed at %dx%d, retrying at %dx%d\n",
                    gVideoWidth, gVideoHeight, newWidth, newHeight);
            gVideoWidth = newWidth;
            gVideoHeight = newHeight;
            err = prepareEncoder(displayMode.refreshRate, &encoder, &encoderInputSurface);
        }
    }
    if (err != NO_ERROR) return err;
    // From here on, we must explicitly release() the encoder before it goes
    // out of scope, or we will get an assertion failure from stagefright
    // later on in a different thread.
    // 创建解码器之后，在使用前必须调用release()方法


    // Draw the "info" page by rendering a frame with GLES and sending
    // it directly to the encoder.
    // TODO: consider displaying this as a regular layer to avoid b/11697754
    // 通过使用GLES渲染帧并发送，绘制“信息”页面
    // 它直接发送到编码器。
    if (gWantInfoScreen) {
        Overlay::drawInfoPage(encoderInputSurface);
    }

    // Configure optional overlay.

    if (gWantFrameTime) {
        // Send virtual display frames to an external texture.
        // 将虚拟帧发送到外部纹理。
        overlay = new Overlay(gMonotonicTime);
        err = overlay->start(encoderInputSurface, &bufferProducer);
        if (err != NO_ERROR) {
            if (encoder != NULL) encoder->release();
            return err;
        }
        if (gVerbose) {
            printf("Bugreport overlay created\n");
            fflush(stdout);
        }
    } else {
        // Use the encoder's input surface as the virtual display surface.
        // 使用编码器的输入面作为虚拟显示面。
        bufferProducer = encoderInputSurface;
    }

    // Configure virtual display.
    // 配置虚拟显示器
    // sp<IBinder> dpy;
    err = prepareVirtualDisplay(displayState, bufferProducer, &virtualDpy);
    if (err != NO_ERROR) {
        if (encoder != NULL) encoder->release();
        return err;
    }

    // AMediaMuxer *muxer = nullptr;
    // FILE* rawFp = NULL;
    switch (gOutputFormat) {
        case FORMAT_MP4:
        case FORMAT_WEBM:
        case FORMAT_3GPP: {
            // Configure muxer.  We have to wait for the CSD blob from the encoder
            // before we can start it.
            err = unlink(fileName);
            if (err != 0 && errno != ENOENT) {
                fprintf(stderr, "ERROR: couldn't remove existing file\n");
                abort();
            }
            int fd = open(fileName, O_CREAT | O_LARGEFILE | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
            if (fd < 0) {
                fprintf(stderr, "ERROR: couldn't open file\n");
                abort();
            }
            if (gOutputFormat == FORMAT_MP4) {
                muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
            } else if (gOutputFormat == FORMAT_WEBM) {
                muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_WEBM);
            } else {
                muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP);
            }
            close(fd);
            if (gRotate) {
                // TODO: does this do anything?
                // 这玩意有用么
                AMediaMuxer_setOrientationHint(muxer, 90);
            }
            break;
        }
        case FORMAT_H264:
        case FORMAT_FRAMES:
        case FORMAT_RAW_FRAMES: {
            rawFp = prepareRawOutput(fileName);
            if (rawFp == NULL) {
                if (encoder != NULL) encoder->release();
                return -1;
            }
            break;
        }
        default:
            fprintf(stderr, "ERROR: unknown format %d\n", gOutputFormat);
            abort();
    }

    orientation = displayState.orientation;
    printf("runEncoder\n");
    // Main encoder loop.
    // 主解码器
    // bool b = true;
    // err = runEncoder(&b,NULL);
    // if (err != NO_ERROR) {
    //     fprintf(stderr, "Encoder failed (err=%d)\n", err);
    //     // fall through to cleanup
    //     // 彻底清理
    // }

    // if (gVerbose) {
    //     printf("Stopping encoder and muxer\n");
    //     fflush(stdout);
    // }


    return err;
}

/*
 * Accepts a string with a bare number ("4000000") or with a single-character
 * unit ("4m").
 *
 * Returns an error if parsing fails.
 */
static status_t parseValueWithUnit(const char *str, uint32_t *pValue) {
    long value;
    char *endptr;

    value = strtol(str, &endptr, 10);
    if (*endptr == '\0') {
        // bare number
        *pValue = value;
        return NO_ERROR;
    } else if (toupper(*endptr) == 'M' && *(endptr + 1) == '\0') {
        *pValue = value * 1000000;  // check for overflow?
        return NO_ERROR;
    } else {
        fprintf(stderr, "Unrecognized value: %s\n", str);
        return UNKNOWN_ERROR;
    }
}

void stopScreenrecord() {
    // Shut everything down, starting with the producer side.
    // 结束
    encoderInputSurface = NULL;
    SurfaceComposerClient::destroyDisplay(virtualDpy);
    if (overlay != NULL) overlay->stop();
    if (encoder != NULL) encoder->stop();
    if (muxer != NULL) {
        // If we don't stop muxer explicitly, i.e. let the destructor run,
        // it may hang (b/11050628).
        AMediaMuxer_stop(muxer);
    } else if (rawFp != stdout) {
        fclose(rawFp);
    }
    if (encoder != NULL) encoder->release();
}

/*
 * Parses args and kicks things off.
 */
int startScreenrecord() {
    std::cout << "screenrecord" << std::endl;
    std::optional<PhysicalDisplayId> displayId = SurfaceComposerClient::getInternalDisplayId();
    if (!displayId) {
        fprintf(stderr, "Failed to get token for internal display\n");
        return 1;
    }

    gPhysicalDisplayId = *displayId;

    gVerbose = false;

    // 视频大小
    gVideoWidth = 0;
    gVideoHeight = 0;
    gSizeSpecified = false;

    // 码率
    gBitRate = 20000000;
    const char *lBitRate = "20M";
    if (parseValueWithUnit(lBitRate, &gBitRate) != NO_ERROR) {
        return 2;
    }

    gTimeLimitSec = kMaxTimeLimitSec;

    // bug上报
    gWantInfoScreen = false;
    gWantFrameTime = false;
    // 视频显示基本信息
    gWantInfoScreen = false;
    // 旋转
    gRotate = false;

    gMonotonicTime = false;
    // gCodecName

    gOutputFormat = FORMAT_H264;

    if (gBitRate < kMinBitRate || gBitRate > kMaxBitRate) {
        fprintf(stderr,
                "Bit rate %dbps outside acceptable range [%d,%d]\n",
                gBitRate, kMinBitRate, kMaxBitRate);
        return 2;
    }

    const char *fileName = "/sdcard/k.mp4";
    if (gOutputFormat == FORMAT_MP4) {
        int fd = open(fileName, O_CREAT | O_RDWR, 0644);
        if (fd < 0) {
            fprintf(stderr, "Unable to open '%s': %s\n", fileName, strerror(errno));
            return 1;
        }
        close(fd);
    }

    std::cout << "recordScreen" << std::endl;
    status_t err = recordScreen(fileName);
    ALOGD(err == NO_ERROR ? "success" : "failed");
    return (int) err;
}
