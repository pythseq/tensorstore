// Copyright 2020 The TensorStore Authors
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

#ifndef TENSORSTORE_DRIVER_KVS_BACKED_CHUNK_DRIVER_H_
#define TENSORSTORE_DRIVER_KVS_BACKED_CHUNK_DRIVER_H_

/// \file
/// Framework for implementing TensorStore drivers for storage formats like
/// zarr, n5, and Neuroglancer precomputed backed by an arbitrary
/// `KeyValueStore`, where there is a KeyValueStore entry for metadata (which
/// may be shared by multiple independent arrays) and one KeyValueStore entry
/// per chunk.

#include <memory>

#include "absl/container/inlined_vector.h"
#include "absl/strings/string_view.h"
#include "tensorstore/box.h"
#include "tensorstore/driver/registry.h"
#include "tensorstore/index.h"
#include "tensorstore/index_space/index_transform.h"
#include "tensorstore/internal/async_storage_backed_cache.h"
#include "tensorstore/internal/cache_pool_resource.h"
#include "tensorstore/internal/chunk_cache.h"
#include "tensorstore/internal/context_binding.h"
#include "tensorstore/internal/data_copy_concurrency_resource.h"
#include "tensorstore/internal/intrusive_ptr.h"
#include "tensorstore/internal/json_bindable.h"
#include "tensorstore/internal/type_traits.h"
#include "tensorstore/kvstore/key_value_store.h"
#include "tensorstore/spec.h"
#include "tensorstore/tensorstore.h"
#include "tensorstore/util/bit_span.h"
#include "tensorstore/util/future.h"
#include "tensorstore/util/result.h"

namespace tensorstore {
namespace internal_kvs_backed_chunk_driver {

/// Base class for specification representations used by drivers, for use with
/// the driver registry.
///
/// This inherits from `DriverConstraints` as required by the driver registry.
template <template <typename> class MaybeBound = internal::ContextUnbound>
struct SpecT : public internal::DriverConstraints {
  MaybeBound<KeyValueStore::Spec::Ptr> store;
  MaybeBound<Context::ResourceSpec<internal::DataCopyConcurrencyResource>>
      data_copy_concurrency;
  MaybeBound<Context::ResourceSpec<internal::CachePoolResource>> cache_pool;
  StalenessBounds staleness;
  bool open;
  bool create;
  bool delete_existing;
  bool allow_metadata_mismatch;

  OpenMode open_mode() const {
    return (open ? OpenMode::open : OpenMode{}) |
           (create ? OpenMode::create : OpenMode{}) |
           (delete_existing ? OpenMode::delete_existing : OpenMode{}) |
           (allow_metadata_mismatch ? OpenMode::allow_option_mismatch
                                    : OpenMode{});
  }

  static constexpr auto ApplyMembers = [](auto& x, auto f) {
    return f(internal::BaseCast<internal::DriverConstraints>(x), x.store,
             x.data_copy_concurrency, x.cache_pool, x.staleness, x.open,
             x.create, x.delete_existing, x.allow_metadata_mismatch);
  };
};

/// JSON binder for the base `SpecT<>` type, must be called by the
/// driver-specific JSON binders.
TENSORSTORE_DECLARE_JSON_BINDER(SpecJsonBinder, SpecT<>,
                                Context::FromJsonOptions,
                                Context::ToJsonOptions,
                                ::nlohmann::json::object_t);

/// Interface by which driver implementations define the metadata cache
/// behavior.
///
/// There is a one-to-one relationship between objects of this type and
/// `MetadataCache` objects.  Typically for a given driver, there will be one
/// `MetadataCache` per underlying `KeyValueStore` with which the driver is
/// used, though there could be more than one if the `MetadataCacheState`
/// depends on some additional parameters.  Entries within the `MetadataCache`
/// correspond to individual paths within the `KeyValueStore` and store the
/// decoded metadata representation.
///
/// Implicitly, instances of this class assume a particular `Metadata` type.
class MetadataCacheState
    : public internal::AtomicReferenceCount<MetadataCacheState> {
 public:
  using Ptr = internal::IntrusivePtr<MetadataCacheState>;
  using MetadataPtr = std::shared_ptr<const void>;

  /// Returns the KeyValueStore key from which to read the encoded metadata
  /// for a given metadata cache entry.
  ///
  /// Typically, this appends a suffix like "/.zarr".
  virtual std::string GetMetadataStorageKey(absl::string_view entry_key) = 0;

  /// Decodes metadata read from `KeyValueStore`.
  ///
  /// \param entry_key The metadata cache entry key (not the KeyValueStore key)
  ///     with which this metadata is associated.
  /// \param encoded_metadata The encoded metadata read from the
  ///     `KeyValueStore`.
  /// \returns On success, non-null pointer to `Metadata` object.
  virtual Result<MetadataPtr> DecodeMetadata(
      absl::string_view entry_key, absl::string_view encoded_metadata) = 0;

  /// Encodes metadata for storage in the `KeyValueStore`.
  ///
  /// \param entry_key The metadata cache entry key (not the KeyValueStore key)
  ///     with which this metadata is associated.
  /// \param metadata Non-null pointer to the metadata to encode, of type
  ///     `Metadata`.
  virtual std::string EncodeMetadata(absl::string_view entry_key,
                                     const void* metadata) = 0;

  virtual ~MetadataCacheState();
};

/// Interface by which driver implementations define the data cache behavior.
///
/// There is a one-to-one relationship between objects of this type and
/// `DataCache` objects.  `DataCache` inherits from `ChunkCache` and represents
/// one or more chunked arrays that are stored within the same set of chunks.
///
/// Implicitly, instances of this class assume a particular `Metadata` type.
class DataCacheState : public internal::AtomicReferenceCount<DataCacheState> {
 public:
  using Ptr = internal::IntrusivePtr<DataCacheState>;

