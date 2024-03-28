/****************************************************************************
 * backstage.c
 * openacousticdevices.info
 * January 2023
 *****************************************************************************/

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __APPLE__
    #define MA_NO_RUNTIME_LINKING
#endif

#define MINIAUDIO_IMPLEMENTATION

#include "xtime.h"
#include "macros.h"
#include "threads.h"
#include "wavFile.h"
#include "xsignal.h"
#include "autosave.h"
#include "miniaudio.h"
#include "heterodyne.h"
#include "xdirectory.h"

/* Callback constants */

#define CALLBACKS_PER_SECOND                10

/* Return state */

#define OKAY_RESPONSE                       0
#define ERROR_RESPONSE                      1

/* Capture constants */

#define NUMBER_OF_VALID_AUTOSAVE_DURATIONS  5
#define NUMBER_OF_VALID_SAMPLE_RATES        8
#define MAXIMUM_RECORD_DURATION             60
#define DEFAULT_SAMPLE_RATE                 48000
#define MAXIMUM_SAMPLE_RATE                 384000

#define AUDIO_BUFFER_SIZE                   (1 << 25)
#define NUMBER_OF_BYTES_IN_SAMPLE           2

/* Buffer constants */

#define ARGUMENT_BUFFER_SIZE                1024
#define FILE_TIME_BUFFER_SIZE               1024
#define DEVICE_NAME_SIZE                    1024
#define FILENAME_SIZE                       8192
#define FILE_DESTINATION_SIZE               8192

/* Unit conversion constants */

#define HERTZ_IN_KILOHERTZ                  1000

#define SECONDS_IN_MINUTE                   60
#define MINUTES_IN_HOUR                     60
#define MILLISECONDS_IN_SECOND              1000
#define MICROSECONDS_IN_SECOND              1000000

/* Frame timer constants */

#define TIME_MISMATCH_LIMIT                 2000

/* Device check constant */

#define DEVICE_STOP_START_TIMEOUT           2.0
#define DEVICE_CHANGE_INTERVAL              1.0
#define DEVICE_CHECK_INTERVAL               (MICROSECONDS_IN_SECOND / 4)

/* Monitor constants */

#define PLAYBACK_SAMPLE_RATE                48000

#if IS_WINDOWS
    #define MAXIMUM_PLAYBACK_LAG            (CALLBACKS_PER_SECOND / 2)
    #define TARGET_PLAYBACK_LAG             (CALLBACKS_PER_SECOND / 10)
#else
    #define MAXIMUM_PLAYBACK_LAG            (CALLBACKS_PER_SECOND / 4)
    #define TARGET_PLAYBACK_LAG             (CALLBACKS_PER_SECOND / 20)
#endif

/* Autosave constants */

#define AUTOSAVE_EVENT_QUEUE_SIZE           16

#define DEVICE_SHUTDOWN_TIMEOUT             2.0

/* Heterodyne constant */

#define MINIMUM_HETERODYNE_FREQUENCY        12000

/* Audio buffer variables */

static int16_t *audioBuffer;

static int32_t audioBufferIndex;

static int32_t audioBufferWriteIndex;

static pthread_mutex_t audioBufferMutex;

/* Playback variables */

static pthread_t startPlaybackThread;

/* Frontend state variables */

static double timeDeviceStarted;

static bool heterodyneEnabled;

static ma_timer timer;

/* File destination variable */

static char fileDestination[FILE_DESTINATION_SIZE];

/* Local time variable */

static bool useLocalTime = true;

/* Background thread variables */

static pthread_t backgroundThread;

static pthread_mutex_t backgroundMutex;

static double backgroundDeviceCheckTime;

static pthread_mutex_t backgroundDeviceCheckMutex;

static bool backgroundDeviceCheckFoundAudioMoth;

static bool backgroundDeviceCheckFoundOldAudioMoth;

/* Autosave capture variables */

static int32_t autosaveDuration;

static int64_t autosaveStartTime;

static int64_t autosaveSampleCount;

static int64_t autosaveStartSampleCount;

/* Autosave file variables */

static time_t autosaveFileStartTime;

static int32_t autosaveFileStartIndex;

static int64_t autosaveFileStartCount;

static int32_t autosaveFileSampleRate;

/* Autosave state variables */

static bool autosaveShutdownCompleted;

static pthread_mutex_t autosaveMutex;

static int64_t autosaveTargetCount = INT64_MAX;

static bool autosaveWaitingForStartEvent = true;

static char autosaveInputDeviceCommentName[DEVICE_NAME_SIZE];

/* Miniaudio contexts */

static ma_context playbackContext;

static ma_context deviceCheckContext;

/* Stop and start variable */

static bool stopped;

static bool started;

static pthread_mutex_t stopStartMutex;

/* State variables */

static bool usingAudioMoth;

static volatile bool success;

static ma_device captureDevice;

static ma_device playbackDevice;

static ma_device_id audioMothDeviceID;

/* Sample rate variables */

static int32_t currentSampleRate;

static int32_t audioMothSampleRate;

static int32_t requestedSampleRate = DEFAULT_SAMPLE_RATE;

static int32_t maximumDefaultSampleRate = DEFAULT_SAMPLE_RATE;

