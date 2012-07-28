/*
 * 
 */

#include <string>
#include <stdio.h>

#include <libzrtpcpp/ZIDCache.h>
#include <libzrtpcpp/ZRtp.h>

#include <CtZrtpStream.h>
#include <CtZrtpCallback.h>
#include <CtZrtpSession.h>

#include <common/Thread.h>

static CMutexClass sessionLock;

CtZrtpSession::CtZrtpSession() : mitmMode(false), signSas(false), enableParanoidMode(false), isReady(false),
    zrtpEnabled(true), sdesEnabled(true) {

    clientIdString = clientId;
    streams[AudioStream] = NULL;
    streams[VideoStream] = NULL;
}

int CtZrtpSession::init(bool audio, bool video, const char *zidFilename, ZrtpConfigure* config )
{
    int32_t ret = 1;

    synchEnter();

    ZrtpConfigure* configOwn = NULL;
    if (config == NULL) {
        config = configOwn = new ZrtpConfigure();
        setupConfiguration(config);
        config->setTrustedMitM(true);
    }
    config->setParanoidMode(enableParanoidMode);

    ZIDCache* zf = getZidCacheInstance();
    if (!zf->isOpen()) {
        std::string fname;
        if (zidFilename == NULL) {
            char *home = getenv("HOME");
            std::string baseDir = (home != NULL) ? (std::string(home) + std::string("/."))
                                                    : std::string(".");
            fname = baseDir + std::string("GNUZRTP.zid");
            zidFilename = fname.c_str();
        }
        if (zf->open((char *)zidFilename) < 0) {
            ret = -1;
        }
    }
    if (ret > 0) {
        const uint8_t* ownZid = zf->getZid();
        CtZrtpStream *stream;

        // Create CTZrtpStream object only once, they are availbe for the whole
        // lifetime of the session.
        if (audio) {
            if (streams[AudioStream] == NULL)
                streams[AudioStream] = new CtZrtpStream();
            stream = streams[AudioStream];
            stream->zrtpEngine = new ZRtp((uint8_t*)ownZid, stream, clientIdString, config, mitmMode, signSas);
            stream->type = Master;
            stream->index = AudioStream;
            stream->session = this;
        }
        if (video) {
            if (streams[VideoStream] == NULL)
                streams[VideoStream] = new CtZrtpStream();
            stream = streams[VideoStream];
            stream->zrtpEngine = new ZRtp((uint8_t*)ownZid, stream, clientIdString, config);
            stream->type = Slave;
            stream->index = VideoStream;
            stream->session = this;
        }
    }
    if (configOwn != NULL) {
        delete configOwn;
    }
    synchLeave();
    isReady = true;
    return ret;
}

CtZrtpSession::~CtZrtpSession() {

    delete streams[AudioStream];
    delete streams[VideoStream];
}

