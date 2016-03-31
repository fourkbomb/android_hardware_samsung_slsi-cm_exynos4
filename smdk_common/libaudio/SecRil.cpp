#define LOG_TAG "SEC-RILC"
#include <utils/Log.h>
#include <SecRil.h>
#include <errno.h>
#include <termios.h>

namespace android {

#define SYSCHECK(c) do {if((c)<0){ALOGE("SEC-RIL, system-error: '%s' (code: %d)", strerror(errno), errno);\
                        return -1;}\
                    }while(0)

const char* AUDIO_AT_CHNL = "gsm0710mux.channel18";//cyit,20130808,sync modem bin update for call recorder

/* 
* Purpose:  Compares two strings.
*                strstr might not work because WebBox sends garbage before the first OK
*                when it's not needed anymore
* Input:      haystack - string to check
*                length - length of string to check
*                needle - reference string to compare to. must be null-terminated.
* Return:    1 if comparison was success, else 0
*/
static int memstr(
    const char *haystack,
    int length,
    const char *needle)
{
    int i;
    int j = 0;
    if (needle[0] == '\0')
        return 1;
    for (i = 0; i < length; i++)
        if (needle[j] == haystack[i]) {
            j++;
            if (needle[j] == '\0') // Entire needle was found
                return 1;
        } else
            j = 0;
    return 0;
}

SecRil* SecRil::mInstance=0;

SecRil::SecRil() : mAudioATFd(-1) {}

int SecRil::openAudioATChannel() {
    char s_muxChannelDevice[PROPERTY_VALUE_MAX] = {0};

    Mutex::Autolock _l(mLock);

    while (mAudioATFd < 0) {
        property_get(AUDIO_AT_CHNL, s_muxChannelDevice, "");
        if (!strcmp(s_muxChannelDevice, "")) {
            ALOGE("[%] get %s failed", __FUNCTION__, AUDIO_AT_CHNL);//cyit,20130808,sync modem bin update for call recorder
            sleep(3);
            continue;
        }

        ALOGE("[%s]=====s_muxChannelDevice = %s", __FUNCTION__, s_muxChannelDevice);
        mAudioATFd = open(s_muxChannelDevice, O_RDWR | O_NONBLOCK);
        if (mAudioATFd < 0) {
            ALOGE("[%s] opening AT interface %s failed!", __FUNCTION__, AUDIO_AT_CHNL);//cyit,20130808,sync modem bin update for call recorder
            sleep(3);
            /*never returns */
        }
    }
    ALOGD("opening AT interface %s OK, mAudioATFd = %d!", AUDIO_AT_CHNL, mAudioATFd);//cyit,20130808,sync modem bin update for call recorder
    configAudioATChannel(mAudioATFd);

    return 0;
}

int SecRil::reOpenAudioATChannel() {
    char s_muxChannelDevice[PROPERTY_VALUE_MAX] = {0};
    mAudioATFd = -1;

    property_get(AUDIO_AT_CHNL, s_muxChannelDevice, "");
    if (!strcmp(s_muxChannelDevice, "")) {
        ALOGE("[%s] get %s failed", __FUNCTION__, AUDIO_AT_CHNL);//cyit,20130808,sync modem bin update for call recorder
        return -1;
    }

    ALOGD("[%s] ====s_muxChannelDevice = %s", __FUNCTION__, s_muxChannelDevice);
    mAudioATFd = open(s_muxChannelDevice, O_RDWR | O_NONBLOCK);
    if (mAudioATFd < 0) {
        ALOGE("[%s] opening AT interface %s failed!", __FUNCTION__, AUDIO_AT_CHNL);//cyit,20130808,sync modem bin update for call recorder
        return -1; // Return -1 if we've tried to open device during 9s.
    }
    ALOGD("reopening AT interface %s OK, mAudioATFd = %d!", AUDIO_AT_CHNL, mAudioATFd);//cyit,20130808,sync modem bin update for call recorder
    configAudioATChannel(mAudioATFd);

    return 0;
}

int SecRil::closeAudioATChannel() {
    if (mAudioATFd < 0) {
        ALOGE("AT interface %s has been closed!", AUDIO_AT_CHNL);//cyit,20130808,sync modem bin update for call recorder
        return 0;
    }

    Mutex::Autolock _l(mLock);
    close(mAudioATFd);
    mAudioATFd = -1;
    return 0;
}

void SecRil::configAudioATChannel(int fd) {
    struct termios  ios;
    tcgetattr(fd, &ios );

    ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
    ios.c_oflag &= ~OCRNL;
    ios.c_iflag &= ~ICRNL;
    ios.c_cflag &= ~PARENB;
    ios.c_cflag &= ~CSTOPB;
    ios.c_cflag &= ~CSIZE;
    ios.c_cflag |= CS8;
    ios.c_iflag &= ~(INLCR | ICRNL | IGNCR);
    ios.c_oflag &= ~(ONLCR | OCRNL);
    ios.c_lflag &= ~ (ICANON | ECHO | ECHOE | ISIG);
    ios.c_iflag &= ~ (IXON | IXOFF | IXANY); //open soft flow control
    tcsetattr(fd, TCSANOW, &ios );
}

