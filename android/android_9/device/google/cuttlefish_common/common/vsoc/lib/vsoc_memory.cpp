/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common/vsoc/lib/vsoc_memory.h"

#include <string.h>
#include <unistd.h>

#include <map>
#include <string>
#include <type_traits>

#include "common/libs/glog/logging.h"
#include "common/vsoc/shm/audio_data_layout.h"
#include "common/vsoc/shm/base.h"
#include "common/vsoc/shm/e2e_test_region_layout.h"
#include "common/vsoc/shm/gralloc_layout.h"
#include "common/vsoc/shm/input_events_layout.h"
#include "common/vsoc/shm/ril_layout.h"
#include "common/vsoc/shm/screen_layout.h"
#include "common/vsoc/shm/socket_forward_layout.h"
#include "common/vsoc/shm/wifi_exchange_layout.h"

#include "uapi/vsoc_shm.h"

namespace {

uint32_t AlignToPageSize(uint32_t val) {
  static uint32_t page_size = sysconf(_SC_PAGESIZE);
  return ((val + (page_size - 1)) / page_size) * page_size;
}

uint32_t AlignToPowerOf2(uint32_t val) {
  uint32_t power_of_2 = 1;
  while (power_of_2 < val) {
    power_of_2 *= 2;
  }
  return power_of_2;
}

// Takes a vector of objects and returns a vector of pointers to those objects.
template <typename T, typename R>
std::vector<R*> GetConstPointers(const std::vector<T>& v) {
  std::vector<R*> result;
  result.reserve(v.size());
  for (auto& element : v) {
    result.push_back(&element);
  }
  return result;
}
}  // namespace

namespace vsoc {

namespace {

class VSoCRegionLayoutImpl : public VSoCRegionLayout {
 public:
  VSoCRegionLayoutImpl(const char* region_name, size_t layout_size,
                       int guest_to_host_signal_table_log_size,
                       int host_to_guest_signal_table_log_size,
                       const char* managed_by)
      : region_name_(region_name),
        layout_size_(layout_size),
        guest_to_host_signal_table_log_size_(
            guest_to_host_signal_table_log_size),
        host_to_guest_signal_table_log_size_(
            host_to_guest_signal_table_log_size),
        managed_by_(managed_by) {
    size_ = GetMinRegionSize();
    LOG(INFO) << region_name << ": is " << size_;
  }
  VSoCRegionLayoutImpl(const VSoCRegionLayoutImpl&) = default;

  const char* region_name() const override { return region_name_; }
  const char* managed_by() const override { return managed_by_; }

  size_t layout_size() const override { return layout_size_; }
  int guest_to_host_signal_table_log_size() const override {
    return guest_to_host_signal_table_log_size_;
  }
  int host_to_guest_signal_table_log_size() const override {
    return host_to_guest_signal_table_log_size_;
  }
  uint32_t begin_offset() const override { return begin_offset_; }
  size_t region_size() const override { return size_; }
  void SetRegionSize(size_t size) { size_ = size; }
  void SetBeginOffset(uint32_t offset) { begin_offset_ = offset; }

  // Returns the minimum size the region needs to accomodate the signaling
  // section and the data layout.
  size_t GetMinRegionSize() const {
    auto size = GetOffsetOfRegionData();
    // Data section
    size += layout_size_;
    size = AlignToPageSize(size);
    return size;
  }

  uint32_t GetOffsetOfRegionData() const {
    uint32_t offset = 0;
    // Signal tables
    offset += (1 << guest_to_host_signal_table_log_size_) * sizeof(uint32_t);
    offset += (1 << host_to_guest_signal_table_log_size_) * sizeof(uint32_t);
    // Interrup signals
    offset += 2 * sizeof(uint32_t);
    return offset;
  }

 private:
  const char* region_name_{};
  const size_t layout_size_{};
  const int guest_to_host_signal_table_log_size_{};
  const int host_to_guest_signal_table_log_size_{};
  const char* managed_by_{};
  uint32_t begin_offset_{};
  size_t size_{};
};

class VSoCMemoryLayoutImpl : public VSoCMemoryLayout {
 public:
  explicit VSoCMemoryLayoutImpl(std::vector<VSoCRegionLayoutImpl>&& regions)
      : regions_(regions), region_idx_by_name_(GetNameToIndexMap(regions)) {
    for (size_t i = 0; i < regions_.size(); ++i) {
      // This link could be resolved later, but doing it here disables
      // managed_by cycles among the regions.
      if (regions[i].managed_by() &&
          !region_idx_by_name_.count(regions[i].managed_by())) {
        LOG(FATAL) << regions[i].region_name()
                   << " managed by unknown region: " << regions[i].managed_by()
                   << ". Manager Regions must be declared before the regions "
                      "they manage";
      }
    }

    uint32_t offset = 0;
    // Reserve space for global header
    offset += sizeof(vsoc_shm_layout_descriptor);
    // and region descriptors
    offset += regions_.size() * sizeof(vsoc_device_region);
    offset = AlignToPageSize(offset);

    // Calculate offsets for all regions and set the size of the device
    UpdateRegionOffsetsAndDeviceSize(offset);
  }

