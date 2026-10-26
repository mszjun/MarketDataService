#include "Tick.h"

#include <float.h>
#include <string.h>

namespace mdgw
{
	const char * const EXCHANGE_ID_ZCE = "ZCE";    // Zhengzhou
	const char * const EXCHANGE_ID_DCE = "DCE";    // Dalian
	const char * const EXCHANGE_ID_SHFE = "SHFE";  // Shanghai
	const char * const EXCHANGE_ID_CFFE = "CFFE";  // CFFEX
	const char * const EXCHANGE_ID_INE = "INE";    // INE (crude oil)
	const char * const EXCHANGE_ID_GFEX = "GFEX";  // Guangzhou

	std::string normalizeExchangeID(const std::string & ctpExchangeID)
	{
		if (ctpExchangeID == "CZCE")
			return EXCHANGE_ID_ZCE;
		if (ctpExchangeID == "CFFEX")
			return EXCHANGE_ID_CFFE;
		return ctpExchangeID;
	}

	double doubleMaxTo0(double v)
	{
		return v >= DBL_MAX ? 0 : v;
	}
}
