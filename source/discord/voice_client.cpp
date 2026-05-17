#include "discord/voice_client.h"
#include "discord/discord_client.h"
#include "log.h"
#include <opus/opus.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sodium.h>
#include "audio/audio_manager.h"
#include <3ds.h>

// Custom randombytes implementation for libsodium on 3DS
// 3DS has no /dev/urandom, so we use PS_GenerateRandomBytes from libctru
static const char *randombytes_3ds_name(void) {
	return "3ds";
}

static void randombytes_3ds_buf(void *const buf, const size_t size) {
	PS_GenerateRandomBytes(buf, size);
}

static uint32_t randombytes_3ds_random(void) {
	uint32_t val;
	PS_GenerateRandomBytes(&val, sizeof(val));
	return val;
}

static struct randombytes_implementation randombytes_3ds_implementation = {
    .implementation_name = randombytes_3ds_name,
    .random = randombytes_3ds_random,
    .stir = NULL,
    .uniform = NULL,
    .buf = randombytes_3ds_buf,
    .close = NULL,
};

namespace Discord {

VoiceClient &VoiceClient::getInstance() {
	static VoiceClient instance;
	return instance;
}

VoiceClient::VoiceClient()
    : state(State::DISCONNECTED), ssrc(0), decoder(nullptr), encoder(nullptr), sequence(0), timestamp(0), muted(false),
      deafened(false), heartbeatInterval(0), lastHeartbeatTime(0) {
	voiceWs.setOnMessage([this](std::string &msg) { handleVoiceWsMessage(msg); });
	voiceWs.setOnError([](const std::string &err) { Logger::log("[Voice] WS Error: %s", err.c_str()); });
	voiceWs.setOnClose([this](int code, const std::string &reason) {
		Logger::log("[Voice] WS Closed: %d %s", code, reason.c_str());
		state = State::DISCONNECTED;
		udp.close();
	});
}

VoiceClient::~VoiceClient() {
	leaveChannel();
	if (decoder) opus_decoder_destroy(decoder);
	if (encoder) opus_encoder_destroy(encoder);
}

void VoiceClient::joinChannel(const std::string &guildId, const std::string &channelId) {
	Logger::log("[Voice] Joining channel %s in guild %s", channelId.c_str(), guildId.c_str());

	// Lazy init sodium & opus (only when actually needed, not at app startup)
	if (!decoder) {
		// Register custom randombytes using 3DS PS service (no /dev/urandom on 3DS)
		randombytes_set_implementation(&randombytes_3ds_implementation);

		if (sodium_init() < 0) {
			Logger::log("[Voice] Failed to initialize libsodium!");
			return;
		}
		int err;
		decoder = opus_decoder_create(16000, 1, &err);
		encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
		Logger::log("[Voice] Opus & Sodium initialized");
	}

	this->guildId = guildId;
	this->channelId = channelId;
	state = State::WAITING_SERVER;

	DiscordClient::getInstance().sendVoiceStateUpdate(guildId, channelId, muted, deafened);
}

void VoiceClient::leaveChannel() {
	if (state == State::DISCONNECTED) return;
	Logger::log("[Voice] Leaving channel");

	if (!channelId.empty()) {
		DiscordClient::getInstance().sendVoiceStateUpdate(guildId, "", muted, deafened);
	}

	voiceWs.disconnect();
	udp.close();
	Audio::AudioManager::getInstance().stopCapture();
	state = State::DISCONNECTED;
	channelId.clear();
	guildId.clear();
	voiceToken.clear();
	voiceEndpoint.clear();
	voiceSessionId.clear();
	micAccumulator.clear();
	heartbeatInterval = 0;
	lastHeartbeatTime = 0;
}

bool VoiceClient::isConnected() const { return state == State::READY; }

bool VoiceClient::isInChannel() const { return !channelId.empty(); }

std::string VoiceClient::getCurrentChannelId() const { return channelId; }

std::string VoiceClient::getCurrentGuildId() const { return guildId; }

void VoiceClient::setMuted(bool mute) {
	if (muted != mute) {
		muted = mute;
		if (isInChannel()) {
			DiscordClient::getInstance().sendVoiceStateUpdate(guildId, channelId, muted, deafened);
		}
	}
}

void VoiceClient::setDeafened(bool deaf) {
	if (deafened != deaf) {
		deafened = deaf;
		if (isInChannel()) {
			DiscordClient::getInstance().sendVoiceStateUpdate(guildId, channelId, muted, deafened);
		}
	}
}

bool VoiceClient::isMuted() const { return muted; }

bool VoiceClient::isDeafened() const { return deafened; }

void VoiceClient::onVoiceServerUpdate(const std::string &token, const std::string &endpoint) {
	if (state != State::WAITING_SERVER) return;

	Logger::log("[Voice] Received server update. Endpoint: %s", endpoint.c_str());
	voiceToken = token;
	
	// Remove port from endpoint if present (e.g., vss1.discord.gg:443 -> vss1.discord.gg)
	voiceEndpoint = endpoint;
	size_t colonPos = voiceEndpoint.find(':');
	if (colonPos != std::string::npos) {
		voiceEndpoint = voiceEndpoint.substr(0, colonPos);
	}

	state = State::CONNECTING_WS;
	connectVoiceWebSocket();
}

void VoiceClient::connectVoiceWebSocket() {
	std::string url = "wss://" + voiceEndpoint + "/?v=7";
	Logger::log("[Voice] Connecting to %s", url.c_str());
	if (!voiceWs.connect(url)) {
		Logger::log("[Voice] Failed to connect to voice WS");
		state = State::DISCONNECTED;
	}
}

void VoiceClient::handleVoiceWsMessage(std::string &msg) {
	rapidjson::Document d;
	d.Parse(msg.c_str());
	if (d.HasParseError() || !d.IsObject()) return;

	int op = d["op"].GetInt();
	const rapidjson::Value &data = d["d"];

	switch (op) {
	case 2: // Ready
		Logger::log("[Voice] Received Ready");
		ssrc = data["ssrc"].GetUint();
		{
			std::string ip = data["ip"].GetString();
			int port = data["port"].GetInt();
			Logger::log("[Voice] UDP target: %s:%d", ip.c_str(), port);

			if (udp.connect(ip, port)) {
				state = State::DISCOVERING_IP;
				performIpDiscovery();
			} else {
				leaveChannel();
			}
		}
		break;
	case 8: // Hello
		if (data.HasMember("heartbeat_interval")) {
			heartbeatInterval = data["heartbeat_interval"].GetInt();
			lastHeartbeatTime = osGetTime();
			Logger::log("[Voice] Received Hello, heartbeat interval: %d ms", heartbeatInterval);
		}
		sendVoiceIdentify();
		break;
	case 4: // Session Description
		Logger::log("[Voice] Received Session Description");
		if (data.HasMember("secret_key") && data["secret_key"].IsArray()) {
			const auto &keyArr = data["secret_key"];
			for (rapidjson::SizeType i = 0; i < keyArr.Size() && i < 32; i++) {
				secretKey[i] = keyArr[i].GetUint();
			}
			state = State::READY;
			Audio::AudioManager::getInstance().startCapture();
			Logger::log("[Voice] Ready to transmit audio!");
		}
		break;
	}
}

void VoiceClient::sendVoiceIdentify() {
	state = State::IDENTIFYING;

	rapidjson::Document d;
	d.SetObject();
	auto &alloc = d.GetAllocator();

	d.AddMember("op", 0, alloc);
	
	rapidjson::Value data(rapidjson::kObjectType);
	std::string serverId = guildId.empty() ? channelId : guildId;
	data.AddMember("server_id", rapidjson::Value(serverId.c_str(), alloc), alloc);
	data.AddMember("user_id", rapidjson::Value(DiscordClient::getInstance().getCurrentUser().id.c_str(), alloc), alloc);
	data.AddMember("session_id", rapidjson::Value(DiscordClient::getInstance().getSessionId().c_str(), alloc), alloc);
	data.AddMember("token", rapidjson::Value(voiceToken.c_str(), alloc), alloc);
	
	d.AddMember("d", data, alloc);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	d.Accept(writer);

	voiceWs.send(buffer.GetString());
	Logger::log("[Voice] Sent Identify");
}

void VoiceClient::performIpDiscovery() {
	Logger::log("[Voice] Performing IP Discovery...");
	uint8_t packet[74] = {0};
	packet[0] = 0x0;
	packet[1] = 0x1;
	packet[2] = 0x0;
	packet[3] = 70;
	packet[4] = (ssrc >> 24) & 0xFF;
	packet[5] = (ssrc >> 16) & 0xFF;
	packet[6] = (ssrc >> 8) & 0xFF;
	packet[7] = (ssrc >> 0) & 0xFF;

	udp.send(packet, 74);
}

void VoiceClient::sendSelectProtocol(const std::string &ip, int port) {
	state = State::SELECTING_PROTOCOL;

	rapidjson::Document d;
	d.SetObject();
	auto &alloc = d.GetAllocator();

	d.AddMember("op", 1, alloc);
	
	rapidjson::Value data(rapidjson::kObjectType);
	data.AddMember("protocol", "udp", alloc);
	
	rapidjson::Value data2(rapidjson::kObjectType);
	data2.AddMember("address", rapidjson::Value(ip.c_str(), alloc), alloc);
	data2.AddMember("port", port, alloc);
	data2.AddMember("mode", "xsalsa20_poly1305", alloc);
	
	data.AddMember("data", data2, alloc);
	d.AddMember("d", data, alloc);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	d.Accept(writer);

	voiceWs.send(buffer.GetString());
	Logger::log("[Voice] Sent Select Protocol");
}

void VoiceClient::update() {
	if (state != State::DISCONNECTED && state != State::WAITING_SERVER) {
		voiceWs.poll();
		
		// Heartbeat WS (keep connection alive)
		if (heartbeatInterval > 0) {
			uint64_t now = osGetTime();
			if (now - lastHeartbeatTime >= (uint64_t)heartbeatInterval) {
				lastHeartbeatTime = now;
				rapidjson::Document d;
				d.SetObject();
				auto &alloc = d.GetAllocator();
				d.AddMember("op", 3, alloc); // Heartbeat
				d.AddMember("d", rapidjson::Value(std::to_string(now).c_str(), alloc), alloc);
				
				rapidjson::StringBuffer buffer;
				rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
				d.Accept(writer);
				voiceWs.send(buffer.GetString());
			}
		}
		
		if (state == State::DISCOVERING_IP) {
			uint8_t buf[74];
			int len = udp.recv(buf, sizeof(buf), 0); // Non-blocking
			if (len == 74) {
				std::string myIp = std::string((char*)&buf[8]);
				uint16_t myPort = (buf[72] << 8) | buf[73];
				Logger::log("[Voice] IP Discovered: %s:%d", myIp.c_str(), myPort);
				sendSelectProtocol(myIp, myPort);
			}
		} else if (state == State::READY) {
			// Ricevi e processa TUTTI i pacchetti audio in coda (UDP)
			uint8_t buf[2048];
			int len;
			while ((len = udp.recv(buf, sizeof(buf), 0)) > 12) {
				std::vector<uint8_t> decrypted;
				if (decryptAudioPacket(buf, len, decrypted)) {
					// Audio decriptato, ora va decodificato
					int16_t pcm[1920]; // 1920 samples (max 120ms at 16kHz)
					int samples = opus_decode(decoder, decrypted.data(), decrypted.size(), pcm, 1920, 0);
					if (samples > 0) {
						Audio::AudioManager::getInstance().queuePcm(pcm, samples);
					}
				}
			}

			// Invio pacchetti audio
			if (!muted) {
				if (Audio::AudioManager::getInstance().hasNewSamples()) {
					int16_t tempBuf[1024];
					size_t read = Audio::AudioManager::getInstance().readSamples(tempBuf, 1024);
					if (read > 0) {
						micAccumulator.insert(micAccumulator.end(), tempBuf, tempBuf + read);
					}
				}
				
				// Invia in blocchi da 20ms (320 samples a 16kHz)
				while (micAccumulator.size() >= 320) {
					int16_t micBuf[320];
					std::copy(micAccumulator.begin(), micAccumulator.begin() + 320, micBuf);
					micAccumulator.erase(micAccumulator.begin(), micAccumulator.begin() + 320);
					
					uint8_t opusBuf[1024];
					int encodedLen = opus_encode(encoder, micBuf, 320, opusBuf, sizeof(opusBuf));
					if (encodedLen > 0) {
						std::vector<uint8_t> encrypted;
						encryptAudioPacket(opusBuf, encodedLen, encrypted);
						udp.send(encrypted.data(), encrypted.size());
					}
				}
			}
		}
	}
}

void VoiceClient::encryptAudioPacket(const uint8_t *opus, size_t len, std::vector<uint8_t> &out) {
	// 12 bytes RTP header
	uint8_t header[12];
	header[0] = 0x80;
	header[1] = 0x78;
	header[2] = (sequence >> 8) & 0xFF;
	header[3] = (sequence >> 0) & 0xFF;
	header[4] = (timestamp >> 24) & 0xFF;
	header[5] = (timestamp >> 16) & 0xFF;
	header[6] = (timestamp >> 8) & 0xFF;
	header[7] = (timestamp >> 0) & 0xFF;
	header[8] = (ssrc >> 24) & 0xFF;
	header[9] = (ssrc >> 16) & 0xFF;
	header[10] = (ssrc >> 8) & 0xFF;
	header[11] = (ssrc >> 0) & 0xFF;

	uint8_t nonce[24] = {0};
	memcpy(nonce, header, 12);

	out.resize(12 + len + crypto_secretbox_MACBYTES);
	memcpy(out.data(), header, 12);

	crypto_secretbox_easy(out.data() + 12, opus, len, nonce, secretKey);

	sequence++;
	timestamp += 960; // RTP timestamp per Opus (RFC 7587) usa sempre 48000Hz (20ms * 48000 = 960)
}

bool VoiceClient::decryptAudioPacket(const uint8_t *data, size_t len, std::vector<uint8_t> &out) {
	if (len <= 12 + crypto_secretbox_MACBYTES) return false;

	uint8_t nonce[24] = {0};
	memcpy(nonce, data, 12);

	size_t cipherLen = len - 12;
	out.resize(cipherLen - crypto_secretbox_MACBYTES);

	if (crypto_secretbox_open_easy(out.data(), data + 12, cipherLen, nonce, secretKey) != 0) {
		return false;
	}

	return true;
}

} // namespace Discord
