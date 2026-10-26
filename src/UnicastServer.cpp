#include "UnicastServer.h"

#include <string>
#include "Logger.h"
#include "MarketDataDispatcher.h"

namespace mdgw
{
	void UnicastSocketConnection::handleWrite(const std::error_code & error)
	{
		if (!error)
		{
			sendQueue.pop_front();
			if (!sendQueue.empty())
			{
				startSend();
			}
		}
		else
		{
			// connection dropped: remove it and close the socket
			unicastDispatcher->removeConnection(shared_from_this());

			MD_LOG_DEBUG("unicast connection closed: error %d [%s]",
				error.value(), error.message().c_str());
			close();
		}
	}

	void UnicastSocketConnection::sendEncodedItem(const EncodedItem & message)
	{
		bool writing = !sendQueue.empty();
		sendQueue.push_back(message);

		if (!writing)
		{
			startSend();
		}
		else if (sendQueue.size() > MAX_QUEUED_MESSAGES &&
			sendQueue.size() > 3 * unicastDispatcher->snapshot().size())
		{
			MD_LOG_DEBUG("unicast socket [%p] queue cut, max sent seq [%lld], original size [%d]",
				this, maxSentMessageSeq, (int)sendQueue.size());

			SendQueue_t::iterator it = sendQueue.begin();
			it++;
			sendQueue.erase(it, sendQueue.end());

			const Snapshot & snapshot = unicastDispatcher->snapshot();
			for (Snapshot::const_iterator sit = snapshot.begin(); sit != snapshot.end(); sit++)
			{
				if (sit->second.seq > maxSentMessageSeq)
					sendQueue.push_back(sit->second);
			}
		}
	}

	UnicastServer::UnicastServer(MarketDataDispatcher * dispatcher, asio::io_context & io_context,
		const std::string & serverName, int port, int maxConn)
		: marketDataDispatcher(dispatcher), theIOService(io_context), theAcceptor(io_context),
		maxConnections(maxConn)
	{
		asio::ip::tcp::resolver resolver(theIOService);
		asio::ip::tcp::resolver::results_type endpoints =
			resolver.resolve(serverName, std::to_string(port));
		asio::ip::tcp::endpoint endpoint = *endpoints.begin();

		theAcceptor.open(endpoint.protocol());
		theAcceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
		theAcceptor.bind(endpoint);
		theAcceptor.listen();

		startAccept();

		MD_LOG_INFO("unicast server started at [%s/%d]", serverName.c_str(), port);
	}

	void UnicastServer::handle_accept(std::shared_ptr<UnicastSocketConnection> connection,
		const std::error_code & error)
	{
		if (!error)
		{
			if ((int)connections.size() < maxConnections)
			{
				connections.insert(connection);

				std::error_code ec;
				MD_LOG_INFO("new unicast connection accepted, remote [%s], connections [%d]",
					connection->socket().remote_endpoint(ec).address().to_string().c_str(),
					(int)connections.size());
			}
			else
			{
				MD_LOG_WARN("new unicast connection rejected: max connections [%d] reached",
					maxConnections);
			}

			startAccept();
		}
		else
		{
			if (error != asio::error::operation_aborted)
			{
				MD_LOG_WARN("unicast accept error: %d [%s]",
					error.value(), error.message().c_str());
			}
		}
	}

	const Snapshot & UnicastServer::snapshot() const
	{
		return marketDataDispatcher->snapshot();
	}
}