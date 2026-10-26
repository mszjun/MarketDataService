#ifndef MDGW_BROADCAST_ENCODER_H
#define MDGW_BROADCAST_ENCODER_H

#include <string>
#include "Tick.h"

namespace mdgw
{
	// Encodes one tick into the market-data wire format used by the original
	// trading engine (raptor::exchange::printMessage in ExchangeMessage.cpp):
	//
	//   single line, unformatted JSON, terminated by "\r\n"
	//   message type marker "T": 300
	//
	// Only bid/ask level 1 is emitted (BPx1/BV1/APx1/AV1): the CTP receiver no
	// longer collects depth levels 2..5, so the level argument is accepted for
	// API compatibility but does not add extra book levels any more.
	//
	// Note: the original encoder emitted CPx/SPx because the CTP path zeroes
	// DBL_MAX through doubleMaxTo0() before encoding; we reproduce that.
	std::string encodeBroadcastJson(const Tick & tick, long long seq, int level);

	// Builds a readable multi-line json (used for debug logs only).
	std::string encodeBroadcastJsonPretty(const Tick & tick, long long seq, int level);
}

#endif // MDGW_BROADCAST_ENCODER_H
