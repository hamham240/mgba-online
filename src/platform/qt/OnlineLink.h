#ifndef MGBA_ONLINE_LINK_H
#define MGBA_ONLINE_LINK_H

#include <asio.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace QGBA {
namespace online {

// Shared-buffer layout, as offsets from the start of the general buffer (0x10000000).
//
// Legacy mirror (overworld sync):
//   0x00000          u8   connected
//   0x00001-0x00200       local state, sent to the peer every frame
//   0x01001-0x01200       peer state, latest received
//
// Message pipe:
//   PIPE + 0x00      u8   connected
//   PIPE + 0x01      u8   player id (0 = host/master, 1 = joined)
//   PIPE + 0x04      u32  outbox write index (ROM)
//   PIPE + 0x08      u32  outbox read index  (emulator)
//   PIPE + 0x0C      u32  inbox write index  (emulator)
//   PIPE + 0x10      u32  inbox read index   (ROM)
//   PIPE + 0x100          outbox ring
//   PIPE + 0x100 + RING   inbox ring
//
// Indices are free-running byte counters; a ring position is index % RING_SIZE.
// Each message in a ring is a little-endian u16 length followed by the payload.
// The emulator only touches the pipe between frames, so the ROM never sees a
// partially written message.
constexpr std::size_t LEGACY_STATE_OFFSET = 0x1;
constexpr std::size_t LEGACY_PEER_OFFSET = 0x1001;
constexpr std::size_t LEGACY_STATE_SIZE = 0x200;

constexpr std::size_t PIPE_OFFSET = 0x10000;
constexpr std::size_t PIPE_CONNECTED = PIPE_OFFSET + 0x00;
constexpr std::size_t PIPE_PLAYER_ID = PIPE_OFFSET + 0x01;
constexpr std::size_t PIPE_OUT_WRITE = PIPE_OFFSET + 0x04;
constexpr std::size_t PIPE_OUT_READ = PIPE_OFFSET + 0x08;
constexpr std::size_t PIPE_IN_WRITE = PIPE_OFFSET + 0x0C;
constexpr std::size_t PIPE_IN_READ = PIPE_OFFSET + 0x10;
constexpr std::size_t PIPE_RING_SIZE = 0x4000;
constexpr std::size_t PIPE_OUTBOX = PIPE_OFFSET + 0x100;
constexpr std::size_t PIPE_INBOX = PIPE_OUTBOX + PIPE_RING_SIZE;

class OnlineLink {
public:
	enum class Role {
		Host,
		Join,
	};

	static std::shared_ptr<OnlineLink> create(Role role, const std::string& address, std::uint16_t port);
	~OnlineLink();

	// Called on the emulator thread once per frame.
	void pump(std::uint8_t* generalBuffer);

	bool connected() const { return m_connected; }

private:
	enum class FrameType : std::uint8_t {
		LegacyState = 0,
		Message = 1,
	};

	OnlineLink(Role role);

	void start(const std::string& address, std::uint16_t port);
	void onConnected();
	void readHeader();
	void readBody();
	void send(FrameType type, const std::uint8_t* data, std::size_t size);
	void writeNext();
	void fail(const char* what, const asio::error_code& ec);

	Role m_role;
	std::atomic_bool m_connected{false};

	asio::io_context m_io;
	asio::executor_work_guard<asio::io_context::executor_type> m_work;
	std::unique_ptr<asio::ip::tcp::acceptor> m_acceptor;
	asio::ip::tcp::socket m_socket;
	std::thread m_thread;

	// io thread only
	std::uint8_t m_readHeader[3];
	std::vector<std::uint8_t> m_readBody;
	std::deque<std::vector<std::uint8_t>> m_writeQueue;

	// shared between the io thread and the emulator thread
	std::mutex m_mutex;
	std::deque<std::vector<std::uint8_t>> m_incoming;
	std::vector<std::uint8_t> m_peerState;
};

} // namespace online
} // namespace QGBA

#endif
