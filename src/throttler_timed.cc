// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/throttler_timed.h"

#include <algorithm>

#include "src/uploader_file.h"
#include "src/uploader_gcs.h"

DEFINE_int32(cprof_interval_sec, cloud::profiler::kProfileWaitSeconds, "");
DEFINE_int32(cprof_duration_sec, cloud::profiler::kProfileDurationSeconds, "");
DEFINE_int32(cprof_delay_sec, 0, "");
DEFINE_int32(cprof_max_count, cloud::profiler::kProfileMaxCount, "");
DEFINE_string(cprof_force, "", "");

namespace cloud {
namespace profiler {

namespace {

const int64_t kRandomRange = 65536;

// Gets the sampling configuration from the flags.
int64_t GetConfiguration(int64_t *duration_cpu_ns, int64_t *duration_wall_ns) {
  int64_t duration_ns = FLAGS_cprof_duration_sec * kNanosPerSecond;

  *duration_cpu_ns = 0;
  *duration_wall_ns = 0;
  if (FLAGS_cprof_force == "") {
    *duration_cpu_ns = duration_ns;
    *duration_wall_ns = duration_ns;
  } else if (FLAGS_cprof_force == kTypeCPU) {
    *duration_cpu_ns = duration_ns;
  } else if (FLAGS_cprof_force == kTypeWall) {
    *duration_wall_ns = duration_ns;
  } else {
    LOG(ERROR) << "Unrecognized option cprof_force=" << FLAGS_cprof_force
               << ", profiling disabled";
  }

  return FLAGS_cprof_interval_sec * kNanosPerSecond;
}

bool StartsWith(const string& s, const string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

string TryStripPrefix(const string& s, const string& prefix) {
  return StartsWith(s, prefix) ? s.substr(prefix.size()) : s;
}

std::unique_ptr<ProfileUploader> UploaderFromFlags(const string& path) {
  if (path.empty()) {
    LOG(ERROR) << "Expected non-empty profile path";
    return nullptr;
  }
  string filename = TryStripPrefix(path, "gs://");
  if (filename != path) {
    LOG(INFO) << "Will upload profiles to Google Cloud Storage";
    return std::unique_ptr<ProfileUploader>(
        new GcsUploader(DefaultCloudEnv(), filename));
  } else {
    LOG(INFO) << "Will save profiles to the local filesystem";
    return std::unique_ptr<ProfileUploader>(new FileUploader(filename));
  }
}

}  // namespace

TimedThrottler::TimedThrottler(const string& path)
    : TimedThrottler(UploaderFromFlags(path), DefaultClock(), false) {}

TimedThrottler::TimedThrottler(std::unique_ptr<ProfileUploader> uploader,
                               Clock* clock, bool fixed_seed)
    : clock_(clock), profile_count_(), uploader_(std::move(uploader)) {
  interval_ns_ = GetConfiguration(&duration_cpu_ns_, &duration_wall_ns_);
  LOG(INFO) << "sampling duration: cpu=" << duration_cpu_ns_ / kNanosPerSecond
            << "s, wall=" << duration_wall_ns_ / kNanosPerSecond << "s";
  LOG(INFO) << "sampling interval: " << interval_ns_ / kNanosPerSecond << "s";
  LOG(INFO) << "sampling delay: " << FLAGS_cprof_delay_sec << "s";

  struct timespec now = clock_->Now();

  next_interval_ = now;
  if (FLAGS_cprof_delay_sec != 0) {
    struct timespec delay_ts =
        NanosToTimeSpec(FLAGS_cprof_delay_sec * kNanosPerSecond);
    next_interval_ = TimeAdd(next_interval_, delay_ts);
  }

  // Create a random number generator, seeded on the microseconds from the
  // current timer if not asked for fixed seed (which should only be used for
  // determinism in tests).
  gen_ = std::default_random_engine(fixed_seed ? 10 : now.tv_nsec / 1000);
  dist_ = std::uniform_int_distribution<int64_t>(0, kRandomRange);

  // This will get popped on the first WaitNext() call.
  cur_.push_back({"", 0});
}

bool TimedThrottler::WaitNext() {
  if (!uploader_ || (duration_cpu_ns_ == 0 && duration_wall_ns_ == 0)) {
    // Refuse profiling if both CPU and wall are disabled or no uploader.
    LOG(WARNING) << "Profiling disabled";
    return false;
  }

  if (cur_.empty()) {
    // Should not normally be reached.
    return false;
  }

  cur_.pop_back();
  if (cur_.empty()) {
    if (FLAGS_cprof_max_count > 0 && profile_count_ >= FLAGS_cprof_max_count) {
      LOG(INFO) << "Reached maximum number of profiles to collect";
      return false;
    }
    profile_count_++;

    int64_t random_value = dist_(gen_);
    int64_t wait_range_ns = interval_ns_ - duration_cpu_ns_ - duration_wall_ns_;
    if (wait_range_ns < 0) {
      wait_range_ns = 0;
    }
    int64_t wait_ns = (wait_range_ns / kRandomRange) * random_value;

    struct timespec profiling_start =
        TimeAdd(next_interval_, NanosToTimeSpec(wait_ns));
    clock_->SleepUntil(profiling_start);
    next_interval_ = TimeAdd(next_interval_, NanosToTimeSpec(interval_ns_));

    if (duration_cpu_ns_ > 0) {
      cur_.push_back({kTypeCPU, duration_cpu_ns_});
    }
    if (duration_wall_ns_ > 0) {
      cur_.push_back({kTypeWall, duration_wall_ns_});
    }
    // Randomize the profile type order.
    std::shuffle(cur_.begin(), cur_.end(), gen_);
  }

  return true;
}

string TimedThrottler::ProfileType() {
  return cur_.empty() ? "" : cur_.back().first;
}

int64_t TimedThrottler::DurationNanos() {
  return cur_.empty() ? 0 : cur_.back().second;
}

bool TimedThrottler::Upload(string profile) {
  if (cur_.empty() || !uploader_) {
    return false;
  }
  return uploader_->Upload(cur_.back().first, profile);
}

}  // namespace profiler
}  // namespace cloud
