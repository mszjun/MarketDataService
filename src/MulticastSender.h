#ifndef MDGW_MULTICAST_SENDER_H
#define MDGW_MULTICAST_SENDER_H

#include <deque>
#include <functional>
#include <string>
#include <asio.hpp>
#include "FanoutTypes.h"

namespace mdgw
{
	class MarketDataDispatcher;

	// Sends every encoded item as one UDP datagram to the configured multicast
	// group. Ported from MarketDataMulticastDispatcher of the original engine:
	// a per-dispatcher FIFO is drained by async_send_to and cut back to the
	// latest snapshot when the queue grows beyond the limit.
	class MulticastSender
	{
	private:
		enum
		{
			MAX_QUEUED_MESSAGES = 1000
		};

		MarketDataDispatcher * const dispatcher;
		asio::io_context & io_service;
		asio::ip::udp::endpoint endPoint;
		asio::ip::udp::socket socket;

		typedef std::deque<EncodedItem> MessageQueue_t;
		MessageQueue_t messageQueue;
		long long maxSentMessageSeq;

		void startSend()
		{
			socket.async_send_to(
				asio::buffer(messageQueue.front().data.data(), messageQueue.front().data.length()),
				endPoint,
				std::bind(&MulticastSender::handleWrite, this, std::placeholders::_1));
			if (messageQueue.front().seq > maxSentMessageSeq)
				maxSentMessageSeq = messageQueue.front().seq;
		}

		void handleWrite(const std::error_code & error);

	public:
		MulticastSender(MarketDataDispatcher * dispatcher_, asio::io_context & io,
			const std::string & multicast_address, int port, int hops);

		virtual ~MulticastSender() {}

		void sendEncodedItem(const EncodedItem & message);
	};
}

#endif // MDGW_MULTICAST_SENDER_H