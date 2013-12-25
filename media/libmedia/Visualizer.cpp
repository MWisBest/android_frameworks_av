/*
**
** Copyright 2010, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/


//#define LOG_NDEBUG 0
#define LOG_TAG "Visualizer"
#include <utils/Log.h>

#include <stdint.h>
#include <sys/types.h>
#include <limits.h>

#include <cutils/bitops.h>

#include <media/Visualizer.h>
#include <audio_utils/fixedfft.h>
#include <utils/Thread.h>

namespace android {

// ---------------------------------------------------------------------------

Visualizer::Visualizer (int32_t priority,
         effect_callback_t cbf,
         void* user,
         int sessionId)
    :   AudioEffect(SL_IID_VISUALIZATION, NULL, priority, cbf, user, sessionId),
        mCaptureRate(CAPTURE_RATE_DEF),
        mCaptureSize(CAPTURE_SIZE_DEF),
        mSampleRate(44100000),
        mScalingMode(VISUALIZER_SCALING_MODE_NORMALIZED),
        mMeasurementMode(MEASUREMENT_MODE_NONE),
        mCaptureCallBack(NULL),
        mCaptureCbkUser(NULL)
{
    initCaptureSize();
}

Visualizer::~Visualizer()
{
}

status_t Visualizer::setEnabled(bool enabled)
{
    Mutex::Autolock _l(mCaptureLock);

    sp<CaptureThread> t = mCaptureThread;
    if (t != 0) {
        if (enabled) {
            if (t->exitPending()) {
                if (t->requestExitAndWait() == WOULD_BLOCK) {
                    ALOGE("Visualizer::enable() called from thread");
                    return INVALID_OPERATION;
                }
            }
        }
        t->mLock.lock();
    }

    status_t status = AudioEffect::setEnabled(enabled);

    if (status == NO_ERROR) {
        if (t != 0) {
            if (enabled) {
                t->run("Visualizer");
            } else {
                t->requestExit();
            }
        }
    }

    if (t != 0) {
        t->mLock.unlock();
    }

    return status;
}

status_t Visualizer::setCaptureCallBack(capture_cbk_t cbk, void* user, uint32_t flags,
        uint32_t rate)
{
    if (rate > CAPTURE_RATE_MAX) {
        return BAD_VALUE;
    }
    Mutex::Autolock _l(mCaptureLock);

    if (mEnabled) {
        return INVALID_OPERATION;
    }

    sp<CaptureThread> t = mCaptureThread;
    if (t != 0) {
        t->mLock.lock();
    }
    mCaptureThread.clear();
    mCaptureCallBack = cbk;
    mCaptureCbkUser = user;
    mCaptureFlags = flags;
    mCaptureRate = rate;

    if (t != 0) {
        t->mLock.unlock();
    }

    if (cbk != NULL) {
        mCaptureThread = new CaptureThread(*this, rate, ((flags & CAPTURE_CALL_JAVA) != 0));
    }
    ALOGV("setCaptureCallBack() rate: %d thread %p flags 0x%08x",
            rate, mCaptureThread.get(), mCaptureFlags);
    return NO_ERROR;
}

status_t Visualizer::setCaptureSize(uint32_t size)
{
    if (size > VISUALIZER_CAPTURE_SIZE_MAX ||
        size < VISUALIZER_CAPTURE_SIZE_MIN ||
        popcount(size) != 1) {
        return BAD_VALUE;
    }

    Mutex::Autolock _l(mCaptureLock);
    if (mEnabled) {
        return INVALID_OPERATION;
    }

    union {
        uint32_t buf32[sizeof(effect_param_t) / sizeof(uint32_t) + 2];
        effect_param_t bufp;
    };
    effect_param_t *p = &bufp;

    p->psize = sizeof(uint32_t);
    p->vsize = sizeof(uint32_t);
    int32_t const vpcs = VISUALIZER_PARAM_CAPTURE_SIZE;
    memcpy(&p->data, &vpcs, sizeof(vpcs));
    memcpy(&p->data+sizeof(int32_t), &size, sizeof(size));
    status_t status = setParameter(p);

    ALOGV("setCaptureSize size %d  status %d p->status %d", size, status, p->status);

    if (status == NO_ERROR) {
        status = p->status;
        if (status == NO_ERROR) {
            mCaptureSize = size;
        }
    }

    return status;
}

status_t Visualizer::setScalingMode(uint32_t mode) {
    if ((mode != VISUALIZER_SCALING_MODE_NORMALIZED)
            && (mode != VISUALIZER_SCALING_MODE_AS_PLAYED)) {
        return BAD_VALUE;
    }

    Mutex::Autolock _l(mCaptureLock);

    uint32_t buf32[sizeof(effect_param_t) / sizeof(uint32_t) + 2];
    effect_param_t *p = (effect_param_t *)buf32;

    p->psize = sizeof(uint32_t);
    p->vsize = sizeof(uint32_t);
    int32_t const vpsm = VISUALIZER_PARAM_SCALING_MODE;
    memcpy(&p->data, &vpsm, sizeof(vpsm));
    memcpy(&p->data+sizeof(int32_t), &mode, sizeof(mode));
    status_t status = setParameter(p);

    ALOGV("setScalingMode mode %d  status %d p->status %d", mode, status, p->status);

    if (status == NO_ERROR) {
        status = p->status;
        if (status == NO_ERROR) {
            mScalingMode = mode;
        }
    }

    return status;
}

status_t Visualizer::setMeasurementMode(uint32_t mode) {
    if ((mode != MEASUREMENT_MODE_NONE)
            //Note: needs to be handled as a mask when more measurement modes are added
            && ((mode & MEASUREMENT_MODE_PEAK_RMS) != mode)) {
        return BAD_VALUE;
    }

    Mutex::Autolock _l(mCaptureLock);

    uint32_t buf32[sizeof(effect_param_t) / sizeof(uint32_t) + 2];
    effect_param_t *p = (effect_param_t *)buf32;

    p->psize = sizeof(uint32_t);
    p->vsize = sizeof(uint32_t);
    int32_t const vpmm = VISUALIZER_PARAM_MEASUREMENT_MODE;
    memcpy(&p->data, &vpmm, sizeof(vpmm));
    memcpy(&p->data+sizeof(vpmm), &mode, sizeof(mode));
    //*(int32_t *)p->data = VISUALIZER_PARAM_MEASUREMENT_MODE;
    //*((int32_t *)p->data + 1)= mode;
    status_t status = setParameter(p);

    ALOGV("setMeasurementMode mode %d  status %d p->status %d", mode, status, p->status);

    if (status == NO_ERROR) {
        status = p->status;
        if (status == NO_ERROR) {
            mMeasurementMode = mode;
        }
    }
    return status;
}

status_t Visualizer::getIntMeasurements(uint32_t type, uint32_t number, int32_t *measurements) {
    if (mMeasurementMode == MEASUREMENT_MODE_NONE) {
        ALOGE("Cannot retrieve int measurements, no measurement mode set");
        return INVALID_OPERATION;
    }
    if (!(mMeasurementMode & type)) {
        // measurement type has not been set on this Visualizer
        ALOGE("Cannot retrieve int measurements, requested measurement mode 0x%x not set(0x%x)",
                type, mMeasurementMode);
        return INVALID_OPERATION;
    }
    // only peak+RMS measurement supported
    if ((type != MEASUREMENT_MODE_PEAK_RMS)
            // for peak+RMS measurement, the results are 2 int32_t values
            || (number != 2)) {
        ALOGE("Cannot retrieve int measurements, MEASUREMENT_MODE_PEAK_RMS returns 2 ints, not %d",
                        number);
        return BAD_VALUE;
    }

    status_t status = NO_ERROR;
    if (mEnabled) {
        uint32_t replySize = number * sizeof(int32_t);
        status = command(VISUALIZER_CMD_MEASURE,
                sizeof(uint32_t)  /*cmdSize*/,
                &type /*cmdData*/,
                &replySize, measurements);
        ALOGV("getMeasurements() command returned %d", status);
        if ((status == NO_ERROR) && (replySize == 0)) {
            status = NOT_ENOUGH_DATA;
        }
    } else {
        ALOGV("getMeasurements() disabled");
        return INVALID_OPERATION;
    }
    return status;
}

