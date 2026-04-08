#!/bin/bash
set -e

if [[ -z "${PRUDYNT_CROSS:-}" ]]; then
	for candidate in \
		"/opt/mipsel-thingino-linux-musl_sdk-buildroot/bin/mipsel-linux-" \
		"/home/paul/output/wyze_cam3_t31x_gc2053_rtl8189ftv/host/bin/mipsel-linux-"
	do
		if [[ -x "${candidate}gcc" ]]; then
			PRUDYNT_CROSS="$candidate"
			break
		fi
	done

	if [[ -z "${PRUDYNT_CROSS:-}" ]] && command -v mipsel-linux-gcc >/dev/null 2>&1; then
		PRUDYNT_CROSS="mipsel-linux-"
	fi
fi

: "${PRUDYNT_CROSS:?Unable to locate a usable mipsel toolchain prefix}"
TOP=$(pwd)

patch_live555_transport() {
	python3 - <<'PY'
from pathlib import Path
import re

p = Path("RTSPServer.cpp")
text = p.read_text()
stock = """    if ((streamingMode == RTP_TCP && rtpChannelId == 0xFF) ||
\t(streamingMode != RTP_TCP && fOurClientConnection->fClientOutputSocket != fOurClientConnection->fClientInputSocket)) {
      // An anomolous situation, caused by a buggy client.  Either:
      //     1/ TCP streaming was requested, but with no "interleaving=" fields.  (QuickTime Player sometimes does this.), or
      //     2/ TCP streaming was not requested, but we're doing RTSP-over-HTTP tunneling (which implies TCP streaming).
      // In either case, we assume TCP streaming, and set the RTP and RTCP channel ids to proper values:
      streamingMode = RTP_TCP;
      rtpChannelId = fTCPStreamIdCount; rtcpChannelId = fTCPStreamIdCount+1;
    }
"""
preferred = """    Boolean const clientRequestedTCP =
\tstrstr(fFullRequestStr, "Transport: RTP/AVP/TCP") != NULL;
    Boolean const clientRequestedUDP =
\t!clientRequestedTCP && strstr(fFullRequestStr, "client_port=") != NULL;
    if (clientRequestedUDP) {
      streamingMode = RTP_UDP;
      rtpChannelId = rtcpChannelId = 0xFF;
    } else if (streamingMode == RTP_TCP &&
\t(rtpChannelId == 0xFF ||
\t fOurClientConnection->fClientOutputSocket != fOurClientConnection->fClientInputSocket)) {
      // If TCP streaming was requested without valid interleaving parameters,
      // repair the RTP/RTCP channel ids.  Otherwise honor the client's chosen
      // UDP/TCP transport instead of promoting unicast UDP to interleaved TCP.
      streamingMode = RTP_TCP;
      rtpChannelId = fTCPStreamIdCount; rtcpChannelId = fTCPStreamIdCount+1;
    }
"""
patched = """    if (streamingMode != RTP_TCP) {
      // Force unicast RTP to stay interleaved over RTSP/TCP on this camera.
      // VLC and other clients are significantly more stable with a single TCP transport path.
      streamingMode = RTP_TCP;
      rtpChannelId = fTCPStreamIdCount; rtcpChannelId = fTCPStreamIdCount+1;
    } else if (rtpChannelId == 0xFF ||
\t       fOurClientConnection->fClientOutputSocket != fOurClientConnection->fClientInputSocket) {
      // An anomalous situation, caused by a buggy client. Either TCP was requested
      // without interleaving fields, or RTSP-over-HTTP tunneling implicitly requires TCP.
      streamingMode = RTP_TCP;
      rtpChannelId = fTCPStreamIdCount; rtcpChannelId = fTCPStreamIdCount+1;
    }
"""
if patched in text:
    text = text.replace(patched, preferred, 1)
elif stock in text:
    text = text.replace(stock, preferred, 1)

text = re.sub(
    r"""    parseTransportHeader\(fFullRequestStr, streamingMode, streamingModeString,\n"""
    r"""(?:.*\n)*?"""
    r"""    Port clientRTPPort\(clientRTPPortNum\);\n"""
    r"""    Port clientRTCPPort\(clientRTCPPortNum\);\n""",
    """    parseTransportHeader(fFullRequestStr, streamingMode, streamingModeString,
\t\t\t clientsDestinationAddressStr, clientsDestinationTTL,
\t\t\t clientRTPPortNum, clientRTCPPortNum,
\t\t\t rtpChannelId, rtcpChannelId);
    Boolean const clientRequestedTCP =
\tstrstr(fFullRequestStr, "Transport: RTP/AVP/TCP") != NULL;
    Boolean const clientRequestedUDP =
\t!clientRequestedTCP && strstr(fFullRequestStr, "client_port=") != NULL;
    if (clientRequestedUDP) {
      streamingMode = RTP_UDP;
      rtpChannelId = rtcpChannelId = 0xFF;
    } else if (streamingMode == RTP_TCP &&
\t(rtpChannelId == 0xFF ||
\t fOurClientConnection->fClientOutputSocket != fOurClientConnection->fClientInputSocket)) {
      // If TCP streaming was requested without valid interleaving parameters,
      // repair the RTP/RTCP channel ids. Otherwise honor the client's explicit
      // UDP/TCP choice instead of promoting unicast UDP to interleaved TCP.
      rtpChannelId = fTCPStreamIdCount; rtcpChannelId = fTCPStreamIdCount+1;
    }
    if (streamingMode == RTP_TCP) fTCPStreamIdCount += 2;
    
    Port clientRTPPort(clientRTPPortNum);
    Port clientRTCPPort(clientRTCPPortNum);
""",
    text,
    count=1,
    flags=re.S,
)

text = re.sub(
    r"""    // Then, get server parameters from the 'subsession':\n"""
    r"""(?:.*\n)*?"""
    r"""    char timeoutParameterString\[100\];\n""",
    """    // Then, get server parameters from the 'subsession':
    int tcpSocketNum = -1;
    if (streamingMode == RTP_TCP) {
      // Note that we'll be streaming over the RTSP TCP connection:
      tcpSocketNum = fOurClientConnection->fClientOutputSocket;
      fStreamStates[trackNum].tcpSocketNum = tcpSocketNum;
      fOurRTSPServer.noteTCPStreamingOnSocket(tcpSocketNum, this, trackNum);
    } else {
      fStreamStates[trackNum].tcpSocketNum = -1;
    }
    struct sockaddr_storage destinationAddress = nullAddress();
        // used to indicate that the address is 'unassigned'
    u_int8_t destinationTTL = 255;
#ifdef RTSP_ALLOW_CLIENT_DESTINATION_SETTING
    if (clientsDestinationAddressStr != NULL) {
      // Use the client-provided "destination" address.
      // Note: This potentially allows the server to be used in denial-of-service
      // attacks, so don't enable this code unless you're sure that clients are
      // trusted.
      NetAddressList destAddresses(clientsDestinationAddressStr);
      if (destAddresses.numAddresses() > 0) {
\tcopyAddress(destinationAddress, destAddresses.firstAddress());
      }
    }
    // Also use the client-provided TTL.
    destinationTTL = clientsDestinationTTL;
#endif
    delete[] clientsDestinationAddressStr;
    Port serverRTPPort(0);
    Port serverRTCPPort(0);
    
    // Make sure that we transmit on the same interface that's used by the client
    // (in case we're a multi-homed server):
    struct sockaddr_storage sourceAddr; SOCKLEN_T namelen = sizeof sourceAddr;
    getsockname(fOurClientConnection->fClientInputSocket, (struct sockaddr*)&sourceAddr, &namelen);
    
    subsession->getStreamParameters(fOurSessionId, fOurClientConnection->fClientAddr,
\t\t\t\t    clientRTPPort, clientRTCPPort,
\t\t\t\t    tcpSocketNum, rtpChannelId, rtcpChannelId,
                                    &fOurClientConnection->fTLS,
\t\t\t\t    destinationAddress, destinationTTL, fIsMulticast,
\t\t\t\t    serverRTPPort, serverRTCPPort,
\t\t\t\t    fStreamStates[trackNum].streamToken);
    
    AddressString destAddrStr(destinationAddress);
    AddressString sourceAddrStr(sourceAddr);
    StreamingMode responseStreamingMode = streamingMode;
    if (clientRequestedUDP) {
      responseStreamingMode = RTP_UDP;
    } else if (clientRequestedTCP) {
      responseStreamingMode = RTP_TCP;
    }
    char timeoutParameterString[100];
""",
    text,
    count=1,
    flags=re.S,
)
text = text.replace("switch (streamingMode) {", "switch (responseStreamingMode) {", 2)

p.write_text(text)
PY
}

