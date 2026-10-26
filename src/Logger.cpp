#include "Logger.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#ifdef WIN32
#include <windows.h>
#include <sys/timeb.h>
#else
#include <sys/time.h>
#endif

namespace mdgw
{
	static const char * LEVEL_TAG[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

	Logger & Logger::instance()
	{
		static Logger logger;
		return logger;
	}

	Logger::Logger()
		: console_(true), fileEnabled_(false), level_(LOG_LEVEL_INFO)
	{
	}

	Logger::~Logger()
	{
	}

	void Logger::setOptions(bool console, const std::string & file, LogLevel level)
	{
		console_ = console;
		file_ = file;
		fileEnabled_ = !file.empty();
		level_ = level;

		// create / truncate check by opening once
		if (fileEnabled_)
		{
			std::ofstream out(file_.c_str(), std::ios::app);
		}
	}

	static std::string nowString()
	{
#ifdef WIN32
		struct _timeb tb;
		_ftime_s(&tb);
		std::time_t t = tb.time;
		struct tm tmv;
		localtime_s(&tmv, &t);
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
			tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
			tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tb.millitm);
		return std::string(buf);
#else
		struct timeval tv;
		gettimeofday(&tv, NULL);
		struct tm tmv;
		localtime_r(&tv.tv_sec, &tmv);
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
			tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
			tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(tv.tv_usec / 1000));
		return std::string(buf);
#endif
	}

	void Logger::write(LogLevel level, const char * fmt, va_list ap)
	{
		if (level < level_)
			return;

		char body[2048];
		std::vsnprintf(body, sizeof(body), fmt, ap);

		std::string line = nowString();
		line += " [";
		line += LEVEL_TAG[level];
		line += "] ";
		line += body;
		line += "\n";

		if (console_)
		{
			std::fputs(line.c_str(), stdout);
			std::fflush(stdout);
		}

		if (fileEnabled_)
		{
			std::ofstream out(file_.c_str(), std::ios::app);
			if (out.is_open())
				out << line;
		}
	}

	void Logger::debug(const char * fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		write(LOG_LEVEL_DEBUG, fmt, ap);
		va_end(ap);
	}

	void Logger::info(const char * fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		write(LOG_LEVEL_INFO, fmt, ap);
		va_end(ap);
	}

	void Logger::warn(const char * fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		write(LOG_LEVEL_WARN, fmt, ap);
		va_end(ap);
	}

	void Logger::error(const char * fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		write(LOG_LEVEL_ERROR, fmt, ap);
		va_end(ap);
	}
}
