/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (c) 2012, Code Aurora Forum. All rights reserved.
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

//#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#define LOG_TAG "MPQAudioPlayer"
#include <utils/Log.h>
#include <utils/threads.h>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/poll.h>
#include <sys/eventfd.h>

#include <binder/IPCThreadState.h>
#include <media/AudioTrack.h>
#include <system/audio.h>
#include "include/ESDS.h"

extern "C" {
    #include <asound.h>
    #include "alsa_audio.h"
}
#include <media/stagefright/MPQAudioPlayer.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaErrors.h>
#include <hardware_legacy/power.h>

#include <linux/unistd.h>
#include <include/linux/msm_audio.h>

#include "include/AwesomePlayer.h"

//Required for A2DP
#define PMEM_CAPTURE_BUFFER_SIZE 4096

//Session Id to diff playback
#define MPQ_AUDIO_SESSION_ID 3
#define TUNNEL_SESSION_ID 2

#define RENDER_LATENCY 24000
#define AAC_AC3_BUFFER_SIZE 32768

namespace android {

int MPQAudioPlayer::mMPQAudioObjectsAlive = 0;

int MPQAudioPlayer::getMPQAudioObjectsAlive(/*bool hasVideo*/) {
    LOGV("getMPQAudioObjectsAlive = %d",mMPQAudioObjectsAlive);
    return mMPQAudioObjectsAlive;
}

MPQAudioPlayer::MPQAudioPlayer(
                    const sp<MediaPlayerBase::AudioSink> &audioSink, bool &initCheck,
                    AwesomePlayer *observer, bool hasVideo)
:AudioPlayer(audioSink,observer),
mInputBuffer(NULL),
mSampleRate(0),
mNumChannels(0),
mFrameSize(0),
mNumFramesPlayed(0),
mIsAACFormatAdif(0),
mLatencyUs(0),
mStarted(false),
mAsyncReset(false),
mPositionTimeMediaUs(-1),
mPositionTimeRealUs(-1),
mSeeking(false),
mInternalSeeking(false),
mPostedEOS(false),
mReachedExtractorEOS(false),
mFinalStatus(OK),
mIsPaused(false),
mPlayPendingSamples(false),
mSourcePaused(false),
mAudioSinkOpen(false),
mIsAudioRouted(false),
mIsA2DPEnabled(false),
mIsFirstBuffer(false),
mFirstBufferResult(OK),
mFirstBuffer(NULL),
mAudioSink(audioSink),
mObserver(observer) {
    LOGV("MPQAudioPlayer::MPQAudioPlayer()");

    mAudioFlinger = NULL;
    mAudioFlingerClient = NULL;
    if(mAudioSink->getSessionId())
        mMPQAudioObjectsAlive++;

    mQueue.start();
    mQueueStarted      = true;
    mPauseEvent        = new MPQAudioEvent(this, &MPQAudioPlayer::onPauseTimeOut);
    mPauseEventPending = false;
    mSourcePaused = false;

    getAudioFlinger();
    LOGD("Registering client with AudioFlinger");
    mAudioFlinger->registerClient(mAudioFlingerClient);

    mMimeType.setTo("");
    mA2dpDisconnectPause = false;

    mTimePaused  = 0;
    mDurationUs = 0;
    mSeekTimeUs = 0;
    mTimeout = -1;
    mPostEOSDelayUs = 0;

    mLocalBuf = NULL;
    mInputBufferSize =  0;

    mPCMStream = NULL;
    mFirstEncodedBuffer = false;
    mHasVideo = hasVideo;
    initCheck = true;
}

MPQAudioPlayer::~MPQAudioPlayer() {
    LOGD("MPQAudioPlayer::~MPQAudioPlayer()");
    if (mQueueStarted) {
        mQueue.stop();
    }

    reset();
    mAudioFlinger->deregisterClient(mAudioFlingerClient);
    if(mMPQAudioObjectsAlive > 0)
        mMPQAudioObjectsAlive--;
}

void MPQAudioPlayer::getAudioFlinger() {
    Mutex::Autolock _l(mAudioFlingerLock);

    if ( mAudioFlinger.get() == 0 ) {
        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder;
        do {
            binder = sm->getService(String16("media.audio_flinger"));
            if ( binder != 0 )
                break;
            LOGW("AudioFlinger not published, waiting...");
            usleep(500000); // 0.5 s
        } while ( true );
        if ( mAudioFlingerClient == NULL ) {
            mAudioFlingerClient = new AudioFlingerMPQAudioDecodeClient(this);
        }

        binder->linkToDeath(mAudioFlingerClient);
        mAudioFlinger = interface_cast<IAudioFlinger>(binder);
    }
    LOGE_IF(mAudioFlinger==0, "no AudioFlinger!?");
}

MPQAudioPlayer::AudioFlingerMPQAudioDecodeClient::AudioFlingerMPQAudioDecodeClient(void *obj)
{
    LOGD("MPQAudioPlayer::AudioFlingerMPQAudioDecodeClient - Constructor");
    pBaseClass = (MPQAudioPlayer*)obj;
}

void MPQAudioPlayer::AudioFlingerMPQAudioDecodeClient::binderDied(const wp<IBinder>& who) {
    Mutex::Autolock _l(pBaseClass->mAudioFlingerLock);

    pBaseClass->mAudioFlinger.clear();
    LOGW("AudioFlinger server died!");
}

void MPQAudioPlayer::AudioFlingerMPQAudioDecodeClient::ioConfigChanged(int event, int ioHandle, void *param2) {
    LOGV("ioConfigChanged() event %d", event);

    if (event != AudioSystem::A2DP_OUTPUT_STATE) {
        return;
    }

    switch ( event ) {
    case AudioSystem::A2DP_OUTPUT_STATE:
        {
            if ( -1 == ioHandle ) {
                if ( pBaseClass->mIsA2DPEnabled ) {
                    pBaseClass->mIsA2DPEnabled = false;
                    if (pBaseClass->mStarted) {
                        pBaseClass->handleA2DPSwitch();
                    }
                    LOGV("ioConfigChanged:: A2DP Disabled");
                }
            } else {
                if ( !pBaseClass->mIsA2DPEnabled ) {
                    pBaseClass->mIsA2DPEnabled = true;
                    if (pBaseClass->mStarted) {
                        pBaseClass->handleA2DPSwitch();
                    }
                    LOGV("ioConfigChanged:: A2DP Enabled");
                }
            }
        }
        break;
    default:
        break;
    }
    LOGV("ioConfigChanged Out");
}

void MPQAudioPlayer::handleA2DPSwitch() {
    Mutex::Autolock autoLock(mLock);

    LOGV("handleA2dpSwitch()");
    if(mIsA2DPEnabled) {

        // 1.	If not paused - pause the driver
        //TODO: CHECK if audio routed has to be checked
        if (!mIsPaused) {
            int err;
            err = mPCMStream->pause(mPCMStream);
            if(err != OK) {
                LOGE("Pause returned error =%d",err);
                return;
            }
        }
        //2.	If not paused - Query the time. - Not reqd , time need not be stored

        //TODO: Is Internal Seeking required ? I believe not
	//3 Signal Notification thread
        mA2dpNotificationCv.signal();

    } else {
        //TODO :
        //If paused signal notification thread
        if (mIsPaused)
            mA2dpNotificationCv.signal();
        //else flag a2dp disconnect- signal from pause()
        else
            mA2dpDisconnectPause = true;
    }
}

void MPQAudioPlayer::setSource(const sp<MediaSource> &source) {
    CHECK_EQ(mSource, NULL);
    LOGD("Setting source from MPQ Audio Player");
    mSource = source;
}

status_t MPQAudioPlayer::start(bool sourceAlreadyStarted) {
    Mutex::Autolock autoLock(mLock);
    CHECK(!mStarted);
    CHECK(mSource != NULL);

    LOGV("start: sourceAlreadyStarted %d", sourceAlreadyStarted);
    //Check if the source is started, start it
    status_t err = OK;
    if (!sourceAlreadyStarted) {
        err = mSource->start();
        if (err != OK) {
            return err;
        }
    }

    err = updateMetaDataInformation();
    if(err != OK) {
        LOGE("updateMetaDataInformation = %d", err);
        return err;
    }

    err = getDecoderAndFormat();
    if(err != OK) {
        LOGV("getDecoderAndFormat return err = %d",err);
        return err;
    }

    //Create event, extractor and a2dp thread and initialize all the
    //mutexes and coditional variables
    LOGV("Creat threads ++");
    createThreads();
    LOGV("All Threads Created.");

    int sessionId = 1;
    if(((!strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_MPEG)))) {
        LOGD("TUNNEL_SESSION_ID");
        sessionId = TUNNEL_SESSION_ID;
    }
    else {
        LOGD("MPQ_AUDIO_SESSION_ID");
        sessionId = MPQ_AUDIO_SESSION_ID;
    }


