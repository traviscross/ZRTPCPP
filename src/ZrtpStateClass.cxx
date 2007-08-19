/*
  Copyright (C) 2006, 2007 Werner Dittmann

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Boston, MA 02111.
*/

/**
 * @author Werner Dittmann <Werner.Dittmann@t-online.de>
 */

#include <iostream>
#include <cstdlib>
#include <ctype.h>

#include <libzrtpcpp/ZRtp.h>
#include <libzrtpcpp/ZrtpStateClass.h>

using namespace std;

state_t states[numberOfStates] = {
    {Initial,      &ZrtpStateClass::evInitial },
    {Detect,       &ZrtpStateClass::evDetect },
    {AckDetected,  &ZrtpStateClass::evAckDetected },
    {WaitCommit,   &ZrtpStateClass::evWaitCommit },
    {CommitSent,   &ZrtpStateClass::evCommitSent },
    {WaitDHPart2,  &ZrtpStateClass::evWaitDHPart2 },
    {WaitConfirm1, &ZrtpStateClass::evWaitConfirm1 },
    {WaitConfirm2, &ZrtpStateClass::evWaitConfirm2 },
    {WaitConfAck,  &ZrtpStateClass::evWaitConfAck },
    {WaitClearAck, &ZrtpStateClass::evWaitClearAck },
    {SecureState,  &ZrtpStateClass::evSecureState },
    {WaitErrorAck, &ZrtpStateClass::evWaitErrorAck }
};

static char* sendErrorText = "Cannot send data via RTP - connection or peer down?";
static char* sendErrorTextSrtp = "Cannot send data via SRTP - connection or peer down?";
static char* timerError = "Cannot start a timer - internal resources exhausted?";
static char* resendError = "Too much retries during ZRTP negotiation - connection or peer down?";
static char* internalProtocolError = "Internal protocol error occured!";
static char* zrtpClosed = "No more security for this session";
static char* goClearReceived = "Received a GoClear message - no security processing!";

ZrtpStateClass::ZrtpStateClass(ZRtp *p) {
    parent = p;
    engine = new ZrtpStates(states, numberOfStates, Initial);

    // Set up timers according to ZRTP spec
    T1.start = 50;
    T1.maxResend = 20;
    T1.capping = 200;

    T2.start = 150;
    T2.maxResend = 10;
    T2.capping = 600;
}

ZrtpStateClass::~ZrtpStateClass(void) {
    if (engine != NULL) {
	delete engine;
    }
}

int32_t ZrtpStateClass::processEvent(Event_t *ev) {

    /*
     * Ignore any events except Initial and GoClear if we are not really 
     * started yet.
     * We may receive additional "GoClear" packets if the "clearAck" packet
     * was lost. Thus just resend a ClearAck in this case.
     * TODO: goclear
     */
    if (inState(Initial) && !(ev->type == ZrtpInitial /* || ev->type == ZrtpGoClear */)) {
	return (Done);
    }
    event = ev;
    return engine->processEvent(*this);
}


int32_t ZrtpStateClass::evInitial(void) {
    DEBUGOUT((cout << "Checking for match in Initial.\n"));

    if (event->type == ZrtpInitial) {
	ZrtpPacketHello *hello = parent->prepareHello();

	// remember packet for easy resend in case timer triggers
	sentPacket = static_cast<ZrtpPacketBase *>(hello);

	if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(hello)) || (startTimer(&T1) <= 0)) {
	    nextState(Initial);
	    parent->sendInfo(Error, sendErrorText);
	    return(Fail);
	}
	nextState(Detect);
    }
    return (Done);
}

/*
 * We are in state "detect" and got an event.
 * When entering this state transition function then:
 * - Assume Initiator mode, mode may change later on peer reaction
 * - sentPacket contains Hello
 * - Hello timer T1 is active
 */
