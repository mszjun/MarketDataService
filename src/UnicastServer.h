#ifndef MDGW_UNICAST_SERVER_H
#define MDGW_UNICAST_SERVER_H

#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <asio.hpp>
#include "FanoutTypes.h"
#include "Logger.h"

namespace mdgw
{
	class MarketDataDispatcher;

	// One accepted TCP connection that receives the CRLF-delimited JSON lines.
	// Ported from the original MarketDataUnicastDispatcher / UnicastSocketConnection:
	// - every connection keeps its own outbound FIFO, written one message at a
	//   time with async_write;
	// - when a slow connection accumulates too many messages the queue is cut
	//   and rebuilt from the latest snapshot items whose seq is newer than the
	//   last message actually sent;
	// - on any write error the connection is closed and removed.
	class UnicastSocketConnection : public std::enable_shared_from_this<UnicastSocketConnection>
	{
	private:
		enum
		{
			MAX_QUEUED_MESSAGES = 1000
		};

		MarketDataDispatcher * const unicastDispatcher;
		asio::ip::tcp::socket theSocket;

		typedef std::deque<EncodedItem> SendQueue_t;
		SendQueue_t sendQueue;
		long long maxSentMessageSeq;

		void startSend()
		{
			asio::async_write(
				theSocket,
				asio::buffer(sendQueue.front().data.data(), sendQueue.front().data.length()),
				std::bind(&UnicastSocketConnection::handleWrite, shared_from_this(),
					std::placeholders::_1));

			if (sendQueue.front().seq > maxSentMessageSeq)
				maxSentMessageSeq = sendQueue.front().seq;
		}

		void handleWrite(const std::error_code & error);

	public:
		UnicastSocketConnection(MarketDataDispatcher * dispatcher, asio::io_context & io_context)
			: unicastDispatcher(dispatcher), theSocket(io_context), maxSentMessageSeq(0)
		{
		}

		virtual ~UnicastSocketConnection()
		{
		}

		asio::ip::tcp::socket & socket() { return theSocket; }

		void sendEncodedItem(const EncodedItem & message);

		void close()
		{
			std::error_code ec;
			theSocket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
			theSocket.close(ec);
		}
	};

	class UnicastServer
	{
	private:
		MarketDataDispatcher * const marketDataDispatcher;

		asio::io_context & theIOService;
		asio::ip::tcp::acceptor theAcceptor;
		int maxConnections;

		typedef std::set<std::shared_ptr<UnicastSocketConnection> > Connections_t;
		Connections_t connections;

		void handle_accept(std::shared_ptr<UnicastSocketConnection> connection,
			const std::error_code & error);

		void startAccept()
		{
			std::shared_ptr<UnicastSocketConnection> newConnection(
				new UnicastSocketConnection(marketDataDispatcher, theIOService));

			theAcceptor.async_accept(newConnection->socket(),
				std::bind(&UnicastServer::handle_accept, this, newConnection,
					std::placeholders::_1));
		}

	public:
		UnicastServer(MarketDataDispatcher * dispatcher, asio::io_context & io_context,
			const std::string & serverName, int port, int maxConn);

		const Snapshot & snapshot() const;

		void connectionGoodbye(std::shared_ptr<UnicastSocketConnection> conn)
		{
			connections.erase(conn);
			MD_LOG_INFO("unicast connection [%p] removed, connections [%d]",
				conn.get(), (int)connections.size());
		}

		void sendToAll(const EncodedItem & msg)
		{
			for (Connections_t::const_iterator it = connections.begin(); it != connections.end(); it++)
			{
				(*it)->sendEncodedItem(msg);
			}
		}
	};
}

#endif // MDGW_UNICAST_SERVER_H