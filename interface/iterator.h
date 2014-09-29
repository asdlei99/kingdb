// Copyright (c) 2014, Emmanuel Goossaert. All rights reserved.
// Use of this source code is governed by the BSD 3-Clause License,
// that can be found in the LICENSE file.

#ifndef KINGDB_ITERATOR_MAIN_H_
#define KINGDB_ITERATOR_MAIN_H_

#include <string>

#include "util/status.h"
#include "kingdb/common.h"
#include "kingdb/byte_array.h"
#include "kingdb/options.h"
#include "storage/storage_engine.h"

namespace kdb {

class Iterator {
 public:
  Iterator(ReadOptions& read_options,
           StorageEngine *se_readonly,
           std::vector<uint32_t>* fileids_iterator)
      : se_readonly_(se_readonly),
        read_options_(read_options),
        fileids_iterator_(fileids_iterator) {
    LOG_TRACE("KingDB::NewIterator()", "start");
    //Begin();
  }

  ~Iterator() {
  }

  void Begin() {
    LOG_TRACE("Iterator::Begin()", "start");
    mutex_.lock();
    fileid_current_ = 0;
    has_file_ = false;
    index_fileid_ = 0;
    is_valid_ = true;
    key_ = nullptr;
    value_ = nullptr;
    mutex_.unlock();
    Next();
    LOG_TRACE("Iterator::Begin()", "end");
  }

  bool IsValid() {
    LOG_TRACE("Iterator::IsValid()", "start");
    std::unique_lock<std::mutex> lock(mutex_);
    LOG_TRACE("Iterator::IsValid()", "end");
    return is_valid_;
  }

  bool Next() {
    LOG_TRACE("Iterator::Next()", "start");
    std::unique_lock<std::mutex> lock(mutex_);
    if (!is_valid_) return false;
    Status s;

    while (1) { 
      if (key_ != nullptr) {
        delete key_;
        delete value_;
        key_ = nullptr;
        value_ = nullptr;
      }
      LOG_TRACE("Iterator::Next()", "loop index_file:[%u] index_location:[%u]", index_fileid_, index_location_);
      if (index_fileid_ >= fileids_iterator_->size()) {
        is_valid_ = false;
        break;
      }

      if (!has_file_) {
        LOG_TRACE("Iterator::Next()", "initialize file");
        fileid_current_ = fileids_iterator_->at(index_fileid_);
        filepath_current_ = se_readonly_->GetFilepath(fileid_current_);
        struct stat info;
        if (stat(filepath_current_.c_str(), &info) != 0) {
          index_fileid_ += 1;
          continue;
        }
        Mmap mmap(filepath_current_.c_str(), info.st_size);
        uint64_t dummy_filesize;
        bool dummy_is_file_large;
        std::multimap<uint64_t, uint64_t> index_temp;
        s = LogfileManager::LoadFile(mmap,
                                     fileid_current_,
                                     index_temp,
                                     &dummy_filesize,
                                     &dummy_is_file_large);
        if (!s.IsOK()) {
          index_fileid_ += 1;
          continue;
        }
        locations_current_.clear();
        for (auto& p: index_temp) {
          locations_current_.push_back(p.second);
        }
        std::sort(locations_current_.begin(), locations_current_.end());
        index_location_ = 0;
        key_ = nullptr;
        value_ = nullptr;
        has_file_ = true;
      }

      LOG_TRACE("Iterator::Next()", "has file");
      if (index_location_ >= locations_current_.size()) {
        LOG_TRACE("Iterator::Next()", "index_location_ is out");
        has_file_ = false;
        index_fileid_ += 1;
        continue;
      }

      // Get entry at the location
      ByteArray *key = nullptr;
      ByteArray *value = nullptr;
      uint64_t location_current = locations_current_[index_location_];
      Status s = se_readonly_->GetEntry(location_current, &key, &value);
      if (!s.IsOK()) {
        LOG_TRACE("Iterator::Next()", "GetEntry() failed");
        delete key; 
        delete value;
        index_location_ += 1;
        continue;
      }

      // Get entry for the key found at the location, and continue if the
      // locations mismatch -- i.e. the current entry has been overwritten
      // by a later entry.
      ByteArray *value_alt = nullptr;
      uint64_t location_out;
      s = se_readonly_->Get(key, &value_alt, &location_out);
      if (!s.IsOK()) {
        LOG_TRACE("Iterator::Next()", "Get(): failed");
        delete key;
        delete value;
        delete value_alt;
        index_fileid_ += 1;
        continue;
      }
        
      if (location_current != location_out) {
        LOG_TRACE("Iterator::Next()", "Get(): wrong location");
        delete key;
        delete value;
        delete value_alt;
        index_location_ += 1;
        continue;
      }

      LOG_TRACE("Iterator::Next()", "has a valid key/value pair");
      key_ = key;
      value_ = value;
      delete value_alt;
      index_location_ += 1;
      return true;
    }

    return false;
  }

  ByteArray *GetKey() {
    return key_;
  }

  ByteArray *GetValue() {
    return value_;
  }

 private:
  std::mutex mutex_;
  uint32_t fileid_current_;
  std::string filepath_current_;
  uint32_t index_fileid_;
  std::vector<uint32_t>* fileids_iterator_;
  uint32_t index_location_;
  std::vector<uint64_t> locations_current_;
  StorageEngine *se_readonly_;
  ReadOptions read_options_;
  bool has_file_;
  bool is_valid_;

  ByteArray* key_;
  ByteArray* value_;
};

} // end namespace kdb

#endif // KINGDB_ITERATOR_MAIN_H_