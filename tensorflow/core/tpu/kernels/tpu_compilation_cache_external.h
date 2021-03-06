/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#ifndef TENSORFLOW_CORE_TPU_KERNELS_TPU_COMPILATION_CACHE_EXTERNAL_H_
#define TENSORFLOW_CORE_TPU_KERNELS_TPU_COMPILATION_CACHE_EXTERNAL_H_

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "absl/container/node_hash_map.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "tensorflow/compiler/xla/service/hlo.pb.h"
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/platform/refcount.h"
#include "tensorflow/core/protobuf/tpu/compile_metadata.pb.h"
#include "tensorflow/core/tpu/kernels/compiled_subgraph.h"
#include "tensorflow/core/tpu/kernels/tpu_compilation_cache.pb.h"
#include "tensorflow/core/tpu/kernels/tpu_compilation_cache_entry.h"
#include "tensorflow/core/tpu/kernels/tpu_compilation_cache_key.h"
#include "tensorflow/core/tpu/kernels/tpu_compile_c_api.h"
#include "tensorflow/core/tpu/kernels/tpu_compile_op_support.h"
#include "tensorflow/core/tpu/kernels/tpu_mesh_state_interface.h"
#include "tensorflow/core/tpu/kernels/tpu_program_group.h"

namespace tensorflow {
namespace tpu {

const char kCompilationCacheResourceName[] = "tpu_compilation_cache";
const char kCompilationCacheUnloaderResourceName[] =
    "tpu_compilation_cache_unloader";

// Base class that holds references to compiled protos so that the protos are
// not garbage-collected before being used by execute ops. Use
// TpuCompilationCache::MakePerStepRefHolder to create an instance of a concrete
// ref holder object.
class TpuCompilationRefHolder : public ResourceBase {
 public:
  ~TpuCompilationRefHolder() override = default;
};

class TpuCompilationCacheExternal : public ResourceBase {
 public:
  using Status = ::stream_executor::port::Status;

  explicit TpuCompilationCacheExternal(int64_t max_cache_size);
  ~TpuCompilationCacheExternal() override;
  TpuCompilationCacheExternal(const TpuCompilationCacheExternal&) = delete;
  TpuCompilationCacheExternal& operator=(const TpuCompilationCacheExternal&) =
      delete;

  Status CompileIfKeyAbsent(
      const TpuCompilationCacheKey& cache_key,
      const SessionMetadata* session_metadata,
      TpuCompilationRefHolder* per_step_ref_holder, int64* uid,
      std::vector<string>* proto_key, std::vector<bool>* may_modify_variables,
      std::vector<std::shared_ptr<const xla::HloProto>>* hlo_metadata,
      const std::function<tensorflow::Status(TpuProgramGroup*)>&
          compile_function);

  static TpuCompilationCacheKey CreateCompilationCacheKey(
      absl::string_view function_name, uint64 function_library_fingerprint,
      absl::string_view mlir_module,
      const tensorflow::OpInputList& guaranteed_constants,
      const std::vector<tensorflow::TensorShape>& dynamic_shapes,
      const tensorflow::tpu::TPUCompileMetadataProto& metadata,
      const TpuMeshStateInterface& mesh_state);

  string DebugString() const override { return "TpuCompilationCacheExternal"; }

  // Makes a reference holder for this cache, that can be stored in the per-step
  // resource manager and will ensure that compiled entries persist until the
  // end of a step.
  TpuCompilationRefHolder* MakePerStepRefHolder();

  // Differences between MarkEntryForEviction and Release:
  // There are two modes of managing cache entries:
  // 1) LRU eviction + pinning; 2) manual.
  // We use mode 1) if CompilationRefHolder is provided to CompileIfKeyAbsent.
  // Otherwise it is manual mode (mainly used by XRT).
  // MarkEntryForEviction should only be used in mode 1) to eagerly evict cache
  // entries when callers know that they do not need them anymore.
  // Release should only be used in mode 2) to explicitly remove an entry.

  // Mark the entry indexed by `subgraph_uid` for eviction. This should only be
  // called if per_step_ref_holder was NOT nullptr in the corresponding call to
  // CompileIfKeyAbsent(subgraph_key, ...). Otherwise, use Release(int64
  // subgraph_uid).
  Status MarkEntryForEviction(int64 subgraph_uid);

  // Manually discards a reference to the compiled subgraph. This should only be
  // called if per_step_ref_holder was nullptr in the corresponding call to
  // CompileIfKeyAbsent(subgraph_key, ...).
  Status Release(int64 subgraph_uid);

  // Looks up an executable corresponding to the model-parallel core index of
  // the subgraph represented by key. On success a pointer to an EntryRef
  // holding the program is returned in entry.
  Status Lookup(const string& proto_key,
                std::unique_ptr<CompilationCacheEntryRef>* entry);

