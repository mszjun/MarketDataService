#ifndef MDGW_FANOUT_TYPES_H
#define MDGW_FANOUT_TYPES_H

#include <map>
#include <string>

namespace mdgw
{
	// One JSON-encoded market-data message ready to be pushed to the wire.
	struct EncodedItem
	{
		long long seq;      // global sequence assigned by the dispatcher
		std::string data;   // single JSON line, terminated by "\r\n" (see BroadcastEncoder)
	};

	// Latest encoded item per instrument ("snapshot"), used by the queue-cut /
	// recovery logic exactly like MarketDataSnapshot_t in the original engine.
	typedef std::map<std::string, EncodedItem> Snapshot;
}

#endif // MDGW_FANOUT_TYPES_H
