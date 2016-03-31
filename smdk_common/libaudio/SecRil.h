/*
 * Copyright (C) 2010 The Android Open Source Project
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


#ifndef __SEC_RIL_H__
#define __SEC_RIL_H__

#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <utils/RefBase.h>
#include <utils/threads.h>
#include <cutils/properties.h>
#include <secril-client.h>

namespace android {

class SecRil : public RefBase {

#define AUDIO_AT_CLVL 0    // change volume in call mode
#define AUDIO_AT_SPATH 1   // change voice path in call mode
#define AUDIO_AT_SVMOP 2   // set call recorder operation code
#define AUDIO_AT_CALIBRATION 3   // audio calibration AT

public:
    static SecRil* instance() {
        if (!mInstance) {
            mInstance = new SecRil();
        }
        return mInstance;
    }
    int openAudioATChannel();
    int closeAudioATChannel();

    //Set in-call volume.
    int setCallVolume(SoundType type, int vol_level);

    //Set external sound device path for noise reduction.
    int setCallAudioPath(AudioPath path);

    //send start,recording and stop order when recording in calling.
    //Return is 0 or -1.
    int setCallVmOperation(int order);
    int readFromChannel(int atType);

    int writeAndReadPhonetoolAT(void* data, int size, char** out);
    int readPhonetoolAT(char** out);
    int chat(char* data, int size, int atType, char** out); // Modified by CYIT 20130927
    void configAudioATChannel(int fd);
    int reOpenAudioATChannel();

private:
    SecRil();
    static SecRil* mInstance;
    int mAudioATFd;
    Mutex mLock;
    SecRil(const SecRil &);
    SecRil &operator=(const SecRil &);
};
}

#endif // __SEC_RIL_H__

// end of file

