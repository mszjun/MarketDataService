#include "MarketDataDispatcher.h"

#include <chrono>
#include <functional>
#include "BroadcastEncoder.h"
#include "Logger.h"

namespace mdgw
{
	MarketDataDispatcher::MarketDataDispatcher(const MulticastConfig & mc, const UnicastConfig & uc,
		const BroadcastConfig & bc)
		: multicastConfig_(mc), unicastConfig_(uc), broadcastConfig_(bc),
		timer(io_service), idleTicks(0),
		snapshotIdleSeconds(bc.snapshotIdleSeconds > 0 ? bc.snapshotIdleSeconds : 30),
		sequenceNumber(0), broadcastLevels_(bc.levels), started_(false), stopped_(false)
	{
	}

	MarketDataDispatcher::~MarketDataDispatcher()
	{
		stop();
	}

	void MarketDataDispatcher::start()
	{
		if (started_)
			return;
		started_ = true;

		if (multicastConfig_.enabled)
		{
			multicastSender.reset(new MulticastSender(this, io_service,
				multicastConfig_.address, multicastConfig_.port, multicastConfig_.hops));
		}
		if (unicastConfig_.enabled)
		{
			unicastServer.reset(new UnicastServer(this, io_service,
				unicastConfig_.address, unicastConfig_.port, unicastConfig_.maxConnections));
		}

		timer.expires_after(std::chrono::seconds(1));
		timer.async_wait(std::bind(&MarketDataDispatcher::checkAndBroadcastSnapshot,
			this, &timer, std::placeholders::_1));

		ioThread = std::thread(&MarketDataDispatcher::run, this);
		MD_LOG_INFO("market data dispatcher thread started");
	}

	void MarketDataDispatcher::stop()
	{
		if (stopped_.exchange(true, std::memory_order_relaxed))
			return;

		if (started_)
		{
			io_service.stop();
			if (ioThread.joinable())
				ioThread.join();
		}
	}

	void MarketDataDispatcher::run()
	{
		while (true)
		{
			try
			{
				io_service.run();
				break;
			}
			catch (const std::exception & ex)
			{
				MD_LOG_ERROR("exception in market data dispatcher thread: %s", ex.what());
			}
		}
	}

	void MarketDataDispatcher::removeConnection(std::shared_ptr<UnicastSocketConnection> conn)
	{
		if (unicastServer)
			unicastServer->connectionGoodbye(conn);
	}

	void MarketDataDispatcher::handleTick(const Tick & tick)
	{
		if (stopped_.load(std::memory_order_relaxed))
			return;

		sequenceNumber++;
		long long seq = sequenceNumber;

		EncodedItem item;
		item.seq = seq;
		item.data = encodeBroadcastJson(tick, seq, broadcastLevels_);

		snapshot_[tick.instrumentID] = item;
		sendEncodedItem(item);
	}

	void MarketDataDispatcher::sendEncodedItem(const EncodedItem & item)
	{
		idleTicks = 0;
		if (multicastSender)
			multicastSender->sendEncodedItem(item);
		if (unicastServer)
			unicastServer->sendToAll(item);
	}

	void MarketDataDispatcher::checkAndBroadcastSnapshot(asio::steady_timer * timerpoi,
		const std::error_code & ec)
	{
		if (stopped_.load(std::memory_order_relaxed))
			return;

		if (idleTicks >= snapshotIdleSeconds)
		{
			idleTicks = 0;
			MD_LOG_DEBUG("resend whole market-data snapshot, instruments [%d]",
				(int)snapshot_.size());
			for (Snapshot::const_iterator it = snapshot_.begin(); it != snapshot_.end(); it++)
			{
				sendEncodedItem(it->second);
			}
		}
		else
		{
			idleTicks++;
		}

		timerpoi->expires_after(std::chrono::seconds(1));
		timerpoi->async_wait(std::bind(&MarketDataDispatcher::checkAndBroadcastSnapshot,
			this, timerpoi, std::placeholders::_1));
	}

	void MarketDataDispatcher::onTick(const Tick & tick)
	{
		io_service.post([this, tick]() { handleTick(tick); });
	}
}