int32_t ZrtpStateClass::evDetect(void) {

    DEBUGOUT((cout << "Checking for match in Detect.\n"));

    char *msg, first, last;
    uint8_t *pkt;
    uint32_t errorCode = 0;

    /*
     * First check the general event type, then discrimnate
     * the real event.
     */
    if (event->type == ZrtpPacket) {
	pkt = event->data.packet;
	msg = (char *)pkt + 4;

	first = tolower(*msg);
	last = tolower(*(msg+7));

	/*
	 * Commit:
	 * - go the responder path
	 * - send our DHPart1
	 * - switch to state WaitDHPart2 and wait for peer's DHPart2
	 * - don't start timer, we are responder
	 */
	if (first == 'c') {
	    ZrtpPacketCommit *cpkt = new ZrtpPacketCommit(pkt);
	    // Only if ZRTP CRC is ok then accept packet and process event
	    cancelTimer();	// stop Hello timer processing, don't delete a Hello packet
	    sentPacket = NULL;

	    // TODO: check for multistream request
	    ZrtpPacketDHPart* dhPart1 = parent->prepareDHPart1(cpkt, &errorCode);
	    delete cpkt;

	    // Something went wrong during processing of the commit packet
	    if (dhPart1 == NULL) {
                sendErrorPacket(errorCode);
		return (Done);
	    }
	    nextState(WaitDHPart2);

	    if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(dhPart1))) {
		delete dhPart1;
		nextState(Initial);
		parent->sendInfo(Error, sendErrorText);
		return(Fail);
	    }
	    // remember packet for easy resend in new state
	    sentPacket = static_cast<ZrtpPacketBase *>(dhPart1);
	    return (Done);
	}
	/*
	 * HelloAck:
	 * - stop resending Hello,
	 * - switch to state AckDetected, wait for peer's Hello (F3)
	 */
	if (first == 'h' && last =='k') {
	    cancelTimer();	// stop Hello timer processing, don't delete my Hello packet
	    sentPacket = NULL;
	    nextState(AckDetected);
	    return (Done);
	}
	/*
	 * Hello:
	 * - stop Hello timer
	 * - prepare and send my Commit,
	 * - switch state to CommitSent, start Commit timer, assume Initiator
	 */
	if (first == 'h' && last ==' ') {
	    ZrtpPacketHello *hpkt = new ZrtpPacketHello(pkt);
	    cancelTimer();
            parent->sendPacketZRTP(sentPacket);    // just resend Hello in case peer missed ours
	    sentPacket = NULL;

	    ZrtpPacketCommit* commit = parent->prepareCommit(hpkt,  &errorCode);
	    delete hpkt;

	    // Something went wrong during processing of the Hello packet
	    if (commit == NULL) {
                sendErrorPacket(errorCode);
                return (Done);
	    }
	    nextState(CommitSent);

            // remember packet for easy resend in case timer triggers
            // Timer trigger received in new state CommitSend
            sentPacket = static_cast<ZrtpPacketBase *>(commit);
            if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(commit)) || (startTimer(&T2) <= 0)) {
		delete sentPacket;
		sentPacket = NULL;
		nextState(Initial);
                parent->zrtpNegotiationFailed(Error, sendErrorText);
		return(Fail);
	    }
	    return (Done);
	}
        return (Done);      // unknown packet for this state - Just ignore it
    }
    // Timer event triggered - this is Timer T1 to resend Hello
    else if (event->type == Timer) {
	if (sentPacket != NULL) {
            if ((nextTimer(&T1)) <= 0 || !parent->sendPacketZRTP(sentPacket)) {
                parent->zrtpNotSuppOther();
                sentPacket = NULL;
                // Stay in state Detect to be prepared get an hello from
                // other peer any time later
                nextState(Detect);
                return (Fail);
            }
        }
        return (Done);
    }
    else {      // unknown Event type for this state
        parent->sendInfo(Error, internalProtocolError);
	sentPacket = NULL;
	nextState(Initial);
        return (Fail);
    }
}

/*
 * When entering this transition function
 * - sentPacket contains NULL, Hello timer stopped
 */
