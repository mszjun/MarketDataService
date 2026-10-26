#include "LocalFileWriter.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <sstream>
#include <thread>
#include "Logger.h"

#ifdef WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mdgw
{
	namespace
	{
		// ---------------- small helpers ----------------------------------

		// compact decimal literal, e.g. 3618 -> "3618", 3020.5 -> "3020.5"
		std::string csvNum(double v)
		{
			// non-finite values have no CSV representation; 0 keeps the row parseable
			if (!std::isfinite(v))
				return std::string("0");

			// The integer shortcut must test the range BEFORE the cast: converting a
			// double that does not fit into long long is undefined behaviour.
			if (v > -1e15 && v < 1e15 && v == (double)(long long)v)
			{
				char intBuf[32];
				snprintf(intBuf, sizeof(intBuf), "%lld", (long long)v);
				return std::string(intBuf);
			}

			// wide enough for the full expansion of any finite double
			char buf[512];
			snprintf(buf, sizeof(buf), "%.6f", v);
			std::string s(buf);
			size_t dot = s.find('.');
			if (dot != std::string::npos)
			{
				size_t last = s.find_last_not_of('0');
				if (last == dot)
					last--;
				s.erase(last + 1);
			}
			return s;
		}

		std::string csvInt(long long v)
		{
			char buf[32];
			snprintf(buf, sizeof(buf), "%lld", v);
			return std::string(buf);
		}

		// HH:MM:SS part of tick.updateTime ("09:30:00.125" -> "09:30:00")
		std::string hmsOf(const std::string & updateTime)
		{
			size_t dot = updateTime.find('.');
			if (dot == std::string::npos)
				return updateTime.size() >= 8 ? updateTime.substr(0, 8) : updateTime;
			return updateTime.substr(0, dot);
		}

		// "20240315" -> "2024-03-15"
		std::string dateFromCTP(const std::string & day)
		{
			if (day.size() != 8)
				return day;
			std::string out;
			out.reserve(10);
			out.append(day, 0, 4); out += '-';
			out.append(day, 4, 2); out += '-';
			out.append(day, 6, 2);
			return out;
		}

		// local date as YYYYMMDD (fallback when the tick carries no trading day)
		std::string todayLocal()
		{
			std::time_t t = std::time(NULL);
			std::tm tmv;
#ifdef WIN32
			localtime_s(&tmv, &t);
#else
			localtime_r(&t, &tmv);
#endif
			char buf[16];
			snprintf(buf, sizeof(buf), "%04d%02d%02d",
				tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
			return std::string(buf);
		}

		// Natural (calendar) day used for the CSV archive file name and for the
		// update_dt column: CTP ActionDay first, TradingDay as fallback, then
		// the local date. Only 8 digits are accepted.
		std::string actionDayOf(const Tick & tick)
		{
			std::string day = tick.actionDay;
			if (day.size() != 8 || !isdigit((unsigned char)day[0]) || !isdigit((unsigned char)day[7]))
			{
				day = tick.tradingDay;
			}
			if (day.size() != 8 || !isdigit((unsigned char)day[0]) || !isdigit((unsigned char)day[7]))
			{
				day = todayLocal();
			}
			return day;
		}

		// CTP TradingDay used for the CSV "trading_day" column; falls back to
		// the action day when the tick carries no usable trading day.
		std::string tradingDayOf(const Tick & tick)
		{
			std::string day = tick.tradingDay;
			if (day.size() != 8 || !isdigit((unsigned char)day[0]) || !isdigit((unsigned char)day[7]))
			{
				day = tick.actionDay;
			}
			if (day.size() != 8 || !isdigit((unsigned char)day[0]) || !isdigit((unsigned char)day[7]))
			{
				day = todayLocal();
			}
			return day;
		}

		// create directory (all missing parents), like "mkdir -p"
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

		// file size in bytes (0 when missing)
		long long fileSize(const std::string & path)
		{
#ifdef WIN32
			struct _stat64 st;
			if (_stat64(path.c_str(), &st) != 0)
				return 0;
			return st.st_size;
#else
			struct stat st;
			if (stat(path.c_str(), &st) != 0)
				return 0;
			return st.st_size;
#endif
		}

		// truncate a file back to a previous size (used to roll a partial
		// append back so a later replay of the same chunk cannot duplicate rows)
		void truncateFile(const std::string & path, long long size)
		{
#ifdef WIN32
			FILE * f = fopen(path.c_str(), "r+b");
			if (f)
			{
				_chsize_s(_fileno(f), size);
				fclose(f);
			}
#else
			truncate(path.c_str(), size);
#endif
		}

		const char * CSV_HEADER =
			"trading_day,exchange,instrument,update_dt,update_time,update_millisec,"
			"pre_settlement_price,pre_close_price,pre_open_interest,"
			"latest_price,open_price,highest_price,lowest_price,"
			"volume,turnover,open_interest,close_price,settlement_price,"
			"upper_limit_price,lower_limit_price,"
			"bid_price_1,bid_volume_1,ask_price_1,ask_volume_1\n";

		// one CSV data line for the tick (all values are plain ASCII: no quoting needed).
		// The "day" argument is the natural/action day: it drives the archive file name
		// and the date part of update_dt. The "trading_day" column shows the CTP
		// TradingDay, which can differ from the action day around night sessions.
		std::string tickCsvRow(const Tick & tick, const std::string & day)
		{
			char msBuf[16];
			snprintf(msBuf, sizeof(msBuf), "%03d", tick.updateMillisec < 0 ? 0 : tick.updateMillisec);

			std::string updateDt = dateFromCTP(day);   // action day
			updateDt += ' ';
			updateDt += hmsOf(tick.updateTime);
			updateDt += '.';
			updateDt += msBuf;

			std::ostringstream os;
			os << tradingDayOf(tick) << ","
				<< tick.exchangeID << ","
				<< tick.instrumentID << ","
				<< updateDt << ","
				<< tick.updateTime << ","
				<< tick.updateMillisec << ","
				<< csvNum(tick.previousSettlementPrice) << ","
				<< csvNum(tick.previousClosePrice) << ","
				<< csvNum(tick.previousOpenInterest) << ","
				<< csvNum(tick.latestPrice) << ","
				<< csvNum(tick.openPrice) << ","
				<< csvNum(tick.highestPrice) << ","
				<< csvNum(tick.lowestPrice) << ","
				<< csvInt(tick.volume) << ","
				<< csvNum(tick.turnover) << ","
				<< csvNum(tick.openInterest) << ","
				<< csvNum(tick.closePrice) << ","
				<< csvNum(tick.settlementPrice) << ","
				<< csvNum(tick.upperLimitPrice) << ","
				<< csvNum(tick.lowerLimitPrice);

			// only depth level 1 is emitted
			os << ","
				<< csvNum(tick.bidPrice[0]) << ","
				<< csvInt(tick.bidVolume[0]) << ","
				<< csvNum(tick.askPrice[0]) << ","
				<< csvInt(tick.askVolume[0]);
			os << "\n";
			return os.str();
		}
	}

	LocalFileWriter::LocalFileWriter(const LocalFileConfig & cfg)
		: cfg_(cfg), stopping_(false),
		currentSeq_(0), currentSize_(0),
		received_(0), written_(0), dropped_(0), failedRows_(0)
	{
	}

	LocalFileWriter::~LocalFileWriter()
	{
		stop();
	}

	void LocalFileWriter::start()
	{
		if (worker_.joinable())
			return;

		if (!mkdirs(cfg_.dataDir))
		{
			MD_LOG_WARN("can not create data directory [%s]", cfg_.dataDir.c_str());
		}

		stopping_ = false;
		worker_ = std::thread(&LocalFileWriter::run, this);
		MD_LOG_INFO("csv file writer started, directory [%s], rotate at [%lld] bytes: %s_YYYYMMDD_001.csv...",
			cfg_.dataDir.c_str(), cfg_.maxFileSize, cfg_.filePrefix.c_str());
	}

	void LocalFileWriter::stop()
	{
		if (!worker_.joinable())
			return;

		{
			std::lock_guard<std::mutex> lock(mutex_);
			stopping_ = true;
		}
		cv_.notify_all();
		worker_.join();

		MD_LOG_INFO("csv file writer stopped");
	}

	size_t LocalFileWriter::queueSize() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return queue_.size();
	}

	void LocalFileWriter::push(const Tick & tick)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (stopping_)
		{
			dropped_.fetch_add(1);
			return;
		}
		received_.fetch_add(1);

		// When the queue is full the worker is probably stuck on a broken /
		// locked disk. Wait for space instead of dropping, but not forever:
		// after 120s of an unusable disk the tick is counted as dropped.
		const std::chrono::steady_clock::time_point deadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(120);
		while ((int)queue_.size() >= cfg_.maxQueuedRows && !stopping_ &&
			std::chrono::steady_clock::now() < deadline)
		{
			cv_.wait_for(lock, std::chrono::milliseconds(20));
		}

		if (!stopping_ && (int)queue_.size() < cfg_.maxQueuedRows)
		{
			queue_.push_back(tick);
			cv_.notify_one();
		}
		else
		{
			dropped_.fetch_add(1);
			MD_LOG_ERROR("csv queue full for more than 120s, one tick dropped "
				"(disk may be unusable)");
		}
	}

	std::string LocalFileWriter::partPath(const std::string & day, int seq) const
	{
		char seqBuf[16];
		snprintf(seqBuf, sizeof(seqBuf), "%03d", seq);
		return cfg_.dataDir + "/" + cfg_.filePrefix + "_" + day + "_" + seqBuf + ".csv";
	}

	// Picks the part file that must receive the next chunk of a day:
	//  - first chunk of a day (or after a restart): scan seq=1.. and resume the
	//    largest existing part that is still below the size limit;
	//  - otherwise: advance to seq+1 when the current part reached the limit.
	std::string LocalFileWriter::openDayPart(const std::string & day)
	{
		if (currentDay_ != day)
		{
			currentDay_ = day;
			currentSeq_ = 1;
			while (fileSize(partPath(day, currentSeq_)) >= cfg_.maxFileSize)
				currentSeq_++;
		}
		else if (currentSize_ >= cfg_.maxFileSize)
		{
			currentSeq_++;
		}

		long long sz = fileSize(partPath(day, currentSeq_));
		currentSize_ = sz;
		return partPath(day, currentSeq_);
	}

	// Low-level append of one chunk. Retries the open a few times (short-lived
	// anti-virus / viewer locks). Writes the CSV header once per file (when the
	// file does not exist yet or is empty). On a write error the file is rolled
	// back to its previous size so a later replay of the chunk is safe.
	bool LocalFileWriter::writePart(const std::string & path, const std::string & rows)
	{
		FILE * fp = NULL;
		for (int attempt = 0; attempt < 5; attempt++)
		{
			fp = fopen(path.c_str(), "ab");
			if (fp)
				break;
#ifdef WIN32
			int winErr = (int)GetLastError();
#else
			int winErr = 0;
#endif
			if (attempt == 0)
			{
				MD_LOG_WARN("can not open csv file [%s] for append (errno [%d] %s, win32 [%d]), retrying",
					path.c_str(), errno, strerror(errno), winErr);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (!fp)
		{
#ifdef WIN32
			int winErr = (int)GetLastError();
#else
			int winErr = 0;
#endif
			MD_LOG_WARN("can not open csv file [%s] for append after retries (errno [%d] %s, win32 [%d])",
				path.c_str(), errno, strerror(errno), winErr);
			return false;
		}

		long long before = fileSize(path);   // 0 => new file: emit the header
		bool ok = true;
		if (before == 0)
		{
			if (fwrite(CSV_HEADER, 1, strlen(CSV_HEADER), fp) != strlen(CSV_HEADER))
				ok = false;
		}
		if (ok && fwrite(rows.data(), 1, rows.size(), fp) != rows.size())
			ok = false;
		if (ok && fflush(fp) != 0)
			ok = false;

		fclose(fp);

		if (ok)
		{
			MD_LOG_DEBUG("csv appended [%s], bytes [%d]", path.c_str(), (int)rows.size());
			return true;
		}

		// IO error: roll the file back so a later replay of this chunk cannot
		// duplicate the rows that were already partially written.
		truncateFile(path, before);
		MD_LOG_ERROR("csv append IO error on [%s], file rolled back to [%lld] bytes",
			path.c_str(), before);
		return false;
	}

	// Append the csv rows of one natural (action) day to the right part file. When the
	// chosen part is locked and stays unopenable, rotate to brand-new parts and
	// retry, so the chunk is only reported failed after several fresh parts
	// failed too (in that case the caller requeues the chunk: nothing is lost).
	bool LocalFileWriter::flushOneDay(const std::string & day, const std::string & rows)
	{
		if (rows.empty())
			return true;

		std::string path = openDayPart(day);
		if (writePart(path, rows))
			return true;

		MD_LOG_WARN("append to [%s] failed, rotating to a new part file", path.c_str());
		for (int attempt = 0; attempt < 5; attempt++)
		{
			currentSeq_++;
			currentSize_ = 0;
			std::string fresh = partPath(day, currentSeq_);
			if (writePart(fresh, rows))
				return true;
		}
		return false;
	}

	// Writes the longest prefix of batch whose ticks share the same action
	// (natural) day so each archive file holds one calendar day. On success
	// the handled ticks are erased from batch. On failure batch is left
	// unchanged (the caller requeues it), so nothing is lost and nothing is
	// duplicated.
	bool LocalFileWriter::flushPrefix(std::deque<Tick> & batch)
	{
			if (batch.empty())
				return true;

			std::string day = actionDayOf(batch.front());
			size_t n = 1;
			while (n < batch.size() && actionDayOf(batch[n]) == day)
				n++;

		std::string rows;
		rows.reserve(n * 220);
		for (size_t i = 0; i < n; i++)
			rows += tickCsvRow(batch[i], day);

		if (!flushOneDay(day, rows))
			return false;

		written_.fetch_add(n);
		for (size_t i = 0; i < n; i++)
			batch.pop_front();
		return true;
	}

	void LocalFileWriter::run()
	{
		unsigned int consecutiveErrors = 0;

		while (true)
		{
			std::deque<Tick> batch;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				if (stopping_ && queue_.empty())
					break;

				if (queue_.empty())
				{
					cv_.wait_for(lock, std::chrono::milliseconds(cfg_.flushIntervalMs));
					if (stopping_ && queue_.empty())
						break;
				}

				// move at most 5000 rows per file write to bound memory usage
				size_t take = queue_.size();
				if (take > 5000)
					take = 5000;
				for (size_t i = 0; i < take; i++)
				{
					batch.push_back(queue_.front());
					queue_.pop_front();
				}
				if (take > 0)
					cv_.notify_all();   // a push() may be waiting for space
			}

			if (batch.empty())
				continue;

			while (!batch.empty())
			{
				if (flushPrefix(batch))
				{
					consecutiveErrors = 0;
					continue;
				}

				// The disk is not writable right now: put the whole remaining
				// chunk back at the front of the queue (order preserved) and
				// back off. No rows are dropped here.
				{
					std::lock_guard<std::mutex> lock(mutex_);
					while (!batch.empty())
					{
						queue_.push_front(batch.back());
						batch.pop_back();
					}
					cv_.notify_all();
				}

				consecutiveErrors++;
				int sleepMs = 1000;
				if (consecutiveErrors > 1)
					sleepMs = (consecutiveErrors > 30) ? 30000 : 1000 * (int)consecutiveErrors;
				MD_LOG_WARN("csv write failed, batch requeued (rows [%d]), next try in [%d] ms "
					"(consecutive errors [%u])", (int)batch.size(), sleepMs, consecutiveErrors);
				std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
			}
		}

		// flush remaining data on shutdown (bounded retries so stop() returns
		// even when the disk stays unusable)
		std::deque<Tick> pending;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			while (!queue_.empty())
			{
				pending.push_back(queue_.front());
				queue_.pop_front();
			}
		}

		int retries = 0;
		while (!pending.empty() && retries < 120)
		{
			if (flushPrefix(pending))
			{
				retries = 0;
				continue;
			}
			retries++;
			MD_LOG_WARN("csv shutdown flush failed, retry [%d]/120", retries);
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		if (!pending.empty())
		{
			failedRows_.fetch_add(pending.size());
			MD_LOG_ERROR("csv shutdown flush gave up after retries, rows [%d] not persisted "
				"(disk unavailable)", (int)pending.size());
		}

		MD_LOG_INFO("csv file writer worker stopped");
	}
}