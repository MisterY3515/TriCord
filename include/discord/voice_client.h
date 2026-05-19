#ifndef VOICE_CLIENT_H
#define VOICE_CLIENT_H

#include "network/udp_client.h"
#include "network/websocket_client.h"
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct OpusDecoder;
struct OpusEncoder;

namespace Discord {

class VoiceClient {
  public:
	static VoiceClient &getInstance();

	std::mutex &getMutex() { return voiceMutex; }

	// Ciclo di vita
	void init();
	void joinChannel(const std::string &guildId, const std::string &channelId);
	void leaveChannel();
	void shutdown();
	bool isConnected() const;
	bool isInChannel() const;

	// Stato
	std::string getCurrentChannelId() const;
	std::string getCurrentGuildId() const;

	// Audio control
	void setMuted(bool mute);
	void setDeafened(bool deaf);
	bool isMuted() const;
	bool isDeafened() const;

	std::string getGuildId() const { return guildId; }
	std::string getChannelId() const { return channelId; }
	
	bool isUserSpeaking(const std::string &userId) const;

	// Callback dal Gateway
	void onVoiceStateUpdate(const std::string &sessionId, const std::string &guildId, const std::string &channelId);
	void onVoiceServerUpdate(const std::string &token, const std::string &endpoint);

	// Main loop
	void update();

  private:
	mutable std::mutex voiceMutex;
	VoiceClient();
	~VoiceClient();

	VoiceClient(const VoiceClient &) = delete;
	VoiceClient &operator=(const VoiceClient &) = delete;

	Network::WebSocketClient voiceWs;
	Network::UdpClient udp;

	enum class State { DISCONNECTED, WAITING_SERVER, CONNECTING_WS, IDENTIFYING, DISCOVERING_IP, SELECTING_PROTOCOL, READY };
	State state;

	// Credenziali
	std::string guildId;
	std::string channelId;
	std::string voiceToken;
	std::string voiceEndpoint;
	std::string selectedEncryptionMode;
	bool hasVoiceServerInfo;
	bool hasVoiceStateInfo;
	
	std::map<std::string, bool> speakingStates;
	std::string voiceSessionId;
	std::string currentUserId;
	uint32_t ssrc;

	// Encryption
	uint8_t secretKey[32];

	// Opus
	OpusDecoder *decoder;
	OpusEncoder *encoder;

	// RTP
	uint16_t sequence;
	uint32_t timestamp;
	uint32_t transportNonceCounter;

	bool muted;
	bool deafened;
	bool shuttingDown;
	bool pendingLeave;
	bool pendingLeaveNotifyGateway;

	int heartbeatInterval;
	uint64_t lastHeartbeatTime;
	uint64_t lastDiscoveryTime;
	uint64_t lastUdpKeepaliveTime;
	uint64_t nextTransmitTime;
	int discoveryRetries;
	uint16_t lastVoiceGatewaySequence;

	std::deque<int16_t> capturePcmAccumulator;
	std::deque<int16_t> micAccumulator;
	std::vector<uint8_t> decodeBuf;
	std::vector<uint8_t> encodeBuf;
	std::vector<int16_t> pcmBuf;
	double captureResamplePosition;

	// Funzioni di supporto
	void handleVoiceWsMessage(std::string &msg);
	void handleVoiceWsBinaryMessage(std::vector<uint8_t> &msg);
	void tryStartVoiceConnectionLocked();
	void leaveChannelLocked(bool notifyGateway);
	void requestLeaveLocked(bool notifyGateway, const char *reason);
	void resetConnectionStateLocked();
	bool initializeCodecsLocked();
	void destroyCodecsLocked();
	void resampleCaptureToDiscordRateLocked();
	void processIncomingAudioLocked();
	void processOutgoingAudioLocked();
	size_t getRtpHeaderSize(const uint8_t *data, size_t len) const;
	void sendVoiceIdentify();
	void sendVoiceSpeaking();
	void performIpDiscovery();
	void sendSelectProtocol(const std::string &ip, int port);

	// Crypto
	void encryptAudioPacket(const uint8_t *opus, size_t len, std::vector<uint8_t> &out);
	bool decryptAudioPacket(const uint8_t *data, size_t len, std::vector<uint8_t> &out);
};

} // namespace Discord

#endif // VOICE_CLIENT_H