static int32_t validAutosaveDurations[NUMBER_OF_VALID_AUTOSAVE_DURATIONS] = {0, 1, 5, 10, 60};

static int32_t validSampleRates[NUMBER_OF_VALID_SAMPLE_RATES] = {8000, 16000, 32000, 48000, 96000, 192000, 250000, 384000};

/* Input device variables */

static int32_t inputDeviceSampleRate;

static char inputDeviceName[DEVICE_NAME_SIZE];

static char inputDeviceCommentName[DEVICE_NAME_SIZE];

/* Structure for device check */

typedef struct {
    bool audioMothFound;
    bool oldAudioMothFound;
} device_check_t;

/* Structure for device enumeration */

typedef struct {
    bool updateAudioMothSettings;
    device_check_t device_check;
} enumerate_devices_t;

/* Callbacks to handle capture and playback of audio samples */

void capture_notification_callback(const ma_device_notification *pNotification) {

    if (pNotification->type == ma_device_notification_type_started) { }

    if (pNotification->type == ma_device_notification_type_stopped) {
        
        pthread_mutex_lock(&stopStartMutex);
        
        stopped = true;

        pthread_mutex_unlock(&stopStartMutex);

    }

    if (pNotification->type == ma_device_notification_type_rerouted) { }

    if (pNotification->type == ma_device_notification_type_interruption_began) { }

    if (pNotification->type == ma_device_notification_type_interruption_ended) { }

}

void playback_data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount) {

    int16_t *outputBuffer = (int16_t*)pOutput;

    /* Static playback variables */

    static double playbackPosition = 0;

    static int32_t playbackReadIndex = 0;

    static double playbackNextSample = 0;

    static double playbackCurrentSample = 0;

    static bool playbackBufferWaiting = false;

    /* Calculate the buffer lag */

    int32_t sampleLag = (AUDIO_BUFFER_SIZE + audioBufferWriteIndex - playbackReadIndex) % AUDIO_BUFFER_SIZE;

    int32_t bufferLag = sampleLag * CALLBACKS_PER_SECOND / currentSampleRate;

    /* Check minimum buffer lag */

    if (bufferLag > MAXIMUM_PLAYBACK_LAG) {
       
        playbackReadIndex = audioBufferWriteIndex;

        playbackBufferWaiting = true;

        sampleLag = 0;

        bufferLag = 0;

    }

    /* Update shared global variables */

    bool starvation = sampleLag < (int32_t)frameCount;

    /* Provide samples to playback device */

    if (playbackBufferWaiting || starvation) {

        for (ma_uint32 i = 0; i < frameCount; i += 1) outputBuffer[i] = 0;

    } else {

        if (heterodyneEnabled) Heterodyne_normalise();

        int32_t sampleRateDivider = MAXIMUM_SAMPLE_RATE / PLAYBACK_SAMPLE_RATE;

        double step = (double)currentSampleRate / (double)MAXIMUM_SAMPLE_RATE;

        for (ma_uint32 i = 0; i < frameCount; i += 1) {

            double playbackAccumulator = 0;

            for (int32_t j = 0; j < sampleRateDivider; j += 1) {

                double sample = playbackCurrentSample + playbackPosition * (playbackNextSample - playbackCurrentSample);

                playbackAccumulator += heterodyneEnabled ? Heterodyne_nextOutput(sample) : sample;

                playbackPosition += step;

                if (playbackPosition >= 1.0) {

                    playbackCurrentSample = playbackNextSample;

                    playbackNextSample = audioBuffer[playbackReadIndex];

                    playbackReadIndex = (playbackReadIndex + 1) % AUDIO_BUFFER_SIZE;

                    playbackPosition -= 1.0;

                }

            }

            double sample = MAX(INT16_MIN, MIN(INT16_MAX, round(playbackAccumulator / (double)sampleRateDivider)));

            outputBuffer[i] = (int16_t)sample;

        }

    }

    if (bufferLag > TARGET_PLAYBACK_LAG) playbackBufferWaiting = false;

}

