#include "CTPMarketDataSource.h"

#include <stdio.h>
#include <iostream>
#include <string.h>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <memory>
#include <thread>
#include <ctime>
#include "TickWriter.h"
#include "Logger.h"
#include "MarketDataDispatcher.h"

namespace mdgw
{
	namespace
	{
		const char * const TENGINE_USER_PRODUCT_INFO = "tengine_md";

		// CThostFtdcMdApi::Release() is called from the thread that created the
		// API (calling it elsewhere segfaults); with an unreachable or half-open
		// front it can block forever, so the shutdown is bounded by a watchdog.
		const int CTP_RELEASE_TIMEOUT_MS = 10000;
	}

	CTPMarketDataSource::CTPMarketDataSource(const CTPMdSourceConfig & cfg,
		const std::vector<InstrumentInfo> & instruments,
		MarketDataDispatcher * dispatcher, TickWriter * writer)
		: cfg_(cfg), instruments_(instruments), brokerID_(cfg.brokerID), userID_(cfg.userID),
		userApi_(NULL), started_(false), stopping_(false), connected_(false), loggedin_(false),
		tickCount_(0), subscribeChunks_(0), dispatcher_(dispatcher), writer_(writer)
	{
		// unique, sorted contract list + contract -> exchange map
		for (size_t i = 0; i < instruments_.size(); i++)
		{
			const InstrumentInfo & info = instruments_[i];
			if (instrument2Exchange_.find(info.contractID) == instrument2Exchange_.end())
			{
				instrument2Exchange_[info.contractID] = info.exchangeID;
				contracts_.push_back(info.contractID);
			}
		}
	}

	CTPMarketDataSource::~CTPMarketDataSource()
	{
		stop();
	}

	// ------------------------------------------------------------------
	// CTP callback boundary
	//
	// The On*() methods below are invoked on CTP's own threads. They must not
	// let an exception escape into the C library: unwinding through the C ABI
	// is undefined and calls std::terminate. Each override only guards that
	// boundary and delegates to the matching handle*() implementation.
	// ------------------------------------------------------------------
	void CTPMarketDataSource::OnFrontConnected()
	{
		try
		{
			handleFrontConnected();
		}
		catch (const std::exception & ex)
		{
			MD_LOG_ERROR("source [%s] OnFrontConnected failed: %s", brokerID_.c_str(), ex.what());
		}
		catch (...)
		{
			MD_LOG_ERROR("source [%s] OnFrontConnected failed: unknown exception",
				brokerID_.c_str());
		}
	}

	void CTPMarketDataSource::OnFrontDisconnected(int nReason)
	{
		try
		{
			handleFrontDisconnected(nReason);
		}
		catch (const std::exception & ex)
		{
			MD_LOG_ERROR("source [%s] OnFrontDisconnected failed: %s", brokerID_.c_str(), ex.what());
		}
		catch (...)
		{
			MD_LOG_ERROR("source [%s] OnFrontDisconnected failed: unknown exception",
				brokerID_.c_str());
		}
	}

	void CTPMarketDataSource::OnHeartBeatWarning(int nTimeLapse)
	{
		try
		{
			handleHeartBeatWarning(nTimeLapse);
		}
		catch (const std::exception & ex)
		{
			MD_LOG_ERROR("source [%s] OnHeartBeatWarning failed: %s", brokerID_.c_str(), ex.what());
		}
		catch (...)
		{
			MD_LOG_ERROR("source [%s] OnHeartBeatWarning failed: unknown exception",
				brokerID_.c_str());
		}
	}

	void CTPMarketDataSource::OnRspUserLogin(CThostFtdcRspUserLoginField * pRspUserLogin,
		CThostFtdcRspInfoField * pRspInfo, int nRequestID, bool bIsLast)
	{
		try
		{
			handleRspUserLogin(pRspUserLogin, pRspInfo, nRequestID, bIsLast);
		}
		catch (const std::exception & ex)
		{
			MD_LOG_ERROR("source [%s] OnRspUserLogin failed: %s", brokerID_.c_str(), ex.what());
		}
		catch (...)
		{
			MD_LOG_ERROR("source [%s] OnRspUserLogin failed: unknown exception", brokerID_.c_str());
		}
	}