  ~VSoCMemoryLayoutImpl() = default;

  std::vector<const VSoCRegionLayout*> GetRegions() const {
    static std::vector<const VSoCRegionLayout*> ret =
        GetConstPointers<VSoCRegionLayoutImpl, const VSoCRegionLayout>(
            regions_);
    return ret;
  }

  const VSoCRegionLayout* GetRegionByName(
      const char* region_name) const override {
    if (!region_idx_by_name_.count(region_name)) {
      return nullptr;
    }
    return &regions_[region_idx_by_name_.at(region_name)];
  }

  uint32_t GetMemoryFileSize() const override { return device_size_; }

  void WriteLayout(void* shared_memory) const override;

  bool ResizeRegion(const char* region_name, size_t new_min_size) override {
    if (!region_idx_by_name_.count(region_name)) {
      LOG(ERROR) << "Unable to resize region: " << region_name
                 << ". Region not found";
      return false;
    }
    auto index = region_idx_by_name_.at(region_name);
    auto& region = regions_[index];
    auto min_required_size = region.GetMinRegionSize();

    // Align to page size
    new_min_size = AlignToPageSize(new_min_size);
    if (new_min_size < min_required_size) {
      LOG(ERROR) << "Requested resize of region " << region_name << " to "
                 << new_min_size << " (after alignment), it needs at least "
                 << min_required_size << " bytes.";
      return false;
    }

    region.SetRegionSize(new_min_size);

    // Get new offset for next region
    auto offset = region.begin_offset() + region.region_size();
    // Update offsets for all following regions
    UpdateRegionOffsetsAndDeviceSize(offset, index + 1);
    return true;
  }

 protected:
  VSoCMemoryLayoutImpl() = delete;
  VSoCMemoryLayoutImpl(const VSoCMemoryLayoutImpl&) = delete;

  // Helper function to allow the creation of the name to index map in the
  // constructor and allow the field to be const
  static std::map<const char*, size_t> GetNameToIndexMap(
      const std::vector<VSoCRegionLayoutImpl>& regions) {
    std::map<const char*, size_t> result;
    for (size_t index = 0; index < regions.size(); ++index) {
      auto region_name = regions[index].region_name();
      if (result.count(region_name)) {
        LOG(FATAL) << region_name << " used for more than one region";
      }
      result[region_name] = index;
    }
    return result;
  }

  // Updates the beginning offset of all regions starting at a specific index
  // (useful after a resize operation) and the device's size.
  void UpdateRegionOffsetsAndDeviceSize(uint32_t offset, size_t index = 0) {
    for (; index < regions_.size(); ++index) {
      regions_[index].SetBeginOffset(offset);
      offset += regions_[index].region_size();
    }

    // Make the device's size the smaller power of two possible
    device_size_ = AlignToPowerOf2(offset);
  }

