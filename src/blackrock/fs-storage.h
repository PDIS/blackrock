// Sandstorm Blackrock
// Copyright (c) 2015 Sandstorm Development Group, Inc.
// All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BLACKROCK_VOLUME_H_
#define BLACKROCK_VOLUME_H_

#include "common.h"
#include <blackrock/storage.capnp.h>
#include <blackrock/fs-storage.capnp.h>
#include <kj/io.h>
#include <sodium/utils.h>

namespace kj {
  class UnixEventPort;
  class Timer;
}

namespace blackrock {

class FilesystemStorage: public StorageRootSet::Server {
public:
  FilesystemStorage(int directoryFd, kj::UnixEventPort& eventPort, kj::Timer& timer,
                    Restorer<SturdyRef>::Client&& restorer);
  ~FilesystemStorage() noexcept(false);

protected:
  kj::Promise<void> set(SetContext context) override;
  kj::Promise<void> get(GetContext context) override;
  kj::Promise<void> tryGet(TryGetContext context) override;
  kj::Promise<void> getOrCreateAssignable(GetOrCreateAssignableContext context) override;
  kj::Promise<void> remove(RemoveContext context) override;
  kj::Promise<void> getFactory(GetFactoryContext context) override;

public:
  struct ObjectKey {
    uint64_t key[4];

    ObjectKey() = default;
    ObjectKey(StoredObjectKey::Reader reader)
        : key { reader.getKey0(), reader.getKey1(), reader.getKey2(), reader.getKey3() } {}
    ~ObjectKey() {
      sodium_memzero(key, sizeof(key));
    }

    static ObjectKey generate();

    inline void copyTo(StoredObjectKey::Builder builder) const {
      builder.setKey0(key[0]);
      builder.setKey1(key[1]);
      builder.setKey2(key[2]);
      builder.setKey3(key[3]);
    }
  };

  struct ObjectId {
    uint64_t id[2];
    // The object ID. Equals the 16-byte blake2b hash of the key.

    ObjectId() = default;
    ObjectId(decltype(nullptr)): id {0, 0} {}
    ObjectId(StoredObjectId::Reader reader)
        : id { reader.getId0(), reader.getId1() } {}
    ObjectId(const ObjectKey& key);

    inline bool operator==(const ObjectId& other) const {
      return ((id[0] ^ other.id[0]) | (id[1] ^ other.id[1])) == 0;  // constant-time
    }
    inline bool operator!=(const ObjectId& other) const {
      return !operator==(other);
    }
    inline bool operator==(decltype(nullptr)) const {
      return (id[0] | id[1]) == 0;
    }
    inline bool operator!=(decltype(nullptr)) const {
      return !operator==(nullptr);
    }

    inline void copyTo(StoredObjectId::Builder builder) const {
      builder.setId0(id[0]);
      builder.setId1(id[1]);
    }

    struct Hash {
      inline size_t operator()(const ObjectId& id) const { return id.id[0]; }
    };

    kj::FixedArray<char, 24> filename(char prefix) const;
  };

private:
  class ObjectBase;
  class BlobImpl;
  class VolumeImpl;
  class ImmutableImpl;
  class AssignableImpl;
  class CollectionImpl;
  class OpaqueImpl;
  class StorageFactoryImpl;
  enum class Type: uint8_t;
  struct Xattr;
  class Journal;
  class DeathRow;
  class ObjectFactory;

  kj::AutoCloseFd mainDirFd;
  kj::AutoCloseFd stagingDirFd;
  kj::AutoCloseFd deathRowFd;
  kj::AutoCloseFd rootsFd;

  kj::Own<DeathRow> deathRow;
  kj::Own<Journal> journal;
  kj::Own<ObjectFactory> factory;

  kj::Promise<void> setImpl(kj::String name, OwnedStorage<>::Client object);

  kj::Maybe<kj::AutoCloseFd> openObject(ObjectId id);
  kj::Maybe<kj::AutoCloseFd> openStaging(uint64_t number);
  kj::AutoCloseFd createObject(ObjectId id);
  kj::AutoCloseFd createTempFile();
  void linkTempIntoStaging(uint64_t number, int fd, const Xattr& xattr);
  void deleteStaging(uint64_t number);
  void deleteAllStaging();
  void createFromStagingIfExists(uint64_t stagingId, ObjectId finalId, const Xattr& attributes);
  void replaceFromStagingIfExists(uint64_t stagingId, ObjectId finalId, const Xattr& attributes);
  void setAttributesIfExists(ObjectId objectId, const Xattr& attributes);
  void moveToDeathRowIfExists(ObjectId id, bool notify = true);
  void sync();

  static bool isStoredObjectType(Type type);
};

}  // namespace blackrock

#endif  // BLACKROCK_VOLUME_H_
