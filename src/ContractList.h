#ifndef MDGW_CONTRACT_LIST_H
#define MDGW_CONTRACT_LIST_H

#include <string>
#include <vector>

namespace mdgw
{
	// One instrument as used by the market-data service.
	struct InstrumentInfo
	{
		std::string exchangeID;   // canonical exchange id (ZCE/DCE/SHFE/CFFE/INE/GFEX)
		std::string contractID;   // CTP InstrumentID, e.g. "rb2609"
	};

	// Whole-market futures instrument list.
	//
	// The primary source is the openctp dict HTTP service:
	//
	//   http://dict.openctp.cn/instruments?types=futures
	//
	//   { "rsp_code":0, "rsp_message":"succeed",
	//     "data":[ { "ExchangeID":"SHFE","InstrumentID":"rb2609",
	//                "ProductClass":"1", ... }, ... ] }
	//
	// Every successful fetch is saved to a local cache file; when the URL is
	// unreachable or unparseable the last saved cache is used instead.
	class ContractList
	{
	public:
		// Downloads the whole-market futures instrument list from the openctp
		// dict HTTP service and returns the raw JSON body on success.
		static bool fetchFromOpenCtp(const std::string & url, std::string & body,
			std::string & err);

		// Parses an openctp instruments JSON body into the canonical instrument
		// list. Non-futures records and duplicates are skipped and counted into
		// ignoredRecords.
		static bool parseOpenCtp(const std::string & text,
			std::vector<InstrumentInfo> & out, size_t & ignoredRecords, std::string & err);

		// Reads a previously saved response body from the cache file and parses
		// it. Used when the dict service is unreachable.
		static bool loadCache(const std::string & cacheFile,
			std::vector<InstrumentInfo> & out, size_t & ignoredRecords, std::string & err);

		// Saves the raw response body so a later start can survive the dict
		// service being unreachable. Written via a temporary file that replaces
		// the target.
		static bool saveCache(const std::string & cacheFile, const std::string & body,
			std::string & err);
	};
}

#endif // MDGW_CONTRACT_LIST_H