  /// Returns the chunk grid specification for `metadata`.
  ///
  /// This function is called once upon construction of the `ChunkCache`.
  virtual internal::ChunkGridSpecification GetChunkGridSpecification(
      const void* metadata) = 0;

  virtual std::string GetChunkStorageKey(const void* metadata,
                                         span<const Index> cell_indices) = 0;

  /// Fills `bounds`, `implicit_lower_bounds`, and `implicit_upper_bounds` with
  /// the current bounds for the chunked dimensions as specified in `metadata`.
  ///
  /// Resizable lower/upper bound should be marked implicit (i.e. a value of
  /// `true` in `implicit_lower_bounds` or `implicit_upper_bounds`).
  ///
  /// \param metadata Non-null pointer to metadata of type `Metadata`.
  /// \param bounds[out] Box of rank equal to `grid_rank`, where
  ///     `grid_rank = GetChunkGridSpecification(metadata).grid_rank()`.
  /// \param implicit_lower_bounds[out] Bit vector of length `bounds.rank()`.
  /// \param implicit_upper_bounds[out] Bit vector of length `bounds.rank()`.
  virtual void GetChunkGridBounds(
      const void* metadata, MutableBoxView<> bounds,
      BitSpan<std::uint64_t> implicit_lower_bounds,
      BitSpan<std::uint64_t> implicit_upper_bounds) = 0;

  /// Sets `*spec` with the bound spec data associated with the specified
  /// component.
  ///
  /// \param spec Non-null pointer to the derived bound Spec type for the
  ///     driver.  The implementation must `static_cast` this pointer to the
  ///     appropriate derived type.
  /// \param metadata Non-null pointer to metadata of type `Metadata`.
  /// \param component_index The component index.
  virtual Status GetBoundSpecData(SpecT<internal::ContextBound>* spec,
                                  const void* metadata,
                                  std::size_t component_index) = 0;

  /// Returns a non-null pointer to a copy of `existing_metadata` with the
  /// specified bounds resized.
  ///
  /// \param existing_metadata Non-null pointer to existing metadata of type
  ///     `Metadata`.
  /// \param new_inclusive_min Specifies the new inclusive lower bounds.  A
  ///     value of `kImplicit` indicates no change.
  /// \param new_exclusive_max Specifies the new exclusive upper bounds, of
  ///     length `grid_rank`.  A value of `kImplicit` indicates no change.
  /// \pre `new_inclusive_min.size()` and `new_exclusive_max.size()` are equal
  ///     to `GetChunkGridSpecification(existing_metadata).grid_rank()`.
  /// \pre `existing_metadata` is compatible with the initial metadata from
  ///     which this `DataCacheState` was constructed, according to
  ///     `ValidateMetadataCompatibility`.
  virtual Result<std::shared_ptr<const void>> GetResizedMetadata(
      const void* existing_metadata, span<const Index> new_inclusive_min,
      span<const Index> new_exclusive_max) = 0;

