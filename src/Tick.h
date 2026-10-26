#ifndef MDGW_TICK_H
#define MDGW_TICK_H

#include <string>

namespace mdgw
{
	// Canonical (raptor-style) exchange ids used by the trading engine,
	// identical to Common.h of the original project.
	//   CZCE (CTP) <-> ZCE,  CFFEX (CTP) <-> CFFE, others stay unchanged.
	extern const char * const EXCHANGE_ID_ZCE;    // Zhengzhou
	extern const char * const EXCHANGE_ID_DCE;    // Dalian
	extern const char * const EXCHANGE_ID_SHFE;   // Shanghai
	extern const char * const EXCHANGE_ID_CFFE;   // CFFEX
	extern const char * const EXCHANGE_ID_INE;    // INE (crude oil)
	extern const char * const EXCHANGE_ID_GFEX;   // Guangzhou

	// CTP native exchange id -> canonical exchange id.
	// (keeps unknown ids unchanged)
	std::string normalizeExchangeID(const std::string & ctpExchangeID);

	// Convert a DBL_MAX "no value" sent by CTP into 0, mirroring doubleMaxTo0().
	double doubleMaxTo0(double v);

	// A single depth-market-data snapshot ("tick").
	//
	// It is a trimmed copy of the fields the original CTPExchangeSession.cpp
	// copies inside CTPMarketDataSession::OnRtnDepthMarketData() into its
	// raptor::MarketData structure, plus TradingDay/ActionDay/raw CTP exchange
	// which are useful for ClickHouse storage.
	struct Tick
	{
		std::string tradingDay;     // CTP TradingDay, e.g. "20240315"
		std::string actionDay;      // CTP ActionDay,  e.g. "20240315"
		std::string exchangeID;     // canonical exchange id (see normalizeExchangeID)
		std::string exchangeRaw;    // CTP native exchange id as delivered in the tick
		std::string instrumentID;   // CTP InstrumentID, e.g. "rb2405"
		std::string updateTime;     // "HH:MM:SS.mmm" (original sprintf("%s.%d") semantics)
		int updateMillisec;         // raw UpdateMillisec, used to build the CH timestamp

		double previousSettlementPrice; // PreSettlementPrice
		double previousClosePrice;      // PreClosePrice
		double previousOpenInterest;    // PreOpenInterest

		double latestPrice;             // LastPrice
		double openPrice;               // OpenPrice
		double highestPrice;            // HighestPrice
		double lowestPrice;             // LowestPrice
		int volume;                     // Volume
		double turnover;                // Turnover
		double openInterest;            // OpenInterest
		double closePrice;              // ClosePrice
		double settlementPrice;         // SettlementPrice
		double upperLimitPrice;         // UpperLimitPrice
		double lowerLimitPrice;         // LowerLimitPrice

		// Depth levels 2..5 are NOT collected any more: only index [0] (level
		// 1) carries real bid/ask data, indexes [1..4] stay 0. The arrays are
		// kept so the legacy broadcast wire format (BPx1..APx5 keys) and any
		// future code can still index them.
		double bidPrice[5];
		int bidVolume[5];
		double askPrice[5];
		int askVolume[5];

		Tick()
			: updateMillisec(0), previousSettlementPrice(0), previousClosePrice(0),
			previousOpenInterest(0), latestPrice(0), openPrice(0), highestPrice(0),
			lowestPrice(0), volume(0), turnover(0), openInterest(0), closePrice(0),
			settlementPrice(0), upperLimitPrice(0), lowerLimitPrice(0)
		{
			for (int i = 0; i < 5; i++)
			{
				bidPrice[i] = askPrice[i] = 0;
				bidVolume[i] = askVolume[i] = 0;
			}
		}
	};
}

#endif // MDGW_TICK_H
