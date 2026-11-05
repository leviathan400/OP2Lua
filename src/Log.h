#pragma once

// Minimal, dependency-free logging used across OP2Lua. Writes timestamped + game-mark-stamped lines
// to up to three log files (and optionally the console / debugger). Streaming API:
//   LOG(INFO|WARNING|ERROR) << ...;          -> the Main log (framework record)
//   LOG_MISSION(INFO|WARNING|ERROR) << ...;  -> the current Mission log (mission events)
//   LOG_AI(INFO|WARNING|ERROR) << ...;       -> the current AI log (a scripted AI's debug)
//   VLOG(n) << ...;   CHECK(cond) << ...;   DCHECK(cond) << ...;
//
// Note on the ERROR token: <windows.h> #defines ERROR as 0, so LOG(ERROR) would expand to LOG(0).
// We dodge that by token-pasting the severity name - the `##` operator suppresses macro expansion
// of its argument, so LOG(ERROR) resolves to our symbol, not LOG(0).

#include <ostream>
#include <sstream>
#include <string>

namespace OP2Lua {

enum class LogLevel { kInfo, kWarning, kError };

// Which physical file a line is written to. Each line targets exactly ONE sink, so the same text is
// never duplicated across files (e.g. a crash writes a one-line summary to Main and the full dump to
// Mission - two different lines, not the same text twice):
//   kSinkMain    -> logs/OP2Lua.log          : persistent, cross-mission framework record
//                                               (mission load/end, version, map, crash summaries).
//   kSinkMission -> logs/OP2Lua-<base>.log    : this mission's events, game messages, errors, and
//                                               the FULL crash dump.
//   kSinkAI      -> logs/OP2Lua-<base>-AI.log : a scripted AI's debug stream. Created lazily on the
//                                               first AI line, so a mission with no AI makes no file.
// (The console/debugger mirror receives every line regardless of sink.)
enum LogSink { kSinkMain, kSinkMission, kSinkAI };

// Open the persistent Main log once (idempotent across missions). Re-point the per-mission files on
// each mission load: SetMissionLog opens the mission file; SetAILog records the AI file path (opened
// lazily on the first AI line). `header` is a short descriptor written at the top of each file.
void InitMainLog(const std::string& logFilePath, bool alsoToConsole);
void SetMissionLog(const std::string& logFilePath, const std::string& header);
void SetAILog(const std::string& logFilePath, const std::string& header);
void ShutdownLogging();

// The logger stamps each line with the current game mark. The core supplies it via this provider
// (Log.cpp can't depend on Tethys). Returns the current mark, or -1 when no game is running yet.
using MarkProvider = int (*)();
void SetMarkProvider(MarkProvider provider);

// A live AI status snapshot (resources, unit counts) for an external monitor. UNLIKE the logs, the
// snapshot file is OVERWRITTEN on each write, so it always holds the *current* state. SetAIStatusPath
// points it at the per-mission file; WriteAIStatusSnapshot (over)writes the whole file.
void SetAIStatusPath(const std::string& path);
void WriteAIStatusSnapshot(const std::string& content);

// One log line. Flushes on destruction.
class LogMessage {
 public:
  LogMessage(LogLevel level, const char* file, int line, LogSink sink = kSinkMain);
  ~LogMessage();
  std::ostream& stream() { return ss_; }

 private:
  LogLevel level_;
  LogSink sink_;
  const char* file_;
  int line_;
  std::ostringstream ss_;
};

// Backing for CHECK/DCHECK. On failure (cond == false) the destructor logs and aborts.
class CheckMessage {
 public:
  CheckMessage(bool cond, const char* expr, const char* file, int line)
      : cond_(cond), expr_(expr), file_(file), line_(line) {}
  ~CheckMessage();
  template <typename T>
  CheckMessage& operator<<(const T& value) {
    if (!cond_) ss_ << value;
    return *this;
  }

 private:
  bool cond_;
  const char* expr_;
  const char* file_;
  int line_;
  std::ostringstream ss_;
};

}  // namespace OP2Lua

#define OP2LUA_LOG_IMPL_INFO \
  ::OP2Lua::LogMessage(::OP2Lua::LogLevel::kInfo, __FILE__, __LINE__).stream()
#define OP2LUA_LOG_IMPL_WARNING \
  ::OP2Lua::LogMessage(::OP2Lua::LogLevel::kWarning, __FILE__, __LINE__).stream()
#define OP2LUA_LOG_IMPL_ERROR \
  ::OP2Lua::LogMessage(::OP2Lua::LogLevel::kError, __FILE__, __LINE__).stream()

#define LOG(severity) OP2LUA_LOG_IMPL_##severity
// Verbose logging: routed to INFO for now (TODO: gate on a configurable verbosity level).
#define VLOG(n) OP2LUA_LOG_IMPL_INFO

// Mission-channel logging: the current mission's own events log (game.message, mission errors,
// a mission author's print()). Kept out of the Main framework log.
#define OP2LUA_LOG_MISSION_INFO \
  ::OP2Lua::LogMessage(::OP2Lua::LogLevel::kInfo, __FILE__, __LINE__, ::OP2Lua::kSinkMission).stream()
#define OP2LUA_LOG_MISSION_WARNING \
  ::OP2Lua::LogMessage(::OP2Lua::LogLevel::kWarning, __FILE__, __LINE__, ::OP2Lua::kSinkMission).stream()
#define OP2LUA_LOG_MISSION_ERROR \
  ::OP2Lua::LogMessage(::OP2Lua::LogLevel::kError, __FILE__, __LINE__, ::OP2Lua::kSinkMission).stream()
#define LOG_MISSION(severity) OP2LUA_LOG_MISSION_##severity

// AI-channel logging: a scripted AI's debug stream, in its own file so the AI can be monitored alone.
#define OP2LUA_LOG_AI_INFO \
  ::OP2Lua::LogMessage(::OP2Lua::LogLevel::kInfo, __FILE__, __LINE__, ::OP2Lua::kSinkAI).stream()
#define OP2LUA_LOG_AI_WARNING \
  ::OP2Lua::LogMessage(::OP2Lua::LogLevel::kWarning, __FILE__, __LINE__, ::OP2Lua::kSinkAI).stream()
#define OP2LUA_LOG_AI_ERROR \
  ::OP2Lua::LogMessage(::OP2Lua::LogLevel::kError, __FILE__, __LINE__, ::OP2Lua::kSinkAI).stream()
#define LOG_AI(severity) OP2LUA_LOG_AI_##severity

#define CHECK(expr) ::OP2Lua::CheckMessage((expr) ? true : false, #expr, __FILE__, __LINE__)
#ifdef NDEBUG
#define DCHECK(expr) ::OP2Lua::CheckMessage(true, #expr, __FILE__, __LINE__)
#else
#define DCHECK(expr) CHECK(expr)
#endif
