#include "Config.h"

#include "Logger.h"
#include "cJSON.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace mdgw
{
	namespace
	{
		// ---- small cJSON helpers ----------------------------------------
		const char * getString(const cJSON * obj, const char * key)
		{
			const cJSON * item = obj ? cJSON_GetObjectItem((cJSON *)obj, key) : NULL;
			if (item && item->type == cJSON_String)
				return item->valuestring;
			return NULL;
		}

		int getInt(const cJSON * obj, const char * key, int def)
		{
			const cJSON * item = obj ? cJSON_GetObjectItem((cJSON *)obj, key) : NULL;
			if (item && item->type == cJSON_Number)
				return item->valueint;
			return def;
		}

		bool getBool(const cJSON * obj, const char * key, bool def)
		{
			const cJSON * item = obj ? cJSON_GetObjectItem((cJSON *)obj, key) : NULL;
			if (item && (item->type == cJSON_True || item->type == cJSON_False))
				return item->type == cJSON_True;
			return def;
		}

		const cJSON * getObject(const cJSON * obj, const char * key)
		{
			const cJSON * item = obj ? cJSON_GetObjectItem((cJSON *)obj, key) : NULL;
			return (item && item->type == cJSON_Object) ? item : NULL;
		}

		const cJSON * getArray(const cJSON * obj, const char * key)
		{
			const cJSON * item = obj ? cJSON_GetObjectItem((cJSON *)obj, key) : NULL;
			return (item && item->type == cJSON_Array) ? item : NULL;
		}

		void parseStringArray(const cJSON * arr, std::vector<std::string> & out)
		{
			out.clear();
			if (!arr)
				return;
			for (const cJSON * it = arr->child; it != NULL; it = it->next)
			{
				if (it->type == cJSON_String && it->valuestring)
					out.push_back(it->valuestring);
			}
		}
	}

	// 辅助函数：获取当前时间的格式化字符串
	std::string getCurrentTimeStr() {
		auto now = std::chrono::system_clock::now();
		auto in_time_t = std::chrono::system_clock::to_time_t(now);
		std::tm bt;
#if defined(_WIN32)
		localtime_s(&bt, &in_time_t);
#else
		localtime_r(&in_time_t, &bt);
#endif
		std::ostringstream oss;
		oss << std::put_time(&bt, "%Y%m%d%H%M%S");
		return oss.str();
	}

	bool isAbsolutePath(const std::string & path)
	{
		if (path.empty())
			return false;
		if (path[0] == '/' || path[0] == '\\')
			return true;
		// windows drive letter, e.g. C:\...
		if (path.size() >= 3 && isalpha((unsigned char)path[0]) && path[1] == ':' &&
			(path[2] == '/' || path[2] == '\\'))
			return true;
		return false;
	}

	bool parseServiceConfig(const std::string & configFile, ServiceConfig & cfg, std::string & err)
	{
		FILE * fp = fopen(configFile.c_str(), "rb");
		if (!fp)
		{
			err = "can not open config file: " + configFile;
			return false;
		}

		std::string text;
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
			text.append(buf, n);
		fclose(fp);

		cJSON * root = cJSON_Parse(text.c_str());
		if (!root)
		{
			err = "can not parse config file: " + configFile;
			return false;
		}
		if (root->type != cJSON_Object)
		{
			cJSON_Delete(root);
			err = "config root is not a JSON object: " + configFile;
			return false;
		}

		// ---- top level ---------------------------------------------------
		{
			const char * s = getString(root, "InstrumentSourceUrl");
			if (s && *s)
				cfg.instrumentSourceUrl = s;

			s = getString(root, "InstrumentCacheFile");
			if (s && *s)
				cfg.instrumentCacheFile = s;

			s = getString(root, "DayExitTime");
			if (s)
				cfg.dayExitTime = s;

			s = getString(root, "NightExitTime");
			if (s)
				cfg.nightExitTime = s;

			s = getString(root, "LogFile");
			if (s)
				cfg.logFile = s;

			cfg.logLevel = getInt(root, "LogLevel", cfg.logLevel);
			if (cfg.logLevel < 0 || cfg.logLevel > 3)
				cfg.logLevel = 1;
		}

		// ---- multicast ---------------------------------------------------
		{
			const cJSON * mc = getObject(root, "Multicast");
			if (mc)
			{
				cfg.multicast.enabled = getBool(mc, "Enabled", false);
				const char * addr = getString(mc, "Address");
				if (addr)
					cfg.multicast.address = addr;
				cfg.multicast.port = getInt(mc, "Port", 0);
				cfg.multicast.hops = getInt(mc, "Hops", cfg.multicast.hops);
			}
		}

		// ---- unicast -----------------------------------------------------
		{
			const cJSON * uc = getObject(root, "Unicast");
			if (uc)
			{
				cfg.unicast.enabled = getBool(uc, "Enabled", false);
				const char * addr = getString(uc, "Address");
				if (addr)
					cfg.unicast.address = addr;
				cfg.unicast.port = getInt(uc, "Port", 0);
				cfg.unicast.maxConnections = getInt(uc, "MaxConnections", cfg.unicast.maxConnections);
			}
		}

		// ---- broadcast ---------------------------------------------------
		{
			const cJSON * bc = getObject(root, "Broadcast");
			if (bc)
			{
				cfg.broadcast.levels = getInt(bc, "Levels", cfg.broadcast.levels);
				if (cfg.broadcast.levels != 1 && cfg.broadcast.levels != 5)
					cfg.broadcast.levels = 1;
				cfg.broadcast.snapshotIdleSeconds = getInt(bc, "SnapshotIdleSeconds", cfg.broadcast.snapshotIdleSeconds);
				if (cfg.broadcast.snapshotIdleSeconds < 1)
					cfg.broadcast.snapshotIdleSeconds = 1;
			}
		}

		// ---- clickhouse --------------------------------------------------
		{
			const cJSON * ch = getObject(root, "ClickHouse");
			if (ch)
			{
				cfg.clickHouse.enabled = getBool(ch, "Enabled", false);
				const char * s;
				if ((s = getString(ch, "Host")) != NULL)
					cfg.clickHouse.host = s;
				cfg.clickHouse.port = getInt(ch, "Port", cfg.clickHouse.port);
				if ((s = getString(ch, "Database")) != NULL)
					cfg.clickHouse.database = s;
				if ((s = getString(ch, "Table")) != NULL)
					cfg.clickHouse.table = s;
				if ((s = getString(ch, "User")) != NULL)
					cfg.clickHouse.user = s;
				if ((s = getString(ch, "Password")) != NULL)
					cfg.clickHouse.password = s;
				cfg.clickHouse.batchRows = getInt(ch, "BatchRows", cfg.clickHouse.batchRows);
				cfg.clickHouse.flushIntervalMs = getInt(ch, "FlushIntervalMs", cfg.clickHouse.flushIntervalMs);
				cfg.clickHouse.maxQueuedRows = getInt(ch, "MaxQueuedRows", cfg.clickHouse.maxQueuedRows);
				cfg.clickHouse.connectTimeoutMs = getInt(ch, "ConnectTimeoutMs", cfg.clickHouse.connectTimeoutMs);
				cfg.clickHouse.readTimeoutMs = getInt(ch, "ReadTimeoutMs", cfg.clickHouse.readTimeoutMs);
				cfg.clickHouse.retrySleepMs = getInt(ch, "RetrySleepMs", cfg.clickHouse.retrySleepMs);

				if (cfg.clickHouse.batchRows < 1) cfg.clickHouse.batchRows = 1;
				if (cfg.clickHouse.flushIntervalMs < 10) cfg.clickHouse.flushIntervalMs = 10;
				if (cfg.clickHouse.maxQueuedRows < cfg.clickHouse.batchRows)
					cfg.clickHouse.maxQueuedRows = cfg.clickHouse.batchRows;
			}
		}

		// ---- local csv file (rotated by size, one series per trading day) ----
		{
			const cJSON * lf = getObject(root, "LocalFile");
			if (lf)
			{
				cfg.localFile.enabled = getBool(lf, "Enabled", false);
				const char * s;
				if ((s = getString(lf, "DataDir")) != NULL)
					cfg.localFile.dataDir = s;
				if ((s = getString(lf, "FilePrefix")) != NULL)
					cfg.localFile.filePrefix = s;
				cfg.localFile.flushIntervalMs = getInt(lf, "FlushIntervalMs", cfg.localFile.flushIntervalMs);
				cfg.localFile.maxQueuedRows = getInt(lf, "MaxQueuedRows", cfg.localFile.maxQueuedRows);
				cfg.localFile.maxFileSize = getInt(lf, "MaxFileSize", (int)cfg.localFile.maxFileSize);

				if (cfg.localFile.flushIntervalMs < 10) cfg.localFile.flushIntervalMs = 10;
				if (cfg.localFile.maxQueuedRows < 1) cfg.localFile.maxQueuedRows = 1;
				if (cfg.localFile.maxFileSize < 1024 * 1024) cfg.localFile.maxFileSize = 1024 * 1024;
				if (cfg.localFile.dataDir.empty()) cfg.localFile.dataDir = "marketdata";
				if (cfg.localFile.filePrefix.empty()) cfg.localFile.filePrefix = "tick";

				// 统一追加当前时间
				cfg.localFile.dataDir += "_" + getCurrentTimeStr();
			}
		}

		// ---- ctp sources -------------------------------------------------
		{
			const cJSON * arr = getArray(root, "CTPSources");
			if (arr)
			{
				for (const cJSON * it = arr->child; it != NULL; it = it->next)
				{
					if (it->type != cJSON_Object)
						continue;

					CTPMdSourceConfig sc;
					const char * s;
					if ((s = getString(it, "BrokerID")) != NULL)
						sc.brokerID = s;
					if ((s = getString(it, "UserID")) != NULL)
						sc.userID = s;
					if ((s = getString(it, "Password")) != NULL)
						sc.password = s;
					if ((s = getString(it, "AppID")) != NULL)
						sc.appID = s;
					if ((s = getString(it, "AuthCode")) != NULL)
						sc.authCode = s;
					if ((s = getString(it, "FlowPath")) != NULL)
						sc.flowPath = s;
					sc.subscribeChunkSize = getInt(it, "SubscribeChunkSize", sc.subscribeChunkSize);
					sc.loginRetrySeconds = getInt(it, "LoginRetrySeconds", sc.loginRetrySeconds);

					const cJSON * uris = getArray(it, "MarketDataServerURIs");
					parseStringArray(uris, sc.marketDataServerURIs);

					const cJSON * exf = getArray(it, "ExchangeFilter");
					parseStringArray(exf, sc.exchangeFilter);

					if (sc.subscribeChunkSize < 1)
						sc.subscribeChunkSize = 300;
					if (sc.loginRetrySeconds < 1)
						sc.loginRetrySeconds = 15;

					cfg.ctpSources.push_back(sc);
				}
			}
		}

		cJSON_Delete(root);

		if (cfg.ctpSources.empty())
		{
			err = "no CTPSources defined in " + configFile;
			return false;
		}
		return true;
	}
}
