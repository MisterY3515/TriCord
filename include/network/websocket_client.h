#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Network {

enum class WebSocketState { DISCONNECTED, CONNECTING, CONNECTED, CLOSING, CLOSED };

enum class WebSocketOpcode : uint8_t {
	CONTINUATION = 0x0,
	TEXT = 0x1,
	BINARY = 0x2,
	CLOSE = 0x8,
	PING = 0x9,
	PONG = 0xA
};

class WebSocketClient {
  public:
	using MessageCallback = std::function<void(std::string &)>;
	using BinaryMessageCallback = std::function<void(std::vector<uint8_t> &)>;
	using ErrorCallback = std::function<void(const std::string &)>;
	using CloseCallback = std::function<void(int code, const std::string &reason)>;

	WebSocketClient();
	~WebSocketClient();

	WebSocketClient(const WebSocketClient &) = delete;
	WebSocketClient &operator=(const WebSocketClient &) = delete;

	bool connect(const std::string &url);
	void disconnect(int code = 1000, const std::string &reason = "");
	bool isConnected() const;
	WebSocketState getState() const;

	bool send(const std::string &message);
	bool sendBinary(const std::vector<uint8_t> &data);

	void setOnMessage(MessageCallback callback);
	void setOnBinaryMessage(BinaryMessageCallback callback);
	void setOnError(ErrorCallback callback);
	void setOnClose(CloseCallback callback);

	void poll();
	void forceClose();

  private:
	int sockfd;
	WebSocketState state;
	std::string host;
	int port;
	std::string path;
	bool useTLS;

	MessageCallback onMessage;
	BinaryMessageCallback onBinaryMessage;
	ErrorCallback onError;
	CloseCallback onClose;

	std::vector<uint8_t> receiveBuffer;
	WebSocketOpcode fragmentedOpcode;
	bool fragmentedMessageInProgress;

	void *sslContext;
	void *sslConfig;
	void *ctrDrbg;
	void *entropy;
	void *serverFd;

	bool parseUrl(const std::string &url);
	bool performHandshake();
	void cleanupTLS();

	int rawSend(const void *data, size_t len);
	int rawRecv(void *data, size_t len);
	bool recvExact(void *data, size_t len);

	bool sendFrame(WebSocketOpcode opcode, const void *data, size_t len);
	bool sendFrameLocked(WebSocketOpcode opcode, const void *data, size_t len);

	enum class ReceiveResult { NONE, TEXT_MESSAGE, BINARY_MESSAGE, CLOSE, ERROR };
	ReceiveResult receiveFrame(std::string &message, std::vector<uint8_t> &binaryMessage, int &closeCode,
	                           std::string &closeReason, std::string &error);
	void disconnectLocked(int code, const std::string &reason, bool sendCloseFrame);

	std::mutex sendMutex;
	mutable std::mutex ioMutex;

	std::string generateWebSocketKey();
};

} // namespace Network

#endif // WEBSOCKET_CLIENT_H
