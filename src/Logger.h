#ifndef MDGW_LOGGER_H
#define MDGW_LOGGER_H

#include <cstdarg>
#include <string>

namespace mdgw
{
	enum LogLevel
	{
		LOG_LEVEL_DEBUG = 0,
		LOG_LEVEL_INFO = 1,
		LOG_LEVEL_WARN = 2,
		LOG_LEVEL_ERROR = 3
	};

	// Very small thread-safe logger (console + optional rotating-free file).
	// Log lines look like:  2018-01-01 12:00:00.123 [INFO ] message
	class Logger
	{
	public:
		static Logger & instance();

		void setOptions(bool console, const std::string & file, LogLevel level);
		void setLevel(LogLevel level) { level_ = level; }
		LogLevel level() const { return level_; }

		void debug(const char * fmt, ...);
		void info(const char * fmt, ...);
		void warn(const char * fmt, ...);
		void error(const char * fmt, ...);

	private:
		Logger();
		~Logger();
		Logger(const Logger &);
		Logger & operator=(const Logger &);

		void write(LogLevel level, const char * fmt, va_list ap);

		bool console_;
		bool fileEnabled_;
		std::string file_;
		LogLevel level_;
	};

	// Convenience macros mirroring the original LOGFMT_* usage.
#define MD_LOG_DEBUG(...)  mdgw::Logger::instance().debug(__VA_ARGS__)
#define MD_LOG_INFO(...)   mdgw::Logger::instance().info(__VA_ARGS__)
#define MD_LOG_WARN(...)   mdgw::Logger::instance().warn(__VA_ARGS__)
#define MD_LOG_ERROR(...)  mdgw::Logger::instance().error(__VA_ARGS__)
}

#endif // MDGW_LOGGER_H
