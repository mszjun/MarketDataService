#include "ClickHouseWriter.h"

#include <asio.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include "Logger.h"

#ifdef WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#endif

namespace mdgw
{
	using asio::ip::tcp;

	namespace
	{
		// ---------------- small helpers ----------------------------------

		// "20240315" -> "2024-03-15" (fallback: return input unchanged)
		std::string dateFromCTP(const std::string & day)
		{
			if (day.size() != 8)
				return day;
			std::string out;
			out.reserve(10);
			out.append(day, 0, 4); out += '-';
			out.append(day, 4, 2); out += '-';
			out.append(day, 6, 2);
			return out;
		}

		// compact decimal literal used in JSONEachRow for Float64 columns
		std::string jsonDouble(double v)
		{
			// NaN/inf are not valid JSON numbers; 0 keeps the insert batch valid
			if (!std::isfinite(v))
				return std::string("0");

			// The integer shortcut must test the range BEFORE the cast: converting a
			// double that does not fit into long long is undefined behaviour.
			if (v > -1e15 && v < 1e15 && v == (double)(long long)v)
			{
				char intBuf[32];
				snprintf(intBuf, sizeof(intBuf), "%lld", (long long)v);
				return std::string(intBuf);
			}

			// wide enough for the full expansion of any finite double
			char buf[512];
			snprintf(buf, sizeof(buf), "%.6f", v);
			std::string s(buf);
			size_t dot = s.find('.');
			if (dot != std::string::npos)
			{
				size_t last = s.find_last_not_of('0');
				if (last == dot)
					last--;
				s.erase(last + 1);
			}
			return s;
		}

		std::string jsonInt(long long v)
		{
			char buf[32];
			snprintf(buf, sizeof(buf), "%lld", v);
			return std::string(buf);
		}

		// very small JSON string escaper (market ids / times are plain ASCII)
		std::string jsonString(const std::string & s)
		{
			std::string out = "\"";
			for (size_t i = 0; i < s.size(); i++)
			{
				char c = s[i];
				if (c == '"' || c == '\\')
				{
					out += '\\';
					out += c;
				}
				else if (c == '\n')
					out += "\\n";
				else if (c == '\r')
					out += "\\r";
				else if (c == '\t')
					out += "\\t";
				else if ((unsigned char)c < 0x20)
				{
					char b[8];
					snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
					out += b;
				}
				else
					out += c;
			}
			out += '"';
			return out;
		}

		// one JSONEachRow line for the tick table (keys == column names)
		std::string tickRowJson(const Tick & tick, long long seq)
		{
			// HH:MM:SS part of updateTime
			std::string hms = tick.updateTime;
			size_t dot = hms.find('.');
			if (dot != std::string::npos)
				hms = hms.substr(0, dot);
			if (hms.size() < 8)
				hms = "00:00:00";

			char msBuf[16];
			snprintf(msBuf, sizeof(msBuf), "%03d", tick.updateMillisec < 0 ? 0 : tick.updateMillisec);

			std::string updateDt = dateFromCTP(tick.actionDay.empty() ? tick.tradingDay : tick.actionDay);
			updateDt += ' ';
			updateDt += hms;
			updateDt += '.';
			updateDt += msBuf;

			std::ostringstream os;
			os << "{"
				<< "\"trading_day\":" << jsonString(dateFromCTP(tick.tradingDay)) << ","
				<< "\"exchange\":" << jsonString(tick.exchangeID) << ","
				<< "\"instrument\":" << jsonString(tick.instrumentID) << ","
				<< "\"update_dt\":" << jsonString(updateDt) << ","
				<< "\"seq\":" << jsonInt(seq) << ","
				<< "\"pre_settlement_price\":" << jsonDouble(tick.previousSettlementPrice) << ","
				<< "\"pre_close_price\":" << jsonDouble(tick.previousClosePrice) << ","
				<< "\"pre_open_interest\":" << jsonDouble(tick.previousOpenInterest) << ","
				<< "\"latest_price\":" << jsonDouble(tick.latestPrice) << ","
				<< "\"open_price\":" << jsonDouble(tick.openPrice) << ","
				<< "\"highest_price\":" << jsonDouble(tick.highestPrice) << ","
				<< "\"lowest_price\":" << jsonDouble(tick.lowestPrice) << ","
				<< "\"volume\":" << jsonInt(tick.volume) << ","
				<< "\"turnover\":" << jsonDouble(tick.turnover) << ","
				<< "\"open_interest\":" << jsonDouble(tick.openInterest) << ","
				<< "\"close_price\":" << jsonDouble(tick.closePrice) << ","
				<< "\"settlement_price\":" << jsonDouble(tick.settlementPrice) << ","
				<< "\"upper_limit_price\":" << jsonDouble(tick.upperLimitPrice) << ","
				<< "\"lower_limit_price\":" << jsonDouble(tick.lowerLimitPrice) << ","
				// only depth level 1 is collected
				<< "\"bid_price_1\":" << jsonDouble(tick.bidPrice[0]) << ","
				<< "\"bid_volume_1\":" << jsonInt(tick.bidVolume[0]) << ","
				<< "\"ask_price_1\":" << jsonDouble(tick.askPrice[0]) << ","
				<< "\"ask_volume_1\":" << jsonInt(tick.askVolume[0]);
			os << "}";
			return os.str();
		}