    if(mIsA2DPEnabled)
        mA2DpState = A2DP_ENABLED;
    else
        mA2DpState = A2DP_DISABLED;

    if(mA2DpState == A2DP_DISABLED) {
        LOGD("Opening a routing session for audio playback:\
                mSampleRate = %d mNumChannels =  %d",\
                mSampleRate, mNumChannels);
        err = mAudioSink->openSession(
                mAudioFormat, sessionId, mSampleRate, mNumChannels);
        if (err != OK) {
            //TODO: Release first buffer if it is being handled
            if (!sourceAlreadyStarted) {
                mSource->stop();
            }
            LOGE("Opening a routing session failed");
            return err;
        }
        mIsAudioRouted = true;
    }
    else if (mA2DpState == A2DP_ENABLED){
        LOGV("Before Audio Sink Open");
        err = mAudioSink->open(
                mSampleRate, mNumChannels,AUDIO_FORMAT_PCM_16_BIT,
                DEFAULT_AUDIOSINK_BUFFERCOUNT);
        if(err != OK) {
            LOGE("Audio Sink -open failed = %d", err);
            return err;
        }
        mAudioSink->start();
        LOGV("After Audio Sink Open");
        mAudioSinkOpen = true;

        err = openAndConfigureCaptureDevice();
        if(err) {
           LOGE("openAndConfigureCaptureDevice return error  = %d",err);
           return err;
        }
    }

    err = configurePCM();
    if (err) {
        LOGE("Error Configuring PCM");
        return err;
    }

    LOGD(" MPQ Audio Driver Started");
    mStarted = true;

    LOGD("Waking up extractor thread");
    mExtractorCv.signal();

    return OK;
}


status_t MPQAudioPlayer::seekTo(int64_t time_us) {
    Mutex::Autolock autoLock1(mSeekLock);
    Mutex::Autolock autoLock(mLock);

    status_t err = OK;
    LOGD("seekTo: time_us %lld", time_us);
    if (mReachedExtractorEOS) {
        mReachedExtractorEOS = false;
        mPostedEOS = false;
        mTimeout = -1;
    }
    mSeeking = true;
    mSeekTimeUs = time_us;

    switch(mDecoderType) {

        case ESoftwareDecoder:
            err = seekSoftwareDecoderPlayback();
        break;


        case EMS11Decoder:
            err = seekMS11DecoderPlayback();
        break;

        case EHardwareDecoder:
            err = seekHardwareDecoderPlayback();
        break;
        default:
           LOGE("Invalid Decoder Type - postEOS ");
        //TODO: Post EOS
        return BAD_VALUE;
    }

    if(err)
        LOGE("seek returned error = %d",err);
    return err;
}

status_t MPQAudioPlayer::seekSoftwareDecoderPlayback() {

    status_t err = OK;
    mPositionTimeRealUs = mPositionTimeMediaUs = -1;
    mNumFramesPlayed = 0;
    //Flush not called.
    //Currently play the buffer
    if(mA2DpState == A2DP_DISABLED) {
        mExtractorCv.signal();
    }
    else if(mA2DpState == A2DP_ENABLED) {

    }
    return OK;
}


status_t MPQAudioPlayer::seekMS11DecoderPlayback() {

    status_t err = OK;
    //Just give the buffer from new location
    mPositionTimeRealUs = mPositionTimeMediaUs = -1;
    mNumFramesPlayed = 0;
    mTimePaused = 0;
    if(mA2DpState == A2DP_DISABLED) {
        if(!mIsAACFormatAdif) {
            err = mPCMStream->flush(mPCMStream);
            if(err != OK) {
                LOGE("flush returned error =%d",err);
                return err;
            }
            mExtractorCv.signal();
        }
    }
    else if(mA2DpState == A2DP_ENABLED) {
    }
    return OK;
}

status_t MPQAudioPlayer::seekHardwareDecoderPlayback() {

    status_t err = OK;
    mInternalSeeking = false;
    mPositionTimeRealUs = mPositionTimeMediaUs = -1;
    mTimePaused = 0;
    LOGV("In seekHardwareDecoderPlayback mSeekTimeUs %lld",mSeekTimeUs);

    if (mA2DpState == A2DP_DISABLED) {
        if (mStarted) {
            err = mPCMStream->flush(mPCMStream);
            if(err != OK) {
                LOGE("flush returned error =%d",err);
                return err;
            }
            if (!mIsPaused)
                mExtractorCv.signal();
        }
    } else if(mA2DpState == A2DP_ENABLED){

    }
    return err;
}

void MPQAudioPlayer::pause(bool playPendingSamples) {

    Mutex::Autolock autolock(mLock);
    status_t err = OK;
    CHECK(mStarted);

    LOGD("Pause: playPendingSamples %d", playPendingSamples);
    mPlayPendingSamples = playPendingSamples;
    mIsPaused = true;

    switch(mDecoderType) {

        case ESoftwareDecoder:
            err = pauseSoftwareDecoderPlayback();
        break;

        case EMS11Decoder:
            err = pauseMS11DecoderPlayback();
        break;

        case EHardwareDecoder:
            err = pauseHardwareDecoderPlayback();
        break;

        default:
            LOGE("Invalid Decoder Type - postEOS ");
            err =  BAD_VALUE;
        break;
    }

    if(err != OK) {
        LOGE("pause returnded err = %d",err);
        mFinalStatus = BAD_VALUE;
        if(mObserver) {
            mObserver->postAudioEOS();
        }
    }
}

status_t MPQAudioPlayer::pauseSoftwareDecoderPlayback() {

    status_t err = OK;
    if(!mAudioSink->getSessionId())
        return err;

    CHECK(mPCMStream);
    if (mPlayPendingSamples) {
        //should call stop ideally
        //No pausing the driver. Allow the playback
        //err = mPCMStream->pause(mPCMStream);
        mNumFramesPlayed = 0;
    }
    else {
        err = mPCMStream->pause(mPCMStream);
        if(err != OK) {
            LOGE("Pause returned error =%d",err);
            return err;
        }
    }

    CHECK(mSource != NULL);
    if ((mSource->pause()) == OK) {
        mSourcePaused = true;
    }
    return err;
}

status_t MPQAudioPlayer::pauseMS11DecoderPlayback() {

    status_t err = OK;
    CHECK(mPCMStream);

    if (mPlayPendingSamples) {
        //Should be stop ideally
        //No pausing the driver. Allow the playback
        //err = mPCMStream->pause(mPCMStream);
        mNumFramesPlayed = 0;
    }
    else {
        err = mPCMStream->pause(mPCMStream);
        if(err != OK) {
            LOGE("Pause returned error =%d",err);
            return err;
        }

    }
    mTimePaused = mSeekTimeUs + mPositionTimeMediaUs;
    return err;
}


status_t MPQAudioPlayer::pauseHardwareDecoderPlayback() {

    status_t err = OK;
    mTimeout  = -1;

    if (mPlayPendingSamples) {
        err = mPCMStream->pause(mPCMStream);
        if(err != OK) {
            LOGE("Pause returned error =%d",err);
            return err;
        }
        //TODO : Add time out if needed.Check tunnel Player
        if(mA2DpState == A2DP_DISABLED) {
                acquire_wake_lock(PARTIAL_WAKE_LOCK, "MPQ_AUDIO_LOCK");
            if (mAudioSink.get() != NULL)
                mAudioSink->pauseSession();
        }
        else if(mA2DpState == A2DP_ENABLED){

        }

        mTimePaused = mSeekTimeUs + getAudioTimeStampUs();
    }
    else {
        if (mA2dpDisconnectPause) {
            mA2dpDisconnectPause = false;
            mA2dpNotificationCv.signal();
        } else {
            LOGV("MPQAudioPlayer::Pause - Pause driver");
            err = mPCMStream->pause(mPCMStream);
            if(err != OK) {
                LOGE("Pause returned error =%d",err);
                return err;
            }
            if(mA2DpState == A2DP_DISABLED) {
                    acquire_wake_lock(PARTIAL_WAKE_LOCK, "MPQ_AUDIO_LOCK");

                if (mAudioSink.get() != NULL) {
                    mAudioSink->pauseSession();
                }
            }
            else if(mA2DpState == A2DP_ENABLED) {
            }

            mTimePaused = mSeekTimeUs + getAudioTimeStampUs();
        }
    }
    return err;
}

