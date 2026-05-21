#ifndef UDP_CLIENT_H
#define UDP_CLIENT_H

#include <cstdint>
#include <string>
#include <vector>

namespace Network {

class UdpClient {
  public:
	UdpClient();
	~UdpClient();

	bool connect(const std::string &host, int port);
	void close();
	bool isConnected() const;

	bool send(const uint8_t *data, size_t len);
	int recv(uint8_t *buffer, size_t maxLen, int timeoutMs);

  private:
	int sockfd;
	bool connected;
};

} // namespace Network

#endif // UDP_CLIENT_H
