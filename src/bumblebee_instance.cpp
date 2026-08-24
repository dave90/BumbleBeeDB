//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bumblebee_instance.cpp
//
// Identification: src/bumblebee_instance.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "bumblebee_instance.h"

#include <utility>

#include "common/exception.h"
#include "common/util/string_util.h"

namespace bumblebee {

static auto ConfigWithTimeout(duration_t timeout) -> DatabaseConfig {
  DatabaseConfig config;
  config.transaction_timeout_ = timeout;
  return config;
}

static auto ConfigWithStorage(size_t frames, duration_t timeout) -> DatabaseConfig {
  auto config = ConfigWithTimeout(timeout);
  config.frames_ = frames;
  return config;
}

BumbleBeeInstance::BumbleBeeInstance(duration_t txn_timeout) : BumbleBeeInstance(ConfigWithTimeout(txn_timeout)) {}

BumbleBeeInstance::BumbleBeeInstance(DatabaseConfig config)
    : database_(std::make_shared<DatabaseInstance>(std::move(config))) {
  sessions_.emplace("default", DatabaseInstance::CreateConnection(database_));
}

BumbleBeeInstance::BumbleBeeInstance(const std::filesystem::path &db_file, size_t num_frames, duration_t txn_timeout)
    : BumbleBeeInstance(db_file, ConfigWithStorage(num_frames, txn_timeout)) {}

BumbleBeeInstance::BumbleBeeInstance(const std::filesystem::path &db_file, DatabaseConfig config)
    : database_(std::make_shared<DatabaseInstance>(db_file, std::move(config))) {
  sessions_.emplace("default", DatabaseInstance::CreateConnection(database_));
}

BumbleBeeInstance::~BumbleBeeInstance() { Close(); }

auto BumbleBeeInstance::CurrentConnection() -> Connection & { return *sessions_.at(current_session_); }

auto BumbleBeeInstance::ExecuteSql(const std::string &sql, ResultWriter &writer) -> bool {
  static const std::string kSession = "\\session ";
  if (sql.rfind(kSession, 0) == 0) {
    auto name = sql.substr(kSession.size());
    StringUtil::LTrim(&name);
    StringUtil::RTrim(&name);
    if (name.empty()) {
      throw Exception("\\session requires a session name");
    }
    auto [it, inserted] = sessions_.try_emplace(name);
    if (inserted) {
      it->second = DatabaseInstance::CreateConnection(database_);
    }
    current_session_ = std::move(name);
    return true;
  }
  return CurrentConnection().ExecuteSql(sql, writer);
}

auto BumbleBeeInstance::ExecuteSqlResults(const std::string &sql) -> std::vector<QueryResult> {
  return CurrentConnection().ExecuteSqlResults(sql);
}

void BumbleBeeInstance::GenerateMockTable() { sessions_.at("default")->GenerateMockTable(); }

void BumbleBeeInstance::Close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  for (auto &[name, connection] : sessions_) {
    connection->Close();
  }
  sessions_.clear();
  database_->Close();
}

}  // namespace bumblebee