void MPQAudioPlayer::resume() {

    Mutex::Autolock autoLock(mLock);
    status_t err = OK;
    CHECK(mStarted);
    CHECK(mSource != NULL);

    LOGD("Resume: mIsPaused %d",mIsPaused);

    switch(mDecoderType) {

        case ESoftwareDecoder:
            err = resumeSoftwareDecoderPlayback();
        break;

        case EMS11Decoder:
            err = resumeMS11DecoderPlayback();
        break;

        case EHardwareDecoder:
            err = resumeHardwareDecoderPlayback();
        break;

        default:
            LOGE("Invalid Decoder Type - postEOS ");
            err =  BAD_VALUE;
        break;
    }

    if(err != OK) {
        LOGE("resume returnded err = %d",err);
        mFinalStatus = BAD_VALUE;
        if(mObserver) {
            mObserver->postAudioEOS();
        }
        return;
    }

    mIsPaused = false;
    mExtractorCv.signal();

}

status_t MPQAudioPlayer::resumeSoftwareDecoderPlayback() {

    status_t err = OK;

    if(!mAudioSink->getSessionId())
        return err;

    if(mIsPaused) {
        if (!mIsAudioRouted) {
            LOGV("Opening a session for MPQ Audio playback - Software Decoder");
            status_t err = mAudioSink->openSession(mAudioFormat,
                    MPQ_AUDIO_SESSION_ID, mSampleRate, mNumChannels);
            if(err != OK) {
                LOGE("openSession - resume = %d",err);
                return err;
            }
            mIsAudioRouted = true;
            mPCMStream = mAudioFlinger->getOutputSession();
        }
        CHECK(mPCMStream);
        if(mPCMStream) {
            err = mPCMStream->resume(mPCMStream);
            if(err != OK && !mPlayPendingSamples) {
                LOGE("resume PCM stream err = %d", err);
                return err;
            }
        }
        else {
            LOGE("Invalide PCM stream");
            err = BAD_VALUE;
        }
        if(mSourcePaused)
            mSource->start();
    }

    return err;
}

status_t MPQAudioPlayer::resumeMS11DecoderPlayback() {

    status_t err = OK;

    if(mIsPaused) {
        if (!mIsAudioRouted) {
            LOGV("Opening a session for MPQ Audio playback - Software Decoder");
            status_t err = mAudioSink->openSession(mAudioFormat,
                    MPQ_AUDIO_SESSION_ID, mSampleRate, mNumChannels);
            if(err != OK) {
                LOGE("openSession - resume = %d",err);
                return err;
            }
            mIsAudioRouted = true;
            mPCMStream = mAudioFlinger->getOutputSession();
        }

        CHECK(mPCMStream);
        if(mPCMStream) {
            err = mPCMStream->resume(mPCMStream);
            if(err != OK && !mPlayPendingSamples) {
                LOGE("resume PCM stream err = %d", err);
                return err;
            }
        }
        else {
            LOGE("Invalide PCM stream");
            err = BAD_VALUE;
        }
    }
    return err;

}
status_t MPQAudioPlayer::resumeHardwareDecoderPlayback() {
    LOGD("Resume: mIsPaused %d",mIsPaused);

    status_t err = OK;

    if (mIsPaused) {
        CHECK(mStarted);
          if (mA2DpState == A2DP_DISABLED) {
            LOGV("MPQ Audio Player::resume - Resuming Driver");
                release_wake_lock("MPQ_AUDIO_LOCK");


            if (!mIsAudioRouted) {
                LOGV("Opening a session for MPQ Audio playback");
                status_t err = mAudioSink->openSession(mAudioFormat, TUNNEL_SESSION_ID,
                         mSampleRate, mNumChannels);
                if(err != OK) {
                    LOGE("openSession - resume = %d",err);
                    return err;
                }
                mIsAudioRouted = true;
            }
            LOGV("Attempting Sync resume\n");
            // check for internal seeking
            err = mPCMStream->resume(mPCMStream);
            if (err != OK) {
                LOGE("AUDIO Resume failed = %d", err);
                return err;
            }
            LOGV("Sync resume done\n");
            if (mAudioSink.get() != NULL) {
                mAudioSink->resumeSession();
            }
        } else if(mA2DpState == A2DP_ENABLED){

        }
    }
    return err;
}

void MPQAudioPlayer::reset() {

    LOGD("Reset called!!!!!");
    mAsyncReset = true;

    // make sure Extractor thread has exited
    requestAndWaitForExtractorThreadExit();
    LOGE("Extractor Thread killed");

    requestAndWaitForA2DPThreadExit();

    requestAndWaitForA2DPNotificationThreadExit();

    if(mDecoderType == ESoftwareDecoder || mDecoderType== EMS11Decoder ||
        mDecoderType == EHardwareDecoder) {
        if(mPCMStream) {
            LOGV("Close the PCM Stream");
            mPCMStream->stop(mPCMStream);
        }
        mPCMStream = NULL;
    }


    // Close the audiosink after all the threads exited to make sure
    // there is no thread writing data to audio sink or applying effect
    if (mA2DpState == A2DP_ENABLED) {
        struct pcm * capture_handle = (struct pcm *)mCaptureHandle;
        mAudioSink->close();
        mAudioSinkOpen = false;
        //TODO : Deallocate the buffer for capture side
        pcm_close(capture_handle);
        mCaptureHandle = (void*)capture_handle;

    } else if(mA2DpState == A2DP_DISABLED){
        LOGE("A2DP not enabled");
        if(mAudioSink.get() != NULL) {
            LOGV("close session ++");
            mAudioSink->closeSession();
            LOGV("close session --");
            mIsAudioRouted =  false;
        }
    }
    mAudioSink.clear();

    if (mFirstBuffer != NULL) {
        mFirstBuffer->release();
        mFirstBuffer = NULL;
    }

    if (mInputBuffer != NULL) {
        LOGV("MPQ Audio Player releasing input buffer.");
        mInputBuffer->release();
        mInputBuffer = NULL;
    }

    mSource->stop();

    // The following hack is necessary to ensure that the OMX
    // component is completely released by the time we may try
    // to instantiate it again.
    wp<MediaSource> tmp = mSource;
    mSource.clear();
    while (tmp.promote() != NULL) {
        usleep(1000);
    }

    bufferDeAlloc();
    LOGD("Buffer Deallocation complete!");
    mPositionTimeMediaUs = -1;
    mPositionTimeRealUs = -1;

    mSeeking = false;
    mInternalSeeking = false;

    mPostedEOS = false;
    mReachedExtractorEOS = false;
    mFinalStatus = OK;

    mIsPaused = false;
    mPauseEventPending = false;
    mPlayPendingSamples = false;

    mTimePaused  = 0;
    mDurationUs = 0;
    mSeekTimeUs = 0;
    mTimeout = -1;

    mNumChannels = 0;
    mMimeType.setTo("");
    mInputBuffer = NULL;

    mStarted = false;
}

bool MPQAudioPlayer::isSeeking() {
    Mutex::Autolock autoLock(mLock);
    return mSeeking;
}

bool MPQAudioPlayer::reachedEOS(status_t *finalStatus) {
    *finalStatus = OK;
    Mutex::Autolock autoLock(mLock);
    *finalStatus = mFinalStatus;
    return mPostedEOS;
}


void *MPQAudioPlayer::extractorThreadWrapper(void *me) {
    static_cast<MPQAudioPlayer *>(me)->extractorThreadEntry();
    return NULL;
}


