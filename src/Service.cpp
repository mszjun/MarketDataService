#include "Service.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>

#ifdef WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "CTPMarketDataSource.h"
#include "ClickHouseWriter.h"
#include "LocalFileWriter.h"
#include "TickWriter.h"
#include "ContractList.h"
#include "Logger.h"
#include "MarketDataDispatcher.h"

namespace mdgw
{
	namespace
	{
		// create a directory (all missing parents), like "mkdir -p"
		bool mkdirs(const std::string & path)
		{
			if (path.empty())
				return false;

			std::string cur;
			for (size_t i = 0; i < path.size(); i++)
			{
				cur += path[i];
				if (path[i] == '/' || path[i] == '\\')
				{
					// skip leading separator and a windows drive root like "C:"
					if (cur.size() > 1 && !(cur.size() == 3 && cur[1] == ':'))
					{
#ifdef WIN32
						if (_mkdir(cur.c_str()) != 0 && errno != EEXIST)
							return false;
#else
						if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
							return false;
#endif
					}
				}
			}
			// final component (path without trailing separator)
			if (!cur.empty() && cur[cur.size() - 1] != '/' && cur[cur.size() - 1] != '\\')
			{
#ifdef WIN32
				if (_mkdir(cur.c_str()) != 0 && errno != EEXIST)
					return false;
#else
				if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
					return false;
#endif
			}
			return true;
		}

		// Parses a local wall-clock time given as "HH:MM" (or "HHMM") into
		// minutes since midnight; returns -1 for an empty / invalid value so
		// the auto-exit feature can be disabled per field.
		int parseClockTime(const std::string & s)
		{
			if (s.empty())
				return -1;

			std::string hpart, mpart;
			size_t colon = s.find(':');
			if (colon != std::string::npos)
			{
				hpart = s.substr(0, colon);
				mpart = s.substr(colon + 1);
			}
			else if (s.size() == 4)
			{
				hpart = s.substr(0, 2);
				mpart = s.substr(2, 2);
			}
			else
			{
				return -1;
			}

			if (hpart.size() != 2 || mpart.size() != 2)
				return -1;
			if (hpart.find_first_not_of("0123456789") != std::string::npos ||
				mpart.find_first_not_of("0123456789") != std::string::npos)
				return -1;

			int hh = (hpart[0] - '0') * 10 + (hpart[1] - '0');
			int mm = (mpart[0] - '0') * 10 + (mpart[1] - '0');
			if (hh < 0 || hh > 23 || mm < 0 || mm > 59)
				return -1;
			return hh * 60 + mm;
		}
	}

	Service::Service()
		: dispatcher_(NULL), tickWriter_(NULL),
		exitDayMinute_(-1), exitNightMinute_(-1),
		stopRequested_(false), initializedForRun_(false)
	{
	}

	Service::~Service()
	{
		// stop CTP sources first (no new ticks), then broadcast, then CH.
		// The destructor also runs when init() failed half-way, therefore every
		// member is checked individually.
		for (size_t i = 0; i < sources_.size(); i++)
		{
			if (sources_[i])
				sources_[i]->stop();
		}
		for (size_t i = 0; i < sources_.size(); i++)
			delete sources_[i];
		sources_.clear();

		if (dispatcher_)
		{
			dispatcher_->stop();
			delete dispatcher_;
			dispatcher_ = NULL;
		}
		if (tickWriter_)
		{
			tickWriter_->stop();
			delete tickWriter_;
			tickWriter_ = NULL;
		}
	}

