#ifndef MDGW_MARKET_DATA_DISPATCHER_H
#define MDGW_MARKET_DATA_DISPATCHER_H

#include <atomic>
#include <asio.hpp>
#include <memory>
#include <thread>
#include "Config.h"
#include "FanoutTypes.h"
#include "MulticastSender.h"
#include "Tick.h"
#include "UnicastServer.h"

namespace mdgw
{
	// Central fan-out point ported from MarketDataDispatcher / MarketDataEngine
	// of the original engine.
	//
	// Everything runs on a single standalone-asio io_context thread:
	//  - the CTP receiver thread posts raw ticks with onTick();
	//  - the io thread assigns a global sequence, encodes the tick into the
	//    legacy JSON wire message, stores the latest message per instrument in
	//    the snapshot and pushes it to the unicast server and the multicast
	//    sender;
	//  - a 1s timer re-broadcasts the whole snapshot after the market stays
	//    quiet for BroadcastConfig.snapshotIdleSeconds.
	class MarketDataDispatcher
	{
	public:
		MarketDataDispatcher(const MulticastConfig & mc, const UnicastConfig & uc,
			const BroadcastConfig & bc);
		~MarketDataDispatcher();

		// io thread lifecycle
		void start();
		void stop();

		// called from any CTP receiver thread
		void onTick(const Tick & tick);

		// ---- accessors used on the io thread ---------------------------
		const Snapshot & snapshot() const { return snapshot_; }

		// forwards a broken connection to the unicast server for removal
		void removeConnection(std::shared_ptr<UnicastSocketConnection> conn);

	private:
		void run();
		void handleTick(const Tick & tick);
		void sendEncodedItem(const EncodedItem & item);
		void checkAndBroadcastSnapshot(asio::steady_timer * timerpoi,
			const std::error_code & ec);

		MulticastConfig multicastConfig_;
		UnicastConfig unicastConfig_;
		BroadcastConfig broadcastConfig_;

		asio::io_context io_service;
		std::thread ioThread;

		asio::steady_timer timer;
		int idleTicks;
		int snapshotIdleSeconds;

		long long sequenceNumber;   // incremented only on the io thread

		Snapshot snapshot_;
		int broadcastLevels_;

		std::unique_ptr<MulticastSender> multicastSender;
		std::unique_ptr<UnicastServer> unicastServer;

		bool started_;

		// written by the thread calling stop(), read on the io thread
		std::atomic<bool> stopped_;
	};

}

#endif // MDGW_MARKET_DATA_DISPATCHER_H