		// ---------- socket helpers (blocking with timeouts) ---------------

		bool waitSocket(intptr_t fd, bool forWrite, int timeoutMs)
		{
			fd_set fds;
			FD_ZERO(&fds);
#ifdef WIN32
			FD_SET((SOCKET)fd, &fds);
#else
			FD_SET((int)fd, &fds);
#endif
			timeval tv;
			tv.tv_sec = timeoutMs / 1000;
			tv.tv_usec = (timeoutMs % 1000) * 1000;
			int sel;
#ifdef WIN32
			sel = select(0, forWrite ? NULL : &fds, forWrite ? &fds : NULL, NULL, &tv);
#else
			sel = select((int)fd + 1, forWrite ? NULL : &fds, forWrite ? &fds : NULL, NULL, &tv);
#endif
			return sel > 0;
		}

		bool connectWithTimeout(tcp::socket & sock, const tcp::endpoint & ep, int timeoutMs)
		{
			std::error_code ec;
			sock.open(ep.protocol(), ec);
			if (ec)
				return false;

			sock.native_non_blocking(true, ec);
			sock.connect(ep, ec);
			if (!ec)
			{
				sock.native_non_blocking(false, ec);
				return true;
			}
			if (ec != asio::error::would_block && ec != asio::error::in_progress &&
				ec != asio::error::try_again)
			{
				return false;
			}

			if (!waitSocket((intptr_t)sock.native_handle(), true, timeoutMs))
				return false;

			int err = 0;
#ifdef WIN32
			int len = (int)sizeof(err);
			getsockopt((SOCKET)sock.native_handle(), SOL_SOCKET, SO_ERROR, (char *)&err, &len);
#else
			socklen_t len = sizeof(err);
			getsockopt((int)sock.native_handle(), SOL_SOCKET, SO_ERROR, &err, &len);
#endif
			sock.native_non_blocking(false, ec);
			return err == 0;
		}

		bool readResponse(tcp::socket & sock, std::string & response, int timeoutMs, int maxBytes)
		{
			response.clear();
			char buf[8192];
			while ((int)response.size() < maxBytes)
			{
				if (!waitSocket((intptr_t)sock.native_handle(), false, timeoutMs))
					return false;

				std::error_code ec;
				size_t n = sock.read_some(asio::buffer(buf, sizeof(buf)), ec);
				if (ec == asio::error::eof)
					break;
				if (ec)
					return false;
				if (n == 0)
					break;
				response.append(buf, n);
			}
			return !response.empty();
		}

		std::string basicAuthHeader(const std::string & user, const std::string & password)
		{
			if (user.empty())
				return std::string();

			// RFC 4648 base64 for "user:password"
			const char * tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string in = user + ":" + password;
			std::string out;
			for (size_t i = 0; i < in.size(); i += 3)
			{
				unsigned char b0 = (unsigned char)in[i];
				unsigned char b1 = i + 1 < in.size() ? (unsigned char)in[i + 1] : 0;
				unsigned char b2 = i + 2 < in.size() ? (unsigned char)in[i + 2] : 0;
				out += tbl[b0 >> 2];
				out += tbl[((b0 & 0x03) << 4) | (b1 >> 4)];
				out += (i + 1 < in.size()) ? tbl[((b1 & 0x0F) << 2) | (b2 >> 6)] : '=';
				out += (i + 2 < in.size()) ? tbl[b2 & 0x3F] : '=';
			}
			return "Authorization: Basic " + out + "\r\n";
		}