	bool Service::init(const std::string & configFile, std::string & err)
	{
		configFile_ = configFile;

		if (!parseServiceConfig(configFile_, cfg_, err))
			return false;

		// make sure the log file directory exists
		if (!cfg_.logFile.empty())
		{
			size_t pos = cfg_.logFile.find_last_of("/\\");
			if (pos != std::string::npos && pos > 0)
			{
				std::string dir = cfg_.logFile.substr(0, pos);
				if (!mkdirs(dir))
				{
					MD_LOG_WARN("can not create log directory [%s]", dir.c_str());
				}
			}
		}

		Logger::instance().setOptions(true, cfg_.logFile, (LogLevel)cfg_.logLevel);
		MD_LOG_INFO("configuration loaded from [%s]", configFile_.c_str());

		// ---- 1. load the whole-market contract list ---------------------
		// Primary source: the openctp dict HTTP service. Every successful
		// fetch is cached locally; when the URL is unreachable or unparseable
		// the last saved cache is used so the service can still start offline.
		std::vector<InstrumentInfo> all;
		size_t ignored = 0;
		std::string sourceDesc;

		{
			std::string body;
			std::string fetchErr;
			if (ContractList::fetchFromOpenCtp(cfg_.instrumentSourceUrl, body, fetchErr))
			{
				std::vector<InstrumentInfo> parsed;
				size_t ign = 0;
				std::string parseErr;
				if (ContractList::parseOpenCtp(body, parsed, ign, parseErr))
				{
					all.swap(parsed);
					ignored = ign;
					sourceDesc = "openctp url [" + cfg_.instrumentSourceUrl + "]";

					// make sure the cache directory exists, then persist the
					// raw response for the next offline start
					size_t pos = cfg_.instrumentCacheFile.find_last_of("/\\");
					if (pos != std::string::npos && pos > 0)
					{
						std::string dir = cfg_.instrumentCacheFile.substr(0, pos);
						if (!mkdirs(dir))
							MD_LOG_WARN("can not create instrument cache directory [%s]",
								dir.c_str());
					}
					std::string cacheErr;
					if (!ContractList::saveCache(cfg_.instrumentCacheFile, body, cacheErr))
						MD_LOG_WARN("can not save instrument cache: %s", cacheErr.c_str());
				}
				else
				{
					MD_LOG_WARN("openctp response parse failed: %s, falling back to cache",
						parseErr.c_str());
				}
			}
			else
			{
				MD_LOG_WARN("openctp instrument fetch failed: %s, falling back to cache",
					fetchErr.c_str());
			}
		}

		if (all.empty())
		{
			// remote fetch failed or unparseable: use the last saved copy
			std::string cacheErr;
			if (!ContractList::loadCache(cfg_.instrumentCacheFile, all, ignored, cacheErr))
			{
				err = "can not obtain the instrument list: openctp URL failed and cache "
					"is unusable (" + cacheErr + ")";
				return false;
			}
			sourceDesc = "local cache [" + cfg_.instrumentCacheFile + "] (remote unavailable)";
			MD_LOG_WARN("using cached instrument list (%d contracts): %s",
				(int)all.size(), cacheErr.empty() ? "ok" : cacheErr.c_str());
		}

		MD_LOG_INFO("contract list loaded from %s: contracts [%d], ignored [%d]",
			sourceDesc.c_str(), (int)all.size(), (int)ignored);

		// ---- 2. shard instruments over the CTP sources ------------------
		// each instrument belongs to exactly one source so no tick is ever
		// duplicated either on the wire or in ClickHouse.
		std::vector<bool> claimed(all.size(), false);
		std::vector<std::vector<InstrumentInfo> > perSource(cfg_.ctpSources.size());

		for (size_t s = 0; s < cfg_.ctpSources.size(); s++)
		{
			const CTPMdSourceConfig & sc = cfg_.ctpSources[s];
			for (size_t i = 0; i < all.size(); i++)
			{
				if (claimed[i])
					continue;
				if (!sc.exchangeFilter.empty())
				{
					bool wanted = false;
					for (size_t f = 0; f < sc.exchangeFilter.size(); f++)
					{
						if (all[i].exchangeID == sc.exchangeFilter[f])
						{
							wanted = true;
							break;
						}
					}
					if (!wanted)
						continue;
				}
				perSource[s].push_back(all[i]);
				claimed[i] = true;
			}
		}

		for (size_t s = 0; s < cfg_.ctpSources.size(); s++)
		{
			size_t unclaimed = 0;
			for (size_t i = 0; i < all.size(); i++)
				if (!claimed[i])
					unclaimed++;
			if (unclaimed > 0 && s + 1 == cfg_.ctpSources.size())
			{
				MD_LOG_WARN("[%d] contracts matched no source exchange filter and are ignored",
					(int)unclaimed);
			}
			MD_LOG_INFO("source [%s:%s] owns [%d] contracts",
				cfg_.ctpSources[s].brokerID.c_str(), cfg_.ctpSources[s].userID.c_str(),
				(int)perSource[s].size());
		}

		// ---- 3. create components --------------------------------------
		try
		{
			dispatcher_ = new MarketDataDispatcher(cfg_.multicast, cfg_.unicast, cfg_.broadcast);
			
			// Create appropriate tick writer based on configuration
			// Priority: LocalFile > ClickHouse > NullWriter
			if (cfg_.localFile.enabled)
			{
				tickWriter_ = new LocalFileWriter(cfg_.localFile);
			}
			else if (cfg_.clickHouse.enabled)
			{
				tickWriter_ = new ClickHouseWriter(cfg_.clickHouse);
			}
			else
			{
				// No storage configured, use null writer
				tickWriter_ = new NullWriter();
			}

			for (size_t s = 0; s < cfg_.ctpSources.size(); s++)
			{
				// a source without any instrument (e.g. another unfiltered
				// source already claimed the whole market) is skipped
				if (perSource[s].empty())
				{
					MD_LOG_WARN("source [%s:%s] owns no instrument and is skipped",
						cfg_.ctpSources[s].brokerID.c_str(), cfg_.ctpSources[s].userID.c_str());
					continue;
				}

				CTPMdSourceConfig sc = cfg_.ctpSources[s];

				// make sure the CTP flow directory exists before Init()
				if (sc.flowPath.empty())
				{
					sc.flowPath = "MarketDataFlow/" + sc.brokerID + "_" + sc.userID;
				}
				if (!mkdirs(sc.flowPath))
				{
					MD_LOG_WARN("can not create flow directory [%s], CTP api may fail",
						sc.flowPath.c_str());
				}

				CTPMarketDataSource * src = new CTPMarketDataSource(sc, perSource[s],
					dispatcher_, tickWriter_);

				std::string srcErr;
				if (!src->start(srcErr))
				{
					err = srcErr;
					delete src;
					return false;
				}
				sources_.push_back(src);
			}

			if (sources_.empty())
			{
				err = "no CTP source could be started (all sources have no instrument)";
				return false;
			}

			dispatcher_->start();
			if (tickWriter_)
				tickWriter_->start();
		}
		catch (const std::exception & ex)
		{
			err = std::string("exception while starting service: ") + ex.what();
			return false;
		}

		exitDayMinute_ = parseClockTime(cfg_.dayExitTime);
		exitNightMinute_ = parseClockTime(cfg_.nightExitTime);

		if (exitDayMinute_ >= 0 || exitNightMinute_ >= 0)
		{
			MD_LOG_INFO("auto exit enabled: day [%s] night [%s]",
				exitDayMinute_ >= 0 ? cfg_.dayExitTime.c_str() : "-",
				exitNightMinute_ >= 0 ? cfg_.nightExitTime.c_str() : "-");
		}

		initializedForRun_ = true;
		logStartupSummary();
		return true;
	}