void MPQAudioPlayer::extractorThreadEntry() {
    mExtractorMutex.lock();
    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"MPQ Audio DecodeThread", 0, 0, 0);
    LOGV("extractorThreadEntry wait for signal \n");

    while (!mStarted && !mKillExtractorThread) {
        mExtractorCv.wait(mExtractorMutex);
    }

    LOGV("extractorThreadEntry ready to work \n");
    mExtractorMutex.unlock();

    while (!mKillExtractorThread) {
        if (mDecoderType ==ESoftwareDecoder || mDecoderType == EMS11Decoder) {

            mExtractorMutex.lock();
            if(mPostedEOS || mIsPaused ||
                    (mIsA2DPEnabled && !mAudioSinkOpen) || mAsyncReset) {
                LOGV("extractorThreadEntry: waiting on mExtractorCv");
                mExtractorCv.wait(mExtractorMutex);
                //TODO: Guess this should be removed plz verify
                mExtractorMutex.unlock();
                LOGV("extractorThreadEntry: received a signal to wake upi - extractor mutex");
                continue;
            }
            mExtractorMutex.unlock();
            int bytesToWrite = 0;
            if(mDecoderType == EMS11Decoder)
                bytesToWrite = fillBuffer(mLocalBuf, AAC_AC3_BUFFER_SIZE);
            else
                bytesToWrite = fillBuffer(mLocalBuf, mInputBufferSize);
            LOGV("fillBuffer returned size %d",bytesToWrite);
            if(mSeeking)
                continue;
            //TODO : What if bytesWritetn is zero
            LOGV("write - pcm  ++  %p",mPCMStream);
            if(mPCMStream && bytesToWrite) {
                ssize_t bytesWritten = 0;
                if(mAudioFormat == AUDIO_FORMAT_AC3 ||
                            mAudioFormat == AUDIO_FORMAT_AAC ||
                            mAudioFormat == AUDIO_FORMAT_AAC_ADIF) {
                    bytesWritten = mPCMStream->write(
                            mPCMStream, mLocalBuf, bytesToWrite);
                    if(mDecoderType == EMS11Decoder) {
                        uint32_t sampleRate = 0, frameCount = 0;
                        mPCMStream->get_render_position(mPCMStream, &frameCount);

                        //TODO : Need to remove the hardcoding to support 24bit
                        // Channesl is 2  and considering 16bit PCM
                        mFrameSize = 4;
                        sampleRate = mPCMStream->common.get_sample_rate(
                                &mPCMStream->common);
                        mInputBufferSize =  mPCMStream->common.get_buffer_size(
                                &mPCMStream->common);
                        if(sampleRate) {
                            LOGV("frameCount = %d, mInputBufferSize = %d, \
                                    mFrameSize = %d, sampleRate = %d", frameCount,\
                                    mInputBufferSize, mFrameSize, sampleRate);
                            mPositionTimeMediaUs = (((int64_t)(((int64_t)(
                                    (frameCount  * mInputBufferSize)/ mFrameSize))
                                    * 1000000)) / sampleRate);
                            mPositionTimeRealUs =
                                   -mLatencyUs + mPositionTimeMediaUs;
                            LOGV("mPositionTimeMediaUs = %lld",mPositionTimeMediaUs);
                        }
                        else {
                            LOGV("mPositionTimeMediaUs zero");
                            mPositionTimeMediaUs = 0;
                        }
                    }
                }
                else {
                     bytesWritten = mPCMStream->write(
                             mPCMStream, mLocalBuf, mInputBufferSize);
                }
                LOGV("bytesWritten = %d",(int)bytesWritten);
            }
            else if(!mAudioSink->getSessionId()) {
                LOGV("bytesToWrite = %d, mInputBufferSize = %d",\
                        bytesToWrite,mInputBufferSize);
                mAudioSink->write(mLocalBuf, bytesToWrite);
            }
            if(mObserver && mReachedExtractorEOS) {
                LOGV("Posting EOS event..zero byte buffer ");
                //TODO : make it POST EOS to amke sense for  Software
                if(!mPostedEOS) {
                    if( mDecoderType == EMS11Decoder) {
                        memset(mLocalBuf, 0x0, AAC_AC3_BUFFER_SIZE);
                        mPCMStream->write(mPCMStream, mLocalBuf, 0);
                    }
                    if(bytesToWrite <=0)
                        mObserver->postAudioEOS();
                    else
                        mObserver->postAudioEOS( mPostEOSDelayUs);
                    mPostedEOS = true;
                }
            }
            continue;
        }
        else if (mDecoderType == EHardwareDecoder) {
            mExtractorMutex.lock();
            if (mReachedExtractorEOS || mIsPaused ||
                    (mIsA2DPEnabled && !mAudioSinkOpen) || mAsyncReset ) {
                LOGV("extractorThreadEntry: mIsPaused %d  mReachedExtractorEOS %d\
                        mIsA2DPEnabled %d mAudioSinkOpen %d mAsyncReset %d ",\
                        mIsPaused, mReachedExtractorEOS, mIsA2DPEnabled,\
                        mAudioSinkOpen, mAsyncReset);
                LOGV("extractorThreadEntry: waiting on mExtractorCv");
                mExtractorCv.wait(mExtractorMutex);
                //TODO: Guess this should be removed plz verify
                mExtractorMutex.unlock();
                LOGV("extractorThreadEntry: received a signal to wake up");
                continue;
            }

            mExtractorMutex.unlock();
            LOGV("Calling fillBuffer for size %d", mInputBufferSize);
            int bytesToWrite = fillBuffer(mLocalBuf, mInputBufferSize);
            LOGV("fillBuffer returned size %d", bytesToWrite);
            if (mSeeking) {
                continue;
            }
            mPCMStream->write(mPCMStream, mLocalBuf, bytesToWrite);

            if (mIsA2DPEnabled)
                mA2dpCv.signal();
                if (bytesToWrite <= 0)
                    continue;
        }
        else if (mDecoderType == EMS11Decoder) {

        }
        else break;
    }
    mExtractorThreadAlive = false;
    LOGD("Extractor Thread is dying");

}

void MPQAudioPlayer::postEOS(int64_t delayUs) {
    /*Post EOS to Awesome player when i/p EOS is reached,
    all input buffers have been decoded and response queue is empty*/
    LOGD("MPQ: Posting EOS event");
    if(mObserver && mReachedExtractorEOS ) {
        LOGD("Posting EOS event after %lld us.", delayUs);
        mPostedEOS = true;
        mObserver->postAudioEOS(delayUs);
    }
}

void *MPQAudioPlayer::A2DPNotificationThreadWrapper(void *me) {
    static_cast<MPQAudioPlayer *>(me)->A2DPNotificationThreadEntry();
    return NULL;
}


