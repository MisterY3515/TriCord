#include "discord/voice_client.h"
#include "audio/audio_manager.h"
#include "discord/discord_client.h"
#include "log.h"
#include "utils/json_utils.h"
#include <3ds.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sodium.h>
#include <opus/opus.h>
#include <cstring>
#include <cmath>

namespace Discord {

VoiceClient &VoiceClient::getInstance() {
	static VoiceClient instance;
	return instance;
}

VoiceClient::VoiceClient() : state(State::DISCONNECTED), selectedEncryptionMode("xsalsa20_poly1305"),
	ssrc(0), decoder(nullptr), encoder(nullptr), sequence(0), timestamp(0), muted(false), deafened(false),
	heartbeatInterval(0), lastHeartbeatTime(0), lastDiscoveryTime(0), discoveryRetries(0) {
	
	memset(secretKey, 0, sizeof(secretKey));
	voiceWs.setOnMessage([this](std::string &msg) {
		handleVoiceWsMessage(msg);
	});
}

VoiceClient::~VoiceClient() {
	shutdown();
}

void VoiceClient::init() {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	// Initialization handled in constructor or joinChannel (lazy init)
}

void VoiceClient::joinChannel(const std::string &guildId, const std::string &channelId) {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	
	if (this->channelId == channelId && state != State::DISCONNECTED) {
		Logger::log("[Voice] Already in/joining channel %s, ignoring join request", channelId.c_str());
		return;
	}

	if (!this->channelId.empty()) {
		Logger::log("[Voice] Leaving current channel %s before joining %s", this->channelId.c_str(), channelId.c_str());
		leaveChannel();
	}

	Logger::log("[Voice] Joining channel %s in guild %s", channelId.c_str(), guildId.c_str());

	if (!decoder) {
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

	Audio::AudioManager::getInstance().playSystemSound(Audio::SystemSound::JOIN);
	DiscordClient::getInstance().sendVoiceStateUpdate(guildId, channelId, muted, deafened);
}

void VoiceClient::leaveChannel() {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	if (state == State::DISCONNECTED && channelId.empty()) return;
	
	Logger::log("[Voice] Leaving channel (current state: %d)", (int)state);

	Audio::AudioManager::getInstance().playSystemSound(Audio::SystemSound::LEAVE);

	if (!channelId.empty()) {
		Logger::log("[Voice] Sending leave request to Gateway for channel %s", channelId.c_str());
		DiscordClient::getInstance().sendVoiceStateUpdate(guildId, "", muted, deafened);
	}

	state = State::DISCONNECTED;
	voiceWs.disconnect();
	udp.close();
	Audio::AudioManager::getInstance().stopCapture();
	
	channelId.clear();
	guildId.clear();
	voiceToken.clear();
	voiceEndpoint.clear();
	voiceSessionId.clear();
	micAccumulator.clear();
	heartbeatInterval = 0;
	lastHeartbeatTime = 0;
	lastDiscoveryTime = 0;
	discoveryRetries = 0;
	
	Logger::log("[Voice] Leave complete");
}

void VoiceClient::shutdown() {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	leaveChannel();
	
	if (decoder) {
		opus_decoder_destroy(decoder);
		decoder = nullptr;
	}
	if (encoder) {
		opus_encoder_destroy(encoder);
		encoder = nullptr;
	}
	Logger::log("[Voice] Shutdown complete");
}

void VoiceClient::onVoiceServerUpdate(const std::string &token, const std::string &endpoint) {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	if (state != State::WAITING_SERVER || channelId.empty()) {
		Logger::log("[Voice] Ignoring Voice Server Update: state=%d, channelId=%s", (int)state, channelId.c_str());
		return;
	}
	
	voiceToken = token;
	voiceEndpoint = endpoint;
	size_t portPos = voiceEndpoint.find(':');
	if (portPos != std::string::npos) {
		voiceEndpoint = voiceEndpoint.substr(0, portPos);
	}
	
	state = State::CONNECTING_WS;
	std::string wsUrl = "wss://" + voiceEndpoint + "/?v=4";
	Logger::log("[Voice] Connecting to Voice WebSocket: %s", wsUrl.c_str());
	voiceWs.connect(wsUrl);
}

void VoiceClient::handleVoiceWsMessage(std::string &msg) {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	rapidjson::Document d;
	d.Parse(msg.c_str());
	if (d.HasParseError()) return;

	int op = d["op"].GetInt();
	const rapidjson::Value &data = d["d"];

	switch (op) {
	case 8: // Hello
		heartbeatInterval = data["heartbeat_interval"].GetInt();
		lastHeartbeatTime = osGetTime();
		sendVoiceIdentify();
		break;
	case 2: // Ready
		ssrc = data["ssrc"].GetUint();
		if (data.HasMember("modes") && data["modes"].IsArray()) {
			selectedEncryptionMode.clear();
			const rapidjson::Value &modes = data["modes"];
			for (rapidjson::SizeType i = 0; i < modes.Size(); i++) {
				if (!modes[i].IsString()) continue;
				std::string mode = modes[i].GetString();
				if (mode == "xsalsa20_poly1305" || mode == "xsalsa20_poly1305_suffix" ||
				    mode == "xsalsa20_poly1305_lite") {
					selectedEncryptionMode = mode;
					break;
				}
			}
			if (selectedEncryptionMode.empty()) {
				Logger::log("[Voice] No supported legacy encryption mode offered by server");
				for (rapidjson::SizeType i = 0; i < modes.Size(); i++) {
					if (modes[i].IsString()) {
						Logger::log("[Voice] Server mode[%u]: %s", (unsigned)i, modes[i].GetString());
					}
				}
				leaveChannel();
				return;
			}
			Logger::log("[Voice] Selected encryption mode: %s", selectedEncryptionMode.c_str());
		}
		udp.connect(data["ip"].GetString(), data["port"].GetInt());
		state = State::DISCOVERING_IP;
		performIpDiscovery();
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
			sendVoiceSpeaking();
		}
		break;
	case 5: // Speaking
		if (data.HasMember("user_id") && data.HasMember("speaking")) {
			std::string userId = data["user_id"].GetString();
			bool speaking = (data["speaking"].GetInt() != 0);
			speakingStates[userId] = speaking;
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

void VoiceClient::sendVoiceSpeaking() {
	rapidjson::Document d;
	d.SetObject();
	auto &alloc = d.GetAllocator();
	d.AddMember("op", 5, alloc);
	
	rapidjson::Value data(rapidjson::kObjectType);
	data.AddMember("speaking", 1, alloc);
	data.AddMember("delay", 0, alloc);
	data.AddMember("ssrc", (uint64_t)ssrc, alloc);
	d.AddMember("d", data, alloc);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	d.Accept(writer);
	voiceWs.send(buffer.GetString());
	Logger::log("[Voice] Sent Speaking (op 5)");
}

void VoiceClient::performIpDiscovery() {
	Logger::log("[Voice] Performing IP Discovery (SSRC: %u)", ssrc);
	uint8_t packet[74] = {0};
	packet[0] = 0x00;
	packet[1] = 0x01;
	packet[2] = 0x00;
	packet[3] = 0x46;
	uint32_t ssrcBE = __builtin_bswap32(ssrc);
	memcpy(packet + 4, &ssrcBE, 4);

	udp.send(packet, 74);
	lastDiscoveryTime = osGetTime();
	discoveryRetries++;
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
	data2.AddMember("mode", rapidjson::Value(selectedEncryptionMode.c_str(), alloc), alloc);
	data.AddMember("data", data2, alloc);
	d.AddMember("d", data, alloc);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	d.Accept(writer);
	voiceWs.send(buffer.GetString());
	Logger::log("[Voice] Sent Select Protocol");
}

void VoiceClient::update() {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	if (state == State::DISCONNECTED || state == State::WAITING_SERVER) return;

	voiceWs.poll();
	uint64_t now = osGetTime();

	if (state == State::READY || state == State::DISCOVERING_IP || state == State::SELECTING_PROTOCOL) {
		// Heartbeat WS (keep connection alive)
		if (heartbeatInterval > 0 && now - lastHeartbeatTime >= (uint64_t)heartbeatInterval) {
			lastHeartbeatTime = now;
			rapidjson::Document d;
			d.SetObject();
			auto &alloc = d.GetAllocator();
			d.AddMember("op", 3, alloc);
			d.AddMember("d", (uint64_t)now, alloc);
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			d.Accept(writer);
			voiceWs.send(buffer.GetString());
		}

		// Heartbeat UDP (Keep-alive every 5 seconds)
		static uint64_t lastUdpHeartbeat = 0;
		if (state == State::READY && now - lastUdpHeartbeat >= 5000) {
			lastUdpHeartbeat = now;
			performIpDiscovery(); // Discord uses the same packet for keep-alive
		}
	}
	
	if (state == State::DISCOVERING_IP) {
		if (osGetTime() - lastDiscoveryTime > 1000) {
			if (discoveryRetries < 5) performIpDiscovery();
			else leaveChannel();
		}

		uint8_t buf[256];
		int len = udp.recv(buf, sizeof(buf), 0);
		if (len >= 74) {
			char ip[65] = {0};
			memcpy(ip, buf + 8, 64);
			std::string myIp = std::string(ip);
			uint16_t portBE;
			memcpy(&portBE, buf + 72, 2);
			uint16_t myPort = __builtin_bswap16(portBE);
			if (!myIp.empty() && myPort > 0) {
				Logger::log("[Voice] IP Discovered: %s:%d", myIp.c_str(), myPort);
				sendSelectProtocol(myIp, myPort);
			}
		}
	} else if (state == State::READY) {
		uint8_t buf[2048];
		int len;
		static int packetCount = 0;
		while ((len = udp.recv(buf, sizeof(buf), 0)) > 12) {
			packetCount++;
			if (decryptAudioPacket(buf, len, decodeBuf)) {
				int16_t pcm[1920];
				int samples = opus_decode(decoder, decodeBuf.data(), decodeBuf.size(), pcm, 1920, 0);
				if (samples > 0) Audio::AudioManager::getInstance().queuePcm(pcm, samples);
			}
		}

		if (!muted) {
			if (Audio::AudioManager::getInstance().hasNewSamples()) {
				int16_t tempBuf[1024];
				size_t read = Audio::AudioManager::getInstance().readSamples(tempBuf, 1024);
				if (read > 0) micAccumulator.insert(micAccumulator.end(), tempBuf, tempBuf + read);
			}
			while (micAccumulator.size() >= 320) {
				int16_t micBuf[320];
				for(int i = 0; i < 320; i++) {
					micBuf[i] = micAccumulator.front();
					micAccumulator.pop_front();
				}
				uint8_t opusBuf[1024];
				int encodedLen = opus_encode(encoder, micBuf, 320, opusBuf, sizeof(opusBuf));
				if (encodedLen > 0) {
					encryptAudioPacket(opusBuf, encodedLen, encodeBuf);
					udp.send(encodeBuf.data(), encodeBuf.size());
				}
			}
		} else {
			if (Audio::AudioManager::getInstance().hasNewSamples()) {
				int16_t discardBuf[1024];
				Audio::AudioManager::getInstance().readSamples(discardBuf, 1024);
			}
			micAccumulator.clear();
		}
	}
}

void VoiceClient::encryptAudioPacket(const uint8_t *opus, size_t len, std::vector<uint8_t> &out) {
	uint8_t header[12];
	header[0] = 0x80;
	header[1] = 0x78;
	header[2] = (sequence >> 8) & 0xFF;
	header[3] = sequence & 0xFF;
	header[4] = (timestamp >> 24) & 0xFF;
	header[5] = (timestamp >> 16) & 0xFF;
	header[6] = (timestamp >> 8) & 0xFF;
	header[7] = timestamp & 0xFF;
	header[8] = (ssrc >> 24) & 0xFF;
	header[9] = (ssrc >> 16) & 0xFF;
	header[10] = (ssrc >> 8) & 0xFF;
	header[11] = ssrc & 0xFF;

	uint8_t nonce[24];
	memcpy(nonce, header, 12);
	memset(nonce + 12, 0, 12);

	out.resize(12 + len + 16);
	memcpy(out.data(), header, 12);
	crypto_secretbox_easy(out.data() + 12, opus, len, nonce, secretKey);
	
	sequence++;
	timestamp += 960; // Discord expects 48kHz increments (960 for 20ms) even for lower bitrates
}

bool VoiceClient::decryptAudioPacket(const uint8_t *data, size_t len, std::vector<uint8_t> &out) {
	if (len < 12 + 16) return false;
	uint8_t nonce[24];
	memcpy(nonce, data, 12);
	memset(nonce + 12, 0, 12);
	size_t cipherLen = len - 12;
	out.resize(cipherLen - 16);
	return crypto_secretbox_open_easy(out.data(), data + 12, cipherLen, nonce, secretKey) == 0;
}

bool VoiceClient::isUserSpeaking(const std::string &userId) const {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	auto it = speakingStates.find(userId);
	return (it != speakingStates.end()) ? it->second : false;
}

void VoiceClient::setMuted(bool mute) {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	if (muted != mute) {
		muted = mute;
		Audio::AudioManager::getInstance().playSystemSound(muted ? Audio::SystemSound::MUTE : Audio::SystemSound::UNMUTE);
		if (isInChannel()) DiscordClient::getInstance().sendVoiceStateUpdate(guildId, channelId, muted, deafened);
	}
}

void VoiceClient::setDeafened(bool deaf) {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	if (deafened != deaf) {
		deafened = deaf;
		if (isInChannel()) DiscordClient::getInstance().sendVoiceStateUpdate(guildId, channelId, muted, deafened);
	}
}

bool VoiceClient::isMuted() const { return muted; }
bool VoiceClient::isDeafened() const { return deafened; }

bool VoiceClient::isConnected() const {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	return state == State::READY;
}

bool VoiceClient::isInChannel() const {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	return !channelId.empty();
}

std::string VoiceClient::getCurrentChannelId() const {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	return channelId;
}

std::string VoiceClient::getCurrentGuildId() const {
	std::lock_guard<std::recursive_mutex> lock(voiceMutex);
	return guildId;
}

} // namespace Discord
