/*
  Copyright (C) 2006 Werner Dittmann

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

/*
 * Authors: Werner Dittmann <Werner.Dittmann@t-online.de>
 */

#include <libzrtpcpp/ZrtpPacketHello.h>


ZrtpPacketHello::ZrtpPacketHello() {
    DEBUGOUT((fprintf(stdout, "Creating Hello packet without data\n")));

    // The NumSupported* data is in ZrtpTextData.h 
    nHash = NumSupportedHashes;
    nCipher = NumSupportedSymCiphers;
    nPubkey = NumSupportedPubKeys;
    nSas = NumSupportedSASTypes;
    nAuth = NumSupportedAuthLenghts;

    int32_t length = sizeof(HelloPacket_t) + CRC_SIZE;
    length += nHash * ZRTP_WORD_SIZE;
    length += nCipher * ZRTP_WORD_SIZE;
    length += nPubkey * ZRTP_WORD_SIZE;
    length += nSas * ZRTP_WORD_SIZE;
    length += nAuth * ZRTP_WORD_SIZE;

    // Don't change order of this sequence
    oHash = sizeof(Hello_t);
    oCipher = oHash + (nHash * ZRTP_WORD_SIZE);
    oAuth = oCipher + (nCipher * ZRTP_WORD_SIZE);
    oPubkey = oAuth + (nAuth * ZRTP_WORD_SIZE);
    oSas = oPubkey + (nPubkey * ZRTP_WORD_SIZE);

    allocated = malloc(length);

    if (allocated == NULL) {
    }
    memset(allocated, 0, length);

    zrtpHeader = (zrtpPacketHeader_t *)&((HelloPacket_t *)allocated)->hdr;	// the standard header
    helloHeader = (Hello_t *)&((HelloPacket_t *)allocated)->hello;

    setZrtpId();

    // minus 1 for CRC size 
    setLength((length / ZRTP_WORD_SIZE) - 1);
    setMessageType((uint8_t*)HelloMsg);

    setVersion((uint8_t*)zrtpVersion);

    uint32_t lenField = nHash << 12;
    for (int32_t i = 0; i < nHash; i++) {
        setHashType(i, (int8_t*)supportedHashes[i]);
    }

    lenField |= nCipher << 16;
    for (int32_t i = 0; i < nCipher; i++) {
        setCipherType(i,  (int8_t*)supportedCipher[i]);
    }

    lenField |= nAuth << 20;
    for (int32_t i = 0; i < nAuth; i++) {
        setAuthLen(i,  (int8_t*)supportedAuthLen[i]);
    }

    lenField |= nPubkey << 24;
    for (int32_t i = 0; i < nPubkey; i++) {
        setPubKeyType(i,  (int8_t*)supportedPubKey[i]);
    }

    lenField |= nSas << 28;
    for (int32_t i = 0; i < nSas; i++) {
        setSasType(i,  (int8_t*)supportedSASType[i]);
    }
    helloHeader->flagLength = htonl(lenField);
}

ZrtpPacketHello::ZrtpPacketHello(uint8_t *data) {
    DEBUGOUT((fprintf(stdout, "Creating Hello packet from data\n")));

    allocated = NULL;
    zrtpHeader = (zrtpPacketHeader_t *)&((HelloPacket_t *)data)->hdr;	// the standard header
    helloHeader = (Hello_t *)&((HelloPacket_t *)data)->hello;

    uint32_t temp = ntohl(helloHeader->flagLength);

    nHash = (temp & (0xf << 12)) >> 12;
    nCipher = (temp & (0xf << 16)) >> 16;
    nAuth = (temp & (0xf << 20)) >> 20;
    nPubkey = (temp & (0xf << 24)) >> 24;
    nSas = (temp & (0xf << 28)) >> 28;

    oHash = sizeof(Hello_t);
    oCipher = oHash + (nHash * ZRTP_WORD_SIZE);
    oAuth = oCipher + (nCipher * ZRTP_WORD_SIZE);
    oPubkey = oAuth + (nAuth * ZRTP_WORD_SIZE);
    oSas = oPubkey + (nPubkey * ZRTP_WORD_SIZE);
}

ZrtpPacketHello::~ZrtpPacketHello() {
    DEBUGOUT((fprintf(stdout, "Deleting Hello packet: alloc: %x\n", allocated)));
    if (allocated != NULL) {
        free(allocated);
    }
}