void MPQAudioPlayer::A2DPNotificationThreadEntry() {

    //Wait on A2DP Notification Mutex
    while (1) {
        mA2dpNotificationMutex.lock();
        mA2dpNotificationCv.wait(mA2dpNotificationMutex);
        mA2dpNotificationMutex.unlock();

        //Check for thread exit signal
        if (mKillA2DPNotificationThread) {
            break;
        }

        status_t err = OK;
        LOGV("A2DP notification has come mIsA2DPEnabled: %d", mIsA2DPEnabled);

        if(mIsA2DPEnabled) {

            // Close Routing session if open
            if (!mAudioSinkOpen) {
                LOGV("Close Session");
                if (mAudioSink.get() != NULL) {
                    mAudioSink->closeSession();
                    LOGV("mAudioSink close session");
                    mIsAudioRouted = false;
                } else {
                    LOGE("close session NULL");
                }

                // Open  and Start Sink
                status_t err = mAudioSink->open(mSampleRate, mNumChannels,
                        AUDIO_FORMAT_PCM_16_BIT,
                        DEFAULT_AUDIOSINK_BUFFERCOUNT);
                mAudioSink->start();
                LOGV("After Audio Sink Open");
                mAudioSinkOpen = true;
            }
            // open capture device
            //TODO : What is the proxy afe device?
            char *captureDevice = (char *) "hw:0,x";
            LOGV("pcm_open capture device hardware %s for MPQ Audio Mode ",\
                    captureDevice);

            // Open the capture device
            if (mNumChannels == 1)
                mCaptureHandle = (void *)pcm_open((PCM_MMAP | DEBUG_ON | PCM_MONO |
                        PCM_IN), captureDevice);
            else
                mCaptureHandle = (void *)pcm_open((PCM_MMAP | DEBUG_ON | PCM_STEREO |
                        PCM_IN) , captureDevice);
            struct pcm * capture_handle = (struct pcm *)mCaptureHandle;
            if (!capture_handle) {
                LOGE("Failed to initialize ALSA hardware hw:0,4");
                break;
            }

            //Set the hardware and software params for the capture device
            err = setCaptureALSAParams();
            if(err != OK) {
                LOGE("Set Capture AALSA Params = %d", err);
                break;
            }

            //MMAP the capture buffer
            mmap_buffer(capture_handle);
            //Prepare the capture  device
            err = pcm_prepare(capture_handle);
            if(err != OK) {
                LOGE("PCM Prepare - capture failed err = %d", err);
                break;
            }
            mCaptureHandle = (void *)capture_handle;
            //TODO:set the mixer controls for proxy port
            //mixer_cntl_set
            //TODO : Allocate the buffer required from capture side

            // RESUME
            if(!mIsPaused) {
                if (ioctl(((struct pcm *)mPlaybackHandle)->fd,
                         SNDRV_PCM_IOCTL_PAUSE,0) < 0)
                    LOGE("AUDIO Resume failed");
                LOGV("Sync resume done\n");
            }

            // Signal Extractor thread
            LOGD("Signalling to extractor condition variable");
            mExtractorCv.signal();

            //A2DP thread will be signalled from extractor thread

        } else {

            // Stop and close the sink
            mAudioSink->stop();
            mAudioSink->close();
            mAudioSinkOpen = false;
            LOGV("resume:: opening audio session with mSampleRate %d\
                    numChannels %d ", mSampleRate, mNumChannels);

            // open session / pause session
            err = mAudioSink->openSession(AUDIO_FORMAT_PCM_16_BIT,
                    MPQ_AUDIO_SESSION_ID, mSampleRate, mNumChannels);
            if (mAudioSink.get() != NULL) {
                mAudioSink->pauseSession();
            }

            // stop capture device
            //TODO : De allocate the buffer for capture side
            pcm_close((struct pcm *)mCaptureHandle);

            // Mixer control disable

            // Signal extractor thread
            mExtractorCv.signal(); //check if we need to signal A2Dp thread
        }
    }
//TODO : Need to see if internal seek is required
//       Since in the decoding is in dsp it might
//       not be require
    mA2dpNotificationThreadAlive = false;
    LOGD("A2DPNotificationThread is dying");

}
void *MPQAudioPlayer::A2DPThreadWrapper(void *me) {
    static_cast<MPQAudioPlayer *>(me)->A2DPThreadEntry();
    return NULL;
}

void MPQAudioPlayer::A2DPThreadEntry() {
}

void MPQAudioPlayer::bufferAlloc(int32_t nSize) {

    switch (mDecoderType) {
        case EHardwareDecoder:
        case ESoftwareDecoder:
            mLocalBuf = malloc(nSize);
            if (NULL == mLocalBuf)
                LOGE("Allocate Buffer for Software decoder failed ");
        break;
        case EMS11Decoder:
            mLocalBuf = malloc(AAC_AC3_BUFFER_SIZE);
            if (NULL == mLocalBuf)
                LOGE("Allocate Buffer for MS11  decoder failed ");
        break;
        default:
        break;
   }


}

void MPQAudioPlayer::bufferDeAlloc() {

    //Remove all the buffers from request queue
    switch (mDecoderType) {

        case EHardwareDecoder:
        case ESoftwareDecoder:
        case EMS11Decoder:
            if(mLocalBuf) {
                free(mLocalBuf);
                mLocalBuf = NULL;
            }
        break;
        default:
        break;
    }
}

void MPQAudioPlayer::createThreads() {

    //Initialize all the Mutexes and Condition Variables
    // Create 4 threads Effect, extractor, event and A2dp
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    mKillExtractorThread = false;
    mKillA2DPThread = false;
    mKillA2DPNotificationThread = false;

    mExtractorThreadAlive = true;
    mA2dpThreadAlive = true;
    mA2dpNotificationThreadAlive = true;

    LOGD("Creating Extractor Thread");
    pthread_create(&mExtractorThread, &attr, extractorThreadWrapper, this);

    LOGD("Creating A2DP Thread");
    pthread_create(&mA2DPThread, &attr, A2DPThreadWrapper, this);

    LOGD("Creating A2DP Notification Thread");
    pthread_create(&mA2DPNotificationThread, &attr, A2DPNotificationThreadWrapper, this);

    pthread_attr_destroy(&attr);
}
size_t MPQAudioPlayer::fillBuffer(void *data, size_t size) {

    switch(mDecoderType) {

        case EHardwareDecoder:
            return fillBufferfromParser(data, size);
        break;

        case ESoftwareDecoder:
            return fillBufferfromSoftwareDecoder(data, size);
        break;

        case EMS11Decoder:
                LOGE("get AAC/ AC3 dat from parser");
                return fillMS11InputBufferfromParser(data, size);
        break;

        default:
         LOGE("Fill Buffer - Invalid Decoder");
         //Returning zero size
         return 0;
    }
}

size_t MPQAudioPlayer::fillBufferfromSoftwareDecoder(void *data, size_t size) {

    LOGE("fillBufferfromSoftwareDecoder");
    if (mReachedExtractorEOS) {
        return 0;
    }

    size_t size_done = 0;
    size_t size_remaining = size;

    while (size_remaining > 0) {
        MediaSource::ReadOptions options;
        {
            Mutex::Autolock autoLock(mLock);

            if (mSeeking) {

                if (mIsFirstBuffer) {
                    if (mFirstBuffer != NULL) {
                        mFirstBuffer->release();
                        mFirstBuffer = NULL;
                    }
                    mIsFirstBuffer = false;
                }

                MediaSource::ReadOptions::SeekMode seekMode;
                seekMode = MediaSource::ReadOptions::SEEK_CLOSEST_SYNC;
                options.setSeekTo(mSeekTimeUs, seekMode );
                if (mInputBuffer != NULL) {
                    mInputBuffer->release();
                    mInputBuffer = NULL;
                }

                // This is to ignore the data already filled in the output buffer
                size_done = 0;
                size_remaining = size;

                mSeeking = false;
                if (mObserver && !mAsyncReset) {
                    LOGD("fillBuffer: Posting audio seek complete event");
                    mObserver->postAudioSeekComplete();
                }
            }
        }
        if (mInputBuffer == NULL) {
            status_t err;

            if (mIsFirstBuffer) {
                mInputBuffer = mFirstBuffer;
                mFirstBuffer = NULL;
                err = mFirstBufferResult;

                mIsFirstBuffer = false;
            } else {
                err = mSource->read(&mInputBuffer, &options);
            }

            CHECK((err == OK && mInputBuffer != NULL)
                  || (err != OK && mInputBuffer == NULL));
            {
                Mutex::Autolock autoLock(mLock);

                if (err != OK) {
                    LOGD("fill buffer - reached eos true");
                    mReachedExtractorEOS = true;
                    mFinalStatus = err;
                    break;
                }
                CHECK(mInputBuffer->meta_data()->findInt64(
                                                      kKeyTime, &mPositionTimeMediaUs));
                //TODO : Get Frame size talk ravi
                //mFrameSize = mAudioSink->frameSize();
                //audio_stream_out_t * local_pcmStream = (audio_stream_out_t*) mPCMStream;

                int format = AUDIO_FORMAT_PCM_16_BIT;//local_pcmStream->format();
                if (audio_is_linear_pcm(format)) mFrameSize = mNumChannels * audio_bytes_per_sample(format);
                else mFrameSize = sizeof(uint8_t);
                if(mSampleRate != 0) {
                    mPositionTimeRealUs =
                          ((mNumFramesPlayed + size_done / mFrameSize) * 1000000)
                          / mSampleRate;
                    //TODO: Do we need to return error here
                }
                LOGV("buffer->size() = %d, "
                      "mPositionTimeMediaUs=%.2f mPositionTimeRealUs=%.2f",
                      mInputBuffer->range_length(),
                      mPositionTimeMediaUs / 1E6, mPositionTimeRealUs / 1E6);
                //TODO: Add number of buffers  for Delay use case
                mPostEOSDelayUs = 8 * mLatencyUs;
            }
        }


        if (mInputBuffer->range_length() == 0) {
            mInputBuffer->release();
            mInputBuffer = NULL;
            continue;
        }

        size_t copy = size_remaining;
        if (copy > mInputBuffer->range_length()) {
            copy = mInputBuffer->range_length();
        }
        memcpy((char *)data + size_done,
               (const char *)mInputBuffer->data() + mInputBuffer->range_offset(),
               copy);

        mInputBuffer->set_range(mInputBuffer->range_offset() + copy,
                                mInputBuffer->range_length() - copy);

        size_done += copy;
        size_remaining -= copy;
    }

    {
        Mutex::Autolock autoLock(mLock);
        if(mFrameSize != 0) {
            mNumFramesPlayed += size_done / mFrameSize;
        }
    }

    LOGV("fill buffer size_done = %d",size_done);
    return size_done;
}