	void CTPMarketDataSource::OnRspSubMarketData(CThostFtdcSpecificInstrumentField * pSpecificInstrument,
		CThostFtdcRspInfoField * pRspInfo, int nRequestID, bool bIsLast)
	{
		try
		{
			handleRspSubMarketData(pSpecificInstrument, pRspInfo, nRequestID, bIsLast);
		}
		catch (const std::exception & ex)
		{
			MD_LOG_ERROR("source [%s] OnRspSubMarketData failed: %s", brokerID_.c_str(), ex.what());
		}
		catch (...)
		{
			MD_LOG_ERROR("source [%s] OnRspSubMarketData failed: unknown exception",
				brokerID_.c_str());
		}
	}

	void CTPMarketDataSource::OnRspError(CThostFtdcRspInfoField * pRspInfo, int nRequestID, bool bIsLast)
	{
		try
		{
			handleRspError(pRspInfo, nRequestID, bIsLast);
		}
		catch (const std::exception & ex)
		{
			MD_LOG_ERROR("source [%s] OnRspError failed: %s", brokerID_.c_str(), ex.what());
		}
		catch (...)
		{
			MD_LOG_ERROR("source [%s] OnRspError failed: unknown exception", brokerID_.c_str());
		}
	}

	void CTPMarketDataSource::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField * pDepthMarketData)
	{
		try
		{
			handleRtnDepthMarketData(pDepthMarketData);
		}
		catch (const std::exception & ex)
		{
			MD_LOG_ERROR("source [%s] OnRtnDepthMarketData failed: %s",
				brokerID_.c_str(), ex.what());
		}
		catch (...)
		{
			MD_LOG_ERROR("source [%s] OnRtnDepthMarketData failed: unknown exception",
				brokerID_.c_str());
		}
	}

	bool CTPMarketDataSource::start(std::string & err)
	{
		if (started_.load())
			return true;

		if (cfg_.marketDataServerURIs.empty())
		{
			err = "no MarketDataServerURIs for source " + name();
			return false;
		}
		if (contracts_.empty())
		{
			err = "no instrument subscribed for source " + name();
			return false;
		}

		const char * flowPath = cfg_.flowPath.empty() ? "" : cfg_.flowPath.c_str();
		userApi_ = CThostFtdcMdApi::CreateFtdcMdApi(flowPath);
		if (userApi_ == NULL)
		{
			err = "failed to create CTP MD API for source " + name();
			return false;
		}

		userApi_->RegisterSpi(this);

		started_ = true;
		stopping_ = false;

		for (size_t i = 0; i < cfg_.marketDataServerURIs.size(); i++)
		{
			userApi_->RegisterFront(const_cast<char *>(cfg_.marketDataServerURIs[i].c_str()));
			MD_LOG_INFO("source [%s] registered market-data front [%s]",
				name().c_str(), cfg_.marketDataServerURIs[i].c_str());
		}

		userApi_->Init();
		MD_LOG_INFO("source [%s] api initialized, subscribing [%d] contracts",
			name().c_str(), (int)contracts_.size());

		// Join() blocks this helper thread until Release() completes; the API
		// itself keeps running on its own internal threads.
		joinThread_ = std::thread(&CThostFtdcMdApi::Join, userApi_);
		return true;
	}

	void CTPMarketDataSource::stopReceiving()
	{
		if (!started_.load())
			return;
		if (stopping_.exchange(true))
			return;   // already stopped

		MD_LOG_INFO("source [%s] stopping...", name().c_str());

		// Unregistering the SPI is enough to stop the tick flow: no OnRtn*
		// callback can reach this object any more.
		if (userApi_ != NULL)
			userApi_->RegisterSpi(NULL);
	}

	void CTPMarketDataSource::releaseApi()
	{
		if (!started_.load())
			return;

		MD_LOG_INFO("source [%s] releasing CTP API...", name().c_str());

		// Release() must be called from the thread that created the API: calling
		// it from another thread makes the CTP library crash (SIGSEGV). With an
		// unreachable / half-open front it can block for ever instead, so a
		// watchdog bounds the shutdown. By the time this runs, Service has
		// already stopped the dispatcher and flushed the tick writer, so no
		// buffered ticks are lost if the watchdog has to fire.
		std::shared_ptr<std::atomic<bool> > released(new std::atomic<bool>(false));
		std::thread watchdog([released]() {
			for (int i = 0; i < CTP_RELEASE_TIMEOUT_MS / 100; i++)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				if (released->load())
					return;
			}
			MD_LOG_ERROR("CTP API did not shut down within [%d] ms, "
				"exiting without a clean CTP logout", CTP_RELEASE_TIMEOUT_MS);
			std::_Exit(1);
		});
		watchdog.detach();

		userApi_->Release();
		if (joinThread_.joinable())
			joinThread_.join();

		released->store(true);
		started_ = false;
		stopping_ = false;
		MD_LOG_INFO("source [%s] stopped", name().c_str());
	}

	void CTPMarketDataSource::stop()
	{
		stopReceiving();
		releaseApi();
	}

	void CTPMarketDataSource::handleFrontConnected()
	{
		if (stopping_.load())
			return;
		connected_ = true;
		MD_LOG_INFO("source [%s] market-data front connected", name().c_str());
		login();
	}

	void CTPMarketDataSource::handleFrontDisconnected(int nReason)
	{
		connected_ = false;
		loggedin_ = false;
		MD_LOG_WARN("source [%s] market-data front disconnected, reason [%d]", name().c_str(), nReason);
	}

	void CTPMarketDataSource::handleHeartBeatWarning(int nTimeLapse)
	{
		MD_LOG_DEBUG("source [%s] heart beat warning, time lapse [%d]", name().c_str(), nTimeLapse);
	}

	void CTPMarketDataSource::login()
	{
		if (stopping_.load())
			return;

		CThostFtdcReqUserLoginField reqUserLogin;
		memset(&reqUserLogin, 0, sizeof(reqUserLogin));
		snprintf(reqUserLogin.BrokerID, sizeof(reqUserLogin.BrokerID), "%s", brokerID_.c_str());
		snprintf(reqUserLogin.UserID, sizeof(reqUserLogin.UserID), "%s", userID_.c_str());
		snprintf(reqUserLogin.Password, sizeof(reqUserLogin.Password), "%s", cfg_.password.c_str());
		snprintf(reqUserLogin.UserProductInfo, sizeof(reqUserLogin.UserProductInfo), "%s",
			TENGINE_USER_PRODUCT_INFO);

		int r = userApi_->ReqUserLogin(&reqUserLogin, 0);
		if (r != 0)
		{
			MD_LOG_WARN("source [%s] ReqUserLogin returned error [%d]", name().c_str(), r);
		}
	}

	void CTPMarketDataSource::handleRspUserLogin(CThostFtdcRspUserLoginField * pRspUserLogin,
		CThostFtdcRspInfoField * pRspInfo, int nRequestID, bool bIsLast)
	{
		if (pRspInfo != NULL && pRspInfo->ErrorID != 0)
		{
			MD_LOG_WARN("source [%s] market-data login failed: errorcode [%d] errormsg [%s]",
				name().c_str(), pRspInfo->ErrorID, pRspInfo->ErrorMsg);

			if (!stopping_.load())
			{
				// retry after loginRetrySeconds, same behaviour as the original engine
				int left = cfg_.loginRetrySeconds;
				while (left > 0 && !stopping_.load())
				{
					std::this_thread::sleep_for(std::chrono::seconds(1));
					left--;
				}
				if (!stopping_.load())
					login();
			}
			return;
		}

		loggedin_ = true;
		MD_LOG_INFO("source [%s] market-data login succeeded, trading day [%s]",
			name().c_str(), userApi_->GetTradingDay());
		subscribeAll();
	}

	void CTPMarketDataSource::subscribeAll()
	{
		if (stopping_.load())
			return;

		// chunk the full contract list to stay below CTP request limits
		size_t chunkSize = cfg_.subscribeChunkSize > 0 ? (size_t)cfg_.subscribeChunkSize : 300;
		size_t okChunks = 0;

		for (size_t begin = 0; begin < contracts_.size() && !stopping_.load(); begin += chunkSize)
		{
			size_t end = begin + chunkSize;
			if (end > contracts_.size())
				end = contracts_.size();

			std::vector<char *> chunk;
			chunk.reserve(end - begin);
			for (size_t i = begin; i < end; i++)
				chunk.push_back(const_cast<char *>(contracts_[i].c_str()));

			int result = userApi_->SubscribeMarketData(chunk.empty() ? NULL : &chunk[0], (int)chunk.size());
			if (result != 0)
			{
				MD_LOG_WARN("source [%s] SubscribeMarketData failed for chunk [%d-%d], result [%d]",
					name().c_str(), (int)begin, (int)end, result);
			}
			else
			{
				okChunks++;
			}

			// keep a small gap between chunks to be gentle with the front's
			// flow control
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		subscribeChunks_ = okChunks;
		MD_LOG_INFO("source [%s] subscribed [%d] contracts in [%d] chunks",
			name().c_str(), (int)contracts_.size(), (int)okChunks);
	}

	void CTPMarketDataSource::handleRspSubMarketData(CThostFtdcSpecificInstrumentField * pSpecificInstrument,
		CThostFtdcRspInfoField * pRspInfo, int nRequestID, bool bIsLast)
	{
		if (pRspInfo != NULL && pRspInfo->ErrorID != 0)
		{
			MD_LOG_WARN("source [%s] OnRspSubMarketData error: errorcode [%d] errormsg [%s] instrument [%s]",
				name().c_str(), pRspInfo->ErrorID, pRspInfo->ErrorMsg,
				pSpecificInstrument ? pSpecificInstrument->InstrumentID : "");
		}
	}

	void CTPMarketDataSource::handleRspError(CThostFtdcRspInfoField * pRspInfo, int nRequestID, bool bIsLast)
	{
		if (pRspInfo != NULL)
		{
			MD_LOG_WARN("source [%s] OnRspError: errorcode [%d] errormsg [%s]",
				name().c_str(), pRspInfo->ErrorID, pRspInfo->ErrorMsg);
		}
	}

	// "HH:MM:SS" -> 当天秒数，失败返回 -1
	inline int ParseUpdateTime(const char* t)
	{
		if (!t || t[0] == '\0') return -1;
		if (t[2] != ':' || t[5] != ':') return -1;

		int h = (t[0] - '0') * 10 + (t[1] - '0');
		int m = (t[3] - '0') * 10 + (t[4] - '0');
		int s = (t[6] - '0') * 10 + (t[7] - '0');

		if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59)
			return -1;

		return h * 3600 + m * 60 + s;
	}

	// 当前本地时间 -> 当天秒数
	inline int GetNowSec()
	{
		std::time_t now = std::time(nullptr);
		std::tm tmNow;

#ifdef _WIN32
		localtime_s(&tmNow, &now);
#else
		localtime_r(&now, &tmNow);
#endif

		return tmNow.tm_hour * 3600 + tmNow.tm_min * 60 + tmNow.tm_sec;
	}

	// 环形时间差，处理 23:59:59 和 00:00:00 跨日
	inline int RingDiffSec(int a, int b)
	{
		int d = a > b ? a - b : b - a;
		if (d > 43200)   // 12小时，超过则取另一方向
			d = 86400 - d;
		return d;
	}

	// 行情时间是否足够新
	inline bool IsFreshMarketData(const char* updateTime)
	{
		int u = ParseUpdateTime(updateTime);
		if (u < 0)
			return true; // 解析失败就先放行，或按你策略改成 false

		int now = GetNowSec();
		int diff = RingDiffSec(u, now);

		// 误差 1 分钟以上过滤掉
		return diff <= 60;
	}

	// Port of CTPMarketDataSession::OnRtnDepthMarketData() of CTPExchangeSession.cpp:
	// convert the raw CTP struct into the trading-engine market-data structure,
	// then fan it out to ClickHouse and the unicast/multicast broadcasters.
	void CTPMarketDataSource::handleRtnDepthMarketData(CThostFtdcDepthMarketDataField * pMarketData)
	{
		if (pMarketData == NULL)
			return;
		if (stopping_.load())
			return;

		/*
		// 仅处理合约 y2701
		if (strcmp(pMarketData->InstrumentID, "y2701") == 0 || strcmp(pMarketData->InstrumentID, "rb2701") == 0)
		{
			std::ostringstream oss;
			oss << pMarketData->InstrumentID << ","
				<< pMarketData->TradingDay << ","
				<< pMarketData->ActionDay << ","
				<< pMarketData->UpdateTime << ","
				<< pMarketData->UpdateMillisec << ","
				<< pMarketData->LastPrice << ","
				<< pMarketData->Volume;

			// 输出一行
			std::cout << oss.str() << std::endl;
		}
		*/

		// find the canonical exchange of this instrument (from the loaded
		// whole-market instrument list)
		std::string canonicalExchange;
		std::map<std::string, std::string>::const_iterator it =
			instrument2Exchange_.find(pMarketData->InstrumentID);
		if (it != instrument2Exchange_.end())
			canonicalExchange = it->second;
		else
			canonicalExchange = normalizeExchangeID(pMarketData->ExchangeID);

		Tick t;
		t.tradingDay = pMarketData->TradingDay;
		t.actionDay = pMarketData->ActionDay;
		t.exchangeID = canonicalExchange;
		t.exchangeRaw = pMarketData->ExchangeID;
		t.instrumentID = pMarketData->InstrumentID;

		char updateBuf[16];
		snprintf(updateBuf, sizeof(updateBuf), "%s.%d",
			pMarketData->UpdateTime, pMarketData->UpdateMillisec);
		t.updateTime = updateBuf;
		t.updateMillisec = pMarketData->UpdateMillisec;

		t.previousSettlementPrice = doubleMaxTo0(pMarketData->PreSettlementPrice);
		t.previousClosePrice = doubleMaxTo0(pMarketData->PreClosePrice);
		t.previousOpenInterest = doubleMaxTo0(pMarketData->PreOpenInterest);

		t.latestPrice = doubleMaxTo0(pMarketData->LastPrice);
		t.openPrice = doubleMaxTo0(pMarketData->OpenPrice);
		t.highestPrice = doubleMaxTo0(pMarketData->HighestPrice);
		t.lowestPrice = doubleMaxTo0(pMarketData->LowestPrice);
		t.volume = pMarketData->Volume;
		t.turnover = doubleMaxTo0(pMarketData->Turnover);
		t.openInterest = doubleMaxTo0(pMarketData->OpenInterest);
		t.closePrice = doubleMaxTo0(pMarketData->ClosePrice);
		t.settlementPrice = doubleMaxTo0(pMarketData->SettlementPrice);
		t.upperLimitPrice = doubleMaxTo0(pMarketData->UpperLimitPrice);
		t.lowerLimitPrice = doubleMaxTo0(pMarketData->LowerLimitPrice);

		// only depth level 1 is collected (levels 2..5 are not used)
		t.bidPrice[0] = doubleMaxTo0(pMarketData->BidPrice1);
		t.bidVolume[0] = pMarketData->BidVolume1;
		t.askPrice[0] = doubleMaxTo0(pMarketData->AskPrice1);
		t.askVolume[0] = pMarketData->AskVolume1;

		// Unified filter at the source: ticks without any traded volume are
		// dropped here so they never reach the broadcast or any writer. The
		// tick counter therefore only counts ticks with volume > 0.
		if (t.volume <= 0)
			return;

		// 历史快照可以进行广播，但不写入csv
		if (dispatcher_ != NULL)
			dispatcher_->onTick(t);

		if (!IsFreshMarketData(pMarketData->UpdateTime)) {
			return; // 早上 8:30 启动时推的历史快照会被过滤
		}

		tickCount_.fetch_add(1);

		if (writer_ != NULL)
			writer_->push(t);
	}
}
