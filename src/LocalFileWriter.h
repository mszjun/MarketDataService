#ifndef MDGW_LOCAL_FILE_WRITER_H
#define MDGW_LOCAL_FILE_WRITER_H

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
	// Asynchronous CSV file writer with size-based rotation and no data loss
	// during normal operation.
	//
	// Ticks pushed by the CTP receiver thread are buffered in memory (deque,
	// bound by LocalFileConfig.maxQueuedRows) and appended by a dedicated
	// worker thread into CSV files, one file series per natural (action) day:
	//
	//     <DataDir>/<FilePrefix>_<ActionDay>_<seq>.csv
	//     e.g. MarketDataCsv/tick_20260904_001.csv, ..._002.csv
	//
	// Each row keeps the CTP TradingDay in its own "trading_day" column (it can
	// differ from the action day around night sessions); update_dt and the file
	// name follow the action day.
	//
	// * rotation: once a part reaches LocalFileConfig.maxFileSize the worker
	//   closes it and continues with seq+1. The CSV header is written once per
	//   part. If the writer is created again against the same dataDir (e.g.
	//   rebuilt inside one process) it resumes the last existing part below
	//   the limit instead of starting over. Note that Config appends a startup
	//   timestamp to dataDir, so a normal process restart starts a new
	//   directory instead.
	// * no data loss:
	//   - a failed append is retried with backoff; if a part stays locked
	//     (e.g. opened by Excel or an anti-virus scan) the writer rotates to a
	//     fresh part so writing can continue instead of dropping the batch;
	//   - when the in-memory queue is full, push() waits for the worker to
	//     drain instead of dropping (dropped_ is only incremented when the
	//     disk stays unusable for a long time);
	//   - on stop() every remaining queued tick is flushed to disk.
	class LocalFileWriter : public TickWriter
	{
	public:
		explicit LocalFileWriter(const LocalFileConfig & cfg);
		~LocalFileWriter();

		void start();
		void stop();

		// thread-safe; called from any CTP receiver thread. Blocks briefly
		// (in 20ms slices) while the queue is full so ticks are not dropped.
		void push(const Tick & tick);

		// statistics
		size_t queueSize() const;
		unsigned long long received() const { return received_.load(); }
		unsigned long long written() const { return written_.load(); }
		unsigned long long dropped() const { return dropped_.load(); }
		unsigned long long failedRows() const { return failedRows_.load(); }

	private:
		void run();
		// Appends the csv rows of one natural (action) day to the current part,
		// rotating when the size limit is hit or the current part is locked.
		// Returns true when the whole chunk was written.
		bool flushOneDay(const std::string & day, const std::string & rows);

		// Low-level append with a few open retries (absorbs short-lived
		// anti-virus / viewer locks). Writes the CSV header when the file
		// does not exist yet or is empty.
		bool writePart(const std::string & path, const std::string & rows);

		// Picks the part file that must receive the next chunk of a day:
		//  - on the first chunk of a day (or after restart) it scans seq=1..n
		//    and resumes the largest part still below the size limit;
		//  - otherwise it advances seq when the current part reached the limit.
		std::string openDayPart(const std::string & day);

		// Writes the longest same-trading-day prefix of batch; erases it from
		// batch on success, leaves batch untouched on failure (caller requeues).
		bool flushPrefix(std::deque<Tick> & batch);

		std::string partPath(const std::string & day, int seq) const;

		LocalFileConfig cfg_;

		mutable std::mutex mutex_;
		std::condition_variable cv_;   // data available / space available
		std::deque<Tick> queue_;
		bool stopping_;

		std::thread worker_;

		// rotation state (worker thread only)
		std::string currentDay_;
		int currentSeq_;
		long long currentSize_;

		std::atomic<unsigned long long> received_;
		std::atomic<unsigned long long> written_;
		std::atomic<unsigned long long> dropped_;
		std::atomic<unsigned long long> failedRows_;
	};
}

#endif // MDGW_LOCAL_FILE_WRITER_H