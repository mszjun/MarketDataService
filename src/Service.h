#ifndef MDGW_SERVICE_H
#define MDGW_SERVICE_H

#include <atomic>
#include <string>
#include <vector>
#include "Config.h"

namespace mdgw
{
	class TickWriter;
	class CTPMarketDataSource;
	class MarketDataDispatcher;

	// Assembles the standalone market-data service:
	//
	//   1. reads MarketDataService.json
	//   2. loads the whole-market instrument list from the openctp dict
	//      service (with a local cache fallback)
	//   3. shards the instruments between the configured CTP sources
	//   4. starts the broadcast dispatcher (unicast + multicast), the tick
	//      writer (ClickHouse or local CSV) and the CTP market-data receivers
	//
	// run() blocks until requestStop() is called (e.g. by a signal handler or
	// by the configured automatic exit times, see checkAutoExit()).
	class Service
	{
	public:
		Service();
		~Service();

		// Returns false and fills err when configuration/contract loading fails.
		bool init(const std::string & configFile, std::string & err);

		// Blocks until requestStop() has been called.
		void run();

		// Async-signal-safe: flips an atomic flag only (no logging, no locks), so
		// it may be called from a signal / console handler; see main.cpp.
		// The shutdown itself is logged by run().
		void requestStop() noexcept;

	private:
		void logStartupSummary();
		void statsLoop();
		// One-shot local-time check against the configured day/night exit
		// times; requests a graceful stop when the session end is reached.
		void checkAutoExit();

		ServiceConfig cfg_;
		std::string configFile_;

		MarketDataDispatcher * dispatcher_;
		TickWriter * tickWriter_;

		// automatic exit points in minutes since midnight, -1 when disabled
		int exitDayMinute_;
		int exitNightMinute_;

		typedef std::vector<CTPMarketDataSource *> Sources_t;
		Sources_t sources_;

		std::atomic<bool> stopRequested_;
		bool initializedForRun_;
		std::string err_;
	};
}

#endif // MDGW_SERVICE_H