  /// Validates that `new_metadata` is compatible with `existing_metadata` for
  /// the purposes of this data cache.
  ///
  /// The `new_metadata` should be considered compatible if chunks encoded using
  /// `existing_metadata` are compatible with chunks encoded using
  /// `new_metadata`.  Changes in the bound of resizable (chunked) dimensions
  /// should generally be considered compatible.
  ///
  /// If this function returns success,
  /// `GetChunkGridSpecification(existing_metadata)` must equal
  /// `GetChunkGridSpecification(new_metadata)`.
  ///
  /// \param existing_metadata Non-null pointer to existing metadata, of type
  ///     `Metadata`.
  /// \param existing_metadata Non-null pointer to new metadata, of type
  ///     `Metadata`.
  /// \returns `Status()` if compatible.
  /// \error `absl::StatusCode::kFailedPrecondition` if not compatible.
  virtual Status ValidateMetadataCompatibility(const void* existing_metadata,
                                               const void* new_metadata) = 0;

  /// Returns a transform from the "external" index space visible in the `Spec`
  /// to the index space of component `component_index` in the `ChunkCache`.
  ///
  /// For example, the returned transform may apply a translation to account for
  /// an offset specified in the `metadata`.
  ///
  /// An identity transform may be indicated by returning a null transform
  /// (which is what is returned by default if this method is not overridden).
  ///
  /// Any implicit bounds in the domain of the returned transform are inferred
  /// from the `ChunkGridSpecification`.
  ///
  /// \param metadata The metadata.
  /// \param component_index The component index.
  /// \returns The index transform, or a null transform.
  /// \pre `component_index` is less than
  ///     `GetChunkGridSpecification(metadata).component.size()`.
  /// \post The returned transform, if not null, must have an input and output
  ///     rank equal to `component_spec.rank()`, where `component_spec` is
  ///     `GetChunkGridSpecification(metadata).components[component_index]`, and
  ///     must be invertible.
  virtual Result<IndexTransform<>> GetExternalToInternalTransform(
      const void* metadata, std::size_t component_index);

  /// Decodes a data chunk.
  ///
  /// \param metadata The metadata (which may determine the decoding).
  /// \param data The encoded chunk data.
  /// \returns On success, returns a decoded array for each component.  The
  ///     shape of each decoded array `i` must equal
  ///     `grid.components[i].cell_shape()`, where
  ///     `grid = GetChunkGridSpecification(metadata)`.
  virtual Result<absl::InlinedVector<SharedArrayView<const void>, 1>>
  DecodeChunk(const void* metadata, span<const Index> chunk_indices,
              std::string data) = 0;

  /// Encodes a data chunk.
  ///
  /// \param metadata The metadata (which may determine the encoding).
  /// \param component_arrays Chunk data for each component.
  /// \pre `component_arrays[i].shape() == grid.components[i].cell_shape()`,
  ///     where `grid = GetChunkGridSpecification(metadata)`.
  virtual Status EncodeChunk(const void* metadata,
                             span<const Index> chunk_indices,
                             span<const ArrayView<const void>> component_arrays,
                             std::string* encoded) = 0;

  virtual ~DataCacheState();
};

/// Specifies constraints for atomic metadata updates.
enum AtomicUpdateConstraint {
  /// No additional constraints.  The update function may be called without
  /// any prior attempt to read the existing metadata (in which case it will
  /// be called with `existing_metadata=nullptr`).
  kNone,

  /// The `update` function is guaranteed to fail if the metadata does not
  /// exist.  It will, however, still be called in such a case to determine
  /// the appropriate error result.
  kRequireExisting,

  /// The `update` function is guaranteed to fail if the metadata already
  /// exists.  It will, however, still be called in such a case to determine
  /// the appropriate error result.  Specifying this option allows
  /// unnecessary re-reads to be avoided.
  kRequireMissing,
};

/// Private data members of `OpenState`.
struct PrivateOpenState {
  internal::RegisteredDriverOpener<SpecT<internal::ContextBound>> spec_;
  ReadWriteMode read_write_mode_;
  KeyValueStore::Ptr store_;
  std::string metadata_cache_key_;
  /// Pointer to `MetadataCache::Entry`, but upcast to type
  /// `internal::AsyncStorageBackedCache::Entry` to avoid having to define
  /// `MetadataCache` in this header.
  internal::PinnedCacheEntry<internal::AsyncStorageBackedCache>
      metadata_cache_entry_;
  /// Time at which open request was initiated.
  absl::Time request_time_;
};

class DataCache;

/// Base class of `RegisteredKvsDriver<Derived>` that defines methods that don't
/// depend on the `Derived` class type.
class DriverBase : public internal::ChunkCacheDriver {
 public:
  struct Initializer {
    internal::CachePtr<DataCache> cache;
    std::size_t component_index;
    StalenessBounds staleness_bounds;
  };
  explicit DriverBase(Initializer&& initializer);