  // Looks up an executable corresponding to the model-parallel core index of
  // the subgraph represented by uid. On success a pointer to an EntryRef
  // holding the program is returned in entry.
  Status Lookup(int64 uid, int proto_index,
                std::unique_ptr<CompilationCacheEntryRef>* entry);

  // Mutates the main entry ref to point to the entry's subentry
  // (for sharding/unsharding) or main entry (unchanged) representing the
  // fetch target. The entry ref needs to point to the main entry before this
  // call.
  //
  // If the requested subentry does not exist, the ref will point to a nullptr
  // entry.
  Status ToSubEntryRef(CompilationCacheEntryRef* entry,
                       CompilationCacheFetchTarget fetch_target) const;

 private:
  // Wrapper for a cache entry that holds a reference to the entry until the
  // wrapper is deleted. This wrapper is the concrete type of
  // CompilationCacheEntryRef returned by Lookup.
  class TpuEntryRefImpl : public CompilationCacheEntryRef {
   public:
    TpuEntryRefImpl(TpuCompilationCacheExternal* parent,
                    CompiledSubgraph* entry, int index);
    ~TpuEntryRefImpl() override;

    TpuCompilationCacheEntry get() override;

    // Mutates this ref to point to the entry's subentry (for
    // sharding/unsharding) or main entry (unchanged) as specified by
    // fetch_target. The refcount is kept unchanged, since we only track the
    // refcount of the main entry. The entry ref needs to point to the main
    // entry before this call.
    //
    // If the requested subentry does not exist, the ref will point to a nullptr
    // entry, and the original entry will be unref'ed.
    Status ToSubEntryRef(CompilationCacheFetchTarget fetch_target);

   private:
    TpuCompilationCacheExternal* parent_;  // Not owned.
    // A reference to entry_ is acquired in the constructor and released via
    // parent->DiscardEntryRefs in the destructor.
    CompiledSubgraph* entry_;
    // The program in entry_ that is returned by the get method.
    int index_;
  };

  // Private implementation of the generic CompilationRefHolder that knows about
  // CompiledSubgraph entries.
  class RefHolder : public TpuCompilationRefHolder {
   public:
    explicit RefHolder(TpuCompilationCacheExternal* parent) : parent_(parent) {
      parent_->Ref();
    }
    ~RefHolder() override {
      // Release our reference to the parent.
      parent_->Unref();
    }

    // Adds entry to the list of entries that will be released when the
    // RefHolder is destroyed. Each entry is released via a call to
    // parent_->DiscardEntryRefs.
    void AddRef(CompiledSubgraph* entry) { entries_.push_back(entry); }

    string DebugString() const override {
      return "TpuCompilationCacheExternal::RefHolder";
    }

   private:
    TpuCompilationCacheExternal* parent_;  // Not owned.
    std::vector<CompiledSubgraph*> entries_;
  };

  // The bulk of implementation of CompileIfKeyAbsent() with the exception
  // of unloading programs that corresponds to possibly removed cache
  // entries. The split helps to manage locking since we prefer to perform
  // unloading without holding extra locks.
  Status CompileIfKeyAbsentHelper(
      const TpuCompilationCacheKey& subgraph_key,
      const SessionMetadata* session_metadata,
      TpuCompilationRefHolder* per_step_ref_holder, int64* uid,
      std::vector<string>* proto_key, std::vector<bool>* may_modify_variables,
      std::vector<CompiledSubgraph*>* removed_entries,
      std::vector<std::shared_ptr<const xla::HloProto>>* hlo_metadata,
      const std::function<Status(TpuProgramGroup*)>& compile_function);

  // This is called by the cache when entry is marked for eviction; by
  // a RefHolder (via DiscardEntryRefs) when a step completes; and by
  // an EntryRefImpl when it is destroyed. Releases one reference to entry
  // if more than 1 remains. If only one reference is left, the entry is removed
  // from cache_ and is returned to the caller; which must eventually call
  // UnloadAndDestroy(). We do not call UnloadAndDestroy within DiscardEntryRef
  // to avoid holding the lock during program unloading.
  ABSL_MUST_USE_RESULT CompiledSubgraph* DiscardEntryRef(
      CompiledSubgraph* entry) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  // Convenience method called by ~RefHolder without mu_ held. Calls
  // DiscardEntryRef on every element of entries.
  void DiscardEntryRefs(gtl::ArraySlice<CompiledSubgraph*> entries);

