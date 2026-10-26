#include "ContractList.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <string>
#include "Tick.h"
#include "cJSON.h"

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace mdgw
{
	namespace
	{
		// ---------------- tiny synchronous HTTP GET (http:// only) ---------
		// Self contained so ContractList does not depend on anything but the
		// platform sockets. Used once at startup to refresh the instrument list.

		const int HTTP_TIMEOUT_MS = 10000;
		const int HTTP_MAX_RESPONSE = 8 * 1024 * 1024;

		struct HttpUrl
		{
			std::string host;
			int port;
			std::string pathAndQuery;
		};

		bool parseHttpUrl(const std::string & url, HttpUrl & out, std::string & err)
		{
			if (url.compare(0, 7, "http://") != 0)
			{
				err = "only http:// URLs are supported: " + url;
				return false;
			}
			std::string rest = url.substr(7);
			size_t slash = rest.find('/');
			std::string hostPort = (slash == std::string::npos) ? rest : rest.substr(0, slash);
			std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);
			if (hostPort.empty())
			{
				err = "invalid URL (no host): " + url;
				return false;
			}

			size_t colon = hostPort.rfind(':');
			if (colon != std::string::npos)
			{
				out.host = hostPort.substr(0, colon);
				out.port = atoi(hostPort.substr(colon + 1).c_str());
				if (out.port <= 0)
					out.port = 80;
			}
			else
			{
				out.host = hostPort;
				out.port = 80;
			}
			out.pathAndQuery = path.empty() ? "/" : path;
			return true;
		}

#ifdef WIN32
		typedef SOCKET sock_t;
		const sock_t INVALID_SOCK = INVALID_SOCKET;

		// Waits for the socket to become readable/writable. select() is retried
		// when a signal interrupts it: otherwise any signal (our own SIGTERM
		// handler included) would look like a socket timeout and abort a fetch
		// that is perfectly healthy.
		bool waitFd(sock_t fd, bool forWrite, int timeoutMs)
		{
			const std::chrono::steady_clock::time_point deadline =
				std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

			for (;;)
			{
				int remainingMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
					deadline - std::chrono::steady_clock::now()).count();
				if (remainingMs < 0)
					remainingMs = 0;

				fd_set fds;
				FD_ZERO(&fds);
				FD_SET(fd, &fds);
				timeval tv;
				tv.tv_sec = remainingMs / 1000;
				tv.tv_usec = (remainingMs % 1000) * 1000;
				int sel = select(0, forWrite ? NULL : &fds, forWrite ? &fds : NULL, NULL, &tv);
				if (sel >= 0)
					return sel > 0;
				if (WSAGetLastError() != WSAEINTR)
					return false;
			}
		}

		int lastSocketError() { return WSAGetLastError(); }

		void setNonBlocking(sock_t fd, bool nb)
		{
			u_long mode = nb ? 1 : 0;
			ioctlsocket(fd, FIONBIO, &mode);
		}

		void closeSocket(sock_t fd) { closesocket(fd); }