int32_t ZrtpStateClass::evAckDetected(void) {

    DEBUGOUT((cout << "Checking for match in AckDetected.\n"));

    char *msg, first, last;
    uint8_t *pkt;
    uint32_t errorCode = 0;

    if (event->type == ZrtpPacket) {
	pkt = event->data.packet;
	msg = (char *)pkt + 4;

	first = tolower(*msg);
	last = tolower(*(msg+7));
	/*
	 * Hello:
	 * - Acknowledge my peers Hello, sending HelloACK (F4)
	 * - switch to state WaitCommit, wait for peer's Commit
         * - we are going to be in the Responder role
	 */
	if (first == 'h') {
// NOTE: may be useful if protocol state engine changes (preshared mode)
//            ZrtpPacketHello *hpkt = new ZrtpPacketHello(pkt);
            ZrtpPacketHelloAck *helloAck = parent->prepareHelloAck();
//            delete hpkt;

	    nextState(WaitCommit);

	    if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(helloAck))) {
		nextState(Initial);
		sentPacket = NULL;
		parent->sendInfo(Error, sendErrorText);
		return(Fail);
	    }
	    // remember packet for easy resend
	    sentPacket = static_cast<ZrtpPacketBase *>(helloAck);
	    return (Done);
	}
       /*
        * Commit:
        * - act as Responder
        * - send our DHPart1
        * - switch to state WaitDHPart2, wait for peer's DHPart2
        * - don't start timer, we are responder
        */
        if (first == 'c') {

            ZrtpPacketCommit *cpkt = new ZrtpPacketCommit(pkt);
            ZrtpPacketDHPart* dhPart1 = parent->prepareDHPart1(cpkt, &errorCode);
            delete cpkt;

            // Something went wrong during processing of the Commit packet
            if (dhPart1 == NULL) {
                sendErrorPacket(errorCode);
                return (Done);
            }
            nextState(WaitDHPart2);

            if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(dhPart1))) {
                delete dhPart1;
                sentPacket = NULL;
                nextState(Initial);
                parent->sendInfo(Error, sendErrorText);
                return(Fail);
            }
	    // remember packet for easy resend in new state
            sentPacket = static_cast<ZrtpPacketBase *>(dhPart1);
            return (Done);
        }
        return (Done);      // unknown packet for this state - Just ignore it
    }
    else {
        parent->sendInfo(Error, internalProtocolError);
	sentPacket = NULL;
	nextState(Initial);
        return (Fail);
    }
}

/*
 * When entering this transition function
 * - Responder mode
 * - sentPacket contains a HelloAck packet
 */
int32_t ZrtpStateClass::evWaitCommit(void) {

    DEBUGOUT((cout << "Checking for match in WaitCommit.\n"));

    char *msg, first, last;
    uint8_t *pkt;
    uint32_t errorCode = 0;

    if (event->type == ZrtpPacket) {
	pkt = event->data.packet;
	msg = (char *)pkt + 4;

	first = tolower(*msg);
	last = tolower(*(msg+7));

	/*
	 * Hello:
	 * - resend HelloAck
	 * - stay in WaitCommit
	 */
	if (first == 'h') {
            ZrtpPacketHello *hpkt = new ZrtpPacketHello(pkt);
	    delete hpkt;
	    if (!parent->sendPacketZRTP(sentPacket)) {
		nextState(Initial);
		sentPacket = NULL;    // don't delete HelloAck, it's preconfigured
		parent->sendInfo(Error, sendErrorText);
		return(Fail);
	    }
	    return (Done);
	}
	/*
	 * Commit:
	 * - prepare DH1Part packet
	 * - send it to peer
	 * - switch state to WaitDHPart2
	 * - don't start timer, we are responder
	 */
	if (first == 'c') {
	    ZrtpPacketCommit *cpkt = new ZrtpPacketCommit(pkt);
            sentPacket = NULL;
	    ZrtpPacketDHPart* dhPart1 = parent->prepareDHPart1(cpkt, &errorCode);
	    delete cpkt;

            // Something went wrong during processing of the Commit packet
            if (dhPart1 == NULL) {
                sendErrorPacket(errorCode);
                return (Done);
            }
	    nextState(WaitDHPart2);

	    if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(dhPart1))){
		delete dhPart1;
		nextState(Initial);
		parent->sendInfo(Error, sendErrorText);
		return(Fail);
	    }
	    sentPacket = static_cast<ZrtpPacketBase *>(dhPart1);
	    return (Done);
	}
        return (Done);      // unknown packet for this state - Just ignore it
    }
    else {
        parent->sendInfo(Error, internalProtocolError);
        sentPacket = NULL;   // Don't delet sent packet - it's a fixed helloack
	nextState(Initial);
        return (Fail);
    }
}

