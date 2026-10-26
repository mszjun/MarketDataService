#ifndef MDGW_CLICKHOUSE_WRITER_H
#define MDGW_CLICKHOUSE_WRITER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Config.h"
#include "Tick.h"
#include "TickWriter.h"

namespace mdgw
{
	// Asynchronous ClickHouse writer.
	//
	// Ticks pushed by the CTP receiver thread are buffered and inserted in
	// batches through the ClickHouse HTTP interface
	//
	//     POST /?query=INSERT INTO <db>.<table> FORMAT JSONEachRow
	//
	// with the batch as the request body. No third-party ClickHouse client
	// library is required. A dedicated worker thread performs the flush; the
	// CTP / broadcast path never blocks on ClickHouse (overflowing ticks are
	// counted and dropped instead).
	class ClickHouseWriter : public TickWriter
	{
	public:
		explicit ClickHouseWriter(const ClickHouseConfig & cfg);
		~ClickHouseWriter();

		void start();
		void stop();

		// thread-safe; called from any CTP receiver thread
		void push(const Tick & tick);

		// statistics
		size_t queueSize() const;
		unsigned long long received() const { return received_.load(); }
		unsigned long long written() const { return written_.load(); }
		unsigned long long dropped() const { return dropped_.load(); }
		unsigned long long failedRows() const { return failedRows_.load(); }

	private:
		void run();
		bool flushBatch(std::vector<Tick> & batch);
		bool httpPost(const std::string & pathAndQuery, const std::string & body, int readTimeoutMs);

		// synchronous startup connectivity check: SELECT 1 through the HTTP
		// interface (must be the HTTP port 8123, NOT the native TCP port 9000)
		bool ping();

		ClickHouseConfig cfg_;

		mutable std::mutex mutex_;
		std::condition_variable cv_;
		std::deque<Tick> queue_;
		bool stopping_;

		std::thread worker_;

		// informational per-row sequence, incremented by the worker thread
		long long rowSeq_;

		std::atomic<unsigned long long> received_;
		std::atomic<unsigned long long> written_;
		std::atomic<unsigned long long> dropped_;
		std::atomic<unsigned long long> failedRows_;
	};
}

#endif // MDGW_CLICKHOUSE_WRITER_H