void CtZrtpSession::setupConfiguration(ZrtpConfigure *conf) {

    // Set TIVI_CONF to a real name that is TRUE if the Tivi client is compiled/built.
#ifdef _WITHOUT_TIVI_ENV
#define GET_CFG_I(RET,_KEY)
#else
void *findGlobalCfgKey(char *key, int iKeyLen, int &iSize, char **opt, int *type);
#define GET_CFG_I(RET,_KEY) {int *p=(int*)findGlobalCfgKey((char*)_KEY,sizeof(_KEY)-1,iSZ,&opt,&type);if(p && iSZ==4)RET=*p;else RET=-1;}
#endif

    int iSZ;
    char *opt;
    int type;
    int b32sas,iDisableDH2K, iDisableAES256, iPreferDH2K;
    int iDisableECDH256, iDisableECDH384, iEnableSHA384;
    int iHas2k=0;

    GET_CFG_I(b32sas,"iDisable256SAS");
    GET_CFG_I(iDisableAES256,"iDisableAES256");
    GET_CFG_I(iDisableDH2K,"iDisableDH2K");
    GET_CFG_I(iPreferDH2K,"iPreferDH2K");

    GET_CFG_I(iDisableECDH256,"iDisableECDH256");
    GET_CFG_I(iDisableECDH384,"iDisableECDH384");
    GET_CFG_I(iEnableSHA384,"iEnableSHA384");

    conf->clear();

    if (iPreferDH2K && !iDisableDH2K) {
        iHas2k=1;
        conf->addAlgo(PubKeyAlgorithm, zrtpPubKeys.getByName("DH2k"));  // If enabled always first
    }

    if(iDisableECDH256 == 0)
        conf->addAlgo(PubKeyAlgorithm, zrtpPubKeys.getByName("EC25"));  // If enabled always before DH3k

    conf->addAlgo(PubKeyAlgorithm, zrtpPubKeys.getByName("DH3k"));      // If enabled it should appear here

    if(iDisableECDH384 == 0)
        conf->addAlgo(PubKeyAlgorithm, zrtpPubKeys.getByName("EC38"));  // If enabled, slowest, thus on last position

    conf->addAlgo(PubKeyAlgorithm, zrtpPubKeys.getByName("Mult"));      // Tivi supports Multi-stream mode

// DEBUG    conf->printConfiguredAlgos(PubKeyAlgorithm);

    if(iEnableSHA384 == 1 || iDisableECDH384 == 0){
        conf->addAlgo(HashAlgorithm, zrtpHashes.getByName("S384"));
    }
    conf->addAlgo(HashAlgorithm, zrtpHashes.getByName("S256"));

    if (iDisableAES256 == 0 ) {
        conf->addAlgo(CipherAlgorithm, zrtpSymCiphers.getByName("2FS3"));
        conf->addAlgo(CipherAlgorithm, zrtpSymCiphers.getByName("AES3"));
    }
    conf->addAlgo(CipherAlgorithm, zrtpSymCiphers.getByName("2FS1"));
    conf->addAlgo(CipherAlgorithm, zrtpSymCiphers.getByName("AES1"));

// DEBUG    conf->printConfiguredAlgos(CipherAlgorithm);

    // Curreently only B32 supported
    if (b32sas == 1 /* || T_getSometing1(NULL,"zrtp_sas_b32") */){
        conf->addAlgo(SasType, zrtpSasTypes.getByName("B32 "));
    }
    else{
//        conf->addAlgo(SasType, zrtpSasTypes.getByName("B256"));
        conf->addAlgo(SasType, zrtpSasTypes.getByName("B32 "));
    }

    conf->addAlgo(AuthLength, zrtpAuthLengths.getByName("SK32"));
    conf->addAlgo(AuthLength, zrtpAuthLengths.getByName("SK64"));
    conf->addAlgo(AuthLength, zrtpAuthLengths.getByName("HS32"));
    conf->addAlgo(AuthLength, zrtpAuthLengths.getByName("HS80"));
}

void CtZrtpSession::setUserCallback(CtZrtpCb* ucb, streamName streamNm) {
    if (!(streamNm >= 0 && streamNm <= AllStreams && streams[streamNm] != NULL))
        return;

    if (streamNm == AllStreams) {
        for (int sn = 0; sn < AllStreams; sn++)
            streams[sn]->setUserCallback(ucb);
    }
    else
        streams[streamNm]->setUserCallback(ucb);
}

void CtZrtpSession::setSendCallback(CtZrtpSendCb* scb, streamName streamNm) {
    if (!(streamNm >= 0 && streamNm <= AllStreams && streams[streamNm] != NULL))
        return;

    if (streamNm == AllStreams) {
        for (int sn = 0; sn < AllStreams; sn++)
            streams[sn]->setSendCallback(scb);
    }
    else
        streams[streamNm]->setSendCallback(scb);

}

