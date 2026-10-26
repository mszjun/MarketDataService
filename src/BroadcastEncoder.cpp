#include "BroadcastEncoder.h"

#include <cstdio>
#include <cstdlib>
#include <float.h>
#include "cJSON.h"

namespace mdgw
{
	// Keep every key/order identical to the original wire encoder
	// (raptor::exchange::printMessage in ExchangeMessage.cpp) so existing
	// market-data consumers do not need any change.
	static cJSON * buildMessageObject(const Tick & tick, long long seq, int level)
	{
		(void)level; // depth levels 2..5 are no longer collected; only level 1 is emitted
		cJSON * root = cJSON_CreateObject();

		cJSON_AddNumberToObject(root, "T", 300);
		cJSON_AddNumberToObject(root, "Seq", (double)seq);
		cJSON_AddStringToObject(root, "EX", tick.exchangeID.c_str());
		cJSON_AddStringToObject(root, "InsID", tick.instrumentID.c_str());

		cJSON_AddNumberToObject(root, "PreStlPx", tick.previousSettlementPrice);
		cJSON_AddNumberToObject(root, "PreClsPx", tick.previousClosePrice);
		cJSON_AddNumberToObject(root, "PreOI", tick.previousOpenInterest);

		cJSON_AddNumberToObject(root, "Px", tick.latestPrice);

		cJSON_AddNumberToObject(root, "OPx", tick.openPrice);
		cJSON_AddNumberToObject(root, "HPx", tick.highestPrice);
		cJSON_AddNumberToObject(root, "LPx", tick.lowestPrice);
		cJSON_AddNumberToObject(root, "V", tick.volume);
		cJSON_AddNumberToObject(root, "Tov", tick.turnover);
		cJSON_AddNumberToObject(root, "OI", tick.openInterest);

		// Original code only emitted CPx/SPx when the value was still DBL_MAX;
		// the CTP receiver zeroes DBL_MAX first, therefore both keys are always
		// present on the wire. We reproduce that behaviour.
		cJSON_AddNumberToObject(root, "CPx", tick.closePrice);
		cJSON_AddNumberToObject(root, "SPx", tick.settlementPrice);

		cJSON_AddNumberToObject(root, "ULPx", tick.upperLimitPrice);
		cJSON_AddNumberToObject(root, "LLPx", tick.lowerLimitPrice);

		cJSON_AddStringToObject(root, "Tm", tick.updateTime.c_str());

		cJSON_AddNumberToObject(root, "BPx1", tick.bidPrice[0]);
		cJSON_AddNumberToObject(root, "BV1", tick.bidVolume[0]);
		cJSON_AddNumberToObject(root, "APx1", tick.askPrice[0]);
		cJSON_AddNumberToObject(root, "AV1", tick.askVolume[0]);

		// Depth levels 2..5 are no longer collected (the CTP receiver only
		// fills level 1), so nothing is appended for level >= 5 either.

		return root;
	}

	std::string encodeBroadcastJson(const Tick & tick, long long seq, int level)
	{
		cJSON * root = buildMessageObject(tick, seq, level);
		char * text = cJSON_PrintUnformatted(root);
		std::string result = text ? text : "{}";
		std::free(text);
		cJSON_Delete(root);
		result.append("\r\n");
		return result;
	}

	std::string encodeBroadcastJsonPretty(const Tick & tick, long long seq, int level)
	{
		cJSON * root = buildMessageObject(tick, seq, level);
		char * text = cJSON_Print(root);
		std::string result = text ? text : "{}";
		std::free(text);
		cJSON_Delete(root);
		return result;
	}
}
