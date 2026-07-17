//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// disk_scheduler.cpp
//
// Identification: src/storage/disk/disk_scheduler.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/disk/disk_scheduler.h"

#include <optional>
#include <utility>
#include <vector>

namespace bumblebee {

DiskScheduler::DiskScheduler(DiskManager *disk_manager, size_t num_workers) : disk_manager_(disk_manager) {
  if (num_workers == 0) {
    num_workers = 1;
  }
  workers_.reserve(num_workers);
  for (size_t i = 0; i < num_workers; i++) {
    workers_.emplace_back([&] { StartWorkerThread(); });
  }
}

DiskScheduler::~DiskScheduler() {
  // Signal every worker to stop (one sentinel each), then wait for them all.
  for (size_t i = 0; i < workers_.size(); i++) {
    request_queue_.Put(std::nullopt);
  }
  for (auto &worker : workers_) {
    worker.join();
  }
}

void DiskScheduler::Schedule(std::vector<DiskRequest> &requests) {
  for (auto &request : requests) {
    request_queue_.Put(std::move(request));
  }
}

void DiskScheduler::Schedule(DiskRequest &request) { request_queue_.Put(std::move(request)); }

void DiskScheduler::StartWorkerThread() {
  while (true) {
    auto request = request_queue_.Get();
    if (!request.has_value()) {
      break;
    }

    // A failing or throwing disk op must still resolve the future, or every waiter deadlocks.
    try {
      bool ok = request->is_write_ ? disk_manager_->WritePage(request->page_id_, request->data_)
                                   : disk_manager_->ReadPage(request->page_id_, request->data_);
      request->callback_.set_value(ok);
    } catch (...) {
      request->callback_.set_exception(std::current_exception());
    }
  }
}

}  // namespace bumblebee
