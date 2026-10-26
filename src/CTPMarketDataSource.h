#ifndef MDGW_CTP_MARKET_DATA_SOURCE_H
#define MDGW_CTP_MARKET_DATA_SOURCE_H

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "Config.h"
#include "ContractList.h"
#include "Tick.h"
#include "TickWriter.h"
#include "ThostFtdcMdApi.h"

namespace mdgw
{
	class MarketDataDispatcher;
	class TickWriter;

	// CTP market-data receiver, extracted from CTPExchangeSession.cpp of the
	// trading engine:
	//
	//   CTPMarketDataSession (CThostFtdcMdSpi) + CTPOneExchangeMarketDataReceiver
	//
	// Differences introduced for the standalone service:
	//  - the instrument list fed into SubscribeMarketData is NOT taken from a
	//    trade-side contract query any more; it is the whole-market list built
	//    by Service (openctp dict service, with a local cache fallback) and
	//    sharded over all configured CTP sources;
	//  - subscription is chunked (SubscribeChunkSize contracts per call);
	//  - every OnRtnDepthMarketData tick is converted into a mdgw::Tick and
	//    forwarded to the market-data dispatcher (broadcast) and, when fresh
	//    enough, to the configured tick writer.
	class CTPMarketDataSource : public CThostFtdcMdSpi
	{
	public:
		CTPMarketDataSource(const CTPMdSourceConfig & cfg,
			const std::vector<InstrumentInfo> & instruments,
			MarketDataDispatcher * dispatcher, TickWriter * writer);
		~CTPMarketDataSource();

		// Creates the md API, registers the front servers and starts the API.
		// Returns false (with err) when the API object can not be created.
		bool start(std::string & err);

		// Stops the tick flow without touching the CTP API: the SPI callback
		// interface is unregistered, so no OnRtn* callback can reach this object.
		void stopReceiving();

		// Releases the CTP API and waits for its threads. Must run on the thread
		// that created the API; a watchdog bounds the wait because Release() can
		// block for ever on an unreachable front.
		void releaseApi();

		// stopReceiving() + releaseApi(); kept for the destructor path.
		void stop();

		std::string name() const { return brokerID_ + ":" + userID_; }
		bool isConnected() const { return connected_.load(); }
		bool isLoggedIn() const { return loggedin_.load(); }
		unsigned long long tickCount() const { return tickCount_.load(); }
		size_t subscribedContracts() const { return contracts_.size(); }

	private:
		// ---- CThostFtdcMdSpi overrides (invoked on CTP internal threads) --
		virtual void OnFrontConnected();
		virtual void OnFrontDisconnected(int nReason);
		virtual void OnHeartBeatWarning(int nTimeLapse);
		virtual void OnRspUserLogin(CThostFtdcRspUserLoginField * pRspUserLogin,
			CThostFtdcRspInfoField * pRspInfo, int nRequestID, bool bIsLast);
		virtual void OnRspSubMarketData(CThostFtdcSpecificInstrumentField * pSpecificInstrument,
			CThostFtdcRspInfoField * pRspInfo, int nRequestID, bool bIsLast);
		virtual void OnRspError(CThostFtdcRspInfoField * pRspInfo, int nRequestID, bool bIsLast);
		virtual void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField * pDepthMarketData);

		// Exception-safe implementations behind the callbacks above: the
		// overrides only guard the C++/C ABI boundary of the CTP library and
		// delegate here.
		void handleFrontConnected();
		void handleFrontDisconnected(int nReason);
		void handleHeartBeatWarning(int nTimeLapse);
		void handleRspUserLogin(CThostFtdcRspUserLoginField * pRspUserLogin,
			CThostFtdcRspInfoField * pRspInfo, int nRequestID, bool bIsLast);
		void handleRspSubMarketData(CThostFtdcSpecificInstrumentField * pSpecificInstrument,
			CThostFtdcRspInfoField * pRspInfo, int nRequestID, bool bIsLast);
		void handleRspError(CThostFtdcRspInfoField * pRspInfo, int nRequestID, bool bIsLast);
		void handleRtnDepthMarketData(CThostFtdcDepthMarketDataField * pDepthMarketData);

		// ---- internal ---------------------------------------------------
		void login();
		void subscribeAll();

		const CTPMdSourceConfig cfg_;
		std::vector<InstrumentInfo> instruments_;       // instruments owned by this source
		std::vector<std::string> contracts_;            // contract ids (sorted, dedup)
		std::map<std::string, std::string> instrument2Exchange_; // contract -> canonical exchange

		std::string brokerID_;
		std::string userID_;

		CThostFtdcMdApi * userApi_;
		std::thread joinThread_;     // runs CThostFtdcMdApi::Join() until Release()

		std::atomic<bool> started_;
		std::atomic<bool> stopping_;
		std::atomic<bool> connected_;
		std::atomic<bool> loggedin_;
		std::atomic<unsigned long long> tickCount_;
		std::atomic<unsigned long long> subscribeChunks_; // chunks requested OK

		MarketDataDispatcher * dispatcher_;
		TickWriter * writer_;
	};
}

#endif // MDGW_CTP_MARKET_DATA_SOURCE_H
