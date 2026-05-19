#include "discord/voice_client.h"
#include "audio/audio_manager.h"
#include "discord/discord_client.h"
#include "log.h"
#include "utils/json_utils.h"
#include "core/config.h"
#include <3ds.h>
#include <opus/opus.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sodium.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Discord {

namespace {
constexpr int kDiscordSampleRate = 48000;
constexpr int kMicCaptureRate = 32730;
constexpr int kDiscordFrameSamples = 960;         // 20ms at 48kHz
constexpr uint64_t kDiscordFrameDurationMs = 20;
constexpr int kMaxDecodeFrameSamples = 5760;      // 120ms at 48kHz
constexpr size_t kRtpHeaderSize = 12;
constexpr size_t kLegacyNonceSize = 24;
constexpr size_t kLiteNonceSuffixSize = 4;
constexpr size_t kSecretBoxMacSize = crypto_secretbox_MACBYTES;
constexpr size_t kXChaChaTagSize = crypto_aead_xchacha20poly1305_ietf_ABYTES;
constexpr uint8_t kRtpPayloadType = 0x78;
constexpr size_t kMaxBufferedVoiceFrames = 10;

void writeBigEndianCounter(uint8_t *dst, uint32_t value) {
	dst[0] = (value >> 24) & 0xFF;
	dst[1] = (value >> 16) & 0xFF;
	dst[2] = (value >> 8) & 0xFF;
	dst[3] = value & 0xFF;
}

void buildLegacyNonceFromHeader(const uint8_t *header, uint8_t *nonce) {
	memcpy(nonce, header, kRtpHeaderSize);
	memset(nonce + kRtpHeaderSize, 0, kLegacyNonceSize - kRtpHeaderSize);
}

bool isSupportedEncryptionMode(const std::string &mode) {
	return mode == "aead_xchacha20_poly1305_rtpsize" || mode == "aead_aes256_gcm_rtpsize" ||
	       mode == "xsalsa20_poly1305" || mode == "xsalsa20_poly1305_suffix" || mode == "xsalsa20_poly1305_lite";
}

bool isDaveRuntimeReady() {
	// Binary voice frames are now recognized, but MLS/libdave/SFrame are not implemented yet.
	return false;
}
} // namespace

VoiceClient &VoiceClient::getInstance() {
	static VoiceClient instance;
	return instance;
}

VoiceClient::VoiceClient()
    : state(State::DISCONNECTED), selectedEncryptionMode("xsalsa20_poly1305"), hasVoiceServerInfo(false),
      hasVoiceStateInfo(false), ssrc(0), decoder(nullptr), encoder(nullptr), sequence(0), timestamp(0),
      transportNonceCounter(0), muted(false), deafened(false), shuttingDown(false), pendingLeave(false),
      pendingLeaveNotifyGateway(false), heartbeatInterval(0), lastHeartbeatTime(0), lastDiscoveryTime(0),
      lastUdpKeepaliveTime(0), nextTransmitTime(0), discoveryRetries(0), lastVoiceGatewaySequence(0),
      captureResamplePosition(0.0) {
	memset(secretKey, 0, sizeof(secretKey));
}

VoiceClient::~VoiceClient() {
	shutdown();
}

void VoiceClient::init() {
	std::lock_guard<std::mutex> lock(voiceMutex);
	(void)lock;
}

bool VoiceClient::initializeCodecsLocked() {
	if (decoder && encoder) {
		return true;
	}

	if (sodium_init() < 0) {
		Logger::log("[Voice] Failed to initialize libsodium");
		return false;
	}

	int decodeErr = OPUS_OK;
	int encodeErr = OPUS_OK;
	decoder = opus_decoder_create(kDiscordSampleRate, 1, &decodeErr);
	encoder = opus_encoder_create(kDiscordSampleRate, 1, OPUS_APPLICATION_VOIP, &encodeErr);
	if (decodeErr != OPUS_OK || encodeErr != OPUS_OK || !decoder || !encoder) {
		Logger::log("[Voice] Opus init failed (dec=%d, enc=%d)", decodeErr, encodeErr);
		destroyCodecsLocked();
		return false;
	}

	opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000));
	opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
	opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
	opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(15));
	opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
	opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
	Logger::log("[Voice] Opus and libsodium initialized for 48kHz transport");
	return true;
}

void VoiceClient::destroyCodecsLocked() {
	if (decoder) {
		opus_decoder_destroy(decoder);
		decoder = nullptr;
	}
	if (encoder) {
		opus_encoder_destroy(encoder);
		encoder = nullptr;
	}
}

