// MarketDataService - standalone CTP market-data gateway.
//
// Usage:
//   MarketDataService -c config/MarketDataService.json
//
// Ctrl+C / SIGTERM triggers a graceful shutdown.
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <string>

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#endif

#include "Logger.h"
#include "Service.h"

namespace
{
	// Set shortly before Service::run() and cleared as soon as it returns, so a
	// signal arriving during shutdown can never reach a destroyed object.
	mdgw::Service* g_service = nullptr;

	// Written only from a signal / console handler and consumed by the run loop.
	// A handler must not log, allocate or lock: none of that is signal-safe.
	volatile sig_atomic_t g_stopRequested = 0;

	// Directory of the running executable, without the trailing separator.
	std::string getExeDir()
	{
#ifdef WIN32
		char buf[4096] = { 0 };
		DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
		if (n == 0 || n >= sizeof(buf)) return std::string(".");
		std::string p(buf, (size_t)n);
		size_t pos = p.find_last_of("\\/");
		return (pos == std::string::npos) ? std::string(".") : p.substr(0, pos);
#else
		char buf[4096] = { 0 };
		ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		if (n <= 0) return std::string(".");
		std::string p(buf, (size_t)n);
		size_t pos = p.find_last_of('/');
		return (pos == std::string::npos) ? std::string(".") : p.substr(0, pos);
#endif
	}

	// Breadcrumb for failures that happen before Logger is configured by
	// Service::init() (a daemon started without a console has no other channel).
	// A normal start writes nothing here.
	void logStartupFailure(const std::string & msg)
	{
#ifdef WIN32
		const std::string path = getExeDir() + "\\startup.log";
#else
		const std::string path = getExeDir() + "/startup.log";
#endif
		FILE* fp = fopen(path.c_str(), "a");
		if (fp == NULL)
			return;

		const time_t now = time(NULL);
		struct tm* lt = localtime(&now);
		char ts[32] = { 0 };
		if (lt != NULL)
			strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", lt);

		fprintf(fp, "[%s] %s\n", ts, msg.c_str());
		fflush(fp);
		fclose(fp);
	}

	// Relative paths (config, logs, flow files) are resolved against the
	// executable directory rather than the caller's working directory.
	void chdirToExeDir()
	{
		const std::string exeDir = getExeDir();
		if (exeDir.empty() || exeDir == ".")
			return;

#ifdef WIN32
		if (!SetCurrentDirectoryA(exeDir.c_str()))
		{
			logStartupFailure("SetCurrentDirectoryA(" + exeDir + ") failed, GetLastError=" +
				std::to_string((unsigned long)GetLastError()));
		}
#else
		if (chdir(exeDir.c_str()) != 0)
		{
			logStartupFailure("chdir(" + exeDir + ") failed, errno=" + std::to_string(errno));
		}
#endif
	}

	// Turns a relative config path into an absolute one under the executable
	// directory (belt and braces on top of chdirToExeDir()).
	std::string resolveConfigPath(const std::string& path)
	{
		if (path.empty()) return path;
#ifdef WIN32
		if (path.size() >= 2 && path[1] == ':') return path;
		if (path.size() >= 2 && (path[0] == '\\' || path[0] == '/')) return path;
		return getExeDir() + "\\" + path;
#else
		if (path[0] == '/') return path;
		return getExeDir() + "/" + path;
#endif
	}

#ifdef WIN32
	BOOL WINAPI consoleHandler(DWORD ctrlType)
	{
		if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT ||
			ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT)
		{
			// Signal-safe path only: flag the request, never open files or format
			// log lines here. Service::run() logs the actual shutdown.
			g_stopRequested = 1;
			if (g_service)
				g_service->requestStop();
			return TRUE;
		}
		return FALSE;
	}
#else
	void signalHandler(int sig)
	{
		(void)sig;
		// async-signal-safe only: no logging, no allocation, no locks
		g_stopRequested = 1;
		if (g_service)
			g_service->requestStop();
	}
#endif

	void installSignalHandlers()
	{
#ifdef WIN32
		if (!SetConsoleCtrlHandler(consoleHandler, TRUE))
			fprintf(stderr, "WARN: SetConsoleCtrlHandler failed, Ctrl+C will not stop the service\n");
#else
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = signalHandler;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGHUP, &sa, NULL);
#endif
	}

	void printUsage(const char* prog)
	{
		printf("Usage: %s -c <MarketDataService.json> [--help]\n", prog);
		printf("\n");
		printf("Standalone CTP market-data service:\n");
		printf("  * subscribes the whole futures market (instrument list fetched from the\n");
		printf("    openctp dict service, with a local cache fallback)\n");
		printf("  * stores every OnRtnDepthMarketData tick (ClickHouse HTTP or local CSV)\n");
		printf("  * broadcasts ticks over TCP unicast and UDP multicast\n");
	}
}

int main(int argc, char** argv)
{
	chdirToExeDir();

	std::string configFile = "config/MarketDataService.local-file.json";

	for (int i = 1; i < argc; i++)
	{
		if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc)
		{
			configFile = argv[++i];
		}
		else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
		{
			printUsage(argv[0]);
			return 0;
		}
		else
		{
			// daemon wrappers may pass extra options (-immediately, ...): ignore
			// them instead of refusing to start
			fprintf(stderr, "ignoring unknown option: %s\n", argv[i]);
		}
	}

	const std::string resolvedConfig = resolveConfigPath(configFile);

	mdgw::Service service;
	installSignalHandlers();

	std::string err;
	if (!service.init(resolvedConfig.c_str(), err))
	{
		logStartupFailure("service.init failed: " + err);
		fprintf(stderr, "ERROR: %s\n", err.c_str());
		mdgw::Logger::instance().error("%s", err.c_str());
		// g_service is still null here, so no handler can reach the object
		return 1;
	}

	g_service = &service;     // handlers may request a stop from now on
	if (g_stopRequested)      // Ctrl+C arrived while init() was still running
		service.requestStop();
	service.run();
	g_service = nullptr;      // ~Service runs from here on; keep handlers away
	return 0;
}
