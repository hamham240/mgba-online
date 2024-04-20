#include "Client.h"

#include <mgba/gba/core.h>

namespace QGBA {
namespace online {

Client::Client(const std::uint16_t a_port, std::uint8_t* a_general_buffer, std::atomic_bool& a_stop_token) 
	: m_general_buffer(a_general_buffer)
	, m_stop_token(a_stop_token) {
	
	using tcp = asio::ip::tcp;

	m_io_ctx = std::make_shared<asio::io_context>();

	m_endpoint = std::make_shared<tcp::endpoint>(tcp::v4(), a_port);

	m_socket = std::make_shared<tcp::socket>(*m_io_ctx);
}

void Client::exec() {
	m_error_code = m_socket->connect(*m_endpoint, m_error_code);

	if (m_error_code) {
		return;
	}

	// Mark connected byte
	m_general_buffer[0] = 1;

	while(true) {
		m_socket->receive(asio::buffer(&m_general_buffer[0x1001], 0x200));
		m_socket->send(asio::buffer(&m_general_buffer[0x1], 0x200));
	}
}

} // namespace online
} // namespace QGBA