void VoiceClient::resetConnectionStateLocked() {
	state = State::DISCONNECTED;
	voiceToken.clear();
	voiceEndpoint.clear();
	voiceSessionId.clear();
	currentUserId.clear();
	selectedEncryptionMode = "xsalsa20_poly1305";
	hasVoiceServerInfo = false;
	hasVoiceStateInfo = false;
	speakingStates.clear();
	ssrc = 0;
	sequence = 0;
	timestamp = 0;
	transportNonceCounter = 0;
	memset(secretKey, 0, sizeof(secretKey));
	heartbeatInterval = 0;
	lastHeartbeatTime = 0;
	lastDiscoveryTime = 0;
	lastUdpKeepaliveTime = 0;
	nextTransmitTime = 0;
	discoveryRetries = 0;
	lastVoiceGatewaySequence = 0;
	pendingLeave = false;
	pendingLeaveNotifyGateway = false;
	capturePcmAccumulator.clear();
	micAccumulator.clear();
	decodeBuf.clear();
	encodeBuf.clear();
	captureResamplePosition = 0.0;
}

void VoiceClient::requestLeaveLocked(bool notifyGateway, const char *reason) {
	if (!pendingLeave) {
		pendingLeave = true;
		pendingLeaveNotifyGateway = notifyGateway;
	} else if (notifyGateway) {
		pendingLeaveNotifyGateway = true;
	}

	if (reason && *reason) {
		Logger::log("[Voice] Scheduling leave: %s", reason);
	}
}

void VoiceClient::leaveChannelLocked(bool notifyGateway) {
	if (state == State::DISCONNECTED && channelId.empty()) {
		return;
	}

	const std::string previousGuildId = guildId;
	const std::string previousChannelId = channelId;
	const bool shouldNotifyGateway = notifyGateway && !previousChannelId.empty();
	Logger::log("[Voice] Leaving channel (state=%d, notify=%d)", (int)state, shouldNotifyGateway ? 1 : 0);

	if (!shuttingDown) {
		Audio::AudioManager::getInstance().playSystemSound(Audio::SystemSound::LEAVE);
	}

	Audio::AudioManager::getInstance().stopCapture();
	voiceWs.setOnMessage({});
	voiceWs.setOnBinaryMessage({});
	voiceWs.setOnError({});
	voiceWs.setOnClose({});
	voiceWs.disconnect();
	udp.close();

	if (shouldNotifyGateway) {
		DiscordClient::getInstance().sendVoiceStateUpdate(previousGuildId, "", muted, deafened);
	}

	channelId.clear();
	guildId.clear();
	resetConnectionStateLocked();
	Logger::log("[Voice] Leave complete");
}

void VoiceClient::tryStartVoiceConnectionLocked() {
	if (state != State::WAITING_SERVER || channelId.empty() || !hasVoiceServerInfo || !hasVoiceStateInfo ||
	    voiceSessionId.empty() || voiceToken.empty() || voiceEndpoint.empty()) {
		return;
	}

	Logger::setCrashContext("voice: opening websocket endpoint=%s channel=%s", voiceEndpoint.c_str(), channelId.c_str());

	voiceWs.setOnMessage([this](std::string &msg) { handleVoiceWsMessage(msg); });
	voiceWs.setOnBinaryMessage([this](std::vector<uint8_t> &msg) { handleVoiceWsBinaryMessage(msg); });
	voiceWs.setOnError([this](const std::string &error) {
		std::lock_guard<std::mutex> lock(voiceMutex);
		if (shuttingDown) {
			return;
		}
		Logger::log("[Voice] Voice WebSocket error: %s", error.c_str());
		requestLeaveLocked(true, "voice websocket error");
	});
	voiceWs.setOnClose([this](int code, const std::string &reason) {
		std::lock_guard<std::mutex> lock(voiceMutex);
		if (shuttingDown) {
			return;
		}
		Logger::log("[Voice] Voice WebSocket closed: %d %s", code, reason.c_str());
		requestLeaveLocked(true, "voice websocket closed");
	});

	state = State::CONNECTING_WS;
	std::string wsUrl = "wss://" + voiceEndpoint + "/?v=8";
	Logger::log("[Voice] Connecting to Voice WebSocket: %s", wsUrl.c_str());
	if (!voiceWs.connect(wsUrl)) {
		Logger::log("[Voice] Failed to connect voice WebSocket");
		leaveChannelLocked(true);
	}
}

void VoiceClient::joinChannel(const std::string &guildId, const std::string &channelId) {
	const std::string localCurrentUserId = DiscordClient::getInstance().getCurrentUser().id;
	std::lock_guard<std::mutex> lock(voiceMutex);
	Logger::setCrashContext("voice: join requested guild=%s channel=%s", guildId.c_str(), channelId.c_str());

	if (!Config::getInstance().isVoiceChatsEnabled()) {
		Logger::log("[Voice] Voice chats are disabled in settings. Aborting join.");
		return;
	}

	if (this->channelId == channelId && state != State::DISCONNECTED) {
		Logger::log("[Voice] Already in or joining channel %s", channelId.c_str());
		return;
	}

	if (!initializeCodecsLocked()) {
		return;
	}

	if (!this->channelId.empty()) {
		Logger::log("[Voice] Switching voice channel from %s to %s", this->channelId.c_str(), channelId.c_str());
		leaveChannelLocked(true);
	}

	resetConnectionStateLocked();
	this->guildId = guildId;
	this->channelId = channelId;
	this->currentUserId = localCurrentUserId;
	if (this->currentUserId.empty()) {
		Logger::log("[Voice] Cannot join voice without a valid current user id");
		resetConnectionStateLocked();
		return;
	}
	state = State::WAITING_SERVER;

	Audio::AudioManager::getInstance().playSystemSound(Audio::SystemSound::JOIN);
	Logger::log("[Voice] Waiting for VOICE_STATE_UPDATE and VOICE_SERVER_UPDATE");
	DiscordClient::getInstance().sendVoiceStateUpdate(guildId, channelId, muted, deafened);
}