  /// Forwards to `ResolveBound` overload below with
  /// `metadata_staleness_bound_`.
  Future<IndexTransform<>> ResolveBounds(IndexTransform<> transform,
                                         ResolveBoundsOptions options) override;

  Future<IndexTransform<>> ResolveBounds(
      IndexTransform<> transform, StalenessBound metadata_staleness_bound,
      ResolveBoundsOptions options);

  Future<IndexTransform<>> Resize(IndexTransform<> transform,
                                  span<const Index> inclusive_min,
                                  span<const Index> exclusive_max,
                                  ResizeOptions options) override;

  DataCache* cache() const;

  Executor data_copy_executor() override;

  const StalenessBound& metadata_staleness_bound() const {
    return metadata_staleness_bound_;
  }

  Result<IndexTransformSpec> GetBoundSpecData(
      SpecT<internal::ContextBound>* spec, IndexTransformView<> transform);

  static Status ConvertSpec(SpecT<internal::ContextUnbound>* spec,
                            const SpecRequestOptions& options);

 private:
  StalenessBound metadata_staleness_bound_;
};

/// Interface by which driver implementations define the open behavior.
///
/// An object of this type is created for each open request.
///
/// Implicitly, instances of this class are associated with a particular
/// `Metadata` type.
///
/// Driver implementations should inherit from
/// `RegisteredKvsDriver<Derived>::OpenStateBase`, rather than this class
/// directly.
class OpenState : private PrivateOpenState {
 public:
  using Ptr = std::unique_ptr<OpenState>;

  struct Initializer {
    internal::RegisteredDriverOpener<SpecT<internal::ContextBound>> spec;
    ReadWriteMode read_write_mode;
  };

  explicit OpenState(Initializer initializer);
  virtual ~OpenState();

  /// Returns the prefix to delete when `OpenMode::delete_existing` is
  /// specified.
  ///
  /// Typically this appends "/" to the key prefix.
  virtual std::string GetPrefixForDeleteExisting() = 0;

  /// Returns the metadata cache entry key to use to find the metadata.
  ///
  /// Typically this is equal to the key prefix.
  virtual std::string GetMetadataCacheEntryKey() = 0;

  /// Returns the constraint that applies to the `Create` method.
  ///
  /// By default, returns `kRequireMissing`.
  virtual AtomicUpdateConstraint GetCreateConstraint();

  /// Returns the metadata with a new array created.
  ///
  /// The behavior must be consistent with the constraint returned by
  /// `GetCreateConstraint()`.
  ///
  /// \param existing_metadata Pointer to the existing metadata of type
  ///     `Metadata`, or `nullptr` if there is no existing metadata.
  /// \error `absl::StatusCode::kAlreadyExists` if create failed because the
  ///     array may already exist.
  virtual Result<std::shared_ptr<const void>> Create(
      const void* existing_metadata) = 0;

  /// Returns a unique identifier (for a given value of `typeid(*this)`) of the
  /// state returned by `GetMetadataCacheState`.
  ///
  /// By default, returns the empty string.
  virtual std::string GetMetadataCacheKey();

  /// Returns a non-null pointer to a `MetadataCacheState` object associated
  /// with the same `Metadata` type as this object.  If there is an existing
  /// metadata cache in `context()` with the same cache key (as returned by
  /// `GetMetadataCacheKey`), it will be used instead and this method will not
  /// be called.
  virtual MetadataCacheState::Ptr GetMetadataCacheState() = 0;

  /// Returns the `KeyValueStore` to use for retrieving the metadata.
  ///
  /// Any parameters of the `OpenState` that affect the returned `KeyValueStore`
  /// must be encoded in the value returned from `GetMetadataCacheKey()`.
  ///
  /// The default implementation simply returns `base_kv_store`.
  virtual Result<KeyValueStore::Ptr> GetMetadataKeyValueStore(
      KeyValueStore::Ptr base_kv_store);