size_t MPQAudioPlayer::fillMS11InputBufferfromParser(void *data, size_t size) {

    LOGV("fillAC3BufferfromParser");
    if (mReachedExtractorEOS) {
        return 0;
    }

    size_t size_done = 0;
    size_t size_remaining =  0;

    while (1) {
        MediaSource::ReadOptions options;
        {
            Mutex::Autolock autoLock(mLock);

            if (mSeeking || mInternalSeeking) {

                MediaSource::ReadOptions::SeekMode seekMode;
                seekMode = MediaSource::ReadOptions::SEEK_CLOSEST_SYNC;
                options.setSeekTo(mSeekTimeUs, seekMode );
                if (mInputBuffer != NULL) {
                    mInputBuffer->release();
                    mInputBuffer = NULL;
                }

                // This is to ignore the data already filled in the output buffer
                size_done = 0;
                //size_remaining = size;

                mSeeking = false;
                if (mObserver && !mAsyncReset && !mInternalSeeking) {
                    LOGD("fillBuffer: Posting audio seek complete event");
                    mObserver->postAudioSeekComplete();
                }
                mInternalSeeking = false;
            }
        }
        if (mInputBuffer == NULL) {

            status_t err = OK;

            if(!mFirstEncodedBuffer && ((mAudioFormat == AUDIO_FORMAT_AAC) || (mAudioFormat == AUDIO_FORMAT_AAC_ADIF))) {
                uint32_t type;
                const void *configData;
                size_t configSize = 0;
                   const void *codec_specific_data;
                   size_t codec_specific_data_size = 0;
                if (mSource->getFormat()->findData(kKeyESDS, &type, &configData, &configSize)) {
                    LOGV("GET ESDS - ");
                    ESDS esds((const char *)configData, configSize);
                    CHECK_EQ(esds.InitCheck(), (status_t)OK);

                   esds.getCodecSpecificInfo(
                        &codec_specific_data, &codec_specific_data_size);

                   memcpy((char *)data,(const char *)codec_specific_data, codec_specific_data_size);
                size_done = codec_specific_data_size;
                LOGV("size_done = %d",size_done);
                const char *configarr = (const char *)codec_specific_data;
                for(size_t k = 0; k < size_done ; k++) {
                LOGV("Config data is 0x%x",configarr[k]);
                }

                }
                else if (mSource->getFormat()->findData(kKeyAacCodecSpecificData, &type, &configData, &configSize)) {
                    LOGI("AAC");
                    memcpy((char *)data,(const char *)configData, configSize);
                size_done = configSize;
                LOGV("size_done = %d",size_done);
                const char *configarr = (const char *)configData;
                for(size_t k = 0; k < size_done ; k++) {
                LOGV("Config data is 0x%x",configarr[k]);
                }

                }
                mFirstEncodedBuffer = true;
                //TODO: Handle Error case if config data is zero
                break;
            }

            err = mSource->read(&mInputBuffer, &options);

            CHECK((err == OK && mInputBuffer != NULL)
                  || (err != OK && mInputBuffer == NULL));
            {
                Mutex::Autolock autoLock(mLock);

                if (err != OK) {
                    LOGD("fill buffer - reached eos true");
                    mReachedExtractorEOS = true;
                    mFinalStatus = err;
                    mFinalStatus = err;
                    break;
                }
            }
        }


        if (mInputBuffer->range_length() == 0) {
            mInputBuffer->release();
            mInputBuffer = NULL;
            continue;
        }

        memcpy((char *)data,
               (const char *)mInputBuffer->data() + mInputBuffer->range_offset(),
               mInputBuffer->range_length());

        size_done =  mInputBuffer->range_length();

        mInputBuffer->set_range(mInputBuffer->range_offset(),
                                mInputBuffer->range_length() - mInputBuffer->range_length());

        break;
    }

    LOGV("fill buffer size_done = %d",size_done);
    return size_done;
}


size_t MPQAudioPlayer::fillBufferfromParser(void *data, size_t size) {

    LOGV("fillBufferfromParser");
    if (mReachedExtractorEOS) {
        return 0;
    }

    size_t size_done = 0;
    size_t size_remaining = size;

    while (size_remaining > 0) {
        MediaSource::ReadOptions options;
        {
            Mutex::Autolock autoLock(mLock);

            if (mSeeking || mInternalSeeking) {

                MediaSource::ReadOptions::SeekMode seekMode;
                seekMode = MediaSource::ReadOptions::SEEK_CLOSEST_SYNC;
                options.setSeekTo(mSeekTimeUs, seekMode );
                if (mInputBuffer != NULL) {
                    mInputBuffer->release();
                    mInputBuffer = NULL;
                }

                // This is to ignore the data already filled in the output buffer
                size_done = 0;
                size_remaining = size;

                mSeeking = false;
                if (mObserver && !mAsyncReset && !mInternalSeeking) {
                    LOGD("fillBuffer: Posting audio seek complete event");
                    mObserver->postAudioSeekComplete();
                }
                mInternalSeeking = false;
            }
        }
        if (mInputBuffer == NULL) {
            status_t err;
            err = mSource->read(&mInputBuffer, &options);

            CHECK((err == OK && mInputBuffer != NULL)
                  || (err != OK && mInputBuffer == NULL));
            {
                Mutex::Autolock autoLock(mLock);

                if (err != OK) {
                    LOGD("fill buffer - reached eos true");
                    mReachedExtractorEOS = true;
                    mFinalStatus = err;
                    break;
                }
            }
        }

        if (mInputBuffer->range_length() == 0) {
            mInputBuffer->release();
            mInputBuffer = NULL;
            continue;
        }

        size_t copy = size_remaining;
        if (copy > mInputBuffer->range_length()) {
            copy = mInputBuffer->range_length();
        }
        memcpy((char *)data + size_done,
               (const char *)mInputBuffer->data() + mInputBuffer->range_offset(),
               copy);

        mInputBuffer->set_range(mInputBuffer->range_offset() + copy,
                                mInputBuffer->range_length() - copy);

        size_done += copy;
        size_remaining -= copy;
    }

    if(mReachedExtractorEOS) {
        memset((char *)data + size_done, 0x0, size_remaining);
    }
    LOGV("fill buffer size_done = %d",size_done);
    return size_done;
}

int64_t MPQAudioPlayer::getRealTimeUs() {

    Mutex::Autolock autoLock(mLock);
    CHECK(mStarted);

    switch(mDecoderType) {

        case EHardwareDecoder:
            mPositionTimeRealUs = 0;
            return mPositionTimeRealUs;
        case ESoftwareDecoder:
            CHECK(mSampleRate != 0);
            return -mLatencyUs + (mNumFramesPlayed * 1000000) / mSampleRate;
        case EMS11Decoder:
            //TODO: get the Timestamp renderred for AC3 and AAC
            mPositionTimeRealUs =  -mLatencyUs + mSeekTimeUs + mPositionTimeMediaUs;
            return mPositionTimeRealUs;
        default:
            LOGV(" Invalide Decoder return zero time");
            mPositionTimeRealUs = 0;
            return mPositionTimeRealUs;
    }
}