void VoiceClient::leaveChannel() {
	std::lock_guard<std::mutex> lock(voiceMutex);
	leaveChannelLocked(true);
}

void VoiceClient::shutdown() {
	std::lock_guard<std::mutex> lock(voiceMutex);
	shuttingDown = true;
	leaveChannelLocked(true);
	destroyCodecsLocked();
	shuttingDown = false;
	Logger::log("[Voice] Shutdown complete");
}

void VoiceClient::onVoiceStateUpdate(const std::string &sessionId, const std::string &guildId, const std::string &channelId) {
	std::lock_guard<std::mutex> lock(voiceMutex);
	Logger::setCrashContext("voice: gateway state update local=%s incoming=%s", this->channelId.c_str(), channelId.c_str());
	if (this->channelId.empty()) {
		return;
	}

	if (!guildId.empty() && !this->guildId.empty() && guildId != this->guildId) {
		Logger::log("[Voice] Ignoring Voice State Update for different guild %s (expected %s)", guildId.c_str(),
		            this->guildId.c_str());
		return;
	}

	if (channelId.empty()) {
		voiceSessionId.clear();
		hasVoiceStateInfo = false;
		return;
	}

	if (channelId != this->channelId) {
		Logger::log("[Voice] Ignoring Voice State Update for channel %s while waiting for %s", channelId.c_str(),
		            this->channelId.c_str());
		return;
	}

	this->guildId = guildId;
	voiceSessionId = sessionId;
	hasVoiceStateInfo = !voiceSessionId.empty();
	Logger::log("[Voice] Received Voice State session_id for channel %s", channelId.c_str());
	tryStartVoiceConnectionLocked();
}

void VoiceClient::onVoiceServerUpdate(const std::string &token, const std::string &endpoint) {
	std::lock_guard<std::mutex> lock(voiceMutex);
	Logger::setCrashContext("voice: gateway server update endpoint=%s", endpoint.c_str());
	if (state != State::WAITING_SERVER || channelId.empty()) {
		Logger::log("[Voice] Ignoring Voice Server Update: state=%d, channel=%s", (int)state, channelId.c_str());
		return;
	}

	voiceToken = token;
	voiceEndpoint = endpoint;
	size_t portPos = voiceEndpoint.find(':');
	if (portPos != std::string::npos) {
		voiceEndpoint = voiceEndpoint.substr(0, portPos);
	}
	hasVoiceServerInfo = !voiceToken.empty() && !voiceEndpoint.empty();
	Logger::log("[Voice] Received Voice Server Update for %s", voiceEndpoint.c_str());
	tryStartVoiceConnectionLocked();
}

void VoiceClient::handleVoiceWsBinaryMessage(std::vector<uint8_t> &msg) {
	std::lock_guard<std::mutex> lock(voiceMutex);
	if (shuttingDown) {
		return;
	}

	Logger::log("[Voice] Received binary Voice WebSocket payload (%u bytes)", (unsigned)msg.size());
	if (!Config::getInstance().isDaveEnabled()) {
		Logger::log("[Voice] Voice binary payload requires DAVE/MLS/E2EE support, but DAVE is disabled");
		requestLeaveLocked(true, "voice binary payload received while DAVE is disabled");
		return;
	}

	Logger::log("[Voice] DAVE/MLS/E2EE binary payload received, but MLS/libdave processing is not implemented yet");
	requestLeaveLocked(true, "voice DAVE payload not implemented");
}