  /// Returns a unique identifier (for a given value of `typeid(*this)`) of the
  /// state returned by `GetDataCacheState`.
  virtual std::string GetDataCacheKey(const void* metadata) = 0;

  /// Returns a non-null pointer to a `DataCacheState` object associated with
  /// the same `Metadata` type as this object.  If there is an existing data
  /// cache in `context()` with the same cache key (as returned by
  /// `GetDataCacheKey`), it will be used instead and this method will not be
  /// called.
  virtual DataCacheState::Ptr GetDataCacheState(const void* metadata) = 0;

  /// Returns the `KeyValueStore` to use for retrieving the data chunks.
  ///
  /// Any parameters of the `OpenState` that affect the returned `KeyValueStore`
  /// must be encoded in the value returned from `GetDataCacheKey(metadata)`.
  ///
  /// The default implementation simply returns `base_kv_store`.
  virtual Result<KeyValueStore::Ptr> GetDataKeyValueStore(
      KeyValueStore::Ptr base_kv_store, const void* metadata);

  /// Returns the component index within the data cache.
  ///
  /// If the `metadata` is not compatible, returns an error.
  virtual Result<std::size_t> GetComponentIndex(const void* metadata,
                                                OpenMode open_mode) = 0;

  /// Returns a mask specifying whether reading and/or writing is supported.
  ///
  /// By default, returns `ReadWriteMode::read_write`.
  virtual ReadWriteMode GetReadWriteMode(const void* metadata);

  /// Returns a newly-allocated object of the appropriate derived `Driver`
  /// class.
  ///
  /// Defined automatically by `RegisteredOpenState`.
  virtual DriverBase* AllocateDriver(DriverBase::Initializer&& initializer) = 0;

  const SpecT<internal::ContextBound>& spec() const { return *spec_; }

  /// Returns the data copy executor.
  const Executor& executor() const {
    return spec_->data_copy_concurrency->executor;
  }