void capture_data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount) {

    int64_t startTime = 0;

    int32_t increment = 0;

    int16_t *inputBuffer = (int16_t*)pInput;

    int32_t sampleRateDivider = (int32_t)ceil(inputDeviceSampleRate / currentSampleRate);

    int32_t interpolationSampleRate = sampleRateDivider * currentSampleRate;

    double step = (double)inputDeviceSampleRate / (double)interpolationSampleRate;

    /* Static resample variables */

    static int32_t resampleCounter = 0;

    static double resamplePosition = 0;

    static double resampleNextSample = 0;

    static double resampleAccumulator = 0;

    static double resampleCurrentSample = 0;

    /* Check for restart */

    pthread_mutex_lock(&stopStartMutex);

    bool restart = started == false;

    pthread_mutex_unlock(&stopStartMutex);

    if (restart) {

        /* Get start time */

        startTime = Time_getMillisecondUTC();

        /* Reset resampler */

        resampleCounter = 0;

        resamplePosition = 0;

        resampleNextSample = 0;

        resampleAccumulator = 0;

        resampleCurrentSample = 0;

    }

    /* Process samples */

    for (ma_uint32 i = 0; i < frameCount; i += 1) {

        resampleCurrentSample = resampleNextSample;

        resampleNextSample = inputBuffer[i];

        while (resamplePosition < 1.0) {

            resampleAccumulator += resampleCurrentSample + resamplePosition * (resampleNextSample - resampleCurrentSample);

            resampleCounter += 1;

            if (resampleCounter == sampleRateDivider) {

                double sample = MAX(INT16_MIN, MIN(INT16_MAX, round(resampleAccumulator / (double)sampleRateDivider)));

                audioBuffer[audioBufferIndex] = (int16_t)sample;

                audioBufferIndex = (audioBufferIndex + 1) % AUDIO_BUFFER_SIZE;

                resampleAccumulator = 0;

                resampleCounter = 0;

                increment += 1;

            }

            resamplePosition += step;

        }

        resamplePosition -= 1.0;
    
    }   

    pthread_mutex_lock(&audioBufferMutex);

    audioBufferWriteIndex = (audioBufferWriteIndex + increment) % AUDIO_BUFFER_SIZE;

    if (restart) {
        
        autosaveStartTime = startTime;

        autosaveStartSampleCount = autosaveSampleCount;

    }

    autosaveSampleCount += increment;

    pthread_mutex_unlock(&audioBufferMutex);

    if (restart) {

        pthread_mutex_lock(&stopStartMutex);

        started = true;

        pthread_mutex_unlock(&stopStartMutex);

    }

}

/* Functions to check for AudioMoth */

ma_bool32 enumerate_devices_callback(ma_context *pContext, ma_device_type deviceType, const ma_device_info* deviceInfo, void *pUserData) {

    enumerate_devices_t *enumerateDevices = (enumerate_devices_t*)pUserData;

    if (strstr(deviceInfo->name, "F32x USBXpress Device")) enumerateDevices->device_check.oldAudioMothFound = true;

    if (strstr(deviceInfo->name, "AudioMoth")) {

        if (strstr(deviceInfo->name, "kHz AudioMoth") == NULL) enumerateDevices->device_check.oldAudioMothFound = true;

        enumerateDevices->device_check.audioMothFound = true;

        if (enumerateDevices->updateAudioMothSettings) { 

            memcpy(&audioMothDeviceID, &(deviceInfo->id), sizeof(ma_device_id));

            char *digit = strstr(deviceInfo->name, "kHz") - 1;

            if (digit == NULL) {

                audioMothSampleRate = MAXIMUM_SAMPLE_RATE;

            } else {

                int32_t multiplier = 1000;

                audioMothSampleRate = 0;

                do {

                    audioMothSampleRate += multiplier * (*digit - '0');

                    multiplier *= 10;

                    digit -= 1;

                } while (*digit >= '0' && *digit <= '9');

            }

        }

        return MA_FALSE;

    }

    return MA_TRUE;

}

static device_check_t checkForAudioMoth(ma_context *context, bool updateAudioMothSettings) {

    enumerate_devices_t info = {
        .updateAudioMothSettings = updateAudioMothSettings,
        .device_check.oldAudioMothFound = false,
        .device_check.audioMothFound = false
    };

    ma_result result = ma_context_enumerate_devices(context, enumerate_devices_callback, (void*)&info);

    if (result != MA_SUCCESS) {

        device_check_t device_check = {
            .oldAudioMothFound = false,
            .audioMothFound = false
        };

        return device_check;

    }

    return info.device_check;
    
}

/* Thread functions to start and stop capture device */

static bool startMicrophone(ma_context *context, bool usingAudioMoth) {

    /* Initialise capture device */

    ma_device_config captureDeviceConfig = ma_device_config_init(ma_device_type_capture);

    captureDeviceConfig.capture.pDeviceID = usingAudioMoth ? &audioMothDeviceID : NULL;
    captureDeviceConfig.capture.format = ma_format_s16;
    captureDeviceConfig.capture.channels = 1;
    captureDeviceConfig.capture.shareMode  = ma_share_mode_shared;

    inputDeviceSampleRate = usingAudioMoth ? audioMothSampleRate : maximumDefaultSampleRate;

    sprintf(inputDeviceName, usingAudioMoth ? "%dkHz AudioMoth USB Microphone" : "%dkHz Default Input", inputDeviceSampleRate / HERTZ_IN_KILOHERTZ);

    sprintf(inputDeviceCommentName, usingAudioMoth ? "a %dkHz AudioMoth USB Microphone" : "the %dkHz default input", inputDeviceSampleRate / HERTZ_IN_KILOHERTZ);

    currentSampleRate = MIN(requestedSampleRate, inputDeviceSampleRate);

    captureDeviceConfig.sampleRate = inputDeviceSampleRate;
    captureDeviceConfig.periodSizeInFrames = inputDeviceSampleRate / CALLBACKS_PER_SECOND;
    captureDeviceConfig.dataCallback = capture_data_callback;
    captureDeviceConfig.notificationCallback = capture_notification_callback;

    ma_result result = ma_device_init(context, &captureDeviceConfig, &captureDevice);

    if (result != MA_SUCCESS) return false;

    /* Start the capture device */

    result = ma_device_start(&captureDevice);

    if (result != MA_SUCCESS) return false;

    return true;

}

void stopMicrophone(void) {

    ma_device_stop(&captureDevice);

    ma_device_uninit(&captureDevice);
        
    memset(&captureDevice, 0, sizeof(ma_device));

}