void VoiceClient::handleVoiceWsMessage(std::string &msg) {
	std::lock_guard<std::mutex> lock(voiceMutex);
	if (shuttingDown) {
		return;
	}
	rapidjson::Document d;
	d.Parse(msg.c_str());
	if (d.HasParseError() || !d.IsObject() || !d.HasMember("op") || !d.HasMember("d")) {
		return;
	}

	int op = d["op"].GetInt();
	const rapidjson::Value &data = d["d"];
	Logger::setCrashContext("voice: ws opcode=%d state=%d", op, (int)state);

	switch (op) {
	case 8: // Hello
		if (data.HasMember("heartbeat_interval") && data["heartbeat_interval"].IsInt()) {
			heartbeatInterval = data["heartbeat_interval"].GetInt();
			lastHeartbeatTime = osGetTime();
			sendVoiceIdentify();
		}
		break;
	case 2: { // Ready
		if (!data.HasMember("ssrc") || !data.HasMember("ip") || !data.HasMember("port") || !data.HasMember("modes") ||
		    !data["modes"].IsArray()) {
			requestLeaveLocked(true, "voice ready payload missing required fields");
			return;
		}

		ssrc = data["ssrc"].GetUint();
		selectedEncryptionMode.clear();
		static const char *kModePreference[] = {
		    "xsalsa20_poly1305_lite",
		    "xsalsa20_poly1305_suffix",
		    "xsalsa20_poly1305",
		    "aead_xchacha20_poly1305_rtpsize",
		    "aead_aes256_gcm_rtpsize",
		};

		const rapidjson::Value &modes = data["modes"];
		for (const char *preferredMode : kModePreference) {
			for (rapidjson::SizeType i = 0; i < modes.Size(); i++) {
				if (!modes[i].IsString()) {
					continue;
				}
				const std::string mode = modes[i].GetString();
				if (mode != preferredMode || !isSupportedEncryptionMode(mode)) {
					continue;
				}
				if (mode == "aead_aes256_gcm_rtpsize" && crypto_aead_aes256gcm_is_available() != 1) {
					continue;
				}
				selectedEncryptionMode = mode;
				break;
			}
			if (selectedEncryptionMode.empty()) {
				continue;
			}
			break;
		}

		if (selectedEncryptionMode.empty()) {
			Logger::log("[Voice] No supported encryption mode offered by server");
			for (rapidjson::SizeType i = 0; i < modes.Size(); i++) {
				if (modes[i].IsString()) {
					Logger::log("[Voice] Offered mode[%u]: %s", (unsigned)i, modes[i].GetString());
				}
			}
			requestLeaveLocked(true, "no supported voice encryption mode offered");
			return;
		}

		Logger::log("[Voice] Selected encryption mode: %s", selectedEncryptionMode.c_str());
		if (!udp.connect(data["ip"].GetString(), data["port"].GetInt())) {
			Logger::log("[Voice] UDP connect failed");
			requestLeaveLocked(true, "voice udp connect failed");
			return;
		}

		state = State::DISCOVERING_IP;
		performIpDiscovery();
		break;
	}
	case 4: { // Session Description
		Logger::log("[Voice] Received Session Description");
		if (data.HasMember("mode") && data["mode"].IsString()) {
			selectedEncryptionMode = data["mode"].GetString();
		}
		if (data.HasMember("dave_protocol_version") && data["dave_protocol_version"].IsInt()) {
			const int daveProtocolVersion = data["dave_protocol_version"].GetInt();
			if (daveProtocolVersion > 0) {
				Logger::log("[Voice] DAVE protocol version %d selected by server; this backend does not implement DAVE/MLS/E2EE and cannot join this voice session",
				            daveProtocolVersion);
				requestLeaveLocked(true, "server selected DAVE protocol version > 0");
				return;
			}
		}
		if (!data.HasMember("secret_key") || !data["secret_key"].IsArray()) {
			requestLeaveLocked(true, "voice session description missing secret key");
			return;
		}

		const rapidjson::Value &keyArr = data["secret_key"];
		const size_t keyLen = std::min<size_t>(keyArr.Size(), sizeof(secretKey));
		memset(secretKey, 0, sizeof(secretKey));
		for (size_t i = 0; i < keyLen; i++) {
			secretKey[i] = keyArr[static_cast<rapidjson::SizeType>(i)].GetUint();
		}

		transportNonceCounter = 0;
		lastUdpKeepaliveTime = 0;
		nextTransmitTime = 0;
		state = State::READY;
		if (!Audio::AudioManager::getInstance().startCapture()) {
			Logger::log("[Voice] Voice transport ready, but microphone capture could not be started; continuing in receive-only mode");
		}
		sendVoiceSpeaking();
		Logger::log("[Voice] Voice transport ready");
		break;
	}
	case 5: // Speaking
		if (data.HasMember("user_id") && data["user_id"].IsString() && data.HasMember("speaking")) {
			speakingStates[data["user_id"].GetString()] = data["speaking"].GetInt() != 0;
		}
		break;
	case 6: // Heartbeat ACK
		break;
	default:
		break;
	}
}

void VoiceClient::sendVoiceIdentify() {
	if (voiceSessionId.empty() || voiceToken.empty()) {
		Logger::log("[Voice] Cannot send Identify without voice session or token");
		return;
	}

	state = State::IDENTIFYING;
	rapidjson::Document d;
	d.SetObject();
	rapidjson::Document::AllocatorType &alloc = d.GetAllocator();
	d.AddMember("op", 0, alloc);

	rapidjson::Value data(rapidjson::kObjectType);
	const std::string serverId = guildId.empty() ? channelId : guildId;
	const std::string &userId = currentUserId;
	const bool daveRequested = Config::getInstance().isDaveEnabled();
	const bool daveAdvertised = daveRequested && isDaveRuntimeReady();
	data.AddMember("server_id", rapidjson::Value(serverId.c_str(), alloc), alloc);
	data.AddMember("user_id", rapidjson::Value(userId.c_str(), alloc), alloc);
	data.AddMember("session_id", rapidjson::Value(voiceSessionId.c_str(), alloc), alloc);
	data.AddMember("token", rapidjson::Value(voiceToken.c_str(), alloc), alloc);
	data.AddMember("max_dave_protocol_version", daveAdvertised ? 1 : 0, alloc);
	d.AddMember("d", data, alloc);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	d.Accept(writer);
	voiceWs.send(buffer.GetString());
	if (daveRequested && !daveAdvertised) {
		Logger::log("[Voice] DAVE/MLS/E2EE requested in settings, but runtime support is not complete yet; advertising version 0");
	}
	Logger::log("[Voice] Sent Identify with voice session_id");
}