int64_t MPQAudioPlayer::getMediaTimeUs() {

    Mutex::Autolock autoLock(mLock);
    switch(mDecoderType) {

        case EHardwareDecoder:
            if (mIsPaused) {
                LOGV("getMediaTimeUs - paused = %lld",mTimePaused);
                return mTimePaused;
            } else {
                LOGV("getMediaTimeUs - mSeekTimeUs = %lld", mSeekTimeUs);
                return  ( mSeekTimeUs + getAudioTimeStampUs());
            }
        case ESoftwareDecoder:
            if (mPositionTimeMediaUs < 0 || mPositionTimeRealUs < 0) {
                if(mSeeking) {
                    return mSeekTimeUs;
                }
                return 0;
            }
            LOGV("getMediaTimeUs -  mPositionTimeMediaUs = %lld", mPositionTimeMediaUs);
            return mPositionTimeMediaUs;
        case EMS11Decoder:
            //TODO: get the Timestamp renderred for AC3 and AAC
            if (mIsPaused) {
                LOGV("getMediaTimeUs - paused = %lld",mTimePaused);
                return mTimePaused;
            } else {
                LOGV("getMediaTimeUs - mSeekTimeUs = %lld, mPositionTimeMediaUs = %lld", mSeekTimeUs,mPositionTimeMediaUs);
                return (mPositionTimeMediaUs + mSeekTimeUs );
            }
            return mPositionTimeMediaUs;
        default:
            LOGE(" Invalide Decoder return zero time");
            mPositionTimeMediaUs = 0;
            return mPositionTimeMediaUs;
    }
}

bool MPQAudioPlayer::getMediaTimeMapping(
                                   int64_t *realtime_us, int64_t *mediatime_us) {
    Mutex::Autolock autoLock(mLock);

    switch(mDecoderType) {

        case EHardwareDecoder:
            mPositionTimeMediaUs = (mSeekTimeUs + getAudioTimeStampUs());
        break;

        case ESoftwareDecoder:
        //Dont do anything here
        break;

        case EMS11Decoder:
        //Need to update the time from MS11Decoder
        break;

        default:
            mPositionTimeRealUs = -1;
            mPositionTimeMediaUs = -1;
        break;
    }

    *realtime_us = mPositionTimeRealUs;
    *mediatime_us = mPositionTimeMediaUs;

    return mPositionTimeRealUs != -1 && mPositionTimeMediaUs != -1;
}

void MPQAudioPlayer::requestAndWaitForExtractorThreadExit() {

    if (!mExtractorThreadAlive)
        return;
    LOGD("mKillExtractorThread true");
    mKillExtractorThread = true;
    mExtractorCv.signal();
    pthread_join(mExtractorThread,NULL);
    LOGD("Extractor thread killed");
}

void MPQAudioPlayer::requestAndWaitForA2DPThreadExit() {

    if (!mA2dpThreadAlive)
        return;
    mKillA2DPThread = true;
    mA2dpCv.signal();
    pthread_join(mA2DPThread,NULL);
    LOGD("a2dp thread killed");
}

void MPQAudioPlayer::requestAndWaitForA2DPNotificationThreadExit() {

    if (!mA2dpNotificationThreadAlive)
        return;
    mKillA2DPNotificationThread = true;
    mA2dpNotificationCv.signal();
    pthread_join(mA2DPNotificationThread,NULL);
    LOGD("a2dp notification thread killed");
}

void MPQAudioPlayer::onPauseTimeOut() {
    Mutex::Autolock autoLock(mLock);
#if 0
    LOGD("onPauseTimeOut");
    if (!mPauseEventPending) {
        return;
    }
    mPauseEventPending = false;

    if(!mIsA2DPEnabled) {

        if(!mSeeking) {
            mInternalSeeking = true;
            mSeekTimeUs += getAudioTimeStampUs();
        }
        if(mReachedExtractorEOS) {
            mReachedExtractorEOS = false;
            mPostedEOS = false;
            mTimeout = -1;
        }

        LOGE("onPauseTimeOut seektime= %lld", mSeekTimeUs);

        mInputPmemResponseMutex.lock();
        mInputPmemRequestMutex.lock();
        mInputPmemFilledQueue.clear();
        mInputPmemEmptyQueue.clear();
        List<BuffersAllocated>::iterator it = mInputBufPool.begin();
        for(;it!=mInputBufPool.end();++it) {
             mInputPmemEmptyQueue.push_back(*it);
        }
        mInputPmemRequestMutex.unlock();
        mInputPmemResponseMutex.unlock();
        LOGV("onPauseTimeOut after Empty Queue size() = %d,\
                Filled Queue.size() = %d ",\
                mInputPmemEmptyQueue.size(),\
                mInputPmemFilledQueue.size());
        mAudioSink->closeSession();
        mIsAudioRouted = false;
        release_wake_lock("MPQ_AUDIO_LOCK");
    }
#endif
}