  std::vector<VSoCRegionLayoutImpl> regions_;
  const std::map<const char*, size_t> region_idx_by_name_;
  uint32_t device_size_{};
};

// Writes a region's signal table layout to shared memory. Returns the region
// offset of free memory after the table and interrupt signaled word.
uint32_t WriteSignalTableDescription(vsoc_signal_table_layout* layout,
                                     uint32_t offset, int log_size) {
  layout->num_nodes_lg2 = log_size;
  // First the signal table
  layout->futex_uaddr_table_offset = offset;
  offset += (1 << log_size) * sizeof(uint32_t);
  // Then the interrupt signaled word
  layout->interrupt_signalled_offset = offset;
  offset += sizeof(uint32_t);
  return offset;
}

// Writes a region's layout description to shared memory
void WriteRegionDescription(vsoc_device_region* shmem_region_desc,
                            const VSoCRegionLayoutImpl& region) {
  // Region versions are deprecated, write some sensible value
  shmem_region_desc->current_version = 0;
  shmem_region_desc->min_compatible_version = 0;

  shmem_region_desc->region_begin_offset = region.begin_offset();
  shmem_region_desc->region_end_offset =
      region.begin_offset() + region.region_size();
  shmem_region_desc->offset_of_region_data = region.GetOffsetOfRegionData();
  strncpy(shmem_region_desc->device_name, region.region_name(),
          VSOC_DEVICE_NAME_SZ - 1);
  shmem_region_desc->device_name[VSOC_DEVICE_NAME_SZ - 1] = '\0';
  // Guest to host signal table at the beginning of the region
  uint32_t offset = 0;
  offset = WriteSignalTableDescription(
      &shmem_region_desc->guest_to_host_signal_table, offset,
      region.guest_to_host_signal_table_log_size());
  // Host to guest signal table right after
  offset = WriteSignalTableDescription(
      &shmem_region_desc->host_to_guest_signal_table, offset,
      region.host_to_guest_signal_table_log_size());
  // Double check that the region metadata does not collide with the data
  if (offset > shmem_region_desc->offset_of_region_data) {
    LOG(FATAL) << "Error: Offset of region data too small (is "
               << shmem_region_desc->offset_of_region_data << " should be "
               << offset << " ) for region " << region.region_name()
               << ". This is a bug";
  }
}

void VSoCMemoryLayoutImpl::WriteLayout(void* shared_memory) const {
  // Device header
  static_assert(CURRENT_VSOC_LAYOUT_MAJOR_VERSION == 2,
                "Region layout code must be updated");
  auto header = reinterpret_cast<vsoc_shm_layout_descriptor*>(shared_memory);
  header->major_version = CURRENT_VSOC_LAYOUT_MAJOR_VERSION;
  header->minor_version = CURRENT_VSOC_LAYOUT_MINOR_VERSION;
  header->size = GetMemoryFileSize();
  header->region_count = regions_.size();

  // Region descriptions go right after the layout descriptor
  header->vsoc_region_desc_offset = sizeof(vsoc_shm_layout_descriptor);
  auto region_descriptions = reinterpret_cast<vsoc_device_region*>(header + 1);
  for (size_t idx = 0; idx < regions_.size(); ++idx) {
    auto shmem_region_desc = &region_descriptions[idx];
    const auto& region = regions_[idx];
    WriteRegionDescription(shmem_region_desc, region);
    // Handle managed_by links
    if (region.managed_by()) {
      auto manager_idx = region_idx_by_name_.at(region.managed_by());
      if (manager_idx == VSOC_REGION_WHOLE) {
        LOG(FATAL) << "Region '" << region.region_name() << "' has owner "
                   << region.managed_by() << " with index " << manager_idx
                   << " which is the default value for regions without an "
                      "owner. Choose a different region to be at index "
                   << manager_idx
                   << ", make sure the chosen region is NOT the owner of any "
                      "other region";
      }
      shmem_region_desc->managed_by = manager_idx;
    } else {
      shmem_region_desc->managed_by = VSOC_REGION_WHOLE;
    }
  }
}

template <class R>
VSoCRegionLayoutImpl ValidateAndBuildLayout(int g_to_h_signal_table_log_size,
                                            int h_to_g_signal_table_log_size,
                                            const char* managed_by = nullptr) {
  // Double check that the Layout is a valid shm type.
  ASSERT_SHM_COMPATIBLE(R);
  return VSoCRegionLayoutImpl(R::region_name, sizeof(R),
                              g_to_h_signal_table_log_size,
                              h_to_g_signal_table_log_size, managed_by);
}

}  // namespace

VSoCMemoryLayout* VSoCMemoryLayout::Get() {
  /*******************************************************************
   * Make sure the first region is not the manager of other regions. *
   *       This error will only be caught on runtime!!!!!            *
   *******************************************************************/
  static VSoCMemoryLayoutImpl layout(
      {ValidateAndBuildLayout<layout::input_events::InputEventsLayout>(2, 2),
       ValidateAndBuildLayout<layout::screen::ScreenLayout>(2, 2),
       ValidateAndBuildLayout<layout::gralloc::GrallocManagerLayout>(2, 2),
       ValidateAndBuildLayout<layout::gralloc::GrallocBufferLayout>(
           0, 0,
           /* managed_by */ layout::gralloc::GrallocManagerLayout::region_name),
       ValidateAndBuildLayout<layout::socket_forward::SocketForwardLayout>(7,
                                                                           7),
       ValidateAndBuildLayout<layout::wifi::WifiExchangeLayout>(2, 2),
       ValidateAndBuildLayout<layout::ril::RilLayout>(2, 2),
       ValidateAndBuildLayout<layout::e2e_test::E2EPrimaryTestRegionLayout>(1,
                                                                            1),
       ValidateAndBuildLayout<layout::e2e_test::E2ESecondaryTestRegionLayout>(
           1, 1),
       ValidateAndBuildLayout<layout::e2e_test::E2EManagerTestRegionLayout>(1,
                                                                            1),
       ValidateAndBuildLayout<layout::e2e_test::E2EManagedTestRegionLayout>(1,
                                                                            1),
       ValidateAndBuildLayout<layout::audio_data::AudioDataLayout>(2, 2)});

  // We need this code to compile on both sides to enforce the static checks,
  // but should only be used host side if for no other reason because of the
  // possible resizing of some regions not being visible on the guest.
#if !defined(CUTTLEFISH_HOST)
  LOG(FATAL) << "Memory layout is not accurate in guest side, use region "
                "classes or the vsoc driver directly instead.";
#endif
  return &layout;
}

}  // namespace vsoc
