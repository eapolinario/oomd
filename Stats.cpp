/*
 * Copyright (C) 2018-present, Facebook, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <json/reader.h>
#include <json/value.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

#include "oomd/Stats.h"
#include "oomd/util/Fs.h"
#include "oomd/util/ScopeGuard.h"
#include "oomd/util/Util.h"

namespace Oomd {

Stats::Stats(const std::string& stats_socket_path)
    : stats_socket_path_(stats_socket_path) {
  if (!this->startSocket()) {
    throw std::runtime_error("Socket thread failed to start");
  }
}

Stats::~Stats() {
  statsThreadRunning_ = false;
  Stats::msgSocket("0");
  if (stats_thread_.joinable()) {
    stats_thread_.join();
  }
  std::array<char, 64> err_buf;
  ::memset(err_buf.data(), '\0', err_buf.size());
  if (::unlink(serv_addr_.sun_path) < 0) {
    OLOG << "Closing stats error: unlinking socket path: "
         << ::strerror_r(errno, err_buf.data(), err_buf.size());
  }
  if (::close(sockfd_) < 0) {
    OLOG << "Closing stats error: closing stats socket: "
         << ::strerror_r(errno, err_buf.data(), err_buf.size());
  }
}

Stats& Stats::get(const std::string& stats_socket_path) {
  static Stats singleton(stats_socket_path);
  return singleton;
}

std::unique_ptr<Stats> Stats::get_for_unittest(
    const std::string& stats_socket_path) {
  return std::unique_ptr<Stats>(new Stats(stats_socket_path));
}

bool Stats::init(const std::string& stats_socket_path) {
  try {
    Stats::get(stats_socket_path);
  } catch (const std::runtime_error& e) {
    OLOG << "Initializing singleton failed: " << e.what();
    return false;
  }
  return true;
}

bool Stats::startSocket() {
  std::array<char, 64> err_buf;
  ::memset(err_buf.data(), '\0', err_buf.size());
  size_t dir_end = stats_socket_path_.find_last_of("/");
  // Iteratively checks if dirs in path exist, creates them if not
  if (dir_end != std::string::npos) {
    std::string dirs = stats_socket_path_.substr(0, dir_end);
    size_t pos = 0;
    if (dirs[0] == '/') {
      pos++;
    }
    while (pos < dirs.size()) {
      pos = dirs.find('/', pos);
      if (pos == std::string::npos) {
        pos = dir_end;
      }
      std::string curr_path = dirs.substr(0, pos);
      int res = ::mkdir(curr_path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
      if (res != 0 && errno != EEXIST) {
        OLOG << "Making socket dir error: "
             << ::strerror_r(errno, err_buf.data(), err_buf.size()) << ": "
             << curr_path;
        return false;
      }
      pos++;
    }
  }

  sockfd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (sockfd_ < 0) {
    OLOG << "Error creating socket: "
         << ::strerror_r(errno, err_buf.data(), err_buf.size() - 1);
    return false;
  }
  ::memset(&serv_addr_, '\0', sizeof(serv_addr_));
  serv_addr_.sun_family = AF_UNIX;
  ::strcpy(serv_addr_.sun_path, stats_socket_path_.c_str());
  if (::unlink(serv_addr_.sun_path) < 0 && errno != ENOENT) {
    OLOG << "Pre-unlinking of socket path failed. " << serv_addr_.sun_path
         << ". Errno: " << ::strerror_r(errno, err_buf.data(), err_buf.size());
    return false;
  }
  if (::bind(sockfd_, (struct sockaddr*)&serv_addr_, sizeof(serv_addr_)) < 0) {
    OLOG << "Error binding stats collection socket: "
         << ::strerror_r(errno, err_buf.data(), err_buf.size());
    return false;
  }
  if (::listen(sockfd_, 5) < 0) {
    OLOG << "Error listening at socket: "
         << ::strerror_r(errno, err_buf.data(), err_buf.size());
  }
  stats_thread_ = std::thread([this] { this->runSocket(); });
  return true;
}

void Stats::runSocket() {
  bool statsThreadRunning = true;
  sockaddr_un cli_addr;
  socklen_t clilen = sizeof(cli_addr);
  std::array<char, 64> err_buf;
  ::memset(err_buf.data(), '\0', err_buf.size());
  while (statsThreadRunning) {
    int sockfd = ::accept(sockfd_, (struct sockaddr*)&cli_addr, &clilen);
    if (sockfd < 0) {
      OLOG << "Stats server error: accepting connection: "
           << ::strerror_r(errno, err_buf.data(), err_buf.size());
      break;
    }
    OOMD_SCOPE_EXIT {
      if (::close(sockfd) < 0) {
        OLOG << "Stats server error: closing file descriptor: "
             << ::strerror_r(errno, err_buf.data(), err_buf.size());
      }
    };
    char mode = 'a';

    Json::Value root;
    root["error"] = 0;
    Json::Value body(Json::objectValue);
    switch (mode) {
      case 'g':
        break;
      case 'r':
        break;
      case '0':
        break;
      default:
        root["error"] = 1;
        OLOG << "Stats server error: received unknown request: " << mode;
    }
    root["body"] = body;
    std::string ret = root.toStyledString();
    if (Util::writeFull(sockfd, ret.c_str(), strlen(ret.c_str())) < 0) {
      OLOG << "Stats server error: writing to socket: "
           << ::strerror_r(errno, err_buf.data(), err_buf.size());
      break;
    }
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statsThreadRunning = statsThreadRunning_;
  }
}

std::unordered_map<std::string, int> Stats::getAll() {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

int Stats::increment(const std::string& key, int val) {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  stats_[key] = stats_[key] + val;
  return 0;
}

int Stats::set(const std::string& key, int val) {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  stats_[key] = val;
  return 0;
}

int Stats::reset() {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  for (const auto& pair : stats_) {
    stats_[pair.first] = 0;
  }
  return 0;
}

std::optional<std::string> Stats::msgSocket(const std::string& msg) {
  std::array<char, 64> err_buf;
  ::memset(err_buf.data(), '\0', err_buf.size());
  int sockfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (sockfd < 0) {
    std::cerr << "Error: creating client socket: "
              << ::strerror_r(errno, err_buf.data(), err_buf.size())
              << std::endl;
    return std::nullopt;
  }
  OOMD_SCOPE_EXIT {
    if (::close(sockfd) < 0) {
      std::cerr << "Error: shutting down client socket: "
                << ::strerror_r(errno, err_buf.data(), err_buf.size())
                << std::endl;
    }
  };
  if (::connect(sockfd, (struct sockaddr*)&serv_addr_, sizeof(serv_addr_)) <
      0) {
    std::cerr << "Error: connecting to stats socket: "
              << ::strerror_r(errno, err_buf.data(), err_buf.size())
              << std::endl;
    return std::nullopt;
  }
  std::string msg_full = msg + "\n";
  if (Util::writeFull(sockfd, msg_full.c_str(), strlen(msg_full.c_str())) < 0) {
    std::cerr << "Error: writing to stats socket: "
              << ::strerror_r(errno, err_buf.data(), err_buf.size())
              << std::endl;
    return std::nullopt;
  }
  std::string ret = "";
  std::array<char, 512> msg_buf;
  while (true) {
    int n = Util::readFull(sockfd, msg_buf.data(), msg_buf.size() - 1);
    if (n < 0) {
      std::cerr << "Error: reading from stats socket: "
                << ::strerror_r(errno, err_buf.data(), err_buf.size())
                << std::endl;
      return std::nullopt;
    } else if (n == 0) {
      break;
    }
    msg_buf[n] = '\0';
    ret += std::string(msg_buf.data());
  }
  return std::optional<std::string>{ret};
}

std::unordered_map<std::string, int> getStats() {
  return Stats::get().getAll();
}

int incrementStats(const std::string& key, int val) {
  return Stats::get().increment(key, val);
}

int setStats(const std::string& key, int val) {
  return Stats::get().set(key, val);
}

int resetStats() {
  return Stats::get().reset();
}

} // namespace Oomd