status_t Visualizer::getWaveForm(uint8_t *waveform)
{
    if (waveform == NULL) {
        return BAD_VALUE;
    }
    if (mCaptureSize == 0) {
        return NO_INIT;
    }

    status_t status = NO_ERROR;
    if (mEnabled) {
        uint32_t replySize = mCaptureSize;
        status = command(VISUALIZER_CMD_CAPTURE, 0, NULL, &replySize, waveform);
        ALOGV("getWaveForm() command returned %d", status);
        if ((status == NO_ERROR) && (replySize == 0)) {
            status = NOT_ENOUGH_DATA;
        }
    } else {
        ALOGV("getWaveForm() disabled");
        memset(waveform, 0x80, mCaptureSize);
    }
    return status;
}

status_t Visualizer::getFft(uint8_t *fft)
{
    if (fft == NULL) {
        return BAD_VALUE;
    }
    if (mCaptureSize == 0) {
        return NO_INIT;
    }

    status_t status = NO_ERROR;
    if (mEnabled) {
        uint8_t buf[mCaptureSize];
        status = getWaveForm(buf);
        if (status == NO_ERROR) {
            status = doFft(fft, buf);
        }
    } else {
        memset(fft, 0, mCaptureSize);
    }
    return status;
}

status_t Visualizer::doFft(uint8_t *fft, uint8_t *waveform)
{
    int32_t workspace[mCaptureSize >> 1];
    int32_t nonzero = 0;

    for (uint32_t i = 0; i < mCaptureSize; i += 2) {
        workspace[i >> 1] =
                ((waveform[i] ^ 0x80) << 24) | ((waveform[i + 1] ^ 0x80) << 8);
        nonzero |= workspace[i >> 1];
    }

    if (nonzero) {
        fixed_fft_real(mCaptureSize >> 1, workspace);
    }

    for (uint32_t i = 0; i < mCaptureSize; i += 2) {
        short tmp = workspace[i >> 1] >> 21;
        while (tmp > 127 || tmp < -128) tmp >>= 1;
        fft[i] = tmp;
        tmp = workspace[i >> 1];
        tmp >>= 5;
        while (tmp > 127 || tmp < -128) tmp >>= 1;
        fft[i + 1] = tmp;
    }

    return NO_ERROR;
}