  // Marks the oldest unmarked entry for eviction. Requires that there is at
  // least one such entry. In case the evicted entry had only 1 reference it
  // is removed from the cache and returned to the caller which must eventually
  // call UnloadAndDestroy.
  CompiledSubgraph* MarkOldestEntryForEviction()
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Updates datastructures to indicate that entry, which had been marked for
  // eviction, has been looked up. This is called by CompileIfKeyAbsent when an
  // entry is newly created, or an entry that has been marked for eviction but
  // not yet evicted is looked up.
  //
  // First the entry is unmarked for eviction, i.e. the cache gains a reference
  // to entry, entry's last_use field is set to be the most recent value of
  // use_counter_ and entries_by_last_use_ is updated accordingly.
  //
  // Next, the size of the cache is examined to see if any other entries need to
  // be marked for eviction now that entry has been unmarked. While the total
  // size of unmarked cached entries is greater than max_cache_size_, entries
  // are marked for eviction in LRU order. The most recently used entry is never
  // marked for eviction, so an entry larger than the max cache size will remain
  // in the cache until it is replaced by something else. In case some entries
  // actually were removed from the cache, they are a returned to the caller via
  // removed_entries. The caller must eventually delete them by calling
  // UnloadAndDestroy.
  void LookupEntryMarkedForEviction(
      CompiledSubgraph* entry, std::vector<CompiledSubgraph*>* removed_entries)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Removes the entry with given key from cache.
  size_t RemoveEntry(const string& key) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Inserts the given key and entry to cache.
  void InsertEntry(const std::string& key,
                   const TpuCompilationCacheKey& subgraph_key,
                   CompiledSubgraph* entry) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Returns the cache key matching given subgraph_key.
  std::string FindCacheKey(const TpuCompilationCacheKey& subgraph_key) const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Creates a new entry by running initialize_programs and places it in the
  // cache to be looked up by key. The new entry is in the 'marked for eviction'
  // state (not present in entries_by_last_use_) and the caller is expected to
  // call LookupEntryMarkedForEviction after InitializeEntry.
  //
  // **InitializeEntry releases mu_ during the call to initialize_programs.**
  CompiledSubgraph* InitializeEntry(
      const string& key,
      const std::function<Status(TpuProgramGroup*)>& initialize_program,
      const TpuCompilationCacheKey& subgraph_key)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Unloads the program associated with the entry from all local devices
  // and deletes the entry itself. It is assumed no one else has a reference
  // to it and all related keys had already been removed from the cache.
  // The call can perform device IO so no locks should be held while calling it.
  void UnloadAndDestroy(CompiledSubgraph* entry) ABSL_LOCKS_EXCLUDED(mu_);

  // The maximum size of entries that are stored in the cache before entries are
  // marked for eviction.
  const int64 max_cache_size_;

  mutable absl::Mutex mu_;
  // The total size of entries that are stored and not marked for eviction.
  int64 cache_size_ ABSL_GUARDED_BY(mu_) = 0;

  // The total size of entries that are marked for eviction.
  int64 marked_for_eviction_size_ ABSL_GUARDED_BY(mu_) = 0;

  // The value to assign to the last_use field of the next entry that is looked
  // up.
  int64 use_counter_ ABSL_GUARDED_BY(mu_) = 0;

  // session_key_map_ and fingerprint_key_map_ are used for looking up the
  // cache_ key matching a given subgraph key. When doing a lookup, check
  // session_key_map_ first to avoid unnecessay fingerprint computation.
  // Map from key prefix + session_handle to a cache_ key.
  std::unordered_map<string, string> session_key_map_ ABSL_GUARDED_BY(mu_);

  // Map from key prefix + fingerprint to a cache_ key.
  std::unordered_map<string, string> fingerprint_key_map_ ABSL_GUARDED_BY(mu_);

  // All the subgraph entries that can be looked up in the cache. An entry is
  // marked for eviction iff it is present in cache_ and not in
  // entries_by_last_use_.
  std::unordered_map<string, CompiledSubgraph*> cache_store_
      ABSL_GUARDED_BY(mu_);

  // All the subgraph entries that can be looked up in the cache, indexed by
  // uid.
  absl::node_hash_map<int64, CompiledSubgraph*> entries_by_uid_
      ABSL_GUARDED_BY(mu_);

  // All the protos that can be looked up in the cache, indexed by proto
  // key. The value of the map is a subgraph and the index of the proto compiled
  // for that subgraph.
  std::unordered_map<string, std::pair<CompiledSubgraph*, int>>
      entries_by_proto_key_ ABSL_GUARDED_BY(mu_);

  // Map from last_use to entry, used to mark entries for eviction in LRU
  // order. If an entry's last_use counter is not present as a key in
  // entries_by_last_use_ then the entry has been marked for eviction.
  std::map<int64, CompiledSubgraph*> entries_by_last_use_ ABSL_GUARDED_BY(mu_);
};

}  // namespace tpu
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_TPU_KERNELS_TPU_COMPILATION_CACHE_EXTERNAL_H_
