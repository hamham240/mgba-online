#include "OnlineLink.h"

#include <mgba/core/log.h>

#include <cstring>

mLOG_DEFINE_CATEGORY(ONLINE, "Online", "platform.qt.online");

namespace QGBA {
namespace online {

namespace {

std::uint32_t read32(const std::uint8_t* buffer, std::size_t offset) {
	std::uint32_t value;
	std::memcpy(&value, &buffer[offset], sizeof(value));
	return value;
}

void write32(std::uint8_t* buffer, std::size_t offset, std::uint32_t value) {
	std::memcpy(&buffer[offset], &value, sizeof(value));
}

std::uint8_t ringRead(const std::uint8_t* ring, std::uint32_t index) {
	return ring[index % PIPE_RING_SIZE];
}

void ringWrite(std::uint8_t* ring, std::uint32_t index, std::uint8_t value) {
	ring[index % PIPE_RING_SIZE] = value;
}

} // namespace

std::shared_ptr<OnlineLink> OnlineLink::create(Role role, const std::string& address, std::uint16_t port) {
	std::shared_ptr<OnlineLink> link(new OnlineLink(role));
	link->start(address, port);
	return link;
}

OnlineLink::OnlineLink(Role role)
	: m_role(role)
	, m_work(asio::make_work_guard(m_io))
	, m_socket(m_io) {
}

OnlineLink::~OnlineLink() {
	m_work.reset();
	m_io.stop();
	if (m_thread.joinable()) {
		m_thread.join();
	}
}

void OnlineLink::start(const std::string& address, std::uint16_t port) {
	using tcp = asio::ip::tcp;

	if (m_role == Role::Host) {
		m_acceptor = std::make_unique<tcp::acceptor>(m_io, tcp::endpoint(tcp::v4(), port));
		mLOG(ONLINE, INFO, "Hosting on port %u, waiting for a player to join", port);
		m_acceptor->async_accept(m_socket, [this](const asio::error_code& ec) {
			if (ec) {
				fail("accept", ec);
				return;
			}
			onConnected();
		});
	} else {
		tcp::endpoint endpoint(asio::ip::make_address(address), port);
		mLOG(ONLINE, INFO, "Joining %s:%u", address.c_str(), port);
		m_socket.async_connect(endpoint, [this](const asio::error_code& ec) {
			if (ec) {
				fail("connect", ec);
				return;
			}
			onConnected();
		});
	}

	m_thread = std::thread([this]() {
		m_io.run();
	});
}

void OnlineLink::onConnected() {
	asio::error_code ec;
	m_socket.set_option(asio::ip::tcp::no_delay(true), ec);
	m_connected = true;
	mLOG(ONLINE, INFO, "Connected as player %u", m_role == Role::Host ? 0 : 1);
	readHeader();
}

void OnlineLink::readHeader() {
	asio::async_read(m_socket, asio::buffer(m_readHeader), [this](const asio::error_code& ec, std::size_t) {
		if (ec) {
			fail("read", ec);
			return;
		}
		std::size_t size = m_readHeader[1] | (m_readHeader[2] << 8);
		m_readBody.resize(size);
		readBody();
	});
}

void OnlineLink::readBody() {
	asio::async_read(m_socket, asio::buffer(m_readBody), [this](const asio::error_code& ec, std::size_t) {
		if (ec) {
			fail("read", ec);
			return;
		}
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			switch (static_cast<FrameType>(m_readHeader[0])) {
			case FrameType::LegacyState:
				m_peerState = m_readBody;
				break;
			case FrameType::Message:
				m_incoming.push_back(m_readBody);
				break;
			default:
				mLOG(ONLINE, WARN, "Dropping frame with unknown type %u", m_readHeader[0]);
				break;
			}
		}
		readHeader();
	});
}

void OnlineLink::send(FrameType type, const std::uint8_t* data, std::size_t size) {
	std::vector<std::uint8_t> frame(3 + size);
	frame[0] = static_cast<std::uint8_t>(type);
	frame[1] = size & 0xFF;
	frame[2] = (size >> 8) & 0xFF;
	std::memcpy(&frame[3], data, size);

	asio::post(m_io, [this, frame = std::move(frame)]() mutable {
		bool idle = m_writeQueue.empty();
		m_writeQueue.push_back(std::move(frame));
		if (idle) {
			writeNext();
		}
	});
}

void OnlineLink::writeNext() {
	asio::async_write(m_socket, asio::buffer(m_writeQueue.front()), [this](const asio::error_code& ec, std::size_t) {
		if (ec) {
			fail("write", ec);
			return;
		}
		m_writeQueue.pop_front();
		if (!m_writeQueue.empty()) {
			writeNext();
		}
	});
}

void OnlineLink::fail(const char* what, const asio::error_code& ec) {
	if (m_connected.exchange(false)) {
		mLOG(ONLINE, ERROR, "Disconnected (%s): %s", what, ec.message().c_str());
	} else {
		mLOG(ONLINE, ERROR, "Failed to %s: %s", what, ec.message().c_str());
	}
	asio::error_code ignored;
	m_socket.close(ignored);
}

void OnlineLink::pump(std::uint8_t* buffer) {
	bool connected = m_connected;
	buffer[0] = connected;
	buffer[PIPE_CONNECTED] = connected;
	buffer[PIPE_PLAYER_ID] = m_role == Role::Host ? 0 : 1;

	if (!connected) {
		return;
	}

	send(FrameType::LegacyState, &buffer[LEGACY_STATE_OFFSET], LEGACY_STATE_SIZE);

	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_peerState.size() == LEGACY_STATE_SIZE) {
		std::memcpy(&buffer[LEGACY_PEER_OFFSET], m_peerState.data(), LEGACY_STATE_SIZE);
	}

