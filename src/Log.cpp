// Log.cpp - implementation of the minimal logger declared in Log.h.
//
// Stamps each line with the wall-clock time AND the current game mark, then writes it to exactly one
// of three files (Main / Mission / AI) plus (when enabled) stderr, and always the debugger via
// OutputDebugString. The AI file is opened lazily so a mission with no AI debug never creates one.
// A failed CHECK logs and aborts.

#include "Log.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace OP2Lua {

namespace {

std::mutex gLogMutex;
std::unique_ptr<std::ofstream> gMainFile;     // persistent: the full framework record across missions
std::unique_ptr<std::ofstream> gMissionFile;  // re-pointed per mission: this mission's events
std::unique_ptr<std::ofstream> gAIFile;       // re-pointed per mission, opened lazily on first AI line
std::string gAIPath, gAIHeader;               // pending AI-file path/header until the first AI line
std::string gAIStatusPath;                    // the AI status snapshot file (overwritten each write)
bool gToConsole = false;
MarkProvider gMarkProvider = nullptr;

std::string DateTimeStamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kInfo:    return "INFO";
    case LogLevel::kWarning: return "WARN";
    case LogLevel::kError:   return "ERROR";
  }
  return "?";
}

// "HH:MM:SS m<mark>" prefix. Time is local wall-clock; mark is the game mark (100 ticks), or "----"
// when no game is running yet. Logging is presentation only and never feeds the simulation.
std::string Prefix() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char timeBuf[16];
  std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tm);

  const int mark = gMarkProvider ? gMarkProvider() : -1;
  char buf[48];
  if (mark >= 0) std::snprintf(buf, sizeof(buf), "%s m%-5d", timeBuf, mark);
  else           std::snprintf(buf, sizeof(buf), "%s m----", timeBuf);
  return buf;
}

void WriteHeader(std::ofstream& f, const char* kind, const std::string& header) {
  f << "\n===== OP2Lua " << kind;
  if (!header.empty()) f << " - " << header;
  f << " - started " << DateTimeStamp() << " =====\n";
  f.flush();
}

// The AI file is opened on demand (first AI line) so non-AI missions create no AI log.
std::ofstream* AIFile() {
  if (!gAIFile && !gAIPath.empty()) {
    gAIFile = std::make_unique<std::ofstream>(gAIPath, std::ios::out | std::ios::app);
    if (gAIFile->is_open()) WriteHeader(*gAIFile, "AI log", gAIHeader);
  }
  return (gAIFile && gAIFile->is_open()) ? gAIFile.get() : nullptr;
}

}  // namespace

void InitMainLog(const std::string& logFilePath, bool alsoToConsole) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  gToConsole = alsoToConsole;
  if (gMainFile && gMainFile->is_open()) return;  // open once; persists across missions
  gMainFile = std::make_unique<std::ofstream>(logFilePath, std::ios::out | std::ios::app);
  if (gMainFile->is_open()) WriteHeader(*gMainFile, "main log session", "");
}

void SetMissionLog(const std::string& logFilePath, const std::string& header) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  if (logFilePath.empty()) { gMissionFile.reset(); return; }  // no per-mission file (main only)
  gMissionFile = std::make_unique<std::ofstream>(logFilePath, std::ios::out | std::ios::app);
  if (gMissionFile->is_open()) WriteHeader(*gMissionFile, "mission log", header);
}

void SetAILog(const std::string& logFilePath, const std::string& header) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  gAIFile.reset();                 // close the previous mission's AI file
  gAIPath = logFilePath;           // opened lazily on the first AI line for this mission
  gAIHeader = header;
}

void SetMarkProvider(MarkProvider provider) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  gMarkProvider = provider;
}

void SetAIStatusPath(const std::string& path) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  gAIStatusPath = path;
}

void WriteAIStatusSnapshot(const std::string& content) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  if (gAIStatusPath.empty()) return;
  // Truncate: the file is a *current-state* snapshot, not an append log - an external monitor reads
  // the whole (small) file to see where the AI is right now.
  std::ofstream f(gAIStatusPath, std::ios::out | std::ios::trunc);
  if (f.is_open()) { f << content; f.flush(); }
}

void ShutdownLogging() {
  std::lock_guard<std::mutex> lock(gLogMutex);
  gAIFile.reset();
  gMissionFile.reset();
  gMainFile.reset();
}

LogMessage::LogMessage(LogLevel level, const char* file, int line, LogSink sink)
    : level_(level), sink_(sink), file_(file), line_(line) {}

LogMessage::~LogMessage() {
  std::lock_guard<std::mutex> lock(gLogMutex);
  std::string line = Prefix();
  line += " [";
  line += LevelName(level_);
  line += "] ";
  line += ss_.str();
  line += '\n';

  std::ofstream* f = nullptr;
  switch (sink_) {
    case kSinkMain:    f = (gMainFile    && gMainFile->is_open())    ? gMainFile.get()    : nullptr; break;
    case kSinkMission: f = (gMissionFile && gMissionFile->is_open()) ? gMissionFile.get() : nullptr; break;
    case kSinkAI:      f = AIFile(); break;  // lazy-open
  }
  if (f != nullptr) {
    (*f) << line;
    f->flush();
  }
  if (gToConsole) {
    std::cerr << line;
  }
  // Always also forward to the debugger output (handy under VS / DebugView).
  OutputDebugStringA(line.c_str());
}

CheckMessage::~CheckMessage() {
  if (cond_) return;
  {
    LogMessage msg(LogLevel::kError, file_, line_);
    msg.stream() << "CHECK failed: " << expr_;
    if (!ss_.str().empty()) msg.stream() << " - " << ss_.str();
  }
  // A failed CHECK means an invariant we depend on (e.g. a hook offset) is wrong - fatal.
  std::abort();
}

}  // namespace OP2Lua