		// Proper application/x-www-form-urlencoded encoding for the SQL that
		// goes into the "query" URL parameter (space must become %20, not '+',
		// otherwise the statement is sent literally with '+' characters and
		// ClickHouse rejects it and drops the connection).
		std::string urlEncode(const std::string & s)
		{
			static const char * hex = "0123456789ABCDEF";
			std::string out;
			for (size_t i = 0; i < s.size(); i++)
			{
				unsigned char c = (unsigned char)s[i];
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
				{
					out += (char)c;
				}
				else
				{
					out += '%';
					out += hex[c >> 4];
					out += hex[c & 0x0F];
				}
			}
			return out;
		}
	}

	ClickHouseWriter::ClickHouseWriter(const ClickHouseConfig & cfg)
		: cfg_(cfg), stopping_(false),
		received_(0), written_(0), dropped_(0), failedRows_(0)
	{
	}

	ClickHouseWriter::~ClickHouseWriter()
	{
		stop();
	}

	void ClickHouseWriter::start()
	{
		if (worker_.joinable())
			return;
		stopping_ = false;

		// startup HTTP connectivity probe (SELECT 1). Wrong port (native TCP
		// 9000 instead of HTTP 8123) or wrong credentials surface here.
		if (ping())
		{
			MD_LOG_INFO("clickhouse HTTP reachable [%s:%d] (SELECT 1 ok)", cfg_.host.c_str(), cfg_.port);
		}
		else
		{
			MD_LOG_ERROR("clickhouse HTTP ping failed [%s:%d]. Check that: "
				"(1) the port is the ClickHouse HTTP port (default 8123), NOT the native TCP port 9000; "
				"(2) user/password are correct; "
				"(3) the table market.%s exists (see sql/create_tick_table.sql). "
				"The writer keeps retrying in background.",
				cfg_.host.c_str(), cfg_.port, cfg_.table.c_str());
		}

		worker_ = std::thread(&ClickHouseWriter::run, this);
		MD_LOG_INFO("clickhouse writer started [%s:%d/%s.%s]", cfg_.host.c_str(), cfg_.port,
			cfg_.database.c_str(), cfg_.table.c_str());
	}