/* Static thread functions to start and stop playback device */

static void *startPlaybackThreadBody(void *ptr) {

    /* Initialise playback device */

    ma_device_config playbackDeviceConfig = ma_device_config_init(ma_device_type_playback);

    playbackDeviceConfig.playback.pDeviceID = NULL;
    playbackDeviceConfig.playback.format = ma_format_s16;
    playbackDeviceConfig.playback.channels = 1;

    playbackDeviceConfig.sampleRate = PLAYBACK_SAMPLE_RATE;
    playbackDeviceConfig.periodSizeInFrames = PLAYBACK_SAMPLE_RATE / CALLBACKS_PER_SECOND;
    playbackDeviceConfig.dataCallback = playback_data_callback;
    playbackDeviceConfig.notificationCallback = NULL;

    ma_result result = ma_device_init(&playbackContext, &playbackDeviceConfig, &playbackDevice);

    if (result != MA_SUCCESS) {
        
        puts("[ERROR] Failed to initialise playback device");

        return NULL;

    }

    /* Start the playback device */

    result = ma_device_start(&playbackDevice);

    if (result != MA_SUCCESS) puts("[ERROR] Failed to start playback device");

    return NULL;

}

/* Background and autosave functions */

static void addAutosaveEvent(AS_event_type_t eventType) {

    AS_event_t event;

    event.type = eventType;

    event.sampleRate = currentSampleRate;

    pthread_mutex_lock(&audioBufferMutex);

    event.currentCount = autosaveSampleCount;
    event.currentIndex = audioBufferWriteIndex;
 
    event.startTime = autosaveStartTime;
    event.startCount = autosaveStartSampleCount;

    pthread_mutex_unlock(&audioBufferMutex);

    memcpy(event.inputDeviceCommentName, inputDeviceCommentName, DEVICE_NAME_SIZE);

    Autosave_addEvent(&event);

}

/* Private function */

static void formatFileTime(char *buffer, time_t start, time_t stop, int32_t timeOffset) {

    struct tm time;

    time_t rawTime = start + timeOffset;

    Time_gmTime(&rawTime, &time);

    buffer += sprintf(buffer, "%02d:%02d:%02d to ", time.tm_hour, time.tm_min, time.tm_sec);

    rawTime = stop + timeOffset;

    Time_gmTime(&rawTime, &time);

    buffer += sprintf(buffer, "%02d:%02d:%02d (UTC", time.tm_hour, time.tm_min, time.tm_sec);

    int32_t timeOffsetMinutes = timeOffset / SECONDS_IN_MINUTE;

    int32_t timezoneHours = timeOffsetMinutes / MINUTES_IN_HOUR;

    int32_t timezoneMinutes = timeOffsetMinutes % MINUTES_IN_HOUR;

    if (timezoneHours < 0) {

        buffer += sprintf(buffer, "%d", timezoneHours);

    } else if (timezoneHours > 0) {

        buffer += sprintf(buffer, "+%d", timezoneHours);

    } else {

        if (timezoneMinutes < 0) buffer += sprintf(buffer, "-0");

        if (timezoneMinutes > 0) buffer += sprintf(buffer, "+0");

    }

    if (timezoneMinutes < 0) buffer += sprintf(buffer, ":%02d", -timezoneMinutes);

    if (timezoneMinutes > 0) buffer += sprintf(buffer, ":%02d", timezoneMinutes);

    sprintf(buffer, ")");

}

static bool writeAutosaveFile(int32_t duration) {

    bool success = false;

    static WAV_header_t autosaveHeader;

    static int32_t previousLocalTimeOffset = 0;

    static char autosaveFilename[FILENAME_SIZE];

    static time_t autosaveFilePreviousStopTime = 0;

    if (duration == 0) return true;

    /* Read local time offset */

    int32_t localTimeOffset = useLocalTime ? Time_getLocalTimeOffset() : 0;

    /* Determine whether file should be appended */

    struct tm timeStart;

    time_t rawTimeStart = autosaveFileStartTime;

    Time_gmTime(&rawTimeStart, &timeStart);
    
    bool append = localTimeOffset == previousLocalTimeOffset;
    
    append &= autosaveFileStartTime == autosaveFilePreviousStopTime;

    append &= timeStart.tm_sec == 0 && timeStart.tm_min % autosaveDuration > 0;

    autosaveFilePreviousStopTime = autosaveFileStartTime + duration;

    previousLocalTimeOffset = localTimeOffset;

    /* Write the output WAV file */

    int32_t numberOfSamples = duration * autosaveFileSampleRate;

    int32_t overlap = autosaveFileStartIndex + numberOfSamples - AUDIO_BUFFER_SIZE;

    if (append == true) {

        if (overlap < 0) {

            success = WavFile_appendFile(autosaveFilename, audioBuffer + autosaveFileStartIndex, numberOfSamples, NULL, 0);

        } else {

            success = WavFile_appendFile(autosaveFilename, audioBuffer + autosaveFileStartIndex, numberOfSamples - overlap, audioBuffer, overlap);

        }

    }

    if (append == false || success == false) {

        WavFile_initialiseHeader(&autosaveHeader);

        WavFile_setHeaderDetails(&autosaveHeader, autosaveFileSampleRate, numberOfSamples);

        WavFile_setHeaderComment(&autosaveHeader, (int32_t)autosaveFileStartTime + localTimeOffset, -1, localTimeOffset, autosaveInputDeviceCommentName);

        WavFile_setFilename(autosaveFilename, (int32_t)autosaveFileStartTime + localTimeOffset, -1, fileDestination);

        if (overlap < 0) {

            success = WavFile_writeFile(&autosaveHeader, autosaveFilename, audioBuffer + autosaveFileStartIndex, numberOfSamples, NULL, 0);

        } else {

            success = WavFile_writeFile(&autosaveHeader, autosaveFilename, audioBuffer + autosaveFileStartIndex, numberOfSamples - overlap, audioBuffer, overlap);

        }

    }

    /* Log output file */

    static char buffer[FILE_TIME_BUFFER_SIZE];

    formatFileTime(buffer, autosaveFileStartTime, autosaveFilePreviousStopTime, localTimeOffset);

    puts(buffer);

    /* Return status */

    return success;

}