patch_live555_rtp_timestamps() {
	python3 - <<'PY'
from pathlib import Path

p = Path("RTPSink.cpp")
text = p.read_text()
old = """u_int32_t RTPSink::presetNextTimestamp() {
  struct timeval timeNow;
  gettimeofday(&timeNow, NULL);

  u_int32_t tsNow = convertToRTPTimestamp(timeNow);
  if (!groupsockBeingUsed().hasMultipleDestinations()) {
    // Don't adjust the timestamp stream if we already have another destination ongoing
    fTimestampBase = tsNow;
    fNextTimestampHasBeenPreset = True;
  }

  return tsNow;
}
"""
new = """u_int32_t RTPSink::presetNextTimestamp() {
  // PRUDYNT-T PATCH: Our camera sources use a local monotonic presentation timeline
  // instead of gettimeofday()-based timestamps. For a fresh sink, preset the next
  // packet so that RTP-Info and the first emitted RTP packet begin at the same value.
  //
  // If the sink has already sent packets, keep advertising the live timestamp for
  // mid-stream joins. However, ignore stale warmup state that may have been populated
  // before PLAY (for example during SDP setup), because that causes RTP-Info to start
  // at large in-stream values instead of a clean session-local origin.
  if (fPacketCount > 0) {
    return fCurrentTimestamp;
  }

  if (fCurrentTimestamp != 0
      || fMostRecentPresentationTime.tv_sec != 0
      || fMostRecentPresentationTime.tv_usec != 0) {
    fCurrentTimestamp = 0;
    resetPresentationTimes();
  }

  u_int32_t initialOffset = 0;
  if (strcmp(sdpMediaType(), "audio") == 0) {
    if (strcmp(rtpPayloadFormatName(), "AAC-hbr") == 0
        || strcmp(rtpPayloadFormatName(), "MPEG4-GENERIC") == 0) {
      // AAC-LC uses 1024 samples per access unit. Starting the first RTP timestamp
      // one AU into the stream keeps ffprobe/ffmpeg from deriving a synthetic
      // negative stream start_time of -1024/sample_rate.
      initialOffset = 1024;
    } else if (strcmp(rtpPayloadFormatName(), "OPUS") == 0) {
      // RFC 7587 mandates 20 ms Opus packetization to advance RTP timestamps by 960.
      initialOffset = 960;
    }
  }

  if (!groupsockBeingUsed().hasMultipleDestinations()) {
    fTimestampBase += initialOffset;
    fNextTimestampHasBeenPreset = True;
  }

  return fTimestampBase;
}
"""
if old in text:
    p.write_text(text.replace(old, new, 1))
PY
}