status_t  MPQAudioPlayer::setCaptureALSAParams() {

     struct pcm * capture_handle = (struct pcm *)mCaptureHandle;
     struct snd_pcm_hw_params *hwParams = NULL;
     struct snd_pcm_sw_params *swParams = NULL;
     status_t err = OK;

     LOGV("setCaptureALSAParams");

     hwParams = (struct snd_pcm_hw_params*) calloc(1, sizeof(struct snd_pcm_hw_params));
     if (!hwParams) {
          LOGE("Failed to allocate ALSA hardware parameters - Capture!");
          err = -ENOMEM;
          goto fail;
     }

     param_init(hwParams);

     param_set_mask(hwParams, SNDRV_PCM_HW_PARAM_ACCESS,
             (capture_handle->flags & PCM_MMAP)?
             SNDRV_PCM_ACCESS_MMAP_INTERLEAVED :
             SNDRV_PCM_ACCESS_RW_INTERLEAVED);
     param_set_mask(hwParams, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
     param_set_mask(hwParams, SNDRV_PCM_HW_PARAM_SUBFORMAT,
                    SNDRV_PCM_SUBFORMAT_STD);
     param_set_min(hwParams, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
             PMEM_CAPTURE_BUFFER_SIZE);


     //param_set_min(params, SNDRV_PCM_HW_PARAM_PERIOD_TIME, 10);
     param_set_int(hwParams, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
     param_set_int(hwParams, SNDRV_PCM_HW_PARAM_FRAME_BITS,
                    mNumChannels - 1 ? 32 : 16);
     param_set_int(hwParams, SNDRV_PCM_HW_PARAM_CHANNELS, mNumChannels);
     param_set_int(hwParams, SNDRV_PCM_HW_PARAM_RATE, mSampleRate);

     param_set_hw_refine(capture_handle, hwParams);

     if (param_set_hw_params(capture_handle, hwParams)) {
         LOGE("Cannot set hw params - Capture");
         err = -errno;
         goto fail;
     }

     param_dump(hwParams);
     capture_handle->buffer_size = pcm_buffer_size(hwParams);
     capture_handle->period_size = pcm_period_size(hwParams);
     capture_handle->period_cnt =
             capture_handle->buffer_size/capture_handle->period_size;
     LOGV("Capture - period_size (%d)", capture_handle->period_size);
     LOGV("Capture - buffer_size (%d)", capture_handle->buffer_size);
     LOGV("Capture - period_cnt  (%d)\n", capture_handle->period_cnt);

     //Set Software params
     swParams = (struct snd_pcm_sw_params*) calloc(1, sizeof(struct snd_pcm_sw_params));
     if (!swParams) {
         LOGE("Failed to allocate ALSA software parameters -Capture !\n");
         err = -ENOMEM;
         goto fail;
     }

     swParams->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
     swParams->period_step = 1;
     swParams->avail_min = (capture_handle->flags & PCM_MONO) ?
             capture_handle->period_size/2 : capture_handle->period_size/4;
     swParams->start_threshold = 1;
     swParams->stop_threshold = (capture_handle->flags & PCM_MONO) ?
            capture_handle->buffer_size/2 : capture_handle->buffer_size/4;

     /* needed for old kernels */
     swParams->xfer_align = (capture_handle->flags & PCM_MONO) ?
            capture_handle->period_size/2 : capture_handle->period_size/4;
     swParams->silence_size = 0;
     swParams->silence_threshold = 0;

     if (param_set_sw_params(capture_handle, swParams)) {
         LOGE("Cannot set sw params - Capture");
         err = -22;
         goto fail;
     }

fail:
     return err;
}

int64_t MPQAudioPlayer::getAudioTimeStampUs() {

    uint64_t tstamp;
    if (mPCMStream->get_time_stamp(mPCMStream, &tstamp)) {
        LOGE("MPQ Player: failed SNDRV_COMPRESS_TSTAMP\n");
        return 0;
    }
    else {
        LOGV("timestamp = %lld\n", tstamp);
        return (tstamp + RENDER_LATENCY);
    }
    return 0;
}

status_t MPQAudioPlayer::configurePCM() {

    int err = 0;
    char *mpqAudioDevice = (char *)"";
    int flags = 0;
    LOGV("configurePCM");
    AudioEventObserver *aeObv;
    switch (mDecoderType) {
        case ESoftwareDecoder:
        case EMS11Decoder:
             LOGV("getOutputSession = %d", mAudioSink->getSessionId());
             if(mAudioSink->getSessionId()) {
                mPCMStream = mAudioFlinger->getOutputSession();
                if(mPCMStream == NULL) {
                   LOGE("PCM stream invalid");
                   return BAD_VALUE;
                }
                mInputBufferSize = mPCMStream->common.get_buffer_size(&mPCMStream->common);
                LOGD("mInputBufferSize = %d",mInputBufferSize);
             }
             else {
                mInputBufferSize = mAudioSink->bufferSize();
                LOGD("get sink buffer size = %d",mInputBufferSize);
             }
             if(mDecoderType == EMS11Decoder) {
                 bufferAlloc(AAC_AC3_BUFFER_SIZE);
                 if (NULL == mLocalBuf) {
                     LOGE("Allocate Buffer for Software decoder failed ");
                     //TODO : Return No memory Error
                    return BAD_VALUE;
                 }
                 break;
             }
             bufferAlloc(mInputBufferSize);
             if (NULL == mLocalBuf) {
                 LOGE("Allocate Buffer for Software decoder failed ");
                 //TODO : Return No memory Error
                 return BAD_VALUE;
             }
             //mLatencyUs = (int64_t) local_pcmStream->latency() * 1000;
             mLatencyUs = 24000;
            break;
        case EHardwareDecoder:
             LOGV("getOutputSession = ");
             mPCMStream = mAudioFlinger->getOutputSession();
             CHECK(mPCMStream);
             LOGV("getOutputSession-- ");
             mInputBufferSize = mPCMStream->common.get_buffer_size(&mPCMStream->common);
             LOGD("mInputBufferSize = %d",mInputBufferSize);
             bufferAlloc(mInputBufferSize);
             aeObv = this;
             mPCMStream->set_observer(mPCMStream, reinterpret_cast<void *>(aeObv));
             LOGV("Hardware break");
           break;

        default:
            //TODO : err - debug message
            LOGE("default case - invalid case ");
            break;
    }
    return err;
}
status_t MPQAudioPlayer::getDecoderAndFormat() {

    status_t err = OK;
    if (   !strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_RAW) ||
           !strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_QCELP) ||
           !strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_EVRC) ||
           !strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_AMR_NB) ||
           !strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_AMR_WB) ||
           !strcasecmp(mMimeType.string(),  MEDIA_MIMETYPE_AUDIO_VORBIS) ||
           !strcasecmp(mMimeType.string(),  MEDIA_MIMETYPE_AUDIO_FLAC)) {
        LOGW("Sw Decoder");
        mAudioFormat = AUDIO_FORMAT_PCM_16_BIT;
        mDecoderType = ESoftwareDecoder;
        err = checkForInfoFormatChanged();
        if(err != OK) {
           LOGE("checkForInfoFormatChanged err = %d", err);
           return err;
        }
    }
    else if (!strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_AC3)) {
        LOGW("MS11 AC3");
       mDecoderType = EMS11Decoder;
       mAudioFormat = AUDIO_FORMAT_AC3;
    }
    else if (!strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_AAC)) {
        LOGW("MS11 AAC");
        mDecoderType = EMS11Decoder;
        mAudioFormat = AUDIO_FORMAT_AAC;
        if(mIsAACFormatAdif)
            mAudioFormat = AUDIO_FORMAT_AAC_ADIF;
    }
    else if (!strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_WMA)) {
        LOGW("Hw Decoder - WMA");
        mAudioFormat = AUDIO_FORMAT_WMA;
        mDecoderType = EHardwareDecoder;
    }
    /*else if(!strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_DTS)) {
        LOGE("### Hw Decoder - DTS");
        mAudioFormat = AUDIO_FORMAT_DTS;
        mDecoderType = EHardwareDecoder;
    }*/
    else if(!strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_MPEG)) {
        LOGW("Hw Decoder - MP3");
        mAudioFormat = AUDIO_FORMAT_MP3;
        mDecoderType = EHardwareDecoder;
    }
    else {

       LOGW("invalid format ");
       err =  BAD_VALUE;
    }
     return err;

}

status_t MPQAudioPlayer::openAndConfigureCaptureDevice() {


        //TODO:set the mixer controls for proxy port
        //mixer_cntl_set
        //TODO : Allocate the buffer required for capture side

         status_t err = OK;
        //TODO : What is the proxy afe device?
        char *captureDevice = (char *) "hw:0,8";
        LOGV("pcm_open capture device hardware %s for MPQ Mode ",\
                captureDevice);

        // Open the capture device
        if (mNumChannels == 1)
            mCaptureHandle = (void *)pcm_open((PCM_MMAP | DEBUG_ON | PCM_MONO |
                    PCM_IN), captureDevice);
        else
            mCaptureHandle = (void *)pcm_open((PCM_MMAP | DEBUG_ON | PCM_STEREO |
                    PCM_IN) , captureDevice);
        struct pcm * capture_handle = (struct pcm *)mCaptureHandle;
        if (!capture_handle) {
            LOGE("Failed to initialize ALSA hardware hw:0,4");
            return BAD_VALUE;
        }

        //Set the hardware and software params for the capture device
        err = setCaptureALSAParams();
        if(err != OK) {
            LOGE("Set Capture AALSA Params = %d", err);
            return err;
        }

        //MMAP the capture buffer
        mmap_buffer(capture_handle);
        //Prepare the capture  device
        err = pcm_prepare(capture_handle);
        if(err != OK) {
            LOGE("PCM Prepare - capture failed err = %d", err);
            return err;
        }
        mCaptureHandle = (void *)capture_handle;
        return err;

}

status_t MPQAudioPlayer::checkForInfoFormatChanged() {

    /* Check for an INFO format change for formats
    *  that use software decoder. Update the format
    *  accordingly
    */
    status_t err = OK;
    CHECK(mFirstBuffer == NULL);
    MediaSource::ReadOptions options;
    if (mSeeking) {
        options.setSeekTo(mSeekTimeUs);
        mSeeking = false;
    }
    mFirstBufferResult = mSource->read(&mFirstBuffer, &options);
    if (mFirstBufferResult == INFO_FORMAT_CHANGED) {
        LOGV("INFO_FORMAT_CHANGED!!!");
        CHECK(mFirstBuffer == NULL);
        mFirstBufferResult = OK;
        mIsFirstBuffer = false;
    } else if(mFirstBufferResult != OK) {
        mReachedExtractorEOS = true;
        mFinalStatus = mFirstBufferResult;
        return mFirstBufferResult;
    } else {
        mIsFirstBuffer = true;
    }

    err = updateMetaDataInformation();
    if(err != OK) {
        LOGE("updateMetaDataInformation = %d", err);
    }
    return err;
}

status_t MPQAudioPlayer::updateMetaDataInformation() {

    sp<MetaData> format = mSource->getFormat();
    const char *mime;
    bool success = format->findCString(kKeyMIMEType, &mime);
    mMimeType = mime;
    CHECK(success);

    success = format->findInt32(kKeySampleRate, &mSampleRate);
    CHECK(success);

    success = format->findInt32(kKeyChannelCount, &mNumChannels);
    CHECK(success);

    if(!mNumChannels)
        mNumChannels = 2;

    success = format->findInt32(kkeyAacFormatAdif, &mIsAACFormatAdif);
    //CHECK(success);

    success = format->findInt64(kKeyDuration, &mDurationUs);
    LOGV("mDurationUs = %lld, %s",mDurationUs,mMimeType.string());
    return OK;
}

} //namespace android