void Visualizer::periodicCapture()
{
    Mutex::Autolock _l(mCaptureLock);
    ALOGV("periodicCapture() %p mCaptureCallBack %p mCaptureFlags 0x%08x",
            this, mCaptureCallBack, mCaptureFlags);
    if (mCaptureCallBack != NULL &&
        (mCaptureFlags & (CAPTURE_WAVEFORM|CAPTURE_FFT)) &&
        mCaptureSize != 0) {
        uint8_t waveform[mCaptureSize];
        status_t status = getWaveForm(waveform);
        if (status != NO_ERROR) {
            return;
        }
        uint8_t fft[mCaptureSize];
        if (mCaptureFlags & CAPTURE_FFT) {
            status = doFft(fft, waveform);
        }
        if (status != NO_ERROR) {
            return;
        }
        uint8_t *wavePtr = NULL;
        uint8_t *fftPtr = NULL;
        uint32_t waveSize = 0;
        uint32_t fftSize = 0;
        if (mCaptureFlags & CAPTURE_WAVEFORM) {
            wavePtr = waveform;
            waveSize = mCaptureSize;
        }
        if (mCaptureFlags & CAPTURE_FFT) {
            fftPtr = fft;
            fftSize = mCaptureSize;
        }
        mCaptureCallBack(mCaptureCbkUser, waveSize, wavePtr, fftSize, fftPtr, mSampleRate);
    }
}

uint32_t Visualizer::initCaptureSize()
{
    union {
        uint32_t buf32[sizeof(effect_param_t) / sizeof(uint32_t) + 2];
        effect_param_t p;
    };

    p.psize = sizeof(uint32_t);
    p.vsize = sizeof(uint32_t);
    int32_t const vpcs = VISUALIZER_PARAM_CAPTURE_SIZE;
    memcpy(&p.data, &vpcs, sizeof(vpcs));
    status_t status = getParameter(&p);

    if (status == NO_ERROR) {
        status = p.status;
    }

    uint32_t size = 0;
    if (status == NO_ERROR) {
        memcpy(&size, &p.data+sizeof(int32_t), sizeof(int32_t));
    }
    mCaptureSize = size;

    ALOGV("initCaptureSize size %d status %d", mCaptureSize, status);

    return size;
}

void Visualizer::controlStatusChanged(bool controlGranted) {
    if (controlGranted) {
        // this Visualizer instance regained control of the effect, reset the scaling mode
        //   and capture size as has been cached through it.
        ALOGV("controlStatusChanged(true) causes effect parameter reset:");
        ALOGV("    scaling mode reset to %d", mScalingMode);
        setScalingMode(mScalingMode);
        ALOGV("    capture size reset to %d", mCaptureSize);
        setCaptureSize(mCaptureSize);
    }
    AudioEffect::controlStatusChanged(controlGranted);
}

//-------------------------------------------------------------------------

Visualizer::CaptureThread::CaptureThread(Visualizer& receiver, uint32_t captureRate,
        bool bCanCallJava)
    : Thread(bCanCallJava), mReceiver(receiver)
{
    mSleepTimeUs = 1000000000 / captureRate;
    ALOGV("CaptureThread cstor %p captureRate %d mSleepTimeUs %d", this, captureRate, mSleepTimeUs);
}

bool Visualizer::CaptureThread::threadLoop()
{
    ALOGV("CaptureThread %p enter", this);
    while (!exitPending())
    {
        usleep(mSleepTimeUs);
        mReceiver.periodicCapture();
    }
    ALOGV("CaptureThread %p exiting", this);
    return false;
}

}; // namespace android