patch_live555_on_demand_startstream() {
	python3 - <<'PY'
from pathlib import Path

p = Path("OnDemandServerMediaSubsession.cpp")
text = p.read_text()
old = """void OnDemandServerMediaSubsession::startStream(unsigned clientSessionId,
\t\t\t\t\t\tvoid* streamToken,
\t\t\t\t\t\tTaskFunc* rtcpRRHandler,
\t\t\t\t\t\tvoid* rtcpRRHandlerClientData,
\t\t\t\t\t\tunsigned short& rtpSeqNum,
\t\t\t\t\t\tunsigned& rtpTimestamp,
\t\t\t\t\t\tServerRequestAlternativeByteHandler* serverRequestAlternativeByteHandler,
\t\t\t\t\t\tvoid* serverRequestAlternativeByteHandlerClientData) {
  StreamState* streamState = (StreamState*)streamToken;
  Destinations* destinations
    = (Destinations*)(fDestinationsHashTable->Lookup((char const*)clientSessionId));
  if (streamState != NULL) {
    streamState->startPlaying(destinations, clientSessionId,
\t\t\t      rtcpRRHandler, rtcpRRHandlerClientData,
\t\t\t      serverRequestAlternativeByteHandler, serverRequestAlternativeByteHandlerClientData);
    RTPSink* rtpSink = streamState->rtpSink(); // alias
    if (rtpSink != NULL) {
      rtpSeqNum = rtpSink->currentSeqNo();
      rtpTimestamp = rtpSink->presetNextTimestamp();
    }
  }
}
"""
new = """void OnDemandServerMediaSubsession::startStream(unsigned clientSessionId,
\t\t\t\t\t\tvoid* streamToken,
\t\t\t\t\t\tTaskFunc* rtcpRRHandler,
\t\t\t\t\t\tvoid* rtcpRRHandlerClientData,
\t\t\t\t\t\tunsigned short& rtpSeqNum,
\t\t\t\t\t\tunsigned& rtpTimestamp,
\t\t\t\t\t\tServerRequestAlternativeByteHandler* serverRequestAlternativeByteHandler,
\t\t\t\t\t\tvoid* serverRequestAlternativeByteHandlerClientData) {
  StreamState* streamState = (StreamState*)streamToken;
  Destinations* destinations
    = (Destinations*)(fDestinationsHashTable->Lookup((char const*)clientSessionId));
  if (streamState != NULL) {
    RTPSink* rtpSink = streamState->rtpSink(); // alias
    if (rtpSink != NULL) {
      // PRUDYNT-T PATCH: capture the RTP-Info sequence/timestamp before the sink
      // can emit its first packet. Fast live sources can otherwise race ahead and
      // make PLAY advertise a later in-stream timestamp instead of the clean start.
      rtpSeqNum = rtpSink->currentSeqNo();
      rtpTimestamp = rtpSink->presetNextTimestamp();
    }

    streamState->startPlaying(destinations, clientSessionId,
\t\t\t      rtcpRRHandler, rtcpRRHandlerClientData,
\t\t\t      serverRequestAlternativeByteHandler, serverRequestAlternativeByteHandlerClientData);
  }
}
"""
if old in text:
    p.write_text(text.replace(old, new, 1))
PY
}