/*
 * When entering this transition function
 * - assume Initiator mode, may change if we reveice a Commit here
 * - sentPacket contains Commit packet, Hello timer stopped, Commit timer active
 */

int32_t ZrtpStateClass::evCommitSent(void) {

    DEBUGOUT((cout << "Checking for match in CommitSend.\n"));

    char *msg, first, last;
    uint8_t *pkt;
    uint32_t errorCode = 0;

    if (event->type == ZrtpPacket) {
	pkt = event->data.packet;
	msg = (char *)pkt + 4;

	first = tolower(*msg);
	last = tolower(*(msg+7));

        /*
	 * HelloAck:
	 * - late "helloAck", maybe  due to network latency, just ignore it
	 * - no switch in state, leave timer as it is
	 */
	if (first == 'h' && last =='k') {
	    return (Done);
	}

	/*
	 * Commit:
         * We have a "Commit" clash. Resolve it.
         *
	 * - switch off resending Commit
	 * - compare my hvi with peer's hvi
	 * - if my hvi is greater
	 *   - we are Initiator, stay in state, wait for peer's DHPart1 packet
	 * - else
         *   - we are Responder, stop timer
	 *   - prepare and send DH1Packt,
	 *   - switch to state WaitDHPart2, implies Responder path
	 */
	if (first == 'c') {
	    ZrtpPacketCommit *zpCo = new ZrtpPacketCommit(pkt);
            sentPacket = NULL;
	    cancelTimer();         // this cancels the Commit timer T2

	    // if our hvi is less then peer's hvi - we are Responder and need
            // to send DHPart1 packet. Peer (as Initiator) will retrigger if
            // necessary
	    if (parent->compareHvi(zpCo) < 0) {
		delete sentPacket;     // delete our sent Commit packet
		sentPacket = NULL;
		ZrtpPacketDHPart* dhPart1 = parent->prepareDHPart1(zpCo, &errorCode);

                // Something went wrong during processing of the Commit packet
                if (dhPart1 == NULL) {
                    sendErrorPacket(errorCode);
                    return (Done);
                }
                nextState(WaitDHPart2);

		if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(dhPart1))){
		    delete dhPart1;
		    delete zpCo;
		    nextState(Initial);
		    parent->sendInfo(Error, sendErrorText);
		    return(Fail);
		}
		sentPacket = static_cast<ZrtpPacketBase *>(dhPart1);
	    }
	    // Stay in state, we are Initiator, wait for DHPart1 packet from peer.
            // Resend Commit after timeout until we get a DHPart1
	    else {
		if (startTimer(&T2) <= 0) { // restart the Commit timer, gives peer more time to react
                    delete sentPacket;
                    sentPacket = NULL;
                    nextState(Initial);
                    parent->sendInfo(Error, timerError);
                    return(Fail);
		}
	    }
	    delete zpCo;
	    return (Done);
	}

	/*
	 * DHPart1:
	 * - switch off resending Commit
	 * - Prepare and send DHPart2
	 * - switch to WaitConfirm1
	 * - start timer to resend DHPart2 if necessary, we are Initiator
	 * - switch on SRTP
	 */
	if (first == 'd') {
	    ZrtpPacketDHPart* dpkt = new ZrtpPacketDHPart(pkt);
	    cancelTimer();
	    delete sentPacket;     // deletes the Commit packet
	    sentPacket = NULL;

	    ZrtpPacketDHPart* dhPart2 = parent->prepareDHPart2(dpkt, &errorCode);
	    delete dpkt;

            // Something went wrong during processing of the DHPart1 packet
            if (dhPart2 == NULL) {
                sendErrorPacket(errorCode);
                return (Done);
            }
	    nextState(WaitConfirm1);

            sentPacket = static_cast<ZrtpPacketBase *>(dhPart2);
            if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(dhPart2)) || (startTimer(&T2) <= 0) ){
		delete sentPacket;
                sentPacket = NULL;
		nextState(Initial);
		parent->sendInfo(Error, sendErrorText);
		return(Fail);
	    }
	    return (Done);
	}
        return (Done);      // unknown packet for this state - Just ignore it
    }
    // Timer event triggered, resend the Commit packet
    else if (event->type == Timer) {
	if (sentPacket != NULL) {
            if ((nextTimer(&T2) <= 0) || !parent->sendPacketZRTP(sentPacket)) {
                parent->sendInfo(Error, resendError);
                delete sentPacket;
                sentPacket = NULL;
                nextState(Initial);
                return (Fail);
            }
        }
        return (Done);
    }
    else {      // unknown Event type for this state
        parent->sendInfo(Error, internalProtocolError);
        delete sentPacket;
	sentPacket = NULL;
	nextState(Initial);
        return (Fail);
    }
}