	std::uint8_t* outbox = &buffer[PIPE_OUTBOX];
	std::uint32_t outWrite = read32(buffer, PIPE_OUT_WRITE);
	std::uint32_t outRead = read32(buffer, PIPE_OUT_READ);
	while (outRead != outWrite) {
		std::uint32_t available = outWrite - outRead;
		std::uint16_t size = ringRead(outbox, outRead) | (ringRead(outbox, outRead + 1) << 8);
		if (available > PIPE_RING_SIZE || available < 2u + size) {
			mLOG(ONLINE, ERROR, "Outbox is corrupt (read %u, write %u, size %u); discarding it", outRead, outWrite, size);
			outRead = outWrite;
			break;
		}
		std::vector<std::uint8_t> message(size);
		for (std::uint16_t i = 0; i < size; ++i) {
			message[i] = ringRead(outbox, outRead + 2 + i);
		}
		send(FrameType::Message, message.data(), message.size());
		outRead += 2 + size;
	}
	write32(buffer, PIPE_OUT_READ, outRead);

	std::uint8_t* inbox = &buffer[PIPE_INBOX];
	std::uint32_t inWrite = read32(buffer, PIPE_IN_WRITE);
	std::uint32_t inRead = read32(buffer, PIPE_IN_READ);
	while (!m_incoming.empty()) {
		const std::vector<std::uint8_t>& message = m_incoming.front();
		std::uint32_t needed = 2 + message.size();
		if (PIPE_RING_SIZE - (inWrite - inRead) < needed) {
			break;
		}
		ringWrite(inbox, inWrite, message.size() & 0xFF);
		ringWrite(inbox, inWrite + 1, (message.size() >> 8) & 0xFF);
		for (std::size_t i = 0; i < message.size(); ++i) {
			ringWrite(inbox, inWrite + 2 + i, message[i]);
		}
		inWrite += needed;
		m_incoming.pop_front();
	}
	write32(buffer, PIPE_IN_WRITE, inWrite);
}

} // namespace online
} // namespace QGBA
