#ifndef MDGW_TICK_WRITER_H
#define MDGW_TICK_WRITER_H

#include <atomic>
#include <string>
#include "Tick.h"

namespace mdgw
{
	// Abstract base class for tick writers (ClickHouseWriter, LocalFileWriter, etc.)
	class TickWriter
	{
	public:
		virtual ~TickWriter() {}

		// Lifecycle
		virtual void start() = 0;
		virtual void stop() = 0;

		// Thread-safe; called from any CTP receiver thread
		virtual void push(const Tick & tick) = 0;

		// Statistics
		virtual size_t queueSize() const = 0;
		virtual unsigned long long received() const = 0;
		virtual unsigned long long written() const = 0;
		virtual unsigned long long dropped() const = 0;
		virtual unsigned long long failedRows() const = 0;
	};

	// Simple writer that does nothing (for testing or when storage is disabled)
	class NullWriter : public TickWriter
	{
	public:
		void start() override {}
		void stop() override {}
		void push(const Tick & tick) override {}
		size_t queueSize() const override { return 0; }
		unsigned long long received() const override { return 0; }
		unsigned long long written() const override { return 0; }
		unsigned long long dropped() const override { return 0; }
		unsigned long long failedRows() const override { return 0; }
	};
}

#endif // MDGW_TICK_WRITER_H