	void Service::logStartupSummary()
	{
		size_t total = 0;
		for (size_t s = 0; s < cfg_.ctpSources.size(); s++)
			total += cfg_.ctpSources[s].marketDataServerURIs.size();

		MD_LOG_INFO("market data service started");
		MD_LOG_INFO("  unicast   [%s] port [%d]",
			cfg_.unicast.enabled ? cfg_.unicast.address.c_str() : "-",
			cfg_.unicast.enabled ? cfg_.unicast.port : 0);
		MD_LOG_INFO("  multicast [%s] port [%d]",
			cfg_.multicast.enabled ? cfg_.multicast.address.c_str() : "-",
			cfg_.multicast.enabled ? cfg_.multicast.port : 0);
		MD_LOG_INFO("  clickhouse [%s]",
			cfg_.clickHouse.enabled ? (cfg_.clickHouse.host + ":" + std::to_string(cfg_.clickHouse.port) + "/" + cfg_.clickHouse.database + "." + cfg_.clickHouse.table).c_str() : "-");
		MD_LOG_INFO("  local csv file [%s]",
			cfg_.localFile.enabled ? (cfg_.localFile.dataDir + "/" + cfg_.localFile.filePrefix + "_<ActionDay>_NNN.csv").c_str() : "-");
	}

	void Service::requestStop() noexcept
	{
		// deliberately free of logging and locking: this is called from the
		// signal handler, where only an atomic store is safe
		stopRequested_.store(true, std::memory_order_relaxed);
	}