void VoiceClient::sendVoiceSpeaking() {
	rapidjson::Document d;
	d.SetObject();
	rapidjson::Document::AllocatorType &alloc = d.GetAllocator();
	d.AddMember("op", 5, alloc);

	rapidjson::Value data(rapidjson::kObjectType);
	data.AddMember("speaking", 1, alloc);
	data.AddMember("delay", 0, alloc);
	data.AddMember("ssrc", static_cast<uint64_t>(ssrc), alloc);
	d.AddMember("d", data, alloc);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	d.Accept(writer);
	voiceWs.send(buffer.GetString());
}

void VoiceClient::performIpDiscovery() {
	Logger::setCrashContext("voice: perform ip discovery ssrc=%u", static_cast<unsigned>(ssrc));
	uint8_t packet[74] = {0};
	packet[0] = 0x00;
	packet[1] = 0x01;
	packet[2] = 0x00;
	packet[3] = 0x46;
	const uint32_t ssrcBE = __builtin_bswap32(ssrc);
	memcpy(packet + 4, &ssrcBE, sizeof(ssrcBE));

	udp.send(packet, sizeof(packet));
	lastDiscoveryTime = osGetTime();
	discoveryRetries++;
}

void VoiceClient::sendSelectProtocol(const std::string &ip, int port) {
	state = State::SELECTING_PROTOCOL;
	Logger::setCrashContext("voice: send select protocol ip=%s port=%d mode=%s", ip.c_str(), port,
	                        selectedEncryptionMode.c_str());

	rapidjson::Document d;
	d.SetObject();
	rapidjson::Document::AllocatorType &alloc = d.GetAllocator();
	d.AddMember("op", 1, alloc);

	rapidjson::Value data(rapidjson::kObjectType);
	data.AddMember("protocol", "udp", alloc);
	rapidjson::Value protocolData(rapidjson::kObjectType);
	protocolData.AddMember("address", rapidjson::Value(ip.c_str(), alloc), alloc);
	protocolData.AddMember("port", port, alloc);
	protocolData.AddMember("mode", rapidjson::Value(selectedEncryptionMode.c_str(), alloc), alloc);
	data.AddMember("data", protocolData, alloc);
	d.AddMember("d", data, alloc);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	d.Accept(writer);
	voiceWs.send(buffer.GetString());
	Logger::log("[Voice] Sent Select Protocol using %s", selectedEncryptionMode.c_str());
}

void VoiceClient::resampleCaptureToDiscordRateLocked() {
	if (capturePcmAccumulator.size() < 2) {
		return;
	}

	const double step = static_cast<double>(kMicCaptureRate) / static_cast<double>(kDiscordSampleRate);
	double position = captureResamplePosition;
	while (position + 1.0 < static_cast<double>(capturePcmAccumulator.size())) {
		const size_t leftIndex = static_cast<size_t>(position);
		const double fraction = position - static_cast<double>(leftIndex);
		const double left = capturePcmAccumulator[leftIndex];
		const double right = capturePcmAccumulator[leftIndex + 1];
		const double sample = left + ((right - left) * fraction);
		micAccumulator.push_back(static_cast<int16_t>(sample));
		position += step;
	}

	const size_t consumedSamples = std::min(static_cast<size_t>(position), capturePcmAccumulator.size() - 1);
	for (size_t i = 0; i < consumedSamples; i++) {
		capturePcmAccumulator.pop_front();
	}
	captureResamplePosition = position - static_cast<double>(consumedSamples);
}

void VoiceClient::processIncomingAudioLocked() {
	Logger::setCrashContext("voice: process incoming audio");
	uint8_t packet[4096];
	int len = 0;
	if (pcmBuf.size() < kMaxDecodeFrameSamples) {
		pcmBuf.resize(kMaxDecodeFrameSamples);
	}

	while ((len = udp.recv(packet, sizeof(packet), 0)) > 0) {
		if (!decryptAudioPacket(packet, static_cast<size_t>(len), decodeBuf)) {
			continue;
		}

		const int samples = opus_decode(decoder, decodeBuf.data(), static_cast<opus_int32>(decodeBuf.size()),
		                                pcmBuf.data(), kMaxDecodeFrameSamples, 0);
		if (samples > 0) {
			Audio::AudioManager::getInstance().queuePcm(pcmBuf.data(), static_cast<size_t>(samples));
		}
	}
}