/*
 * When entering this transition function
 * - sentPacket contains DHPart1 packet, no timer active
 */
int32_t ZrtpStateClass::evWaitDHPart2(void) {

    DEBUGOUT((cout << "Checking for match in DHPart2.\n"));

    char *msg, first, last;
    uint8_t *pkt;
    uint32_t errorCode = 0;

    if (event->type == ZrtpPacket) {
	pkt = event->data.packet;
	msg = (char *)pkt + 4;

	first = tolower(*msg);
	last = tolower(*(msg+7));

	/*
	 * Commit:
	 * - resend DHPart1
	 * - stay in state
	 */
	if (first == 'c') {
	    // ZrtpPacketCommit *zpCo = new ZrtpPacketCommit(pkt); TODO
	    if (!parent->sendPacketZRTP(sentPacket)) {
		delete sentPacket;
		sentPacket = NULL;
		nextState(Initial);
		parent->sendInfo(Error, sendErrorText);
		return(Fail);
	    }
	    return (Done);
	}
	/*
	 * DHPart2:
	 * - prepare Confirm1 packet
	 * - switch to WaitConfirm2
	 * - switch on SRTP
	 * - No timer, we are responder
	 */
	if (first == 'd') {
	    ZrtpPacketDHPart* dpkt = new ZrtpPacketDHPart(pkt);
	    delete sentPacket;     // delete DHPart1 packet
	    sentPacket = NULL;

	    ZrtpPacketConfirm* confirm = parent->prepareConfirm1(dpkt, &errorCode);
	    delete dpkt;

            if (confirm == NULL) {
                sendErrorPacket(errorCode);
                return (Done);
            }
	    nextState(WaitConfirm2);

	    if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(confirm))){
		delete confirm;
		nextState(Initial);
                parent->sendInfo(Error, sendErrorTextSrtp);
		return(Fail);
	    }
	    sentPacket = static_cast<ZrtpPacketBase *>(confirm);
	    return (Done);
	}
        return (Done);      // unknown packet for this state - Just ignore it
    }
    else {      // unknown Event type for this state
        parent->sendInfo(Error, internalProtocolError);
        delete sentPacket;
        sentPacket = NULL;
        nextState(Initial);
        return (Fail);
    }
}

/*
 * When entering this transition function
 * - Initiator mode
 * - sentPacket contains DHPart2 packet, DHPart2 timer active
 */