prudynt() {
	echo "Build prudynt"

	cd $TOP
	make clean

	# Rebuild live555 to ensure latest changes are included
	echo "Rebuilding live555 with latest changes..."
	cd 3rdparty/live
	if [[ -f Makefile ]]; then
		cd liveMedia
		patch_live555_transport
		patch_live555_rtp_timestamps
		patch_live555_on_demand_startstream
		cd ..
		make clean
		PRUDYNT_ROOT="${TOP}" PRUDYNT_CROSS="${PRUDYNT_CROSS}" make -j$(nproc)
		PRUDYNT_ROOT="${TOP}" PRUDYNT_CROSS="${PRUDYNT_CROSS}" make install
		echo "live555 rebuilt successfully"
	else
		echo "Warning: live555 Makefile not found, skipping live555 rebuild"
	fi
	cd $TOP

	# Parse build type flags - default to dynamic linking (ideal for buildroot/firmware)
	BIN_TYPE=""
	for arg in "$@"; do
		if [ "$arg" = "-static" ]; then
			BIN_TYPE="-DBINARY_STATIC"
		elif [ "$arg" = "-hybrid" ]; then
			BIN_TYPE="-DBINARY_HYBRID"
		fi
	done
	# If no explicit flag provided, default to dynamic (no flag needed in Makefile)

	# Detect optional -ffmpeg flag in args to enable USE_FFMPEG in Makefile
	FFMPEG_FLAG=""
	for arg in "$@"; do
		if [ "$arg" = "-ffmpeg" ]; then
			FFMPEG_FLAG="USE_FFMPEG=1"
		fi
	done

	# Ensure cross-built FFmpeg pkg-configs are found
	export PKG_CONFIG_PATH="$TOP/3rdparty/install/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

	/usr/bin/make -j$(nproc) \
	ARCH= CROSS_COMPILE="${PRUDYNT_CROSS}" \
	CFLAGS="-DPLATFORM_$1 $BIN_TYPE -O2 -DALLOW_RTSP_SERVER_PORT_REUSE=1 -DNO_OPENSSL=1 \
	-isystem ./3rdparty/install/include \
	-isystem ./3rdparty/install/include/liveMedia \
	-isystem ./3rdparty/install/include/groupsock \
	-isystem ./3rdparty/install/include/UsageEnvironment \
	-isystem ./3rdparty/install/include/BasicUsageEnvironment" \
	LDFLAGS=" -L./3rdparty/install/lib" \
	-C $PWD all

	echo "DONE. COPYING BINARY TO NFS"
	cp -vf bin/prudynt /nfs/
	cp -vf bin/prudyntctl /nfs/
	cp -vf res/prudynt.json /nfs/

	exit 0
}