#else
		typedef int sock_t;
		const sock_t INVALID_SOCK = -1;

		// See the Windows variant above: EINTR must not be reported as a timeout.
		bool waitFd(sock_t fd, bool forWrite, int timeoutMs)
		{
			const std::chrono::steady_clock::time_point deadline =
				std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

			for (;;)
			{
				int remainingMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
					deadline - std::chrono::steady_clock::now()).count();
				if (remainingMs < 0)
					remainingMs = 0;

				fd_set fds;
				FD_ZERO(&fds);
				FD_SET(fd, &fds);
				timeval tv;
				tv.tv_sec = remainingMs / 1000;
				tv.tv_usec = (remainingMs % 1000) * 1000;
				int sel = select(fd + 1, forWrite ? NULL : &fds, forWrite ? &fds : NULL, NULL, &tv);
				if (sel >= 0)
					return sel > 0;
				if (errno != EINTR)
					return false;
			}
		}

		int lastSocketError() { return errno; }

		void setNonBlocking(sock_t fd, bool nb)
		{
			int flags = fcntl(fd, F_GETFL, 0);
			fcntl(fd, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
		}

		void closeSocket(sock_t fd) { close(fd); }
#endif

		bool resolveHost(const std::string & host, sockaddr_in & out)
		{
			addrinfo hints;
			memset(&hints, 0, sizeof(hints));
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_STREAM;
			addrinfo * res = NULL;
			if (getaddrinfo(host.c_str(), NULL, &hints, &res) != 0 || res == NULL)
				return false;
			memcpy(&out, res->ai_addr, sizeof(sockaddr_in));
			freeaddrinfo(res);
			return true;
		}

		bool sendAll(sock_t fd, const std::string & data)
		{
			size_t sent = 0;
			while (sent < data.size())
			{
				int n = send(fd, data.data() + sent, (int)(data.size() - sent), 0);
				if (n <= 0)
					return false;
				sent += (size_t)n;
			}
			return true;
		}

		// ---------------- HTTP/1.1 response reading -----------------------
		// The openctp dict service answers with "HTTP/1.1 200", a chunked
		// transfer encoding and "Connection: keep-alive": there is no
		// Content-Length and the peer never closes the socket, so the body has
		// to be delimited by the chunk framing itself. Relying on EOF or
		// Content-Length alone made the read block until the timeout.

		std::string toLowerAscii(const std::string & s)
		{
			std::string out(s);
			for (size_t i = 0; i < out.size(); i++)
			{
				if (out[i] >= 'A' && out[i] <= 'Z')
					out[i] = (char)(out[i] - 'A' + 'a');
			}
			return out;
		}

		// Case-insensitive header lookup; header names are ASCII and the raw
		// header block is returned lower-cased by toLowerAscii().
		bool findHttpHeader(const std::string & head, const char * name, std::string & value)
		{
			const std::string haystack = "\r\n" + toLowerAscii(head);
			const std::string needle = "\r\n" + toLowerAscii(std::string(name)) + ":";
			const size_t pos = haystack.find(needle);
			if (pos == std::string::npos)
				return false;

			size_t begin = pos + needle.size();
			size_t end = haystack.find("\r\n", begin);
			if (end == std::string::npos)
				end = haystack.size();

			size_t first = haystack.find_first_not_of(" \t", begin);
			if (first == std::string::npos || first >= end)
			{
				value.clear();
				return true;
			}
			size_t last = haystack.find_last_not_of(" \t", end - 1);
			value = haystack.substr(first, last - first + 1);
			return true;
		}

		// Incremental decoder for "Transfer-Encoding: chunked" bodies; it is
		// fed with whatever the socket returned, so framing may be split
		// across recv() calls.
		class ChunkedDecoder
		{
		public:
			explicit ChunkedDecoder(std::string & out) : out_(out) {}

			bool done() const { return state_ == DONE; }

			// Returns false when the chunk framing is malformed.
			bool feed(const char * data, size_t len)
			{
				for (size_t i = 0; i < len; i++)
				{
					const char c = data[i];
					switch (state_)
					{
					case SIZE_LINE:
						if (c != '\n')
						{
							line_ += c;
							if (line_.size() > 256)   // no sensible chunk header is longer
								return false;
							break;
						}
						if (!parseChunkSize())
							return false;
						break;

					case DATA:
						out_ += c;
						if (--remaining_ == 0)
						{
							seenCr_ = false;
							state_ = DATA_END;
						}
						break;

					case DATA_END:
						if (c == '\r')
							seenCr_ = true;
						else if (c == '\n' && seenCr_)
							state_ = SIZE_LINE;
						else
							return false;
						break;

					case TRAILER:
						if (c != '\n')
						{
							line_ += c;
							break;
						}
						if (line_.empty() || line_ == "\r")
							state_ = DONE;
						line_.clear();
						break;

					case DONE:
						return true;
					}
				}
				return true;
			}

		private:
			enum State
			{
				SIZE_LINE,
				DATA,
				DATA_END,
				TRAILER,
				DONE
			};

			bool parseChunkSize()
			{
				std::string line = line_;
				line_.clear();

				const size_t semi = line.find(';');   // chunk extensions
				if (semi != std::string::npos)
					line.resize(semi);
				if (!line.empty() && line[line.size() - 1] == '\r')
					line.resize(line.size() - 1);
				if (line.empty())
					return false;

				const char * begin = line.c_str();
				char * end = NULL;
				const long long size = strtoll(begin, &end, 16);
				if (size < 0 || end == begin || (end != NULL && *end != '\0'))
					return false;

				if (size == 0)
				{
					state_ = TRAILER;
					return true;
				}
				remaining_ = size;
				state_ = DATA;
				return true;
			}

			std::string & out_;
			State state_ = SIZE_LINE;
			long long remaining_ = 0;
			std::string line_;
			bool seenCr_ = false;
		};

		// Reads one whole HTTP response. Supports the three body delimiters a
		// server may use: chunked framing (stops at the terminating chunk
		// without waiting for EOF), Content-Length, and read-until-close.
		bool readHttpResponse(sock_t fd, int & statusCode, std::string & body,
			std::string & err, int timeoutMs, int maxBytes)
		{
			statusCode = 0;
			body.clear();
			char buf[16384];

			// ---- headers ----------------------------------------------------
			std::string received;
			size_t headerEnd = std::string::npos;
			while (headerEnd == std::string::npos)
			{
				if (!waitFd(fd, false, timeoutMs))
				{
					err = "timed out while reading the response headers";
					return false;
				}
				const int n = recv(fd, buf, (int)sizeof(buf), 0);
				if (n <= 0)
				{
					err = "connection closed before the response headers";
					return false;
				}
				received.append(buf, (size_t)n);
				headerEnd = received.find("\r\n\r\n");
				if (received.size() > 256 * 1024)
				{
					err = "response headers too large";
					return false;
				}
			}

			const std::string head = received.substr(0, headerEnd);
			const size_t space = head.find(' ');
			if (head.compare(0, 5, "HTTP/") != 0 || space == std::string::npos)
			{
				err = "malformed HTTP status line";
				return false;
			}
			statusCode = atoi(head.c_str() + space + 1);
			if (statusCode < 100 || statusCode > 599)
			{
				err = "malformed HTTP status code";
				return false;
			}

			std::string value;
			bool chunked = false;
			if (findHttpHeader(head, "transfer-encoding", value))
				chunked = toLowerAscii(value).find("chunked") != std::string::npos;

			long long contentLength = -1;
			if (!chunked && findHttpHeader(head, "content-length", value))
			{
				const char * begin = value.c_str();
				char * end = NULL;
				contentLength = strtoll(begin, &end, 10);
				if (end == begin || contentLength < 0)
				{
					err = "invalid Content-Length [" + value + "]";
					return false;
				}
			}

			// ---- body -------------------------------------------------------
			ChunkedDecoder decoder(body);
			bool failed = false;
			// Returns true once the body is complete.
			auto consume = [&](const char * data, size_t len) -> bool
			{
				if (chunked)
				{
					if (!decoder.feed(data, len))
					{
						err = "malformed chunked response body";
						failed = true;
					}
					return decoder.done();
				}
				if (contentLength >= 0)
				{
					const size_t need = (size_t)(contentLength - (long long)body.size());
					body.append(data, len < need ? len : need);
					return (long long)body.size() >= contentLength;
				}
				body.append(data, len);
				return false;   // close-delimited: only EOF ends the body
			};

			const std::string pending = received.substr(headerEnd + 4);
			bool complete = !pending.empty() && consume(pending.data(), pending.size());

			while (!complete)
			{
				if (failed)
					return false;
				if ((long long)body.size() > maxBytes)
				{
					err = "response body exceeds the limit";
					return false;
				}
				if (!waitFd(fd, false, timeoutMs))
				{
					err = "timed out while reading the response body";
					return false;
				}
				const int n = recv(fd, buf, (int)sizeof(buf), 0);
				if (n < 0)
				{
					err = "recv() failed while reading the response body";
					return false;
				}
				if (n == 0)
				{
					// close-delimited bodies end here; anything else is truncated
					if (contentLength < 0 && !chunked)
						return true;
					err = "connection closed before the response body was complete";
					return false;
				}
				complete = consume(buf, (size_t)n);
			}

			if (failed)
				return false;
			if ((long long)body.size() > maxBytes)
			{
				err = "response body exceeds the limit";
				return false;
			}
			return true;
		}

		bool httpGetOnce(const HttpUrl & url, std::string & body, std::string & err)
		{
			sockaddr_in addr;
			memset(&addr, 0, sizeof(addr));
			if (!resolveHost(url.host, addr))
			{
				err = "can not resolve host: " + url.host;
				return false;
			}
			addr.sin_family = AF_INET;
			addr.sin_port = htons((unsigned short)url.port);

			sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
			if (fd == INVALID_SOCK)
			{
				err = "socket() failed";
				return false;
			}

			setNonBlocking(fd, true);
			int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
			if (rc != 0)
			{
				bool inProgress =
#ifdef WIN32
					(lastSocketError() == WSAEWOULDBLOCK || lastSocketError() == WSAEINPROGRESS);
#else
					(lastSocketError() == EINPROGRESS || lastSocketError() == EWOULDBLOCK);
#endif
				if (!inProgress)
				{
					closeSocket(fd);
					err = "connect to [" + url.host + ":" + std::to_string(url.port) + "] failed";
					return false;
				}
				if (!waitFd(fd, true, HTTP_TIMEOUT_MS))
				{
					closeSocket(fd);
					err = "connect to [" + url.host + ":" + std::to_string(url.port) + "] timed out";
					return false;
				}
				int sockErr = 0;
#ifdef WIN32
				int len = (int)sizeof(sockErr);
				getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&sockErr, &len);
#else
				socklen_t len = sizeof(sockErr);
				getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockErr, &len);
#endif
				if (sockErr != 0)
				{
					closeSocket(fd);
					err = "connect to [" + url.host + ":" + std::to_string(url.port) + "] failed";
					return false;
				}
			}
			setNonBlocking(fd, false);

			// HTTP/1.1 + explicit "Connection: close" so the peer closes after
			// the response; "identity" keeps the body free of content coding
			// (no Accept-Encoding is sent, but be explicit for proxies).
			std::string hostHeader = url.host;
			if (url.port != 80)
				hostHeader += ":" + std::to_string(url.port);
			std::string req = "GET " + url.pathAndQuery + " HTTP/1.1\r\n"
				"Host: " + hostHeader + "\r\n"
				"User-Agent: MarketDataService/1.0\r\n"
				"Accept: application/json, */*\r\n"
				"Accept-Encoding: identity\r\n"
				"Connection: close\r\n\r\n";
			if (!sendAll(fd, req))
			{
				closeSocket(fd);
				err = "send request to [" + url.host + "] failed";
				return false;
			}

			int statusCode = 0;
			std::string readErr;
			if (!readHttpResponse(fd, statusCode, body, readErr,
				HTTP_TIMEOUT_MS, HTTP_MAX_RESPONSE))
			{
				closeSocket(fd);
				err = "GET " + url.pathAndQuery + " from [" + url.host + "] failed: " + readErr;
				return false;
			}
			closeSocket(fd);

			if (statusCode != 200)
			{
				err = "GET " + url.pathAndQuery + " from [" + url.host +
					"] returned HTTP status " + std::to_string(statusCode);
				return false;
			}
			return true;
		}

		bool ensureWsaStarted()
		{
#ifdef WIN32
			static bool started = false;
			static bool ok = false;
			if (!started)
			{
				WSADATA wsa;
				ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
				started = true;
			}
			return ok;
#else
			return true;
#endif
		}

		// parse helpers reused for both remote bodies and cache files
		bool parseOpenCtpBody(const std::string & text, std::vector<InstrumentInfo> & out,
			size_t & ignoredRecords, std::string & err)
		{
			out.clear();
			ignoredRecords = 0;

			cJSON * root = cJSON_Parse(text.c_str());
			if (!root)
			{
				err = "can not parse openctp instruments JSON";
				return false;
			}
			if (root->type != cJSON_Object)
			{
				cJSON_Delete(root);
				err = "unexpected openctp response structure (expected an object)";
				return false;
			}

			cJSON * rsp = cJSON_GetObjectItem(root, "rsp_code");
			if (rsp && rsp->type == cJSON_Number && rsp->valueint != 0)
			{
				err = "openctp dict service returned an error (rsp_code " +
					std::to_string(rsp->valueint) + ")";
				cJSON_Delete(root);
				return false;
			}

			cJSON * data = cJSON_GetObjectItem(root, "data");
			if (!data || data->type != cJSON_Array)
			{
				err = "openctp response has no usable \"data\" array";
				cJSON_Delete(root);
				return false;
			}

			for (cJSON * poi = data->child; poi != NULL; poi = poi->next)
			{
				if (poi->type != cJSON_Object)
				{
					ignoredRecords++;
					continue;
				}

				const char * exchangeID = NULL;
				const char * instrumentID = NULL;
				const char * productClass = NULL;
				for (cJSON * exp = poi->child; exp != NULL; exp = exp->next)
				{
					if (strcmp(exp->string, "ExchangeID") == 0 && exp->type == cJSON_String)
						exchangeID = exp->valuestring;
					else if (strcmp(exp->string, "InstrumentID") == 0 && exp->type == cJSON_String)
						instrumentID = exp->valuestring;
					else if (strcmp(exp->string, "ProductClass") == 0 && exp->type == cJSON_String)
						productClass = exp->valuestring;
				}

				// keep futures only ("1"); anything else (options etc.) skipped
				if (productClass != NULL && strcmp(productClass, "1") != 0)
				{
					ignoredRecords++;
					continue;
				}
				if (exchangeID == NULL || instrumentID == NULL ||
					*exchangeID == 0 || *instrumentID == 0)
				{
					ignoredRecords++;
					continue;
				}

				InstrumentInfo info;
				info.exchangeID = normalizeExchangeID(exchangeID);
				info.contractID = instrumentID;

				// keep only the first occurrence of a contract so one
				// instrument is never subscribed twice
				bool duplicated = false;
				for (size_t i = 0; i < out.size(); i++)
				{
					if (out[i].contractID == info.contractID &&
						out[i].exchangeID == info.exchangeID)
					{
						duplicated = true;
						break;
					}
				}
				if (duplicated)
					continue;

				out.push_back(info);
			}

			cJSON_Delete(root);

			if (out.empty())
			{
				err = "openctp instruments JSON contains no usable futures contract";
				return false;
			}
			return true;
		}
	}

	bool ContractList::fetchFromOpenCtp(const std::string & url, std::string & body,
		std::string & err)
	{
		body.clear();
		if (!ensureWsaStarted())
		{
			err = "Winsock init failed";
			return false;
		}

		HttpUrl u;
		if (!parseHttpUrl(url, u, err))
			return false;

		return httpGetOnce(u, body, err);
	}

	bool ContractList::parseOpenCtp(const std::string & text,
		std::vector<InstrumentInfo> & out, size_t & ignoredRecords, std::string & err)
	{
		return parseOpenCtpBody(text, out, ignoredRecords, err);
	}

	bool ContractList::loadCache(const std::string & cacheFile,
		std::vector<InstrumentInfo> & out, size_t & ignoredRecords, std::string & err)
	{
		out.clear();
		ignoredRecords = 0;

		FILE * fp = fopen(cacheFile.c_str(), "rb");
		if (!fp)
		{
			err = "can not open instrument cache file: " + cacheFile;
			return false;
		}

		std::string text;
		char buf[8192];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
			text.append(buf, n);
		fclose(fp);

		if (text.empty())
		{
			err = "instrument cache file is empty: " + cacheFile;
			return false;
		}
		return parseOpenCtpBody(text, out, ignoredRecords, err);
	}

	bool ContractList::saveCache(const std::string & cacheFile, const std::string & body,
		std::string & err)
	{
		std::string tmp = cacheFile + ".tmp";
		FILE * fp = fopen(tmp.c_str(), "wb");
		if (!fp)
		{
			err = "can not open cache temp file for writing: " + tmp;
			return false;
		}
		bool ok = fwrite(body.data(), 1, body.size(), fp) == body.size();
		if (fclose(fp) != 0)
			ok = false;
		if (!ok)
		{
			err = "can not write instrument cache temp file: " + tmp;
			remove(tmp.c_str());
			return false;
		}

#ifdef WIN32
		DeleteFileA(cacheFile.c_str());   // rename does not overwrite on Windows
#endif
		if (rename(tmp.c_str(), cacheFile.c_str()) != 0)
		{
			err = "can not move instrument cache into place: " + cacheFile;
			return false;
		}
		return true;
	}
}