int32_t ZrtpStateClass::evWaitConfirm1(void) {

    DEBUGOUT((cout << "Checking for match in WaitConfirm1.\n"));

    char *msg, first, last;
    uint8_t *pkt;
    uint32_t errorCode = 0;

    if (event->type == ZrtpPacket) {
	pkt = event->data.packet;
	msg = (char *)pkt + 4;

	first = tolower(*msg);
	last = tolower(*(msg+7));

	/*
	 * Confirm1:
	 * - Switch off resending DHPart2
	 * - prepare a Confirm2 packet
	 * - switch to state WaitConfAck
	 * - set timer to monitor Confirm2 packet, we are initiator
	 */
	if (first == 'c' && last == '1') {
	    ZrtpPacketConfirm* cpkt = new ZrtpPacketConfirm(pkt);
	    cancelTimer();
	    delete sentPacket;             // delete DHPart2 packet
	    sentPacket = NULL;

	    ZrtpPacketConfirm* confirm = parent->prepareConfirm2(cpkt, &errorCode);
	    delete cpkt;

            // Something went wrong during processing of the Confirm1 packet
            if (confirm == NULL) {
                sendErrorPacket(errorCode);
                return (Done);
            }
	    nextState(WaitConfAck);

            sentPacket = static_cast<ZrtpPacketBase *>(confirm);
            if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(confirm)) || (startTimer(&T2) <= 0)){
                delete sentPacket;
                sentPacket = NULL;
		nextState(Initial);
		parent->sendInfo(Error, sendErrorText);
		return(Fail);
	    }
	    return (Done);
	}
        return (Done);      // unknown packet for this state - Just ignore it
    }
    else if (event->type == Timer) {
	if (sentPacket != NULL) {
            if ((nextTimer(&T2) <= 0) || !parent->sendPacketZRTP(sentPacket)) {
                parent->sendInfo(Error, resendError);
                delete sentPacket;
                sentPacket = NULL;
                nextState(Initial);
                return (Fail);
            }
        }
        return (Done);
    }
    else {      // unknown Event type for this state
        parent->sendInfo(Error, internalProtocolError);
        delete sentPacket;
        sentPacket = NULL;
	nextState(Initial);
        return (Fail);
    }
}

/*
 * When entering this transition function
 * - Responder mode
 * - sentPacket contains Confirm1 packet, no timer active
 * - Security switched on
 */
int32_t ZrtpStateClass::evWaitConfirm2(void) {

    DEBUGOUT((cout << "Checking for match in WaitConfirm2.\n"));

    char *msg, first, last;
    uint8_t *pkt;
    uint32_t errorCode = 0;

    if (event->type == ZrtpPacket) {
	pkt = event->data.packet;
	msg = (char *)pkt + 4;

	first = tolower(*msg);
	last = tolower(*(msg+7));

	/*
	 * DHPart2:
	 * - resend Confirm1 packet via SRTP
	 * - stay in state
	 */
	if (first == 'd') {
	    if (!parent->sendPacketZRTP(sentPacket)) {
		delete sentPacket;
		sentPacket = NULL;
		nextState(Initial);
		parent->sendInfo(Error, sendErrorText);
		return(Fail);
	    }
	    return (Done);
	}
	/*
	 * Confirm2:
	 * - prepare ConfAck
	 * - switch on security
	 * - switch to SecureState
	 */
	if (first == 'c' && last == '2') {

	    // send ConfAck via SRTP
	    ZrtpPacketConfirm* cpkt = new ZrtpPacketConfirm(pkt);
	    delete sentPacket;             // delete Confirm1 packet
	    sentPacket = NULL;

	    ZrtpPacketConf2Ack* confack = parent->prepareConf2Ack(cpkt, &errorCode);
	    delete cpkt;

            // Something went wrong during processing of the confirm2 packet
            if (confack == NULL) {
                sendErrorPacket(errorCode);
                return (Done);
            }
	    nextState(SecureState);

	    if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(confack))){
		sentPacket = NULL;        // don't delete conf2Ack packet, it's preconfigured
		nextState(Initial);
		parent->sendInfo(Error, sendErrorTextSrtp);
		return(Fail);
	    }
	    sentPacket = static_cast<ZrtpPacketBase *>(confack);
	    parent->sendInfo(Info, "Switching to secure state");
            // TODO: error handling here 
            parent->srtpSecretsReady(ForSender);
            parent->srtpSecretsReady(ForReceiver);

	    return (Done);
	}
        return (Done);      // unknown packet for this state - Just ignore it
    }
    else {      // unknown Event type for this state
        parent->sendInfo(Error, internalProtocolError);
        delete sentPacket;
        sentPacket = NULL;
        nextState(Initial);
        return (Fail);
    }
}

