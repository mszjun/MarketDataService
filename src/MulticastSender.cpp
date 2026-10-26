#include "MulticastSender.h"

#include "Logger.h"
#include "MarketDataDispatcher.h"

namespace mdgw
{
	MulticastSender::MulticastSender(MarketDataDispatcher * dispatcher_, asio::io_context & io,
		const std::string & multicast_address, int port, int hops)
		: dispatcher(dispatcher_), io_service(io),
		endPoint(asio::ip::address::from_string(multicast_address), (unsigned short)port),
		socket(io_service, endPoint.protocol()), maxSentMessageSeq(0)
	{
		asio::ip::multicast::hops option(hops);
		socket.set_option(option);

		MD_LOG_INFO("multicast sender started at [%s/%d]", multicast_address.c_str(), port);
	}

	void MulticastSender::handleWrite(const std::error_code & error)
	{
		if (!error)
		{
			messageQueue.pop_front();
			if (!messageQueue.empty())
			{
				startSend();
			}
		}
		else
		{
			MD_LOG_ERROR("multicast send failed: %d %s", error.value(), error.message().c_str());
		}
	}

	void MulticastSender::sendEncodedItem(const EncodedItem & message)
	{
		bool write_in_progress = !messageQueue.empty();
		messageQueue.push_back(message);

		if (!write_in_progress)
		{
			startSend();
		}
		else if (messageQueue.size() > MAX_QUEUED_MESSAGES &&
			messageQueue.size() > 3 * dispatcher->snapshot().size())
		{
			MD_LOG_DEBUG("multicast queue cut, max sent seq [%lld], original size [%d]",
				maxSentMessageSeq, (int)messageQueue.size());

			MessageQueue_t::iterator it = messageQueue.begin();
			it++;
			messageQueue.erase(it, messageQueue.end());

			const Snapshot & snapshot = dispatcher->snapshot();
			for (Snapshot::const_iterator sit = snapshot.begin(); sit != snapshot.end(); sit++)
			{
				if (sit->second.seq > maxSentMessageSeq)
					messageQueue.push_back(sit->second);
			}
		}
	}
}