void VoiceClient::processOutgoingAudioLocked() {
	if (Audio::AudioManager::getInstance().hasNewSamples()) {
		int16_t tempBuf[1024];
		const size_t read = Audio::AudioManager::getInstance().readSamples(tempBuf, 1024);
		if (read > 0) {
			capturePcmAccumulator.insert(capturePcmAccumulator.end(), tempBuf, tempBuf + read);
			resampleCaptureToDiscordRateLocked();
		}
	}

	if (muted) {
		micAccumulator.clear();
		nextTransmitTime = 0;
		return;
	}

	const uint64_t now = osGetTime();
	const size_t maxBufferedSamples = static_cast<size_t>(kDiscordFrameSamples) * kMaxBufferedVoiceFrames;
	if (micAccumulator.size() > maxBufferedSamples) {
		const size_t samplesToDrop = micAccumulator.size() - maxBufferedSamples;
		for (size_t i = 0; i < samplesToDrop; i++) {
			micAccumulator.pop_front();
		}
		static uint64_t lastDropLogTime = 0;
		if (now - lastDropLogTime > 2000) {
			lastDropLogTime = now;
			Logger::log("[Voice] Dropped %u resampled MIC samples to keep voice latency bounded", (unsigned)samplesToDrop);
		}
	}

	if (nextTransmitTime == 0 || now + 250 < nextTransmitTime) {
		nextTransmitTime = now;
	}
	if (now > nextTransmitTime + 250) {
		nextTransmitTime = now;
	}

	while (micAccumulator.size() >= kDiscordFrameSamples && now + 1 >= nextTransmitTime) {
		int16_t frame[kDiscordFrameSamples];
		for (int i = 0; i < kDiscordFrameSamples; i++) {
			frame[i] = micAccumulator.front();
			micAccumulator.pop_front();
		}

		uint8_t opusBuf[1500];
		const int encodedLen = opus_encode(encoder, frame, kDiscordFrameSamples, opusBuf, sizeof(opusBuf));
		if (encodedLen <= 0) {
			continue;
		}

		encryptAudioPacket(opusBuf, static_cast<size_t>(encodedLen), encodeBuf);
		if (!encodeBuf.empty()) {
			udp.send(encodeBuf.data(), encodeBuf.size());
		}
		nextTransmitTime += kDiscordFrameDurationMs;
	}
}

size_t VoiceClient::getRtpHeaderSize(const uint8_t *data, size_t len) const {
	if (!data || len < kRtpHeaderSize) {
		return 0;
	}

	size_t headerSize = kRtpHeaderSize + static_cast<size_t>(data[0] & 0x0F) * 4;
	if (headerSize > len) {
		return 0;
	}

	const bool hasExtension = (data[0] & 0x10) != 0;
	if (!hasExtension) {
		return headerSize;
	}

	if (len < headerSize + 4) {
		return 0;
	}

	const uint16_t extensionLengthWords = static_cast<uint16_t>(data[headerSize + 2] << 8) | data[headerSize + 3];
	headerSize += 4 + static_cast<size_t>(extensionLengthWords) * 4;
	return headerSize <= len ? headerSize : 0;
}

void VoiceClient::update() {
	{
		std::lock_guard<std::mutex> lock(voiceMutex);
		Logger::setCrashContext("voice: update state=%d pending=%d", (int)state, pendingLeave ? 1 : 0);
		if (state == State::DISCONNECTED || state == State::WAITING_SERVER) {
			return;
		}
	}

	voiceWs.poll();

	std::lock_guard<std::mutex> lock(voiceMutex);
	if (pendingLeave) {
		leaveChannelLocked(pendingLeaveNotifyGateway);
		return;
	}

	if (state == State::DISCONNECTED || state == State::WAITING_SERVER) {
		return;
	}

	const uint64_t now = osGetTime();

	if (state == State::READY || state == State::DISCOVERING_IP || state == State::SELECTING_PROTOCOL) {
		if (heartbeatInterval > 0 && now - lastHeartbeatTime >= static_cast<uint64_t>(heartbeatInterval)) {
			lastHeartbeatTime = now;
			rapidjson::Document d;
			d.SetObject();
			rapidjson::Document::AllocatorType &alloc = d.GetAllocator();
			d.AddMember("op", 3, alloc);
			d.AddMember("d", static_cast<uint64_t>(now), alloc);
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			d.Accept(writer);
			voiceWs.send(buffer.GetString());
		}

		if (state == State::READY && now - lastUdpKeepaliveTime >= 5000) {
			lastUdpKeepaliveTime = now;
			performIpDiscovery();
		}
	}

	if (state == State::DISCOVERING_IP) {
		if (now - lastDiscoveryTime > 1000) {
			if (discoveryRetries < 5) {
				performIpDiscovery();
			} else {
				Logger::log("[Voice] IP discovery timed out");
				leaveChannelLocked(true);
			}
		}

		uint8_t buf[256];
		const int len = udp.recv(buf, sizeof(buf), 0);
		if (len >= 74) {
			char ip[65] = {0};
			memcpy(ip, buf + 8, 64);
			uint16_t portBE = 0;
			memcpy(&portBE, buf + 72, sizeof(portBE));
			const uint16_t discoveredPort = __builtin_bswap16(portBE);
			if (ip[0] != '\0' && discoveredPort > 0) {
				Logger::log("[Voice] IP discovered: %s:%u", ip, discoveredPort);
				sendSelectProtocol(ip, discoveredPort);
			}
		}
	} else if (state == State::READY) {
		processIncomingAudioLocked();
		processOutgoingAudioLocked();
	}
}