	// One-shot check of the local wall clock against the configured session
	// end times. The checks are limited to their natural windows so a process
	// that is meant to cover the night session is never killed by the day exit
	// time (and vice versa):
	//   - night exit: local time within [00:00, 06:00), time >= nightExitTime
	//   - day exit  : local time within [06:00, 20:00), time >= dayExitTime
	// Outside both windows (20:00-24:00) nothing triggers; such a process is a
	// night-session run that waits for midnight to roll into the night window.
	void Service::checkAutoExit()
	{
		if (exitDayMinute_ < 0 && exitNightMinute_ < 0)
			return;

		std::time_t now = std::time(NULL);
		std::tm tmv;
#ifdef WIN32
		localtime_s(&tmv, &now);
#else
		localtime_r(&now, &tmv);
#endif
		int cur = tmv.tm_hour * 60 + tmv.tm_min;
		bool hit = false;
		const char * which = NULL;
		const char * at = NULL;

		if (exitNightMinute_ >= 0 && tmv.tm_hour < 6 && cur >= exitNightMinute_)
		{
			hit = true;
			which = "night";
			at = cfg_.nightExitTime.c_str();
		}
		else if (exitDayMinute_ >= 0 && tmv.tm_hour >= 6 && tmv.tm_hour < 20 &&
			cur >= exitDayMinute_)
		{
			hit = true;
			which = "day";
			at = cfg_.dayExitTime.c_str();
		}

		if (hit)
		{
			MD_LOG_INFO("auto exit: %s session end time [%s] reached, shutting down",
				which, at);
			requestStop();
		}
	}

	void Service::run()
	{
		if (!initializedForRun_)
			return;

		std::thread stats(std::thread(&Service::statsLoop, this));

		int elapsedChecks = 0;
		while (!stopRequested_.load(std::memory_order_relaxed))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(200));

			// check the automatic session-end exit times once per second
			if (++elapsedChecks >= 5)
			{
				elapsedChecks = 0;
				checkAutoExit();
			}
		}

		MD_LOG_INFO("stop requested, shutting down...");

		// 1. stop the tick flow: unregistering the CTP SPI guarantees that no
		//    new tick can arrive, without touching the CTP API itself
		for (size_t i = 0; i < sources_.size(); i++)
			sources_[i]->stopReceiving();

		dispatcher_->stop();

		if (tickWriter_)
			tickWriter_->stop();   // flush every buffered tick before the CTP teardown

		// 2. only now release the CTP API: Release() can block for a long time
		//    when the front is unreachable, and the buffered ticks must already
		//    be on disk / in ClickHouse by then
		for (size_t i = 0; i < sources_.size(); i++)
			sources_[i]->releaseApi();

		if (stats.joinable())
			stats.join();

		MD_LOG_INFO("market data service stopped");
	}

	void Service::statsLoop()
	{
		unsigned long long lastTicks = 0;
		int minuteCounter = 0;
		while (!stopRequested_.load(std::memory_order_relaxed))
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));
			minuteCounter++;

			if (minuteCounter < 60)
				continue;
			minuteCounter = 0;

			unsigned long long total = 0;
			for (size_t i = 0; i < sources_.size(); i++)
				total += sources_[i]->tickCount();

			MD_LOG_INFO("stats: ticks total [%llu], last minute [%llu]",
				(unsigned long long)total,
				(unsigned long long)(total - lastTicks));
			lastTicks = total;

			if (tickWriter_)
			{
				// Determine writer type for log message
				const char* writerType = "unknown";
				if (dynamic_cast<ClickHouseWriter*>(tickWriter_))
					writerType = "clickhouse";
				else if (dynamic_cast<LocalFileWriter*>(tickWriter_))
					writerType = "local file";
				else if (dynamic_cast<NullWriter*>(tickWriter_))
					writerType = "null";
					
				MD_LOG_INFO("stats: %s received [%llu] written [%llu] dropped [%llu] failed [%llu], queue [%d]",
					writerType,
					tickWriter_->received(), tickWriter_->written(), tickWriter_->dropped(),
					tickWriter_->failedRows(), (int)tickWriter_->queueSize());
			}
		}
	}
}