	void ClickHouseWriter::stop()
	{
		if (!worker_.joinable())
			return;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stopping_ = true;
		}
		cv_.notify_all();
		worker_.join();
	}

	size_t ClickHouseWriter::queueSize() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return queue_.size();
	}

	void ClickHouseWriter::push(const Tick & tick)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if ((int)queue_.size() >= cfg_.maxQueuedRows)
			{
				dropped_.fetch_add(1);
				return;
			}
			queue_.push_back(tick);
		}
		received_.fetch_add(1);

		bool full = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			full = (int)queue_.size() >= cfg_.batchRows;
		}
		if (full)
			cv_.notify_one();
	}

	void ClickHouseWriter::run()
	{
		rowSeq_ = 0;
		unsigned int consecutiveErrors = 0;

		while (true)
		{
			std::vector<Tick> batch;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				if (stopping_ && queue_.empty())
					break;

				if (queue_.empty())
				{
					cv_.wait_for(lock, std::chrono::milliseconds(cfg_.flushIntervalMs));
					if (stopping_ && queue_.empty())
						break;
				}

				// move at most 5000 rows per HTTP request to bound body size
				size_t take = queue_.size();
				if (take > 5000)
					take = 5000;
				for (size_t i = 0; i < take; i++)
				{
					batch.push_back(queue_.front());
					queue_.pop_front();
				}
			}

			if (batch.empty())
				continue;

			if (flushBatch(batch))
			{
				consecutiveErrors = 0;
			}
			else
			{
				consecutiveErrors++;
				int sleepMs = cfg_.retrySleepMs;
				if (consecutiveErrors > 1)
				{
					// avoid hammering a down server
					sleepMs = (consecutiveErrors > 10) ? 30000 : cfg_.retrySleepMs * (int)consecutiveErrors;
				}
				MD_LOG_WARN("clickhouse insert failed: batch rows [%d] lost, next try in [%d] ms (consecutive errors [%u])",
					(int)batch.size(), sleepMs, consecutiveErrors);
				failedRows_.fetch_add(batch.size());
				std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
			}
		}

		MD_LOG_INFO("clickhouse writer stopped");
	}

	bool ClickHouseWriter::flushBatch(std::vector<Tick> & batch)
	{
		std::string body;
		body.reserve(batch.size() * 480);
		for (size_t i = 0; i < batch.size(); i++)
		{
			if (i)
				body += '\n';
			rowSeq_++;
			body += tickRowJson(batch[i], rowSeq_);
		}

		// query = INSERT INTO <db>.<table> FORMAT JSONEachRow
		std::string q = "INSERT INTO " + cfg_.database + "." + cfg_.table + " FORMAT JSONEachRow";

		if (!httpPost("/?query=" + urlEncode(q), body, cfg_.readTimeoutMs))
			return false;

		written_.fetch_add(batch.size());
		MD_LOG_DEBUG("clickhouse insert ok, rows [%d], total [%llu]", (int)batch.size(),
			(unsigned long long)written_.load());
		return true;
	}

	bool ClickHouseWriter::httpPost(const std::string & pathAndQuery, const std::string & body,
		int readTimeoutMs)
	{
		try
		{
			asio::io_context io;
			tcp::resolver resolver(io);
			std::error_code resolveEc;
			tcp::resolver::results_type endpoints =
				resolver.resolve(cfg_.host, jsonInt(cfg_.port), resolveEc);
			if (resolveEc)
			{
				MD_LOG_WARN("clickhouse resolve [%s] failed: %s", cfg_.host.c_str(),
					resolveEc.message().c_str());
				return false;
			}
			if (endpoints.empty())
				return false;

			tcp::socket sock(io);
			if (!connectWithTimeout(sock, *endpoints.begin(), cfg_.connectTimeoutMs))
			{
				MD_LOG_WARN("clickhouse connect [%s:%d] failed", cfg_.host.c_str(), cfg_.port);
				return false;
			}

			std::string req;
			req = "POST ";
			req += pathAndQuery;
			req += " HTTP/1.1\r\n";
			req += "Host: " + cfg_.host + ":" + jsonInt(cfg_.port) + "\r\n";
			req += "User-Agent: MarketDataService/1.0\r\n";
			req += basicAuthHeader(cfg_.user, cfg_.password);
			req += "Content-Type: text/plain; charset=UTF-8\r\n";
			req += "Content-Length: ";
			req += jsonInt((long long)body.size());
			req += "\r\n";
			req += "Connection: close\r\n";
			req += "\r\n";
			req += body;

			std::error_code writeEc;
			asio::write(sock, asio::buffer(req.data(), req.size()), writeEc);
			if (writeEc)
			{
				// The server usually closes the connection because it already
				// answered (often with an error page). Try to read whatever it
				// sent so the real ClickHouse error message is visible.
				std::string resp;
				std::string respHead;
				if (readResponse(sock, resp, 2000, 1024 * 1024))
				{
					respHead = resp.substr(0, 500);
				}
				MD_LOG_WARN("clickhouse request write failed: %s (query [%s])",
					writeEc.message().c_str(), pathAndQuery.c_str());
				if (!respHead.empty())
				{
					MD_LOG_WARN("clickhouse server replied before closing: [%.500s]",
						respHead.c_str());
				}
				return false;
			}

			std::string response;
			if (!readResponse(sock, response, readTimeoutMs, 4 * 1024 * 1024))
			{
				MD_LOG_WARN("clickhouse response read failed or timed out (query [%s])",
					pathAndQuery.c_str());
				return false;
			}

			sock.close();

			// status line: HTTP/1.1 200 OK
			bool ok = response.compare(0, 9, "HTTP/1.1 ") == 0 &&
				response.size() >= 12 && response[9] == '2' && response[10] == '0' &&
				(response[11] == '0' || response[11] == '1' || response[11] == '4');

			if (!ok)
			{
				MD_LOG_WARN("clickhouse http status not ok (query [%s]), response head [%.500s]",
					pathAndQuery.c_str(), response.c_str());
				return false;
			}
			return true;
		}
		catch (const std::exception & ex)
		{
			MD_LOG_WARN("clickhouse http post exception: %s", ex.what());
			return false;
		}
	}

	bool ClickHouseWriter::ping()
	{
		// SELECT 1 through the HTTP interface; the native ClickHouse TCP port
		// (9000) does not speak HTTP and will close the connection right away.
		return httpPost("/?query=" + urlEncode("SELECT 1"), "", 5000);
	}
}