void VoiceClient::encryptAudioPacket(const uint8_t *opus, size_t len, std::vector<uint8_t> &out) {
	uint8_t header[kRtpHeaderSize];
	header[0] = 0x80;
	header[1] = kRtpPayloadType;
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

	out.clear();
	if (selectedEncryptionMode == "xsalsa20_poly1305") {
		uint8_t nonce[kLegacyNonceSize];
		buildLegacyNonceFromHeader(header, nonce);
		out.resize(kRtpHeaderSize + len + kSecretBoxMacSize);
		memcpy(out.data(), header, kRtpHeaderSize);
		crypto_secretbox_easy(out.data() + kRtpHeaderSize, opus, len, nonce, secretKey);
	} else if (selectedEncryptionMode == "xsalsa20_poly1305_suffix") {
		uint8_t nonce[kLegacyNonceSize];
		randombytes_buf(nonce, sizeof(nonce));
		out.resize(kRtpHeaderSize + len + kSecretBoxMacSize + sizeof(nonce));
		memcpy(out.data(), header, kRtpHeaderSize);
		crypto_secretbox_easy(out.data() + kRtpHeaderSize, opus, len, nonce, secretKey);
		memcpy(out.data() + kRtpHeaderSize + len + kSecretBoxMacSize, nonce, sizeof(nonce));
	} else if (selectedEncryptionMode == "xsalsa20_poly1305_lite") {
		uint8_t nonce[kLegacyNonceSize] = {0};
		const uint32_t counter = transportNonceCounter++;
		writeBigEndianCounter(nonce, counter);
		out.resize(kRtpHeaderSize + len + kSecretBoxMacSize + kLiteNonceSuffixSize);
		memcpy(out.data(), header, kRtpHeaderSize);
		crypto_secretbox_easy(out.data() + kRtpHeaderSize, opus, len, nonce, secretKey);
		writeBigEndianCounter(out.data() + kRtpHeaderSize + len + kSecretBoxMacSize, counter);
	} else if (selectedEncryptionMode == "aead_xchacha20_poly1305_rtpsize") {
		uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {0};
		const uint32_t counter = transportNonceCounter++;
		writeBigEndianCounter(nonce, counter);
		unsigned long long cipherLen = 0;
		out.resize(kRtpHeaderSize + len + kXChaChaTagSize + kLiteNonceSuffixSize);
		memcpy(out.data(), header, kRtpHeaderSize);
		if (crypto_aead_xchacha20poly1305_ietf_encrypt(out.data() + kRtpHeaderSize, &cipherLen, opus, len, header,
		                                                kRtpHeaderSize, nullptr, nonce, secretKey) != 0) {
			out.clear();
		} else {
			out.resize(kRtpHeaderSize + static_cast<size_t>(cipherLen) + kLiteNonceSuffixSize);
			writeBigEndianCounter(out.data() + kRtpHeaderSize + cipherLen, counter);
		}
	} else if (selectedEncryptionMode == "aead_aes256_gcm_rtpsize" && crypto_aead_aes256gcm_is_available() == 1) {
		uint8_t nonce[crypto_aead_aes256gcm_NPUBBYTES] = {0};
		const uint32_t counter = transportNonceCounter++;
		writeBigEndianCounter(nonce, counter);
		unsigned long long cipherLen = 0;
		out.resize(kRtpHeaderSize + len + crypto_aead_aes256gcm_ABYTES + kLiteNonceSuffixSize);
		memcpy(out.data(), header, kRtpHeaderSize);
		if (crypto_aead_aes256gcm_encrypt(out.data() + kRtpHeaderSize, &cipherLen, opus, len, header, kRtpHeaderSize,
		                                  nullptr, nonce, secretKey) != 0) {
			out.clear();
		} else {
			out.resize(kRtpHeaderSize + static_cast<size_t>(cipherLen) + kLiteNonceSuffixSize);
			writeBigEndianCounter(out.data() + kRtpHeaderSize + cipherLen, counter);
		}
	}

	sequence++;
	timestamp += kDiscordFrameSamples;
}