int SecRil::chat(char* data, int size, int atType, char** out) {
    Mutex::Autolock _l(mLock);
    int ret = -1;

    if (mAudioATFd < 0) {
        if (reOpenAudioATChannel() < 0) {
            ALOGE("[%s] mAudioATFd reopen failed return -1 !!!", __FUNCTION__);
            return -1;
        }
    }

    do {
        ret = ::write(mAudioATFd, data, size);
        if (ret <= 0) {
            if (reOpenAudioATChannel() < 0)
                return -1;
        }
    } while (ret <= 0);

    switch (atType) {
    case AUDIO_AT_CLVL:
    case AUDIO_AT_SPATH:
    case AUDIO_AT_SVMOP:
        ret = readFromChannel(atType);
        break;
    case AUDIO_AT_CALIBRATION:
        ret = readPhonetoolAT(out);
        break;
    }

    return ret;
}

//SetCallVolume:the argument "type" not use
int SecRil::setCallVolume(SoundType type, int vol_level) {
    int size = sizeof("AT+CLVL=z\r");
    char setVolAT[size];

    int volume = (int)(vol_level / 10);

    if (volume >= 9) {
        sprintf(setVolAT, "AT+CLVL=%d\r", 9);
        ALOGE("setVolAT = %s", setVolAT);
    } else {
        sprintf(setVolAT, "AT+CLVL=%d\r", volume);
        ALOGE("setVolAT = %s", setVolAT);
    }

    return chat(setVolAT, size, AUDIO_AT_CLVL, NULL);

}

int SecRil::setCallAudioPath(AudioPath path) {
    int arg1 = 0;
    int arg2 = 0;
    int size = sizeof("AT^SPATH=z,z\r");
    char setPathAT[size];

    switch (path) {
    case SOUND_AUDIO_PATH_HANDSET:
        arg1 = arg2 = 0;
        break;
    case SOUND_AUDIO_PATH_HEADSET:
        arg1 = arg2 = 1;
        break;
    case SOUND_AUDIO_PATH_SPEAKER:
        arg1 = arg2 = 4;
        break;
    case SOUND_AUDIO_PATH_BLUETOOTH:
        arg1 = arg2 = 3;
        break;
    case SOUND_AUDIO_PATH_BLUETOOTH_NO_NR:
        arg1 = 3;
        arg2 = 3; //Modified by CYIT
        break;
    case SOUND_AUDIO_PATH_HEADPHONE:
        arg1 = 0;
        arg2 = 1;
        break;
    default:
        arg1 = arg2 = 0;
        break;
    }
    sprintf(setPathAT, "AT^SPATH=%d,%d\r", arg1, arg2);
    ALOGE("svmopAT = %s", setPathAT);

    return chat(setPathAT, size, AUDIO_AT_SPATH, NULL);

}

int SecRil::setCallVmOperation(int order) {

    int size = sizeof("AT^SVMOP=z\r");
    char setVoiceMemoOPAT[size];

    sprintf(setVoiceMemoOPAT, "AT^SVMOP=%d\r", order);
    ALOGE("svmopAT = %s", setVoiceMemoOPAT);

    return chat(setVoiceMemoOPAT, size, AUDIO_AT_SVMOP, NULL);
}

// Add by CYIT 20130818------------------------start------------
int SecRil::writeAndReadPhonetoolAT(void* data, int size, char** out) {
    int ret = -1;

//    int i = 0;
//    for (; i < size; i++)
//        ALOGE("atData === %x", ((char*)data)[i]);

    ret = chat((char* )data, size, AUDIO_AT_CALIBRATION, out);

    return ret;
}

int SecRil::readPhonetoolAT(char** out) {
    int len;
    int sel;
    static char atResp[1024] = {0};

    if (mAudioATFd < 0) {
        ALOGE("[readPhonetoolResp] mAudioATFd < 0!!!");
        return -1;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(mAudioATFd, &rfds);
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    do {
        SYSCHECK(sel = select(mAudioATFd + 1, &rfds, NULL, NULL, &timeout));
        if (FD_ISSET(mAudioATFd, &rfds)) {
            len = read(mAudioATFd, atResp, sizeof(atResp));
            SYSCHECK(len);
            errno = 0;
            *out = atResp;
            ALOGE("readPhonetoolAT resp len = %d", len);
            return len;
        }
    } while (sel);
    return -1;
}
// Add by CYIT 20130818-------------------------end-------------

int SecRil::readFromChannel(int atType) {
    int len;
    int sel;
    char atResp[1024] = {0};

    if (mAudioATFd < 0) {
        ALOGE("[setCallVmOperation] mAudioATFd < 0!!!");
        return -1;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(mAudioATFd, &rfds);
    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    do {
        SYSCHECK(sel = select(mAudioATFd + 1, &rfds, NULL, NULL, &timeout));
        if (FD_ISSET(mAudioATFd, &rfds)) {
            memset(atResp, 0, sizeof(atResp));
            len = read(mAudioATFd, atResp, sizeof(atResp));
            SYSCHECK(len);
            errno = 0;
            if (memstr((char *)atResp, len, "OK")) {
                ALOGE("Audio AT %d Received OK", atType);
                return 0;
            }
            if (memstr((char *)atResp, len, "ERROR")) {
                ALOGE("Audio AT %d Received ERROR!!!", atType);
                return -1;
            }
        }
    } while (sel);
    return -1;
}
}