void CtZrtpSession::masterStreamSecure(CtZrtpStream *masterStream) {
    // Here we know that the AudioStream is the master and VideoStream the slave.
    // Otherwise we need to loop and find the Master stream and the Slave streams.

    multiStreamParameter = masterStream->zrtpEngine->getMultiStrParams();
    CtZrtpStream *strm = streams[VideoStream];
    if (strm->enableZrtp) {
        strm->zrtpEngine->setMultiStrParams(multiStreamParameter);
        strm->zrtpEngine->startZrtpEngine();
        strm->started = true;
        strm->tiviState = eLookingPeer;
        if (strm->zrtpUserCallback != 0)
            strm->zrtpUserCallback->onNewZrtpStatus(this, NULL, strm->index);

    }
}

int CtZrtpSession::startIfNotStarted(unsigned int uiSSRC, int streamNm) {
    if (!(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return 0;

    if ((streamNm == VideoStream && !isSecure(AudioStream)) || streams[streamNm]->started)
        return 0;

    start(uiSSRC, streamNm == VideoStream ? CtZrtpSession::VideoStream : CtZrtpSession::AudioStream);
    return 0;
}

void CtZrtpSession::start(unsigned int uiSSRC, CtZrtpSession::streamName streamNm) {
    if (!zrtpEnabled || !(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return;

    CtZrtpStream *stream = streams[streamNm];

    stream->ownSSRC = uiSSRC;
    stream->enableZrtp = true;
    if (stream->type == Master) {
        stream->zrtpEngine->startZrtpEngine();
        stream->started = true;
        stream->tiviState = eLookingPeer;
        if (stream->zrtpUserCallback != 0)
            stream->zrtpUserCallback->onNewZrtpStatus(this, NULL, stream->index);
        return;
    }
    // Process a Slave stream.
    if (!multiStreamParameter.empty()) {        // Multi-stream parameters available
        stream->zrtpEngine->setMultiStrParams(multiStreamParameter);
        stream->zrtpEngine->startZrtpEngine();
        stream->started = true;
        stream->tiviState = eLookingPeer;
        if (stream->zrtpUserCallback != 0)
            stream->zrtpUserCallback->onNewZrtpStatus(this, NULL, stream->index);
    }
}

void CtZrtpSession::stop(streamName streamNm) {
    if (!(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return;

    streams[streamNm]->isStopped = true;
}

void CtZrtpSession::release() {
    release(AudioStream);
    release(VideoStream);
}

void CtZrtpSession::release(streamName streamNm) {
    if (!(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return;

    CtZrtpStream *stream = streams[streamNm];
    stream->stopStream();                      // stop and reset stream
}

void CtZrtpSession::setLastPeerNameVerify(const char *name, int iIsMitm) {
    CtZrtpStream *stream = streams[AudioStream];

    if (!isReady || !stream || stream->isStopped)
        return;

    uint8_t peerZid[IDENTIFIER_LEN];
    std::string nm(name);
    stream->zrtpEngine->getPeerZid(peerZid);
    getZidCacheInstance()->putPeerName(peerZid, nm);
    setVerify(1);
}

int CtZrtpSession::isSecure(streamName streamNm) {
    if (!(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return 0;

    CtZrtpStream *stream = streams[streamNm];
    return stream->isSecure();
}

bool CtZrtpSession::processOutoingRtp(uint8_t *buffer, size_t length, size_t *newLength, streamName streamNm) {
    if (!isReady || !(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return false;

    CtZrtpStream *stream = streams[streamNm];
    if (stream->isStopped)
        return false;

    return stream->processOutgoingRtp(buffer, length, newLength);
}

int32_t CtZrtpSession::processIncomingRtp(uint8_t *buffer, size_t length, size_t *newLength, streamName streamNm) {
    if (!isReady || !(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return fail;

    CtZrtpStream *stream = streams[streamNm];
    if (stream->isStopped)
        return fail;

    return stream->processIncomingRtp(buffer, length, newLength);
}

bool CtZrtpSession::isStarted(streamName streamNm) {
    if (!isReady || !(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return false;

    return streams[streamNm]->isStarted();
}

bool CtZrtpSession::isEnabled(streamName streamNm) {
    if (!isReady || !(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return false;

    CtZrtpStream *stream = streams[streamNm];
    if (stream->isStopped)
        return false;

    return stream->isEnabled();
}

CtZrtpSession::tiviStatus CtZrtpSession::getCurrentState(streamName streamNm) {
    if (!isReady || !(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return eWrongStream;

    CtZrtpStream *stream = streams[streamNm];
    if (stream->isStopped)
        return eWrongStream;

    return stream->getCurrentState();
}

CtZrtpSession::tiviStatus CtZrtpSession::getPreviousState(streamName streamNm) {
    if (!isReady || !(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return eWrongStream;

    CtZrtpStream *stream = streams[streamNm];
    if (stream->isStopped)
        return eWrongStream;

    return stream->getPreviousState();
}

bool CtZrtpSession::isZrtpEnabled() {
    return zrtpEnabled;
}

bool CtZrtpSession::isSdesEnabled() {
    return sdesEnabled;
}

void CtZrtpSession::setZrtpEnabled(bool yesNo) {
    zrtpEnabled = yesNo;
}

void CtZrtpSession::setSdesEnabled(bool yesNo) {
    sdesEnabled = yesNo;
}

int CtZrtpSession::getSignalingHelloHash(char *helloHash, streamName streamNm) {
    if (!isReady || !(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return 0;

    CtZrtpStream *stream = streams[streamNm];
    if (stream->isStopped)
        return 0;

    return stream->getSignalingHelloHash(helloHash);
}

void CtZrtpSession::setSignalingHelloHash(const char *helloHash, streamName streamNm) {
    if (!isReady || !(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return;

    CtZrtpStream *stream = streams[streamNm];
    if (stream->isStopped)
        return;

    stream->setSignalingHelloHash(helloHash);
}

void CtZrtpSession::setVerify(int iVerified) {
    CtZrtpStream *stream = streams[AudioStream];

    if (!isReady || !stream || stream->isStopped)
        return;

    if (iVerified) {
        stream->zrtpEngine->SASVerified();
        stream->sasVerified = true;
    }
    else {
        stream->zrtpEngine->resetSASVerified();
        stream->sasVerified = false;
    }
}

int CtZrtpSession::getInfo(const char *key, char *buffer, int maxLen, streamName streamNm) {
    if (!isReady || !(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return fail;

    CtZrtpStream *stream = streams[streamNm];
    return stream->getInfo(key, buffer, maxLen);
}

int CtZrtpSession::enrollAccepted(char *p) {
    if (!isReady || !(streams[AudioStream] != NULL))
        return fail;

    CtZrtpStream *stream = streams[AudioStream];
    int ret = stream->enrollAccepted(p);
    setVerify(true);
    return ret;
}

void CtZrtpSession::setClientId(std::string id) {
    clientIdString = id;
}

bool CtZrtpSession::createSdes(char *cryptoString, size_t *maxLen, streamName streamNm, const sdesSuites suite) {

    if (!isReady || !sdesEnabled || !(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return fail;

    CtZrtpStream *stream = streams[streamNm];
    return stream->createSdes(cryptoString, maxLen, static_cast<ZrtpSdesStream::sdesSuites>(suite));
}

bool CtZrtpSession::parseSdes(char *recvCryptoStr, size_t recvLength, char *sendCryptoStr,
                              size_t *sendLength, bool sipInvite, streamName streamNm) {

    if (!isReady || !sdesEnabled || !(streamNm >= 0 && streamNm < AllStreams && streams[streamNm] != NULL))
        return fail;

    CtZrtpStream *stream = streams[streamNm];
    return stream->parseSdes(recvCryptoStr, recvLength, sendCryptoStr, sendLength, sipInvite);
}

void CtZrtpSession::synchEnter() {
    sessionLock.Lock();
}

void CtZrtpSession::synchLeave() {
    sessionLock.Unlock();
}