bool VoiceClient::decryptAudioPacket(const uint8_t *data, size_t len, std::vector<uint8_t> &out) {
	const size_t headerSize = getRtpHeaderSize(data, len);
	if (headerSize < kRtpHeaderSize) {
		return false;
	}

	if (selectedEncryptionMode == "xsalsa20_poly1305") {
		if (len < headerSize + kSecretBoxMacSize) {
			return false;
		}
		uint8_t nonce[kLegacyNonceSize];
		buildLegacyNonceFromHeader(data, nonce);
		const size_t cipherLen = len - headerSize;
		out.resize(cipherLen - kSecretBoxMacSize);
		return crypto_secretbox_open_easy(out.data(), data + headerSize, cipherLen, nonce, secretKey) == 0;
	}

	if (selectedEncryptionMode == "xsalsa20_poly1305_suffix") {
		if (len < headerSize + kSecretBoxMacSize + kLegacyNonceSize) {
			return false;
		}
		const size_t cipherLen = len - headerSize - kLegacyNonceSize;
		const uint8_t *nonce = data + len - kLegacyNonceSize;
		out.resize(cipherLen - kSecretBoxMacSize);
		return crypto_secretbox_open_easy(out.data(), data + headerSize, cipherLen, nonce, secretKey) == 0;
	}

	if (selectedEncryptionMode == "xsalsa20_poly1305_lite") {
		if (len < headerSize + kSecretBoxMacSize + kLiteNonceSuffixSize) {
			return false;
		}
		const size_t cipherLen = len - headerSize - kLiteNonceSuffixSize;
		uint8_t nonce[kLegacyNonceSize] = {0};
		memcpy(nonce, data + len - kLiteNonceSuffixSize, kLiteNonceSuffixSize);
		out.resize(cipherLen - kSecretBoxMacSize);
		return crypto_secretbox_open_easy(out.data(), data + headerSize, cipherLen, nonce, secretKey) == 0;
	}

	if (selectedEncryptionMode == "aead_xchacha20_poly1305_rtpsize") {
		if (len < headerSize + kXChaChaTagSize + kLiteNonceSuffixSize) {
			return false;
		}
		const size_t cipherLen = len - headerSize - kLiteNonceSuffixSize;
		uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {0};
		memcpy(nonce, data + len - kLiteNonceSuffixSize, kLiteNonceSuffixSize);
		unsigned long long plainLen = 0;
		out.resize(cipherLen - kXChaChaTagSize);
		return crypto_aead_xchacha20poly1305_ietf_decrypt(out.data(), &plainLen, nullptr, data + headerSize, cipherLen,
		                                                   data, headerSize, nonce, secretKey) == 0;
	}

	if (selectedEncryptionMode == "aead_aes256_gcm_rtpsize" && crypto_aead_aes256gcm_is_available() == 1) {
		if (len < headerSize + crypto_aead_aes256gcm_ABYTES + kLiteNonceSuffixSize) {
			return false;
		}
		const size_t cipherLen = len - headerSize - kLiteNonceSuffixSize;
		uint8_t nonce[crypto_aead_aes256gcm_NPUBBYTES] = {0};
		memcpy(nonce, data + len - kLiteNonceSuffixSize, kLiteNonceSuffixSize);
		unsigned long long plainLen = 0;
		out.resize(cipherLen - crypto_aead_aes256gcm_ABYTES);
		return crypto_aead_aes256gcm_decrypt(out.data(), &plainLen, nullptr, data + headerSize, cipherLen, data,
		                                     headerSize, nonce, secretKey) == 0;
	}

	return false;
}

bool VoiceClient::isUserSpeaking(const std::string &userId) const {
	std::lock_guard<std::mutex> lock(voiceMutex);
	auto it = speakingStates.find(userId);
	return it != speakingStates.end() ? it->second : false;
}

void VoiceClient::setMuted(bool mute) {
	std::lock_guard<std::mutex> lock(voiceMutex);
	if (muted == mute) {
		return;
	}

	muted = mute;
	Audio::AudioManager::getInstance().playSystemSound(muted ? Audio::SystemSound::MUTE : Audio::SystemSound::UNMUTE);
	if (!channelId.empty()) {
		DiscordClient::getInstance().sendVoiceStateUpdate(guildId, channelId, muted, deafened);
	}
}

void VoiceClient::setDeafened(bool deaf) {
	std::lock_guard<std::mutex> lock(voiceMutex);
	if (deafened == deaf) {
		return;
	}

	deafened = deaf;
	if (!channelId.empty()) {
		DiscordClient::getInstance().sendVoiceStateUpdate(guildId, channelId, muted, deafened);
	}
}

bool VoiceClient::isMuted() const { return muted; }
bool VoiceClient::isDeafened() const { return deafened; }

bool VoiceClient::isConnected() const {
	std::lock_guard<std::mutex> lock(voiceMutex);
	return state == State::READY;
}

bool VoiceClient::isInChannel() const {
	std::lock_guard<std::mutex> lock(voiceMutex);
	return !channelId.empty();
}

std::string VoiceClient::getCurrentChannelId() const {
	std::lock_guard<std::mutex> lock(voiceMutex);
	return channelId;
}

std::string VoiceClient::getCurrentGuildId() const {
	std::lock_guard<std::mutex> lock(voiceMutex);
	return guildId;
}

} // namespace Discord
