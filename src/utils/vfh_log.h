//
// Copyright (c) 2015-2016, Chaos Software Ltd
//
// V-Ray For Houdini
//
// ACCESSIBLE SOURCE CODE WITHOUT DISTRIBUTION OF MODIFICATION LICENSE
//
// Full license text: https://github.com/ChaosGroup/vray-for-houdini/blob/master/LICENSE
//

#ifndef VRAY_FOR_HOUDINI_LOG_H
#define VRAY_FOR_HOUDINI_LOG_H

#include "vfh_defines.h"
#include "vfh_vray.h" // For proper "systemstuff.h" inclusion

/// this define enables compilation of anything from <atomic> which is included by the boost lockfree queue
/// it was introduced because the old implementation of atomic (before vs 2015 update 2) is bugged
/// if the atomic variables are not properly aligned
#define _ENABLE_ATOMIC_ALIGNMENT_FIX
#include <boost/lockfree/queue.hpp>

#include <functional>
#include <array>

namespace VRayForHoudini {
namespace Log {

enum LogLevel {
	LogLevelMsg = 0,
	LogLevelInfo,
	LogLevelProgress,
	LogLevelWarning,
	LogLevelError,
	LogLevelDebug,
};

/// Simple logger class wrapping over printf
struct Logger {
	Logger()
		: m_logLevel(LogLevelDebug)
	{}

	/// Log string with msg level, always show not taking current log level into account
	void msg(const tchar *format, ...);
	/// Log string with info level
	void info(const tchar *format, ...);
	/// Log string with progress level
	void progress(const tchar *format, ...);
	/// Log string with warning level
	void warning(const tchar *format, ...);
	/// Log string with error level
	void error(const tchar *format, ...);
	/// Log string with debug level
	void debug(const tchar *format, ...);

	/// Set max log level to be printed, unless Logger::msg is used where current filter is ignored
	void setLogLevel(LogLevel logLevel) { m_logLevel = logLevel; }

	/// Initialize the logger, needs to be called only once
	/// Dont call this from static variable's ctor, which is inside a module (causes deadlock)
	static void startLogging();

	/// Call this before exiting the application, and not from static variable's dtor
	/// It will stop and join the logger thread
	static void stopLogging();
private:
	/// Implementation for the actual logging
	void log(LogLevel level, const tchar *format, va_list args);

	LogLevel m_logLevel; ///< Current max log level to be shown

#ifndef VFH_NO_THREAD_LOGGER
public:
	/// One line of log, assume it wont be longer than 1k
	typedef std::array<tchar, 1024> LogLineType;

	/// Using this pair instead of std::pair, because lockfree::queue requires a template
	/// argument which has trivial ctor, dtor and assignment operator
	struct LogPair {
		LogLevel level; ///< the message's log level
		LogLineType line; ///< the message data, null terminated
	};

private:
	boost::lockfree::queue<LogPair> m_queue; ///< queue for messages to be logged

	/// Loop and dump any messages from the queue to stdout
	/// Used as base for the thread that is processing the messages
	static void writeMessages();
#endif
};

/// Get singleton instance to Logger
Logger &getLog();

} // namespace Log
} // namespace VRayForHoudini

#endif // VRAY_FOR_HOUDINI_LOG_H
