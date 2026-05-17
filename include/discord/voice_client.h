#ifndef VOICE_CLIENT_H
#define VOICE_CLIENT_H

#include "network/udp_client.h"
#include "network/websocket_client.h"
#include <cstdint>
#include <string>
#include <vector>

struct OpusDecoder;
struct OpusEncoder;

namespace Discord {

class VoiceClient {
  public:
	static VoiceClient &getInstance();

	// Ciclo di vita
	void joinChannel(const std::string &guildId, const std::string &channelId);
	void leaveChannel();
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

	// Callback dal Gateway
	void onVoiceServerUpdate(const std::string &token, const std::string &endpoint);

	// Main loop
	void update();

  private:
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
	std::string voiceSessionId;
	uint32_t ssrc;

	// Encryption
	uint8_t secretKey[32];

	// Opus
	OpusDecoder *decoder;
	OpusEncoder *encoder;

	// RTP
	uint16_t sequence;
	uint32_t timestamp;

	bool muted;
	bool deafened;

	// Funzioni di supporto
	void handleVoiceWsMessage(std::string &msg);
	void connectVoiceWebSocket();
	void sendVoiceIdentify();
	void performIpDiscovery();
	void sendSelectProtocol(const std::string &ip, int port);

	// Crypto
	void encryptAudioPacket(const uint8_t *opus, size_t len, std::vector<uint8_t> &out);
	bool decryptAudioPacket(const uint8_t *data, size_t len, std::vector<uint8_t> &out);
};

} // namespace Discord

#endif // VOICE_CLIENT_H