static bool makeMinuteTransitionRecording(void) {

    /* Generate partial recording */

    int64_t sampleCountDifference = autosaveTargetCount - autosaveFileStartCount;

    int32_t duration = (int32_t)(sampleCountDifference / autosaveFileSampleRate);

    bool success = writeAutosaveFile(duration);

    /* Update for next minute transition */

    autosaveFileStartTime += duration;

    autosaveFileStartIndex = (autosaveFileStartIndex + sampleCountDifference) % AUDIO_BUFFER_SIZE;

    autosaveFileStartCount = autosaveTargetCount;

    autosaveTargetCount = autosaveFileStartCount + SECONDS_IN_MINUTE * autosaveFileSampleRate;

    return success;

}

static void updateForMillisecondOffset(int32_t milliseconds) {

    /* Update count, index and time for millisecond offset */

    if (milliseconds > 0) {
        
        int32_t millisecondOffset = MILLISECONDS_IN_SECOND - milliseconds;

        int32_t sampleOffset = ROUNDED_DIV(autosaveFileSampleRate * millisecondOffset, MILLISECONDS_IN_SECOND);

        autosaveFileStartCount += sampleOffset;

        autosaveFileStartIndex = (autosaveFileStartIndex + sampleOffset) % AUDIO_BUFFER_SIZE;

        autosaveFileStartTime += 1;

    }

    /* Calculate target sample count for the next minute transition */

    struct tm time;

    time_t rawTime = autosaveFileStartTime;

    Time_gmTime(&rawTime, &time);

    autosaveTargetCount = autosaveFileStartCount + (SECONDS_IN_MINUTE - time.tm_sec) * autosaveFileSampleRate;

}

static void *backgroundThreadBody(void *ptr) {

    static AS_event_t event;
    
    while (true) {

        /* Check for AudioMoth */

        pthread_mutex_lock(&backgroundDeviceCheckMutex);

        device_check_t device_check = checkForAudioMoth(&deviceCheckContext, false);

        bool audioMothFound = device_check.audioMothFound;
        
        bool oldAudioMothFound = device_check.oldAudioMothFound;

        pthread_mutex_unlock(&backgroundDeviceCheckMutex);

        pthread_mutex_lock(&backgroundMutex);

        backgroundDeviceCheckTime = ma_timer_get_time_in_seconds(&timer);

        backgroundDeviceCheckFoundAudioMoth = audioMothFound;

        backgroundDeviceCheckFoundOldAudioMoth = oldAudioMothFound;

        pthread_mutex_unlock(&backgroundMutex);

        /* Get current sample count and autosave duration */

        pthread_mutex_lock(&audioBufferMutex);

        int64_t currentSampleCount = autosaveSampleCount;

        pthread_mutex_unlock(&audioBufferMutex);

        /* Process autosave events */

        bool success = true;

        while (Autosave_hasEvents()) {

            /* Copy and remove first event */

            Autosave_getFirstEvent(&event);

            /* Process event */

            if (autosaveWaitingForStartEvent && event.type == AS_START) {

                /* Set sample rate and device */

                autosaveFileSampleRate = event.sampleRate;

                memcpy(autosaveInputDeviceCommentName, event.inputDeviceCommentName, DEVICE_NAME_SIZE);

                /* Adjust start time to match current count and index */

                int64_t countDifference = event.currentCount - event.startCount;

                int64_t updatedStartTime = event.startTime + ROUNDED_DIV(countDifference * MILLISECONDS_IN_SECOND, autosaveFileSampleRate);

                int32_t milliseconds = updatedStartTime % MILLISECONDS_IN_SECOND;

                autosaveFileStartTime = updatedStartTime / MILLISECONDS_IN_SECOND;

                autosaveFileStartCount = event.currentCount;

                autosaveFileStartIndex = event.currentIndex;

                /* Update start time, count and index for millisecond offset */

                updateForMillisecondOffset(milliseconds); 

                /* Reset flag */
                
                autosaveWaitingForStartEvent = false;

            }

            if (currentSampleCount >= autosaveTargetCount && autosaveTargetCount < event.currentCount) {

                success &= makeMinuteTransitionRecording();

            }

            if (event.type == AS_RESTART) {

                /* Write samples since last start to file */

                int32_t duration = (int32_t)(event.startCount - autosaveFileStartCount) / autosaveFileSampleRate;

                success &= writeAutosaveFile(duration);

                /* Set sample rate and device */

                autosaveFileSampleRate = event.sampleRate;

                memcpy(autosaveInputDeviceCommentName, event.inputDeviceCommentName, DEVICE_NAME_SIZE);

                /* Adjust current index to match start time and count */

                int32_t milliseconds = event.startTime % MILLISECONDS_IN_SECOND;

                autosaveFileStartTime = event.startTime / MILLISECONDS_IN_SECOND;

                autosaveFileStartCount = event.startCount;

                int64_t countDifference = (event.currentCount - event.startCount);

                autosaveFileStartIndex = (AUDIO_BUFFER_SIZE + event.currentIndex - countDifference) % AUDIO_BUFFER_SIZE;

                /* Update start time, count and index for millisecond offset */

                updateForMillisecondOffset(milliseconds);

            }

            if (event.type == AS_STOP) {

                /* Write samples since last start to file */

                int32_t duration = (int32_t)(event.currentCount - autosaveFileStartCount) / autosaveFileSampleRate;

                success &= writeAutosaveFile(duration);

                /* Reset flags */

                autosaveWaitingForStartEvent = true;

                autosaveTargetCount = INT64_MAX;

            }

            if (event.type == AS_SHUTDOWN) {

                if (autosaveWaitingForStartEvent == false) {

                    /* Write samples since last start to file */

                    int32_t duration = (int32_t)(event.currentCount - autosaveFileStartCount) / autosaveFileSampleRate;

                    writeAutosaveFile(duration);

                }

                pthread_mutex_lock(&autosaveMutex);

                autosaveShutdownCompleted = true;

                pthread_mutex_unlock(&autosaveMutex);

                /* Reset flags */

                autosaveWaitingForStartEvent = true;

                autosaveTargetCount = INT64_MAX;

            }

        }

        if (currentSampleCount >= autosaveTargetCount) {

            success &= makeMinuteTransitionRecording();

        }

        /* Thread safe callback */

        if (success == false) {

            puts("[AUTOSAVE] Could not write WAV file");

        }

        /* Calculate delay period to wait for next update */

        uint32_t microseconds = Time_getMicroseconds();

        uint32_t delay = DEVICE_CHECK_INTERVAL - microseconds % DEVICE_CHECK_INTERVAL;

        usleep(delay);

    }

    return NULL;

}