deps() {
	# Parse flags for dependency builds
	CLEAN_ALL=0
	STATIC_BUILD=0
	HYBRID_BUILD=0
	for arg in "$@"; do
		if [ "$arg" = "--clean-all" ]; then CLEAN_ALL=1; fi
		if [ "$arg" = "-static" ]; then STATIC_BUILD=1; fi
		if [ "$arg" = "-hybrid" ]; then HYBRID_BUILD=1; fi
	done
	if [ $CLEAN_ALL -eq 1 ]; then
		echo "Cleaning 3rdparty/ (requested via --clean-all)"
		rm -rf 3rdparty
	fi
	mkdir -p 3rdparty/install
	mkdir -p 3rdparty/install/include
	CROSS_COMPILE=${PRUDYNT_CROSS}

	echo "Build libhelix-aac"
	cd 3rdparty
	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_libhelixaac_deps.sh -static
	else
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_libhelixaac_deps.sh
	fi
	cd ../

	echo "Build libwebsockets"
	cd 3rdparty
	if [[ $STATIC_BUILD -eq 1 ]]; then
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_libwebsockets_deps.sh -static
	else
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_libwebsockets_deps.sh
	fi
	cd ../

	echo "Build opus"
	cd 3rdparty
	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_opus_deps.sh -static
	else
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_opus_deps.sh
	fi
	cd ../

	echo "Build libschrift"
	cd 3rdparty

	# Smart libschrift handling
	if [[ ! -d libschrift ]]; then
		echo "Cloning libschrift..."
		git clone --depth=1 https://github.com/tomolt/libschrift/
		cd libschrift
		git apply ../../res/libschrift.patch
	else
		echo "libschrift directory exists, using existing version..."
		cd libschrift
	fi
	mkdir -p $TOP/3rdparty/install/lib
	mkdir -p $TOP/3rdparty/install/include
	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		${PRUDYNT_CROSS}gcc -std=c99 -pedantic -Wall -Wextra -Wconversion -c -o schrift.o schrift.c
		${PRUDYNT_CROSS}ar rc libschrift.a schrift.o
		${PRUDYNT_CROSS}ranlib libschrift.a
		cp libschrift.a $TOP/3rdparty/install/lib/
	else
		${PRUDYNT_CROSS}gcc -std=c99 -pedantic -Wall -Wextra -Wconversion -fPIC -c -o schrift.o schrift.c
		${PRUDYNT_CROSS}gcc -shared -o libschrift.so schrift.o
		cp libschrift.so $TOP/3rdparty/install/lib/
	fi
	cp schrift.h $TOP/3rdparty/install/include/
	cd ../../

	echo "Build json-c"
	cd 3rdparty
	rm -rf json-c
	if [[ ! -f json-c-0.18.zip ]]; then
		wget 'https://github.com/json-c/json-c/archive/refs/heads/json-c-0.18.zip' -O json-c-0.18.zip
	fi
	unzip json-c-0.18.zip
	mv json-c-json-c-0.18 json-c
	cd json-c
	mkdir -p build
	cd build
	cmake -DCMAKE_SYSTEM_NAME=Linux \
		-DCMAKE_C_COMPILER="${PRUDYNT_CROSS}gcc" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$TOP/3rdparty/install" \
		-DBUILD_SHARED_LIBS=ON \
		..
	make -j$(nproc)
	make install
	cd ../../../

	echo "Build live555"
	cd 3rdparty

	# Smart live555 handling - only clone if directory doesn't exist
	if [[ ! -d live ]]; then
		echo "Cloning live555..."
		git clone https://github.com/themactep/thingino-live555.git live
	fi
	cd live

	echo "live555 directory exists, checking for updates..."
	# Reset to clean state and pull latest changes
	git reset --hard HEAD
	git clean -fd
	git pull origin master

	if [[ -f Makefile ]]; then
		make distclean
	fi

	if [[ "$2" == "-static" || "$2" == "-hybrid" ]]; then
		echo "STATIC LIVE555"
		cp "$TOP/res/live555-config.prudynt-static" ./config.prudynt-static
		./genMakefiles prudynt-static
	else
		echo "SHARED LIVE555"
		patch config.linux-with-shared-libraries "$TOP/res/live555-prudynt.patch" --output=./config.prudynt
		./genMakefiles prudynt
	fi

	cd liveMedia
	patch_live555_transport
	cd ..

	PRUDYNT_ROOT="${TOP}" PRUDYNT_CROSS="${PRUDYNT_CROSS}" make -j$(nproc)
	PRUDYNT_ROOT="${TOP}" PRUDYNT_CROSS="${PRUDYNT_CROSS}" make install
	cd ../../

	echo "import libimp"
	cd 3rdparty
	if [[ $CLEAN_ALL -eq 1 ]]; then rm -rf ingenic-lib; fi
	if [[ ! -d ingenic-lib ]]; then
	git clone --depth=1 https://github.com/gtxaspec/ingenic-lib

	case "$1" in
		T10|T20)
			echo "use T20 libs"
			cp ingenic-lib/T20/lib/3.12.0/uclibc/4.7.2/* $TOP/3rdparty/install/lib
			;;
		T21)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/1.0.33/uclibc/5.4.0/* $TOP/3rdparty/install/lib
			;;
		T23)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/1.1.0/uclibc/5.4.0/* $TOP/3rdparty/install/lib
			;;
		T30)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/1.0.5/uclibc/5.4.0/* $TOP/3rdparty/install/lib
			;;
		T31)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/1.1.6/uclibc/5.4.0/* $TOP/3rdparty/install/lib
			;;
		C100)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/2.1.0/uclibc/5.4.0/* $TOP/3rdparty/install/lib
			;;
		T40)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/1.2.0/uclibc/7.2.0/* $TOP/3rdparty/install/lib
			;;
		T41)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/1.2.0/uclibc/7.2.0/* $TOP/3rdparty/install/lib
			;;
		*)
			echo "Unsupported or unspecified SoC model."
			;;
	esac
	fi

	cd ../

	echo "import libmuslshim"
	cd 3rdparty
	if [[ $CLEAN_ALL -eq 1 ]]; then rm -rf ingenic-musl; fi
	if [[ ! -d ingenic-musl ]]; then
	git clone --depth=1 https://github.com/gtxaspec/ingenic-musl
	fi
	cd ingenic-musl
	if [[ $STATIC_BUILD -eq 1 ]]; then
		make CC="${PRUDYNT_CROSS}gcc" -j$(nproc) static
		make CC="${PRUDYNT_CROSS}gcc" -j$(nproc)
	else
		make CC="${PRUDYNT_CROSS}gcc" -j$(nproc)
	fi
	cp libmuslshim.* ../install/lib/
	cd $TOP

	echo "import libaudioshim"
	cd 3rdparty

	# Smart libaudioshim handling
	if [[ ! -d libaudioshim ]]; then
		echo "Cloning libaudioshim..."
		git clone --depth=1 https://github.com/gtxaspec/libaudioshim
		cd libaudioshim
		make CC="${PRUDYNT_CROSS}gcc" -j$(nproc)
		cp libaudioshim.* ../install/lib/
	else
		echo "libaudioshim directory exists, using existing version..."
		cd libaudioshim
		make CC="${PRUDYNT_CROSS}gcc" -j$(nproc)
		cp libaudioshim.* ../install/lib/
	fi
	cd $TOP

	echo "Build faac"
	cd 3rdparty

	# Smart faac handling
	if [[ ! -d faac ]]; then
		echo "Cloning faac..."
		git clone --depth=1 https://github.com/knik0/faac.git
		cd faac
	else
		echo "faac directory exists, using existing version..."
		cd faac
	fi

	if [[ -f meson.build ]]; then
		cat > meson-cross.ini <<EOF
[binaries]
c = '${PRUDYNT_CROSS}gcc'
ar = '${PRUDYNT_CROSS}ar'
strip = '${PRUDYNT_CROSS}strip'
pkgconfig = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'mips'
cpu = 'mips32'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOF
		rm -rf build
		meson setup build \
			--cross-file meson-cross.ini \
			--prefix="$TOP/3rdparty/install" \
			--buildtype=release \
			-Dc_std=gnu99 \
			-Ddefault_library=both \
			-Dfrontend=false \
			-Dmax-channels=2
		meson compile -C build -j"$(nproc)"
		meson install -C build
	else
		if [[ -x ./bootstrap ]]; then
			./bootstrap
		elif [[ -x ./autogen.sh ]]; then
			./autogen.sh
		elif [[ -f configure.ac || -f configure.in ]]; then
			autoreconf -fi
		elif [[ ! -x ./configure ]]; then
			echo "faac build files are missing (no Meson/autotools entrypoint available)" >&2
			exit 1
		fi
		if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
			CC="${PRUDYNT_CROSS}gcc" ./configure --host mipsel-linux-gnu --prefix="$TOP/3rdparty/install" --enable-static --disable-shared
		else
			CC="${PRUDYNT_CROSS}gcc" ./configure --host mipsel-linux-gnu --prefix="$TOP/3rdparty/install" --disable-static --enable-shared
		fi
		make -j$(nproc)
		make install
	fi
	cd ../../

	# Optional: Build FFmpeg when -ffmpeg is requested
	if [[ "$3" == "-ffmpeg" || "$4" == "-ffmpeg" || "$5" == "-ffmpeg" ]]; then
		echo "Build FFmpeg minimal (parsers + BSFs)"
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_ffmpeg_deps.sh "$2"
	fi
}

if [ $# -eq 0 ]; then
	echo "Standalone Prudynt Build"
	echo "Usage: ./build.sh deps <platform> [options]"
	echo "       ./build.sh prudynt <platform> [options]"
	echo "       ./build.sh full <platform> [options]"
	echo ""
	echo "Platforms: T20, T21, T23, T30, T31, C100, T40, T41"
	echo "Options:   -static | -hybrid | -ffmpeg (enable USE_FFMPEG)"
	exit 1
elif [[ "$1" == "deps" ]]; then
	deps "${@:2}"
elif [[ "$1" == "prudynt" ]]; then
	prudynt "${@:2}"
elif [[ "$1" == "full" ]]; then
	deps "${@:2}"
	prudynt "${@:2}"
fi

exit 0
