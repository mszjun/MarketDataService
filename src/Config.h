#ifndef MDGW_CONFIG_H
#define MDGW_CONFIG_H

#include <string>
#include <vector>

namespace mdgw
{
	struct MulticastConfig
	{
		bool enabled = false;
		std::string address;   // e.g. "239.0.0.3"
		int port = 0;          // e.g. 12345
		int hops = 5;
	};

	struct UnicastConfig
	{
		bool enabled = false;
		std::string address;   // e.g. "0.0.0.0"
		int port = 0;
		int maxConnections = 66;
	};

	struct BroadcastConfig
	{
		int levels = 1;          // 1 = legacy wire format, 5 = include 5-level book
		int snapshotIdleSeconds = 30; // re-broadcast whole snapshot after N quiet seconds
	};

	struct ClickHouseConfig
	{
		bool enabled = false;
		std::string host = "127.0.0.1";
		int port = 8123;
		std::string database = "market";
		std::string table = "tick";
		std::string user = "default";
		std::string password;
		int batchRows = 1000;       // rows collected before one HTTP insert
		int flushIntervalMs = 1000; // max wait before a forced flush
		int maxQueuedRows = 200000; // queue bound, above which ticks are dropped
		int connectTimeoutMs = 5000;
		int readTimeoutMs = 30000;
		int retrySleepMs = 3000;    // pause after a failed HTTP insert
	};

	// CSV file storage backend (files rotated by size; one series per
	// trading day: tick_YYYYMMDD_001.csv, tick_YYYYMMDD_002.csv, ...).
	struct LocalFileConfig
	{
		bool enabled = false;
		std::string dataDir = "marketdata";  // where tick_*.csv files are written
		std::string filePrefix = "tick";        // -> <dataDir>/<filePrefix>_<TradingDay>_<seq>.csv
		int flushIntervalMs = 1000;             // max wait before appending a batch
		int maxQueuedRows = 200000;             // in-memory queue bound
		long long maxFileSize = 100 * 1024 * 1024; // rotate to a new part when a file exceeds this (bytes)
	};

	struct CTPMdSourceConfig
	{
		std::string brokerID;
		std::string userID;
		std::string password;
		std::string appID;      // reserved: this SDK release has no MD ReqAuthenticate
		std::string authCode;   // reserved
		std::string flowPath;   // CTP MD API flow/work directory (empty = default)
		std::vector<std::string> marketDataServerURIs; // e.g. "tcp://1.2.3.4:41213"
		std::vector<std::string> exchangeFilter;       // canonical exchange ids; empty = all
		int subscribeChunkSize = 300; // contracts per SubscribeMarketData call
		int loginRetrySeconds = 15;
	};

	struct ServiceConfig
	{
		// whole-market instrument list source: the openctp dict HTTP service.
		// The response is cached to instrumentCacheFile; when the URL is
		// unreachable the last saved cache is used instead.
		std::string instrumentSourceUrl = "http://dict.openctp.cn/instruments?types=futures";
		std::string instrumentCacheFile = "config/instruments_cache.json";

		// Automatic exit times, local time "HH:MM" (empty disables):
		//   dayExitTime   - e.g. "15:18": stop after the day session
		//   nightExitTime - e.g. "02:35": stop after the night session
		std::string dayExitTime = "15:18";
		std::string nightExitTime = "02:35";

		std::string logFile;            // empty = console only
		int logLevel = 1;               // 0 debug,1 info,2 warn,3 error

		MulticastConfig multicast;
		UnicastConfig unicast;
		BroadcastConfig broadcast;
		ClickHouseConfig clickHouse;
		LocalFileConfig localFile;

		std::vector<CTPMdSourceConfig> ctpSources;

		bool createFlowDirs = true;
	};

	// Returns true and fills cfg; on failure returns false and err contains reason.
	bool parseServiceConfig(const std::string & configFile, ServiceConfig & cfg, std::string & err);

	// Returns true when path is an absolute filesystem path.
	bool isAbsolutePath(const std::string & path);
}

#endif // MDGW_CONFIG_H
