#include "log.h"

#include "../util_env.h"

// GESVR: defined in d3d9_vr.cpp. Copies a warning or error into vrmod_log.txt,
// which is appended to across launches; hl2_d3d9.log is truncated at every
// start, so the error behind a failed launch was gone by the time anyone looked.
void GESVR_DxvkLogLine(const char* prefix, const char* line);

namespace dxvk {
  
  Logger::Logger(const std::string& file_name)
  : m_minLevel(getMinLogLevel()) {
    if (m_minLevel != LogLevel::None) {
      auto path = getFileName(file_name);

      if (!path.empty())
        m_fileStream = std::ofstream(str::tows(path.c_str()).c_str());
    }
  }
  
  
  Logger::~Logger() { }
  
  
  void Logger::trace(const std::string& message) {
    s_instance.emitMsg(LogLevel::Trace, message);
  }
  
  
  void Logger::debug(const std::string& message) {
    s_instance.emitMsg(LogLevel::Debug, message);
  }
  
  
  void Logger::info(const std::string& message) {
    s_instance.emitMsg(LogLevel::Info, message);
  }
  
  
  void Logger::warn(const std::string& message) {
    s_instance.emitMsg(LogLevel::Warn, message);
  }
  
  
  void Logger::err(const std::string& message) {
    s_instance.emitMsg(LogLevel::Error, message);
  }
  
  
  void Logger::log(LogLevel level, const std::string& message) {
    s_instance.emitMsg(level, message);
  }
  
  
  void Logger::emitMsg(LogLevel level, const std::string& message) {
    if (level >= m_minLevel) {
      static std::array<const char*, 5> s_prefixes
        = {{ "trace: ", "debug: ", "info:  ", "warn:  ", "err:   " }};

      const char* prefix = s_prefixes.at(static_cast<uint32_t>(level));

      { std::lock_guard<dxvk::mutex> lock(m_mutex);

        std::stringstream stream(message);
        std::string       line;

        while (std::getline(stream, line, '\n')) {
          std::cerr << prefix << line << std::endl;

          if (m_fileStream)
            m_fileStream << prefix << line << std::endl;
        }
      }

      // Outside our lock: the mod's log has its own.
      if (level >= LogLevel::Warn)
        GESVR_DxvkLogLine(prefix, message.c_str());
    }
  }
  
  
  LogLevel Logger::getMinLogLevel() {
    const std::array<std::pair<const char*, LogLevel>, 6> logLevels = {{
      { "trace", LogLevel::Trace },
      { "debug", LogLevel::Debug },
      { "info",  LogLevel::Info  },
      { "warn",  LogLevel::Warn  },
      { "error", LogLevel::Error },
      { "none",  LogLevel::None  },
    }};
    
    const std::string logLevelStr = env::getEnvVar("DXVK_LOG_LEVEL");
    
    for (const auto& pair : logLevels) {
      if (logLevelStr == pair.first)
        return pair.second;
    }
    
    return LogLevel::Info;
  }
  
  
  std::string Logger::getFileName(const std::string& base) {
    std::string path = env::getEnvVar("DXVK_LOG_PATH");
    
    if (path == "none")
      return "";

    if (!path.empty() && *path.rbegin() != '/')
      path += '/';

    std::string exeName = env::getExeBaseName();
    path += exeName + "_" + base;
    return path;
  }
  
}