/*
 * When entering this transition function
 * - Initiator mode
 * - sentPacket contains Confirm2 packet, Confirm2 timer active
 * - sender and receiver security switched on
 */
int32_t ZrtpStateClass::evWaitConfAck(void) {

    DEBUGOUT((cout << "Checking for match in WaitConfAck.\n"));

    char *msg, first, last;
    uint8_t *pkt;

    if (event->type == ZrtpPacket) {
	pkt = event->data.packet;
	msg = (char *)pkt + 4;

	first = tolower(*msg);
	last = tolower(*(msg+7));

	/*
	 * ConfAck:
	 * - Switch off resending Confirm2
	 * - switch to SecureState
	 */
	if (first == 'c') {
            cancelTimer();
            delete sentPacket;
            sentPacket = NULL;
	    parent->sendInfo(Info, "Switching to secure state");
	    nextState(SecureState);
      // TODO: error handling here 
            parent->srtpSecretsReady(ForSender);
            parent->srtpSecretsReady(ForReceiver);
	    return (Done);
	}
        return (Done);      // unknown packet for this state - Just ignore it
    }
    else if (event->type == Timer) {
	if (sentPacket != NULL) {
            if ((nextTimer(&T2) <= 0) || !parent->sendPacketZRTP(sentPacket)) {
                parent->sendInfo(Error, resendError);
                delete sentPacket;
                sentPacket = NULL;
                nextState(Initial);
                parent->srtpSecretsOff(ForSender);
                parent->srtpSecretsOff(ForReceiver);
                return (Fail);
            }
        }
        return (Done);
    }
    else {      // another Event type for this state
        parent->sendInfo(Error, internalProtocolError);
        delete sentPacket;
	sentPacket = NULL;
	nextState(Initial);
        return (Fail);
    }
}

/*
 * When entering this transition function
 * - sentPacket contains GoClear packet, GoClear timer active
 * The GoClear packet should no be deleted because it is a static packet.
 */

int32_t ZrtpStateClass::evWaitClearAck(void) {
    DEBUGOUT((cout << "Checking for match in ClearAck.\n"));

    char *msg, first, last;
    uint8_t *pkt;

    if (event->type == ZrtpPacket) {
	pkt = event->data.packet;
	msg = (char *)pkt + 4;

	first = tolower(*msg);
	last = tolower(*(msg+7));

	/*
	 * ClearAck:
	 * - stop resending GoClear,
	 * - switch to state AckDetected, wait for peer's Hello
	 */
	if (first == 'c' && last =='k') {
	    cancelTimer();	// stop send go clear timer processing, don't delete this GoClear packet
	    sentPacket = NULL;
	    nextState(Initial);
	}
        return (Done);      // unknown packet for this state - Just ignore it
    }
    // Timer event triggered - this is Timer T2 to resend GoClear w/o HMAC
    else if (event->type == Timer) {
	if (sentPacket != NULL) {
            if ((nextTimer(&T2)) <= 0 || !parent->sendPacketZRTP(sentPacket)) {
                parent->sendInfo(Error, resendError);
                sentPacket = NULL;
                nextState(Initial);
                return (Fail);
            }
        }
        return (Done);
    }
    else {      // unknown Event type for this state
        parent->sendInfo(Error, internalProtocolError);
	sentPacket = NULL;
	nextState(Initial);
        return (Fail);
    }
}


/*
 * When entering this transition function
 * - sentPacket contains Error packet, Error timer active ?? TODO
 * The Error packet should no be deleted because it is a static packet.
 */