  const Context::Resource<internal::CachePoolResource>& cache_pool() const {
    return spec_->cache_pool;
  }
};

/// Attempts to open a TensorStore with a KeyValueStore-backed chunk driver.
///
/// This is intended to be used within the implementation of the open function
/// for drivers based on this framework.
///
/// Creating/opening a KeyValueStore-backed chunked array proceeds as follows:
///
/// 1. Opens the `KeyValueStore` specified by `open_state.spec().store`.
///
/// 2. If `OpenMode::delete_existing` is specified, deletes all keys starting
///    with `open_state->GetPrefixForDeleteExisting()`.
///
/// 3. If `OpenMode::open` is specified, attempt to read the existing metadata
///    from the metadata cache using an entry key of
///    `open_state->GetMetadataCacheEntryKey()` (the read may be satisfied by
///    the metadata cache).
///
///    - If the read is successful, checks whether the metadata is compatible
///      with the open request by calling `open_state->GetComponentIndex`.
///      If it is compatible, proceeds to step 5.
///
///    - If `OpenMode::create` is specified, and either the existing metadata is
///      not found or `GetComponentIndex` returned an error of
///      `absl::StatusCode::kNotFound`, proceeds to step 4.
///
///    - Otherwise, fails.
///
/// 4. If `OpenMode::create` is specified, attempts to atomically create/update
///    the metadata in `open_state->GetMetadataCacheEntryKey()` by calling
///    `open_state->Create` (possibly more than once if retries are needed as
///    part of the atomic read-modify-write).
///
///    - If the metadata is created/updated successfully, proceeds to step 5.
///
///    - If `open_state->Create` fails with an error of
///      `absl::StatusCode::kAlreadyExists`, then if `OpenMode::open`
///      is specified and the metadata can be read successfully, proceeds to
///      step 5.
///
///    - Otherwise, fails.
///
/// 5. Checks whether the metadata that was read (or retrieved from the cache)
///    is compatible with the open request by calling
///    `open_state->GetComponentIndex`.
///
///    - If it is, either re-uses an existing `DataCache` with a cache key that
///      matches `open_state->GetDataCacheKey`, or creates a new `DataCache`
///      from the state returned by `open_state->GetDataCacheState`.
///
///    - Otherwise, fails.
///
/// 6. Calls `data_cache_state->GetExternalToInternalTransform` to compute the
///    `IndexTransform` to use, where `data_cache_state` is either the existing
///    or newly created `DataCacheState`.
///
/// \param open_state Non-null pointer to open state.
Future<internal::Driver::ReadWriteHandle> OpenDriver(OpenState::Ptr open_state);

/// CRTP base class for KeyValueStore-backed driver implementations.
///
/// `Derived` driver implementations should inherit from this class and define
/// the following members:
///
///     class Derived
///         : public internal_kvs_backed_chunk_driver::RegisteredKvsDriver<
///                      Derived> {
///       using Base = internal_kvs_backed_chunk_driver::RegisteredKvsDriver<
///                        Derived>;
///      public:
///       // Must inherit the constructors.
///       using Base::Base;
///
///       // Specifies the driver identifier.
///       constexpr static char id[] = "...";
///
///
///       // Defines the specification used to open the driver.
///       template <template <typename>
///                 class MaybeBound = internal::ContextUnbound>
///       struct SpecT
///         : public internal_kvs_backed_chunk_driver::SpecT<MaybeBound> {
///         // ...
///         constexpr static auto ApplyMembers = [](auto& x, auto f) {
///           return f(
///               internal::BaseCast<
///                   internal_kvs_backed_chunk_driver::SpecT<MaybeBound>>(
///                   x),
///               ...);
///         };
///       };
///
///       // Defines the `OpenState` class used to open the driver.  It must
///       // inherit from `Base::OpenStateBase`, inherit its constructors, and
///       // implement all of the required virtual methods of
///       // `internal_kvs_backed_chunk_driver::OpenState` defined above.  The
///       // `spec()` method defined by `Base::OpenState` may be used to access
///       // the bound spec data of type `SpecT<ContextUnbound>`.
///       class OpenState : public Base::OpenStateBase {
///        public:
///         using Base::OpenStateBase::OpenStateBase;
///
///         // ...
///       };
///
///       // Defines the JSON binder for `SpecT<>`.
///       static inline const auto json_binder = jb::Sequence(
///           internal_kvs_backed_chunk_driver::SpecJsonBinder,
///           ...);
///
///
///       // Applies the specified options in place to `*spec`.
///       static Status ConvertSpec(SpecT<>* spec,
///                                 const SpecRequestOptions& options);
///     };
///
template <typename Derived>
class RegisteredKvsDriver
    : public internal::RegisteredDriver<Derived, DriverBase> {
  using Base = internal::RegisteredDriver<Derived, DriverBase>;

 public:
  using Base::Base;

  /// CRTP base class for the OpenState associated with KeyValueStore-backed
  /// driver implementations.
  class OpenStateBase : public internal_kvs_backed_chunk_driver::OpenState {
   public:
    using internal_kvs_backed_chunk_driver::OpenState::OpenState;

    /// Returns a reference to the bound spec data of type
    /// `Derived::SpecT<ContextBound>` used to open the driver.
    ///
    /// This is intended to be called by the derived class to implement the
    /// `OpenState` interface.
    decltype(auto) spec() const {
      using SpecData = typename Derived::template SpecT<internal::ContextBound>;
      return static_cast<const SpecData&>(
          internal_kvs_backed_chunk_driver::OpenState::spec());
    }

    /// Returns a newly allocated object of the `Derived` driver type, as
    /// required by `internal_kvs_backed_chunk_driver::Driver`.
    DriverBase* AllocateDriver(DriverBase::Initializer&& initializer) override {
      return new Derived(std::move(initializer));
    }
  };

  /// Implements the `Open` method required by `internal::RegisteredDriver` in
  /// terms of `internal_kvs_backed_chunk_driver::OpenDriver`.
  template <typename Spec>
  static Future<internal::Driver::ReadWriteHandle> Open(
      internal::RegisteredDriverOpener<Spec> spec,
      ReadWriteMode read_write_mode) {
    // We have to use a template parameter because `Derived` is incomplete when
    // `RegisteredKvsDriver` is instantiated.
    static_assert(
        std::is_same_v<
            Spec, typename Derived::template SpecT<internal::ContextBound>>);
    return internal_kvs_backed_chunk_driver::OpenDriver(
        std::make_unique<typename Derived::OpenState>(
            internal_kvs_backed_chunk_driver::OpenState::Initializer{
                std::move(spec), read_write_mode}));
  }
};

}  // namespace internal_kvs_backed_chunk_driver
}  // namespace tensorstore

#endif  // TENSORSTORE_DRIVER_KVS_BACKED_CHUNK_DRIVER_H_
