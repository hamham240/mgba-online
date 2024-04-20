#ifndef MGBA_ONLINE_SERVER_H
#define MGBA_ONLINE_SERVER_H

#include <asio.hpp>

#include <atomic>
#include <cstdint>
#include <memory>

namespace QGBA {
namespace online {

class Server {
public:
	Server(const std::uint16_t a_port, std::uint8_t* a_general_buffer, std::atomic_bool& a_stop_token);

	void exec();

	std::uint8_t* general_buffer() {
		return m_general_buffer;
	}

	void set_general_buffer(std::uint8_t* a_general_buffer) {
		m_general_buffer = a_general_buffer;
	}

private:
	std::uint8_t* m_general_buffer;

	asio::error_code m_error_code;
	std::shared_ptr<asio::io_context> m_io_ctx;
	std::shared_ptr<asio::ip::tcp::endpoint> m_endpoint;
	std::shared_ptr<asio::ip::tcp::socket> m_socket;
	std::shared_ptr<asio::ip::tcp::acceptor> m_acceptor;

	std::atomic_bool& m_stop_token;

};

} // namespace online
} // namespace QGBA

#endif