int32_t ZrtpStateClass::evWaitErrorAck(void) {
    DEBUGOUT((cout << "Checking for match in ErrorAck.\n"));

    char *msg, first, last;
    uint8_t *pkt;

    if (event->type == ZrtpPacket) {
	pkt = event->data.packet;
	msg = (char *)pkt + 4;

	first = tolower(*msg);
	last = tolower(*(msg+7));

	/*
	 * ClearAck:
	 * - stop resending GoClear,
	 * - switch to state AckDetected, wait for peer's Hello
	 */
	if (first == 'e' && last =='k') {
	    cancelTimer();	// stop send Error timer processing, don't delete Error packet
	    sentPacket = NULL;
	    nextState(Initial);
	}
        return (Done);
    }
    // Timer event triggered - this is Timer T2 to resend Error.
    else if (event->type == Timer) {
	if (sentPacket != NULL) {
            if ((nextTimer(&T2)) <= 0 || !parent->sendPacketZRTP(sentPacket)) {
                parent->sendInfo(Error, resendError);
                sentPacket = NULL;
                nextState(Initial);
                return (Fail);
            }
        }
        return (Done);
    }
    else {      // unknown Event type for this state
        parent->sendInfo(Error, internalProtocolError);
	sentPacket = NULL;
	nextState(Initial);
        return (Fail);
    }
}

int32_t ZrtpStateClass::evSecureState(void) {

    DEBUGOUT((cout << "Checking for match in SecureState.\n"));

    char *msg, first, last;
    uint8_t *pkt;

    if (event->type == ZrtpPacket) {
	pkt = event->data.packet;
	msg = (char *)pkt + 4;

	first = tolower(*msg);
	last = tolower(*(msg+7));

	/*
	 * Confirm2:
	 * - resend ConfAck packet
	 * - stay in state
	 * - don't delete this confAck packet - it's preconfigured
	 */
	if (first == 'c' && last == '2') {
	    if (sentPacket != NULL && !parent->sendPacketZRTP(sentPacket)) {
		sentPacket = NULL;        // don't delete confAck packet, it's preconfigured
		nextState(Initial);
                parent->srtpSecretsOff(ForSender);
                parent->srtpSecretsOff(ForReceiver);
		parent->sendInfo(Error, sendErrorTextSrtp);
		return(Fail);
	    }
	    return (Done);
	}
        /*
         * GoClear received, handle it. TODO fix go clear handling
         */
        if (first == 'g' && last == 'r') {
            ZrtpPacketGoClear* gpkt = new ZrtpPacketGoClear(pkt);
            ZrtpPacketClearAck* clearAck = parent->prepareClearAck(gpkt);
            delete gpkt;

            if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(clearAck))) {
                return(Done);
            }
        // TODO Timeout to resend clear ack until user user confirmation
        }
    }
    else {
	sentPacket = NULL;
        parent->srtpSecretsOff(ForSender);
        parent->srtpSecretsOff(ForReceiver);
	nextState(Initial);
	parent->sendInfo(Info, zrtpClosed);
    }
    return (Done);
}

int32_t ZrtpStateClass::startTimer(zrtpTimer_t *t) {

    t->time = t->start;
    t->counter = 0;
    return parent->activateTimer(t->time);
}

int32_t ZrtpStateClass::nextTimer(zrtpTimer_t *t) {

    t->time += t->time;
    t->time = (t->time > t->capping)? t->capping : t->time;
    t->counter++;
    if (t->counter > t->maxResend) {
	return -1;
    }
    return parent->activateTimer(t->time);
}

int32_t ZrtpStateClass::sendErrorPacket(uint32_t errorCode) {
    ZrtpPacketError* err = parent->prepareError(errorCode);
    if (!parent->sendPacketZRTP(static_cast<ZrtpPacketBase *>(err)) || (startTimer(&T2) <= 0)) {
        nextState(Initial);
        parent->sendInfo(Error, sendErrorText);
        return (Fail);
    }
    sentPacket =  static_cast<ZrtpPacketBase *>(err);
    nextState(WaitErrorAck);
    return (Done);
}

/** EMACS **
 * Local variables:
 * mode: c++
 * c-default-style: ellemtel
 * c-basic-offset: 4
 * End:
 */
