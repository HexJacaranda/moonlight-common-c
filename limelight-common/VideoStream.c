#include "Limelight-internal.h"
#include "PlatformSockets.h"
#include "PlatformThreads.h"
#include "LinkedBlockingQueue.h"

#define FIRST_FRAME_MAX 1500

#define RTP_PORT 47998
#define FIRST_FRAME_PORT 47996

static DECODER_RENDERER_CALLBACKS callbacks;
static STREAM_CONFIGURATION configuration;
static IP_ADDRESS remoteHost;
static PCONNECTION_LISTENER_CALLBACKS listenerCallbacks;

static SOCKET rtpSocket = INVALID_SOCKET;
static SOCKET firstFrameSocket = INVALID_SOCKET;

static PLT_THREAD udpPingThread;
static PLT_THREAD receiveThread;
static PLT_THREAD decoderThread;

/* Initialize the video stream */
void initializeVideoStream(IP_ADDRESS host, PSTREAM_CONFIGURATION streamConfig, PDECODER_RENDERER_CALLBACKS drCallbacks,
	PCONNECTION_LISTENER_CALLBACKS clCallbacks) {
	memcpy(&callbacks, drCallbacks, sizeof(callbacks));
	memcpy(&configuration, streamConfig, sizeof(configuration));
	remoteHost = host;
	listenerCallbacks = clCallbacks;

	initializeVideoDepacketizer(configuration.packetSize);
}

/* Clean up the video stream */
void destroyVideoStream(void) {
	callbacks.release();

	destroyVideoDepacketizer();
}

/* UDP Ping proc */
static void UdpPingThreadProc(void *context) {
	char pingData [] = { 0x50, 0x49, 0x4E, 0x47 };
	struct sockaddr_in saddr;
	SOCK_RET err;

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(RTP_PORT);
	memcpy(&saddr.sin_addr, &remoteHost, sizeof(remoteHost));

	while (!PltIsThreadInterrupted(&udpPingThread)) {
		err = sendto(rtpSocket, pingData, sizeof(pingData), 0, (struct sockaddr*)&saddr, sizeof(saddr));
		if (err != sizeof(pingData)) {
			Limelog("UDP ping thread terminating #1\n");
			listenerCallbacks->connectionTerminated(LastSocketError());
			return;
		}

		PltSleepMs(100);
	}
}

/* Receive thread proc */
static void ReceiveThreadProc(void* context) {
	SOCK_RET err;
	int bufferSize;
	char* buffer;

	bufferSize = configuration.packetSize + MAX_RTP_HEADER_SIZE;
	buffer = (char*)malloc(bufferSize);
	if (buffer == NULL) {
		Limelog("Receive thread terminating\n");
		listenerCallbacks->connectionTerminated(-1);
		return;
	}

	while (!PltIsThreadInterrupted(&receiveThread)) {
		err = recv(rtpSocket, buffer, bufferSize, 0);
		if (err <= 0) {
			Limelog("Receive thread terminating #2\n");
			listenerCallbacks->connectionTerminated(LastSocketError());
			break;
		}

		// queueRtpPacket() copies the data it needs to we can reuse the buffer
		queueRtpPacket((PRTP_PACKET) buffer, (int)err);
	}

	free(buffer);
}

/* Decoder thread proc */
static void DecoderThreadProc(void* context) {
	PDECODE_UNIT du;
	while (!PltIsThreadInterrupted(&decoderThread)) {
		if (!getNextDecodeUnit(&du)) {
			printf("Decoder thread terminating\n");
			return;
		}

		callbacks.submitDecodeUnit(du);

		freeDecodeUnit(du);
	}
}

/* Read the first frame of the video stream */
int readFirstFrame(void) {
	char* firstFrame;
	SOCK_RET err;
	int offset = 0;

	firstFrameSocket = connectTcpSocket(remoteHost, FIRST_FRAME_PORT);
	if (firstFrameSocket == INVALID_SOCKET) {
		return LastSocketError();
	}

	firstFrame = (char*) malloc(FIRST_FRAME_MAX);
	if (firstFrame == NULL) {
		return -1;
	}

	Limelog("Waiting for first frame\n");
	for (;;) {
		err = recv(firstFrameSocket, &firstFrame[offset], FIRST_FRAME_MAX - offset, 0);
		if (err <= 0) {
			break;
		}

		offset += err;
	}
	Limelog("Read %d bytes\n", offset);

	processRtpPayload((PNV_VIDEO_PACKET) firstFrame, offset);

	return 0;
}

/* Terminate the video stream */
void stopVideoStream(void) {
	callbacks.stop();

	PltInterruptThread(&udpPingThread);
	PltInterruptThread(&receiveThread);
	PltInterruptThread(&decoderThread);

	if (firstFrameSocket != INVALID_SOCKET) {
		closesocket(firstFrameSocket);
		firstFrameSocket = INVALID_SOCKET;
	}
	if (rtpSocket != INVALID_SOCKET) {
		closesocket(rtpSocket);
		rtpSocket = INVALID_SOCKET;
	}

	PltJoinThread(&udpPingThread);
	PltJoinThread(&receiveThread);
	PltJoinThread(&decoderThread);

	PltCloseThread(&udpPingThread);
	PltCloseThread(&receiveThread);
	PltCloseThread(&decoderThread);
}

/* Start the video stream */
int startVideoStream(void* rendererContext, int drFlags) {
	int err;

	callbacks.setup(configuration.width,
		configuration.height, 60, rendererContext, drFlags);

	rtpSocket = bindUdpSocket();
	if (rtpSocket == INVALID_SOCKET) {
		return LastSocketError();
	}

	err = PltCreateThread(UdpPingThreadProc, NULL, &udpPingThread);
	if (err != 0) {
		return err;
	}

	err = readFirstFrame();
	if (err != 0) {
		return err;
	}

	err = PltCreateThread(ReceiveThreadProc, NULL, &receiveThread);
	if (err != 0) {
		return err;
	}

	//This must be called before the decoder thread starts submitting
	// decode units
	callbacks.start();

	err = PltCreateThread(DecoderThreadProc, NULL, &decoderThread);
	if (err != 0) {
		return err;
	}

	return 0;
}
