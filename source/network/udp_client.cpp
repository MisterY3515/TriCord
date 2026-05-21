#include "network/udp_client.h"
#include "log.h"

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Network {

UdpClient::UdpClient() : sockfd(-1), connected(false) {}

UdpClient::~UdpClient() { close(); }

bool UdpClient::connect(const std::string &host, int port) {
	if (connected) {
		close();
	}

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		Logger::log("[UDP] Failed to create socket");
		return false;
	}

	struct hostent *server = gethostbyname(host.c_str());
	if (!server) {
		Logger::log("[UDP] DNS resolution failed for %s", host.c_str());
		close();
		return false;
	}

	struct sockaddr_in serv_addr;
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
	serv_addr.sin_port = htons(port);

	if (::connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		Logger::log("[UDP] Connection failed to %s:%d", host.c_str(), port);
		close();
		return false;
	}

	// Set socket to non-blocking
	int flags = fcntl(sockfd, F_GETFL, 0);
	if (flags != -1) {
		fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
	}

	connected = true;
	Logger::log("[UDP] Connected to %s:%d", host.c_str(), port);
	return true;
}

void UdpClient::close() {
	if (sockfd >= 0) {
		::close(sockfd);
		sockfd = -1;
	}
	connected = false;
}

bool UdpClient::isConnected() const { return connected; }

bool UdpClient::send(const uint8_t *data, size_t len) {
	if (!connected || sockfd < 0) {
		return false;
	}

	ssize_t sent = ::send(sockfd, data, len, 0);
	if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		Logger::log("[UDP] Send error: %d", errno);
		return false;
	}
	return true;
}

int UdpClient::recv(uint8_t *buffer, size_t maxLen, int timeoutMs) {
	if (!connected || sockfd < 0) {
		return -1;
	}

	ssize_t bytesRead = ::recv(sockfd, buffer, maxLen, 0);
	if (bytesRead > 0) {
		return bytesRead;
	} else if (bytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		Logger::log("[UDP] Recv error: %d", errno);
		return -1;
	}
	
	return 0; // Timeout or no data
}

} // namespace Network