/* Interrupt handler */

void Signal_handleSignal(void) {

    success = false;

}

/* Argument parsing functions */

static bool parseArgument(char *pattern, char *text) {

    static char buffer[ARGUMENT_BUFFER_SIZE];

    for (int32_t i = 0; i < ARGUMENT_BUFFER_SIZE; i += 1) {

        buffer[i] = toupper(text[i]);

        if (buffer[i] == 0) break;

    }

    return strncmp(pattern, buffer, ARGUMENT_BUFFER_SIZE) == 0;

}

static bool parseNumber(char *text, int32_t *number) {

    for (int32_t i = 0; i < ARGUMENT_BUFFER_SIZE; i += 1) {

        char character = text[i];

        if (character == 0) break;

        if (character < '0' || character > '9') return false;

    }

    if (number != NULL) *number = atoi(text);

    return true;

}

static bool parseNumberAgainstList(char *text, int32_t *validNumbers, int32_t length, int32_t *number) {

    int32_t value;

    bool isNumber = parseNumber(text, &value);

    if (isNumber == false) return false;

    for (int32_t i = 0; i < length; i += 1) {

        if (value == validNumbers[i]) {

            *number = value;

            return true;

        }

    }

    return false;

}

/* Exported functions */

int main(int argc, char **argv) {

    success = true;

    bool monitorEnabled = false;

    puts("AudioMoth-Live 1.0.0");

    /* Set default file destination */

    strncpy(fileDestination, ".", FILE_DESTINATION_SIZE);

    /* Parse arguments */

    bool parseError = false;

    int32_t argumentCounter = 1;

    int32_t heterodyneFrequency = 0;

    int32_t possibleFileDestinationCount = 0;

    while (argumentCounter < argc) {

        char *argument = argv[argumentCounter];

        if (argumentCounter == possibleFileDestinationCount) {

            bool exists = Directory_exists(argument);

            if (exists) {

                strncpy(fileDestination, argument, FILE_DESTINATION_SIZE);

                argumentCounter += 1;

                continue;

            }

        }

        if (parseArgument("HIGHSAMPLERATE", argument) || parseArgument("HSR", argument)) {
            
            maximumDefaultSampleRate = MAXIMUM_SAMPLE_RATE;

        } else if (parseArgument("UTC", argument)) {
            
            useLocalTime = false;

        } else if (parseArgument("AUTOSAVE", argument)) {

            argumentCounter += 1;

            argument = argv[argumentCounter];

            possibleFileDestinationCount = argumentCounter + 1;
        
            parseError = argumentCounter == argc || parseNumberAgainstList(argument, validAutosaveDurations, NUMBER_OF_VALID_AUTOSAVE_DURATIONS, &autosaveDuration) == false;

        } else if (parseArgument("MONITOR", argument)) {
            
            monitorEnabled = true;

        } else if (parseArgument("HETERODYNE", argument)) { 

            argumentCounter += 1;

            heterodyneEnabled = true;

            argument = argv[argumentCounter];

            parseError = argumentCounter == argc || parseNumber(argument, &heterodyneFrequency) == false;

        } else {

            bool isNumber = parseNumber(argument, NULL);

            if (isNumber) {

                parseError = parseNumberAgainstList(argument, validSampleRates, NUMBER_OF_VALID_SAMPLE_RATES, &requestedSampleRate) == false;

            } else if (argumentCounter == possibleFileDestinationCount) {

                puts("[ERROR] Could not find file destination.");

                return ERROR_RESPONSE;

            } else {

                parseError = true;

            }

        }

        if (parseError) break;

        argumentCounter += 1;

    }

    /* End if nothing parse error or nothing to do */

    if (parseError) {

        puts("[ERROR] Could not parse arguments.");

        return ERROR_RESPONSE;

    }
    
    if (monitorEnabled == false && heterodyneEnabled == false && autosaveDuration == 0) return OKAY_RESPONSE;

    /* Initialise timers */

    ma_timer_init(&timer);

    /* Initialise the contexts */

    ma_result result = ma_context_init(NULL, 0, NULL, &deviceCheckContext);

    if (result != MA_SUCCESS) {
        
        puts("[ERROR] Could not initialise audio input context.");

        success = false;

    }

    result = ma_context_init(NULL, 0, NULL, &playbackContext);

    if (result != MA_SUCCESS) {
        
        puts("[ERROR] Could not initialise audio output context.");

        success = false;

    }

    /* Initialise autosave queue */

    bool initialised = Autosave_initialise(AUTOSAVE_EVENT_QUEUE_SIZE);

    if (initialised == false) {
        
        puts("[ERROR] Could not initialise autosave queue.");

        success = false;

    }

    /* Initialise the audio buffer */

    audioBuffer = (int16_t*)malloc(AUDIO_BUFFER_SIZE * NUMBER_OF_BYTES_IN_SAMPLE);

    if (audioBuffer == NULL) {

        puts("[ERROR] Could not initialise audio buffer.");

        success = false;

    }

    /* Initialise mutexes */

    pthread_mutex_init(&autosaveMutex, NULL);

    pthread_mutex_init(&stopStartMutex, NULL);

    pthread_mutex_init(&backgroundMutex, NULL);

    pthread_mutex_init(&audioBufferMutex, NULL);

    pthread_mutex_init(&backgroundDeviceCheckMutex, NULL);

    /* Start the background thread */

    pthread_create(&backgroundThread, NULL, backgroundThreadBody, NULL);

    /* Reset the start flag */

    pthread_mutex_lock(&stopStartMutex);

    started = false;

    pthread_mutex_unlock(&stopStartMutex);
        
    /* Start device */

    pthread_mutex_lock(&backgroundDeviceCheckMutex);

    device_check_t device_check = checkForAudioMoth(&deviceCheckContext, true);

    usingAudioMoth = device_check.audioMothFound;

    bool startedMicrophone = startMicrophone(&deviceCheckContext, usingAudioMoth);

    pthread_mutex_unlock(&backgroundDeviceCheckMutex);

    if (startedMicrophone) {

        printf("Connected to %s with sample rate of %dkHz.\nCtrl-C to exit.\n", inputDeviceCommentName, currentSampleRate / HERTZ_IN_KILOHERTZ);

    } else {
        
        success = false;

    }

    if (success == false) return ERROR_RESPONSE;

    /* Wait for device to start */

    bool threadStarted = false;

    double startTime = ma_timer_get_time_in_seconds(&timer);

    while (threadStarted == false) {

        double currentTime = ma_timer_get_time_in_seconds(&timer);

        if (currentTime - startTime > DEVICE_STOP_START_TIMEOUT) {

            puts("[ERROR] Timed out waiting for device to start.");

            break;

        }

        pthread_mutex_lock(&stopStartMutex);

        threadStarted = started;

        pthread_mutex_unlock(&stopStartMutex);

    }

    if (threadStarted == false) return ERROR_RESPONSE;

    /* Check if heterodyne is possible */

    if (heterodyneEnabled) {

        if (heterodyneFrequency < MINIMUM_HETERODYNE_FREQUENCY || heterodyneFrequency > currentSampleRate / 2) {

            puts("[ERROR] Could not set requested heterodyne frequency.");

            return ERROR_RESPONSE;

        } else {

            Heterodyne_initialise(currentSampleRate, heterodyneFrequency);
    
        }

    }

    /* Start autosave, monitor and heterodyne */

    if (autosaveDuration > 0) addAutosaveEvent(AS_START);

    if (monitorEnabled || heterodyneEnabled) pthread_create(&startPlaybackThread, NULL, startPlaybackThreadBody, NULL);

    /* Register signal handler */

    Signal_registerHandler();

    /* Main loop */

    while (success) {

        /* Wait for next iteration */

        usleep(MICROSECONDS_IN_SECOND / CALLBACKS_PER_SECOND);

        /* Get the current audio time */

        pthread_mutex_lock(&audioBufferMutex);

        int64_t audioCount = autosaveSampleCount - autosaveStartSampleCount;

        int64_t audioTime = autosaveStartTime;

        pthread_mutex_unlock(&audioBufferMutex);

        audioTime += ROUNDED_DIV(audioCount * MILLISECONDS_IN_SECOND, currentSampleRate);

        /* Check the audio time against the current time */

        int64_t currentTime = Time_getMillisecondUTC();

        bool timeMismatch = ABS(currentTime - audioTime) > TIME_MISMATCH_LIMIT;
      
        /* Check for device change or old AudioMoth found */

        bool deviceChanged = false;

        static bool oldAudioMothFound = false;

        bool showOldAudioMothFoundWarning = false;

        pthread_mutex_lock(&backgroundMutex);

        if (backgroundDeviceCheckTime - timeDeviceStarted > DEVICE_CHANGE_INTERVAL) {

            if (backgroundDeviceCheckFoundAudioMoth == true && usingAudioMoth == false) deviceChanged = true;
        
            if (backgroundDeviceCheckFoundAudioMoth == false && usingAudioMoth == true) deviceChanged = true;

            if (backgroundDeviceCheckFoundOldAudioMoth && oldAudioMothFound == false) showOldAudioMothFoundWarning = true;

            oldAudioMothFound = backgroundDeviceCheckFoundOldAudioMoth;

        }

        pthread_mutex_unlock(&backgroundMutex);

        /* Show warning if old AudioMoth found */

        if (showOldAudioMothFoundWarning) puts("[WARNING] The AudioMoth USB Microphone firmware running on your AudioMoth device is out of date.");

        /* Continue if the device has not changed */

        if (deviceChanged == false && timeMismatch == false) continue;

        if (timeMismatch) puts("[WARNING] Restarting due to time mismatch.");

        /* Reset the stopped flag */

        pthread_mutex_lock(&stopStartMutex);

        stopped = false;

        pthread_mutex_unlock(&stopStartMutex);

        /* Stop the device */

        pthread_mutex_lock(&backgroundDeviceCheckMutex);

        stopMicrophone();

        pthread_mutex_unlock(&backgroundDeviceCheckMutex);

        /* Wait for device to stop */

        bool threadStopped = false;

        double startTime = ma_timer_get_time_in_seconds(&timer);

        while (threadStopped == false) {

            double currentTime = ma_timer_get_time_in_seconds(&timer);

            if (currentTime - startTime > DEVICE_STOP_START_TIMEOUT) {

                if (IS_WINDOWS == false) puts("[ERROR] Timed out waiting for device to stop.");

                break;

            }

            pthread_mutex_lock(&stopStartMutex);

            threadStopped = stopped;

            pthread_mutex_unlock(&stopStartMutex);

        }

        /* Reset the start flag */

        pthread_mutex_lock(&stopStartMutex);

        started = false;

        pthread_mutex_unlock(&stopStartMutex);

        /* Start the device  */

        pthread_mutex_lock(&backgroundDeviceCheckMutex);

        device_check_t device_check = checkForAudioMoth(&deviceCheckContext, true);

        usingAudioMoth = device_check.audioMothFound;
        
        bool startedMicrophone = startMicrophone(&deviceCheckContext, usingAudioMoth);

        pthread_mutex_unlock(&backgroundDeviceCheckMutex);

        if (startedMicrophone) {

            printf("Connected to %s with sample rate of %dkHz.\n", inputDeviceCommentName, currentSampleRate / HERTZ_IN_KILOHERTZ);

        }

        timeDeviceStarted = ma_timer_get_time_in_seconds(&timer);

        /* Wait for device to start */

        bool threadStarted = false;

        startTime = ma_timer_get_time_in_seconds(&timer);

        while (threadStarted == false) {

            double currentTime = ma_timer_get_time_in_seconds(&timer);

            if (currentTime - startTime > DEVICE_STOP_START_TIMEOUT) {

                puts("[ERROR] Timed out waiting for device to start.");

                break;

            }

            pthread_mutex_lock(&stopStartMutex);

            threadStarted = started;

            pthread_mutex_unlock(&stopStartMutex);

        }

        /* Add autosave event */

        if (threadStarted && autosaveDuration > 0) addAutosaveEvent(AS_RESTART);

    }

    if (success == false && IS_WINDOWS == false) puts("");

    /* Exit if not using autosave */

    if (autosaveDuration == 0) return OKAY_RESPONSE;

    /* Set shutdown flag */

    pthread_mutex_lock(&autosaveMutex);

    autosaveShutdownCompleted = false;

    pthread_mutex_unlock(&autosaveMutex);

    /* Send shutdown message */

    addAutosaveEvent(AS_SHUTDOWN);

    /* Wait for shutdown to complete */

    bool shutdownCompleted = false;

    startTime = ma_timer_get_time_in_seconds(&timer);

    while (shutdownCompleted == false) {

        double currentTime = ma_timer_get_time_in_seconds(&timer);

        if (currentTime - startTime > DEVICE_SHUTDOWN_TIMEOUT) {

            break;

        }

        pthread_mutex_lock(&autosaveMutex);

        shutdownCompleted = autosaveShutdownCompleted;

        pthread_mutex_unlock(&autosaveMutex);

    }

    /* Exit */

    return OKAY_RESPONSE;

}
