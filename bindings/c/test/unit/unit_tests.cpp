/*
 * unit_tests.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2020 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Unit tests for the FoundationDB C API.

#define FDB_API_VERSION 700
#include <foundationdb/fdb_c.h>
#include <assert.h>
#include <string.h>

#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include "fdb_api.hpp"

void fdb_check(fdb_error_t e) {
  if (e) {
    std::cerr << fdb_get_error(e) << std::endl;
    std::abort();
  }
}

FDBDatabase *fdb_open_database(const char *clusterFile) {
  FDBDatabase *db;
  fdb_check(fdb_create_database(clusterFile, &db));
  return db;
}

static FDBDatabase *db = nullptr;
static std::string prefix;
static std::string clusterFilePath = "";

std::string key(const std::string& key) {
  return prefix + key;
}

// Blocks until the given future is ready, returning an error code if there was
// an issue.
fdb_error_t wait_future(fdb::Future &f) {
  fdb_check(f.block_until_ready());
  return f.get_error();
}

// Given a string s, returns the "lowest" string greater than any string that
// starts with s. Taken from
// https://github.com/apple/foundationdb/blob/e7d72f458c6a985fdfa677ae021f357d6f49945b/flow/flow.cpp#L223.
std::string strinc(const std::string &s) {
  int index = -1;
  for (index = s.size() - 1; index >= 0; --index) {
    if ((uint8_t)s[index] != 255) {
      break;
    }
  }

  assert(index >= 0);

  std::string r = s.substr(0, index + 1);
  char *p = r.data();
  p[r.size() - 1]++;
  return r;
}

TEST_CASE("strinc") {
  CHECK(strinc("a").compare("b") == 0);
  CHECK(strinc("y").compare("z") == 0);
  CHECK(strinc("!").compare("\"") == 0);
  CHECK(strinc("*").compare("+") == 0);
  CHECK(strinc("fdb").compare("fdc") == 0);
  CHECK(strinc("foundation database 6").compare("foundation database 7") == 0);

  char terminated[] = {'a', 'b', '\xff'};
  CHECK(strinc(std::string(terminated, 3)).compare("ac") == 0);
}

// Helper function to add `prefix` to all keys in the given map. Returns a new
// map.
std::map<std::string, std::string>
create_data(std::map<std::string, std::string> &&map) {
  std::map<std::string, std::string> out;
  for (const auto & [ key, val ] : map) {
    out[prefix + key] = val;
  }
  return out;
}

// Clears all data in the database, then inserts the given key value pairs.
void insert_data(FDBDatabase *db,
                 const std::map<std::string, std::string> &data) {
  fdb::Transaction tr(db);
  auto end_key = strinc(prefix);
  while (1) {
    tr.clear_range(prefix, end_key);
    for (const auto & [ key, val ] : data) {
      tr.set(key, val);
    }

    fdb::EmptyFuture f1 = tr.commit();
    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }
}

// Get the value associated with `key_name` from the database. Accepts a list
// of transaction options to apply (values for options not supported). Returns
// an optional which will be populated with the result if one was found.
std::optional<std::string>
get_value(std::string_view key, fdb_bool_t snapshot,
          std::vector<FDBTransactionOption> options) {
  fdb::Transaction tr(db);
  while (1) {
    for (auto &option : options) {
      fdb_check(tr.set_option(option, nullptr, 0));
    }
    fdb::ValueFuture f1 = tr.get(key, snapshot);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));
    return out_present ? std::make_optional(std::string(val, vallen)) : std::nullopt;
  }
}

struct GetRangeResult {
  // List of key-value pairs in the range read.
  std::vector<std::pair<std::string, std::string>> kvs;
  // True if values remain in the key range requested.
  bool more;
  // Set to a non-zero value if an error occurred during the transaction.
  fdb_error_t err;
};

// Helper function to get a range of kv pairs. Returns a GetRangeResult struct
// containing the results of the range read. Caller is responsible for checking
// error on failure and retrying if necessary.
GetRangeResult
get_range(fdb::Transaction& tr, const uint8_t* begin_key_name,
          int begin_key_name_length, fdb_bool_t begin_or_equal,
          int begin_offset, const uint8_t* end_key_name,
          int end_key_name_length, fdb_bool_t end_or_equal, int end_offset,
          int limit, int target_bytes, FDBStreamingMode mode,
          int iteration, fdb_bool_t snapshot, fdb_bool_t reverse) {
  fdb::KeyValueArrayFuture f1 = tr.get_range(
      begin_key_name, begin_key_name_length, begin_or_equal, begin_offset,
      end_key_name, end_key_name_length, end_or_equal, end_offset, limit,
      target_bytes, mode, iteration, snapshot, reverse);

  fdb_error_t err = wait_future(f1);
  if (err) {
    return GetRangeResult{{}, false, err};
  }

  const FDBKeyValue *out_kv;
  int out_count;
  fdb_bool_t out_more;
  fdb_check(f1.get(&out_kv, &out_count, &out_more));

  std::vector<std::pair<std::string, std::string>> results;
  for (int i = 0; i < out_count; ++i) {
    std::string key((const char *)out_kv[i].key, out_kv[i].key_length);
    std::string value((const char *)out_kv[i].value, out_kv[i].value_length);
    results.push_back(std::make_pair(key, value));
  }
  return GetRangeResult{results, out_more != 0, 0};
}

// Clears all data in the database.
void clear_data(FDBDatabase *db) {
  insert_data(db, {});
}

struct FdbEvent {
  void wait() {
    std::unique_lock<std::mutex> l(mutex);
    cv.wait(l, [this]() { return this->complete; });
  }
  void set() {
    std::unique_lock<std::mutex> l(mutex);
    complete = true;
    cv.notify_all();
  }

 private:
  std::mutex mutex;
  std::condition_variable cv;
  bool complete = false;
};

TEST_CASE("fdb_future_set_callback") {
  fdb::Transaction tr(db);
  while (1) {
    fdb::ValueFuture f1 = tr.get("foo", /*snapshot*/ true);

    struct Context {
      FdbEvent event;
    };
    Context context;
    fdb_check(f1.set_callback(
        +[](FDBFuture *, void *param) {
          auto *context = static_cast<Context *>(param);
          context->event.set();
        },
        &context));

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    context.event.wait();
    break;
  }
}

TEST_CASE("fdb_future_cancel after future completion") {
  fdb::Transaction tr(db);
  while (1) {
    fdb::ValueFuture f1 = tr.get("foo", false);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    // Should have no effect
    f1.cancel();

    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));
    break;
  }
}

TEST_CASE("fdb_future_is_ready") {
  fdb::Transaction tr(db);
  while (1) {
    fdb::ValueFuture f1 = tr.get("foo", false);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    CHECK(f1.is_ready());
    break;
  }
}

TEST_CASE("fdb_future_release_memory") {
  fdb::Transaction tr(db);
  while (1) {
    fdb::ValueFuture f1 = tr.get("foo", false);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    // "After [fdb_future_release_memory] has been called the same number of
    // times as fdb_future_get_*(), further calls to fdb_future_get_*() will
    // return a future_released error".
    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));

    f1.release_memory();
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));
    f1.release_memory();
    f1.release_memory();
    err = f1.get(&out_present, (const uint8_t **)&val, &vallen);
    CHECK(err == 1102); // future_released
    break;
  }
}

TEST_CASE("fdb_future_get_int64") {
  fdb::Transaction tr(db);
  while (1) {
    fdb::Int64Future f1 = tr.get_read_version();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int64_t rv;
    fdb_check(f1.get(&rv));
    CHECK(rv > 0);
    break;
  }
}

TEST_CASE("fdb_future_get_key") {
  insert_data(db,
              create_data({ { "a", "1" }, { "baz", "2" }, { "bar", "3" } }));

  fdb::Transaction tr(db);
  while (1) {
    fdb::KeyFuture f1 = tr.get_key(
        FDB_KEYSEL_FIRST_GREATER_THAN(
          (const uint8_t *)key("a").c_str(),
          key("a").size()
        ), /* snapshot */ false);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    const uint8_t *key;
    int keylen;
    fdb_check(f1.get(&key, &keylen));

    std::string dbKey((const char *)key, keylen);
    CHECK(dbKey.compare(prefix + "bar") == 0);
    break;
  }
}

TEST_CASE("fdb_future_get_value") {
  insert_data(db, create_data({ { "foo", "bar" } }));

  fdb::Transaction tr(db);
  while (1) {
    fdb::ValueFuture f1 = tr.get(key("foo"), /* snapshot */ false);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));

    CHECK(out_present);
    std::string dbValue(val, vallen);
    CHECK(dbValue.compare("bar") == 0);
    break;
  }
}

TEST_CASE("fdb_future_get_string_array") {
  insert_data(db, create_data({ { "foo", "bar" } }));

  fdb::Transaction tr(db);
  while (1) {
    fdb::StringArrayFuture f1 = tr.get_addresses_for_key(key("foo"));

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    const char **strings;
    int count;
    fdb_check(f1.get(&strings, &count));

    CHECK(count > 0);
    for (int i = 0; i < count; ++i) {
      CHECK(strlen(strings[i]) > 0);
    }
    break;
  }
}

TEST_CASE("fdb_future_get_keyvalue_array") {
  std::map<std::string, std::string> data =
      create_data({ { "a", "1" }, { "b", "2" }, { "c", "3" }, { "d", "4" } });
  insert_data(db, data);

  fdb::Transaction tr(db);
  while (1) {
    fdb::KeyValueArrayFuture f1 = tr.get_range(
        FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(
          (const uint8_t *)key("a").c_str(),
          key("a").size()
        ),
        FDB_KEYSEL_LAST_LESS_OR_EQUAL(
          (const uint8_t *)key("c").c_str(),
          key("c").size()
        ) + 1, /* limit */ 0, /* target_bytes */ 0,
        /* FDBStreamingMode */ FDB_STREAMING_MODE_WANT_ALL, /* iteration */ 0,
        /* snapshot */ false, /* reverse */ 0);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    FDBKeyValue const *out_kv;
    int out_count;
    int out_more;
    fdb_check(f1.get(&out_kv, &out_count, &out_more));

    CHECK(out_count > 0);
    CHECK(out_count <= 3);
    if (out_count < 3) {
      CHECK(out_more);
    }

    for (int i = 0; i < out_count; ++i) {
      FDBKeyValue kv = *out_kv++;

      std::string key((const char *)kv.key, kv.key_length);
      std::string value((const char *)kv.value, kv.value_length);

      CHECK(data[key].compare(value) == 0);
    }
    break;
  }
}

TEST_CASE("cannot read system key") {
  fdb::Transaction tr(db);

  fdb::ValueFuture f1 = tr.get("\xff/coordinators", /* snapshot */ false);

  fdb_error_t err = wait_future(f1);
  CHECK(err == 2004); // key_outside_legal_range
}

TEST_CASE("read system key") {
  auto value = get_value("\xff/coordinators", /* snapshot */ false,
                         { FDB_TR_OPTION_READ_SYSTEM_KEYS });
  REQUIRE(value.has_value());
}

TEST_CASE("cannot write system key") {
  fdb::Transaction tr(db);

  tr.set("\xff\x02", "bar");

  fdb::EmptyFuture f1 = tr.commit();
  fdb_error_t err = wait_future(f1);
  CHECK(err == 2004); // key_outside_legal_range
}

TEST_CASE("write system key") {
  fdb::Transaction tr(db);

  std::string syskey("\xff\x02");
  fdb_check(tr.set_option(FDB_TR_OPTION_ACCESS_SYSTEM_KEYS, nullptr, 0));
  tr.set(syskey, "bar");

  while (1) {
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }

  auto value = get_value(syskey, /* snapshot */ false,
                         { FDB_TR_OPTION_READ_SYSTEM_KEYS });
  REQUIRE(value.has_value());
  CHECK(value->compare("bar") == 0);
}

TEST_CASE("fdb_transaction read_your_writes") {
  fdb::Transaction tr(db);
  clear_data(db);

  while (1) {
    tr.set("foo", "bar");
    fdb::ValueFuture f1 = tr.get("foo", /*snapshot*/ false);

    // Read before committing, should read the initial write.
    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));

    CHECK(out_present);
    std::string value(val, vallen);
    CHECK(value.compare("bar") == 0);
    break;
  }
}

TEST_CASE("fdb_transaction_set_option read_your_writes_disable") {
  clear_data(db);

  fdb::Transaction tr(db);
  while (1) {
    fdb_check(tr.set_option(FDB_TR_OPTION_READ_YOUR_WRITES_DISABLE, nullptr, 0));
    tr.set("foo", "bar");
    fdb::ValueFuture f1 = tr.get("foo", /*snapshot*/ false);

    // Read before committing, shouldn't read the initial write because
    // read_your_writes is disabled.
    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));

    CHECK(!out_present);
    break;
  }
}

TEST_CASE("fdb_transaction_set_option snapshot_read_your_writes_enable") {
  clear_data(db);

  fdb::Transaction tr(db);
  while (1) {
    // Enable read your writes for snapshot reads.
    fdb_check(tr.set_option(FDB_TR_OPTION_SNAPSHOT_RYW_ENABLE, nullptr, 0));
    tr.set("foo", "bar");
    fdb::ValueFuture f1 = tr.get("foo", /*snapshot*/ true);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));

    CHECK(out_present);
    std::string value(val, vallen);
    CHECK(value.compare("bar") == 0);
    break;
  }
}

TEST_CASE("fdb_transaction_set_option snapshot_read_your_writes_disable") {
  clear_data(db);

  fdb::Transaction tr(db);
  while (1) {
    // Disable read your writes for snapshot reads.
    fdb_check(tr.set_option(FDB_TR_OPTION_SNAPSHOT_RYW_DISABLE, nullptr, 0));
    tr.set("foo", "bar");
    fdb::ValueFuture f1 = tr.get("foo", /*snapshot*/ true);
    fdb::ValueFuture f2 = tr.get("foo", /*snapshot*/ false);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f3 = tr.on_error(err);
      fdb_check(wait_future(f3));
      continue;
    }

    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));

    CHECK(!out_present);

    // Non-snapshot reads should still read writes in the transaction.
    err = wait_future(f2);
    if (err) {
      fdb::EmptyFuture f3 = tr.on_error(err);
      fdb_check(wait_future(f3));
      continue;
    }
    fdb_check(f2.get(&out_present, (const uint8_t **)&val, &vallen));

    CHECK(out_present);
    std::string value(val, vallen);
    CHECK(value.compare("bar") == 0);
    break;
  }
}

TEST_CASE("fdb_transaction_set_option timeout") {
  fdb::Transaction tr(db);
  // Set smallest possible timeout, retry until a timeout occurs.
  int64_t timeout = 1;
  fdb_check(tr.set_option(FDB_TR_OPTION_TIMEOUT, (const uint8_t *)&timeout,
                          sizeof(timeout)));

  fdb_error_t err = 0;
  while (!err) {
    fdb::ValueFuture f1 = tr.get("foo", /* snapshot */ false);
    err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      err = wait_future(f2);
    }
  }
  CHECK(err == 1031); // transaction_timed_out
}

TEST_CASE("FDB_DB_OPTION_TRANSACTION_TIMEOUT") {
  // Set smallest possible timeout, retry until a timeout occurs.
  int64_t timeout = 1;
  fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_TRANSACTION_TIMEOUT,
                                    (const uint8_t *)&timeout,
                                    sizeof(timeout)));

  fdb::Transaction tr(db);
  fdb_error_t err = 0;
  while (!err) {
    fdb::ValueFuture f1 = tr.get("foo", /* snapshot */ false);
    err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      err = wait_future(f2);
    }
  }
  CHECK(err == 1031); // transaction_timed_out

  // Reset transaction timeout (disable timeout).
  timeout = 0;
  fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_TRANSACTION_TIMEOUT,
                                    (const uint8_t *)&timeout,
                                    sizeof(timeout)));
}

TEST_CASE("fdb_transaction_set_option size_limit too small") {
  fdb::Transaction tr(db);

  // Size limit must be at least 32 to be valid, so test a smaller size.
  int64_t size_limit = 31;
  fdb_check(tr.set_option(FDB_TR_OPTION_SIZE_LIMIT,
                          (const uint8_t *)&size_limit, sizeof(size_limit)));
  tr.set("foo", "bar");
  fdb::EmptyFuture f1 = tr.commit();

  CHECK(wait_future(f1) == 2006); // invalid_option_value
}

TEST_CASE("fdb_transaction_set_option size_limit too large") {
  fdb::Transaction tr(db);

  // Size limit must be less than or equal to 10,000,000.
  int64_t size_limit = 10000001;
  fdb_check(tr.set_option(FDB_TR_OPTION_SIZE_LIMIT,
                          (const uint8_t *)&size_limit, sizeof(size_limit)));
  tr.set("foo", "bar");
  fdb::EmptyFuture f1 = tr.commit();

  CHECK(wait_future(f1) == 2006); // invalid_option_value
}

TEST_CASE("fdb_transaction_set_option size_limit") {
  fdb::Transaction tr(db);

  int64_t size_limit = 32;
  fdb_check(tr.set_option(FDB_TR_OPTION_SIZE_LIMIT,
                          (const uint8_t *)&size_limit, sizeof(size_limit)));
  tr.set("foo", "foundation database is amazing");
  fdb::EmptyFuture f1 = tr.commit();

  CHECK(wait_future(f1) == 2101); // transaction_too_large
}

// Setting the transaction size limit as a database option causes issues when
// outside the bounds of acceptable values. TODO: Needs investigating...
// TEST_CASE("FDB_DB_OPTION_TRANSACTION_SIZE_LIMIT too small") {
//   // Size limit must be at least 32 to be valid, so test a smaller size.
//   int64_t size_limit = 31;
//   fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_TRANSACTION_SIZE_LIMIT,
//                                     (const uint8_t *)&size_limit,
//                                     sizeof(size_limit)));
//
//   fdb::Transaction tr(db);
//   tr.set((const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3);
//   fdb::EmptyFuture f1 = tr.commit();
//
//   CHECK(wait_future(f1) == 2006); // invalid_option_value
//
//   // Set size limit back to default.
//   size_limit = 10000000;
//   fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_TRANSACTION_SIZE_LIMIT,
//                                     (const uint8_t *)&size_limit,
//                                     sizeof(size_limit)));
// }

// TEST_CASE("FDB_DB_OPTION_TRANSACTION_SIZE_LIMIT too large") {
//   // Size limit must be less than or equal to 10,000,000.
//   int64_t size_limit = 10000001;
//   fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_TRANSACTION_SIZE_LIMIT,
//                                     (const uint8_t *)&size_limit,
//                                     sizeof(size_limit)));
//
//   fdb::Transaction tr(db);
//   tr.set((const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3);
//   fdb::EmptyFuture f1 = tr.commit();
//
//   CHECK(wait_future(f1) == 2006); // invalid_option_value
//
//   // Set size limit back to default.
//   size_limit = 10000000;
//   fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_TRANSACTION_SIZE_LIMIT,
//                                     (const uint8_t *)&size_limit,
//                                     sizeof(size_limit)));
// }

TEST_CASE("FDB_DB_OPTION_TRANSACTION_SIZE_LIMIT") {
  int64_t size_limit = 32;
  fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_TRANSACTION_SIZE_LIMIT,
                                    (const uint8_t *)&size_limit,
                                    sizeof(size_limit)));

  fdb::Transaction tr(db);
  tr.set("foo", "foundation database is amazing");
  fdb::EmptyFuture f1 = tr.commit();

  CHECK(wait_future(f1) == 2101); // transaction_too_large

  // Set size limit back to default.
  size_limit = 10000000;
  fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_TRANSACTION_SIZE_LIMIT,
                                    (const uint8_t *)&size_limit,
                                    sizeof(size_limit)));
}

TEST_CASE("fdb_transaction_set_read_version old_version") {
  fdb::Transaction tr(db);

  tr.set_read_version(1);
  fdb::ValueFuture f1 = tr.get("foo", /*snapshot*/ true);

  fdb_error_t err = wait_future(f1);
  CHECK(err == 1007); // transaction_too_old
}

TEST_CASE("fdb_transaction_set_read_version future_version") {
  fdb::Transaction tr(db);

  tr.set_read_version(1UL << 62);
  fdb::ValueFuture f1 = tr.get("foo", /*snapshot*/ true);

  fdb_error_t err = wait_future(f1);
  CHECK(err == 1009); // future_version
}

TEST_CASE("fdb_transaction_get_range reverse") {
  std::map<std::string, std::string> data =
      create_data({ { "a", "1" }, { "b", "2" }, { "c", "3" }, { "d", "4" } });
  insert_data(db, data);

  fdb::Transaction tr(db);
  while (1) {
    auto result = get_range(
        tr, FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(
          (const uint8_t*)key("a").c_str(),
          key("a").size()
        ),
        FDB_KEYSEL_LAST_LESS_OR_EQUAL(
          (const uint8_t*)key("d").c_str(),
          key("d").size()
        ) + 1, /* limit */ 0, /* target_bytes */ 0,
        /* FDBStreamingMode */ FDB_STREAMING_MODE_WANT_ALL, /* iteration */ 0,
        /* snapshot */ false, /* reverse */ 1);

    if (result.err) {
      fdb::EmptyFuture f1 = tr.on_error(result.err);
      fdb_check(wait_future(f1));
      continue;
    }

    CHECK(result.kvs.size() > 0);
    CHECK(result.kvs.size() <= 4);
    if (result.kvs.size() < 4) {
      CHECK(result.more);
    }

    // Read data in reverse order.
    auto it = data.rbegin();
    for (auto results_it = result.kvs.begin(); results_it != result.kvs.end(); ++results_it, ++it) {
      std::string data_key = it->first;
      std::string data_value = it->second;

      auto [key, value] = *results_it;

      CHECK(data_key.compare(key) == 0);
      CHECK(data[data_key].compare(value) == 0);
    }
    break;
  }
}

TEST_CASE("fdb_transaction_get_range limit") {
  std::map<std::string, std::string> data =
      create_data({ { "a", "1" }, { "b", "2" }, { "c", "3" }, { "d", "4" } });
  insert_data(db, data);

  fdb::Transaction tr(db);
  while (1) {
    auto result = get_range(
        tr, FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(
          (const uint8_t*)key("a").c_str(),
          key("a").size()
        ),
        FDB_KEYSEL_LAST_LESS_OR_EQUAL(
          (const uint8_t*)key("d").c_str(),
          key("d").size()
        ) + 1, /* limit */ 2, /* target_bytes */ 0,
        /* FDBStreamingMode */ FDB_STREAMING_MODE_WANT_ALL, /* iteration */ 0,
        /* snapshot */ false, /* reverse */ 0);

    if (result.err) {
      fdb::EmptyFuture f1 = tr.on_error(result.err);
      fdb_check(wait_future(f1));
      continue;
    }

    CHECK(result.kvs.size() > 0);
    CHECK(result.kvs.size() <= 2);
    if (result.kvs.size() < 4) {
      CHECK(result.more);
    }

    for (const auto& [ key, value ] : result.kvs) {
      CHECK(data[key].compare(value) == 0);
    }
    break;
  }
}

TEST_CASE("fdb_transaction_get_range FDB_STREAMING_MODE_EXACT") {
  std::map<std::string, std::string> data =
      create_data({ { "a", "1" }, { "b", "2" }, { "c", "3" }, { "d", "4" } });
  insert_data(db, data);

  fdb::Transaction tr(db);
  while (1) {
    auto result = get_range(
        tr, FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(
          (const uint8_t*)key("a").c_str(),
          key("a").size()
        ),
        FDB_KEYSEL_LAST_LESS_OR_EQUAL(
          (const uint8_t*)key("d").c_str(),
          key("d").size()
        ) + 1, /* limit */ 3, /* target_bytes */ 0,
        /* FDBStreamingMode */ FDB_STREAMING_MODE_EXACT, /* iteration */ 0,
        /* snapshot */ false, /* reverse */ 0);

    if (result.err) {
      fdb::EmptyFuture f1 = tr.on_error(result.err);
      fdb_check(wait_future(f1));
      continue;
    }

    CHECK(result.kvs.size() == 3);
    CHECK(result.more);

    for (const auto& [ key, value ] : result.kvs) {
      CHECK(data[key].compare(value) == 0);
    }
    break;
  }
}

TEST_CASE("fdb_transaction_clear") {
  insert_data(db, create_data({ { "foo", "bar" } }));

  fdb::Transaction tr(db);
  while (1) {
    tr.clear(key("foo"));
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }

  auto value = get_value(key("foo"), /* snapshot */ false, {});
  REQUIRE(!value.has_value());
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_ADD") {
  insert_data(db, create_data({ { "foo", "a" } }));

  fdb::Transaction tr(db);
  int8_t param = 1;
  while (1) {
    tr.atomic_op(key("foo"), (const uint8_t *)&param, sizeof(param),
                 FDB_MUTATION_TYPE_ADD);
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }

  auto value = get_value(key("foo"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->size() == 1);
  CHECK(value->data()[0] == 'b'); // incrementing 'a' results in 'b'
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_BIT_AND") {
  // Test bitwise and on values of same length:
  //   db key = foo
  //   db value = 'a' == 97
  //   param = 'b' == 98
  //
  //   'a' == 97 == 0b01100001
  // & 'b' == 98 == 0b01100010
  //   -----------------------
  //                0b01100000 == 96 == '`'
  //
  // Test bitwise and on extended database value:
  //   db key = bar
  //   db value = 'c' == 99
  //   param = "ad"
  //
  //   'c' == 99 == 0b0110001100000000 (zero extended on right to match length of param)
  // & "ad"   ==    0b0110000101100100
  //   -------------------------------
  //                0b0110000100000000 == 'a' followed by null (0)
  //
  // Test bitwise and on truncated database value:
  //   db key = baz
  //   db value = "abc"
  //   param = 'e' == 101
  //
  //   "abc"  ->  0b01100001 (truncated to "a" to match length of param)
  // & 'e' == 101 0b01100101
  //   ---------------------
  //              0b01100001 == 97 == 'a'
  //
  insert_data(db, create_data({ { "foo", "a" }, { "bar", "c" }, { "baz", "abc" } }));

  fdb::Transaction tr(db);
  char param[] = { 'a', 'd' };
  while (1) {
    tr.atomic_op(key("foo"), (const uint8_t *)"b", 1,
                 FDB_MUTATION_TYPE_BIT_AND);
    tr.atomic_op(key("bar"), (const uint8_t *)param, 2,
                 FDB_MUTATION_TYPE_BIT_AND);
    tr.atomic_op(key("baz"), (const uint8_t *)"e", 1,
                 FDB_MUTATION_TYPE_BIT_AND);
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }

  auto value = get_value(key("foo"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->size() == 1);
  CHECK(value->data()[0] == 96);

  value = get_value(key("bar"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->size() == 2);
  CHECK(value->data()[0] == 97);
  CHECK(value->data()[1] == 0);

  value = get_value(key("baz"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->size() == 1);
  CHECK(value->data()[0] == 97);
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_BIT_OR") {
  // Test bitwise or on values of same length:
  //   db key = foo
  //   db value = 'a' == 97
  //   param = 'b' == 98
  //
  //   'a' == 97 == 0b01100001
  // | 'b' == 98 == 0b01100010
  //   -----------------------
  //                0b01100011 == 99 == 'c'
  //
  // Test bitwise or on extended database value:
  //   db key = bar
  //   db value = 'b' == 98
  //   param = "ad"
  //
  //   'b' == 98 -> 0b0110001000000000 (zero extended on right to match length of param)
  // | "ad"   ==    0b0110000101100100
  //   -------------------------------
  //                0b0110001101100100 == "cd"
  //
  // Test bitwise or on truncated database value:
  //   db key = baz
  //   db value = "abc"
  //   param = 'd' == 100
  //
  //   "abc"  ->  0b01100001 (truncated to "a" to match length of param)
  // | 'd' == 100 0b01100100
  //   ---------------------
  //              0b01100101 == 101 == 'e'
  //
  insert_data(db, create_data({ { "foo", "a" }, { "bar", "b" }, { "baz", "abc" } }));

  fdb::Transaction tr(db);
  char param[] = { 'a', 'd' };
  while (1) {
    tr.atomic_op(key("foo"), (const uint8_t *)"b", 1,
                 FDB_MUTATION_TYPE_BIT_OR);
    tr.atomic_op(key("bar"), (const uint8_t *)param, 2,
                 FDB_MUTATION_TYPE_BIT_OR);
    tr.atomic_op(key("baz"), (const uint8_t *)"d", 1,
                 FDB_MUTATION_TYPE_BIT_OR);
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }

  auto value = get_value(key("foo"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->size() == 1);
  CHECK(value->data()[0] == 99);

  value = get_value(key("bar"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("cd") == 0);

  value = get_value(key("baz"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->size() == 1);
  CHECK(value->data()[0] == 101);
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_BIT_XOR") {
  // Test bitwise xor on values of same length:
  //   db key = foo
  //   db value = 'a' == 97
  //   param = 'b' == 98
  //
  //   'a' == 97 == 0b01100001
  // ^ 'b' == 98 == 0b01100010
  //   -----------------------
  //                0b00000011 == 0x3
  //
  // Test bitwise xor on extended database value:
  //   db key = bar
  //   db value = 'b' == 98
  //   param = "ad"
  //
  //   'b' == 98 -> 0b0110001000000000 (zero extended on right to match length of param)
  // ^ "ad"   ==    0b0110000101100100
  //   -------------------------------
  //                0b0000001101100100 == 0x3 followed by 0x64
  //
  // Test bitwise xor on truncated database value:
  //   db key = baz
  //   db value = "abc"
  //   param = 'd' == 100
  //
  //   "abc"  ->  0b01100001 (truncated to "a" to match length of param)
  // ^ 'd' == 100 0b01100100
  //   ---------------------
  //              0b00000101 == 0x5
  //
  insert_data(db, create_data({ { "foo", "a" }, { "bar", "b" }, { "baz", "abc" } }));

  fdb::Transaction tr(db);
  char param[] = { 'a', 'd' };
  while (1) {
    tr.atomic_op(key("foo"), (const uint8_t *)"b", 1,
                 FDB_MUTATION_TYPE_BIT_XOR);
    tr.atomic_op(key("bar"), (const uint8_t *)param, 2,
                 FDB_MUTATION_TYPE_BIT_XOR);
    tr.atomic_op(key("baz"), (const uint8_t *)"d", 1,
                 FDB_MUTATION_TYPE_BIT_XOR);
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }

  auto value = get_value(key("foo"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->size() == 1);
  CHECK(value->data()[0] == 0x3);

  value = get_value(key("bar"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->size() == 2);
  CHECK(value->data()[0] == 0x3);
  CHECK(value->data()[1] == 0x64);

  value = get_value(key("baz"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->size() == 1);
  CHECK(value->data()[0] == 0x5);
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_COMPARE_AND_CLEAR") {
  // Atomically remove a key-value pair from the database based on a value
  // comparison.
  insert_data(db, create_data({ { "foo", "bar" }, { "fdb", "foundation" } }));

  fdb::Transaction tr(db);
  while (1) {
    tr.atomic_op(key("foo"), (const uint8_t *)"bar", 3,
                 FDB_MUTATION_TYPE_COMPARE_AND_CLEAR);
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }

  auto value = get_value(key("foo"), /* snapshot */ false, {});
  CHECK(!value.has_value());

  value = get_value(key("fdb"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("foundation") == 0);
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_APPEND_IF_FITS") {
  // Atomically append a value to an existing key-value pair, or insert the
  // key-value pair if an existing key-value pair doesn't exist.
  insert_data(db, create_data({ { "foo", "f" } }));

  fdb::Transaction tr(db);
  while (1) {
    tr.atomic_op(key("foo"), (const uint8_t *)"db", 2,
                 FDB_MUTATION_TYPE_APPEND_IF_FITS);
    tr.atomic_op(key("bar"), (const uint8_t *)"foundation", 10,
                 FDB_MUTATION_TYPE_APPEND_IF_FITS);
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }

  auto value = get_value(key("foo"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("fdb") == 0);

  value = get_value(key("bar"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("foundation") == 0);
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_MAX") {
  insert_data(db, create_data({ { "foo", "a" }, { "bar", "b" }, { "baz", "cba" } }));

  fdb::Transaction tr(db);
  while (1) {
    tr.atomic_op(key("foo"), (const uint8_t *)"b", 1, FDB_MUTATION_TYPE_MAX);
    // Value in database will be extended with zeros to match length of param.
    tr.atomic_op(key("bar"), (const uint8_t *)"aa", 2, FDB_MUTATION_TYPE_MAX);
    // Value in database will be truncated to match length of param.
    tr.atomic_op(key("baz"), (const uint8_t *)"b", 1, FDB_MUTATION_TYPE_MAX);
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }

  auto value = get_value(key("foo"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("b") == 0);

  value = get_value(key("bar"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("aa") == 0);

  value = get_value(key("baz"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("c") == 0);
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_MIN") {
  insert_data(db, create_data({ { "foo", "a" }, { "bar", "b" }, { "baz", "cba" } }));

  fdb::Transaction tr(db);
  while (1) {
    tr.atomic_op(key("foo"), (const uint8_t *)"b", 1, FDB_MUTATION_TYPE_MIN);
    // Value in database will be extended with zeros to match length of param.
    tr.atomic_op(key("bar"), (const uint8_t *)"aa", 2, FDB_MUTATION_TYPE_MIN);
    // Value in database will be truncated to match length of param.
    tr.atomic_op(key("baz"), (const uint8_t *)"b", 1, FDB_MUTATION_TYPE_MIN);
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }

  auto value = get_value(key("foo"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("a") == 0);

  value = get_value(key("bar"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->size() == 2);
  CHECK(value->data()[0] == 'b');
  CHECK(value->data()[1] == 0);

  value = get_value(key("baz"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("b") == 0);
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_BYTE_MAX") {
  // The difference with FDB_MUTATION_TYPE_MAX is that strings will not be
  // extended/truncated so lengths match.
  insert_data(db, create_data({ { "foo", "a" }, { "bar", "b" }, { "baz", "cba" } }));

  fdb::Transaction tr(db);
  while (1) {
    tr.atomic_op(key("foo"), (const uint8_t *)"b", 1,
                 FDB_MUTATION_TYPE_BYTE_MAX);
    tr.atomic_op(key("bar"), (const uint8_t *)"cc", 2,
                 FDB_MUTATION_TYPE_BYTE_MAX);
    tr.atomic_op(key("baz"), (const uint8_t *)"b", 1,
                 FDB_MUTATION_TYPE_BYTE_MAX);
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }

  auto value = get_value(key("foo"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("b") == 0);

  value = get_value(key("bar"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("cc") == 0);

  value = get_value(key("baz"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("cba") == 0);
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_BYTE_MIN") {
  // The difference with FDB_MUTATION_TYPE_MIN is that strings will not be
  // extended/truncated so lengths match.
  insert_data(db, create_data({ { "foo", "a" }, { "bar", "b" }, { "baz", "abc" } }));

  fdb::Transaction tr(db);
  while (1) {
    tr.atomic_op(key("foo"), (const uint8_t *)"b", 1,
                 FDB_MUTATION_TYPE_BYTE_MIN);
    tr.atomic_op(key("bar"), (const uint8_t *)"aa", 2,
                 FDB_MUTATION_TYPE_BYTE_MIN);
    tr.atomic_op(key("baz"), (const uint8_t *)"b", 1,
                 FDB_MUTATION_TYPE_BYTE_MIN);
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }
    break;
  }

  auto value = get_value(key("foo"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("a") == 0);

  value = get_value(key("bar"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("aa") == 0);

  value = get_value(key("baz"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("abc") == 0);
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_SET_VERSIONSTAMPED_KEY") {
  int offset = prefix.size() + 3;
  const char *p = reinterpret_cast<const char *>(&offset);
  char keybuf[] = {'f', 'o', 'o', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', p[0], p[1], p[2], p[3]};
  std::string key = prefix + std::string(keybuf, 17);
  std::string versionstamp("");

  fdb::Transaction tr(db);
  while (1) {
    tr.atomic_op(key, (const uint8_t *)"bar", 3,
                 FDB_MUTATION_TYPE_SET_VERSIONSTAMPED_KEY);
    fdb::KeyFuture f1 = tr.get_versionstamp();
    fdb::EmptyFuture f2 = tr.commit();

    fdb_error_t err = wait_future(f2);
    if (err) {
      fdb::EmptyFuture f3 = tr.on_error(err);
      fdb_check(wait_future(f3));
      continue;
    }

    fdb_check(wait_future(f1));

    const uint8_t *key;
    int keylen;
    fdb_check(f1.get(&key, &keylen));

    versionstamp = std::string((const char *)key, keylen);
    break;
  }

  REQUIRE(versionstamp.size() > 0);
  std::string dbKey(prefix + "foo" + versionstamp);
  auto value = get_value(dbKey, /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("bar") == 0);
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_SET_VERSIONSTAMPED_VALUE") {
  // Don't care about prefixing value like we did the key.
  char valbuf[] = {'b', 'a', 'r', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', 3, 0, 0, 0};
  std::string versionstamp("");

  fdb::Transaction tr(db);
  while (1) {
    tr.atomic_op(key("foo"), (const uint8_t *)valbuf, 17,
                 FDB_MUTATION_TYPE_SET_VERSIONSTAMPED_VALUE);
    fdb::KeyFuture f1 = tr.get_versionstamp();
    fdb::EmptyFuture f2 = tr.commit();

    fdb_error_t err = wait_future(f2);
    if (err) {
      fdb::EmptyFuture f3 = tr.on_error(err);
      fdb_check(wait_future(f3));
      continue;
    }

    fdb_check(wait_future(f1));

    const uint8_t *key;
    int keylen;
    fdb_check(f1.get(&key, &keylen));

    versionstamp = std::string((const char *)key, keylen);
    break;
  }

  REQUIRE(versionstamp.size() > 0);
  auto value = get_value(key("foo"), /* snapshot */ false, {});
  REQUIRE(value.has_value());
  CHECK(value->compare("bar" + versionstamp) == 0);
}

TEST_CASE("fdb_transaction_atomic_op FDB_MUTATION_TYPE_SET_VERSIONSTAMPED_KEY invalid index") {
  // Only 9 bytes available starting at index 4 (ten bytes needed), should
  // return an error.
  char keybuf[] = {'f', 'o', 'o', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', 4, 0, 0, 0};

  fdb::Transaction tr(db);
  while (1) {
    tr.atomic_op(keybuf, (const uint8_t *)"bar", 3,
                 FDB_MUTATION_TYPE_SET_VERSIONSTAMPED_KEY);
    fdb::EmptyFuture f1 = tr.commit();

    CHECK(wait_future(f1) != 0); // type of error not specified
    break;
  }
}

TEST_CASE("fdb_transaction_get_committed_version read_only") {
  // Read-only transaction should have a committed version of -1.
  fdb::Transaction tr(db);
  while (1) {
    fdb::ValueFuture f1 = tr.get("foo", /*snapshot*/ false);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int64_t out_version;
    fdb_check(tr.get_committed_version(&out_version));
    CHECK(out_version == -1);
    break;
  }
}

TEST_CASE("fdb_transaction_get_committed_version") {
  fdb::Transaction tr(db);
  while (1) {
    tr.set(key("foo"), "bar");
    fdb::EmptyFuture f1 = tr.commit();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int64_t out_version;
    fdb_check(tr.get_committed_version(&out_version));
    CHECK(out_version >= 0);
    break;
  }
}

TEST_CASE("fdb_transaction_get_approximate_size") {
  fdb::Transaction tr(db);
  while (1) {
    tr.set(key("foo"), "bar");
    fdb::Int64Future f1 = tr.get_approximate_size();

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int64_t size;
    fdb_check(f1.get(&size));
    CHECK(size >= 3);
    break;
  }
}

TEST_CASE("fdb_get_server_protocol") {
  FDBFuture* protocolFuture = fdb_get_server_protocol(clusterFilePath.c_str());
  uint64_t out;

  fdb_check(fdb_future_block_until_ready(protocolFuture));
  fdb_check(fdb_future_get_uint64(protocolFuture, &out));
  fdb_future_destroy(protocolFuture);
}

TEST_CASE("fdb_transaction_watch read_your_writes_disable") {
  // Watches created on a transaction with the option READ_YOUR_WRITES_DISABLE
  // should return a watches_disabled error.
  fdb::Transaction tr(db);
  fdb_check(tr.set_option(FDB_TR_OPTION_READ_YOUR_WRITES_DISABLE, nullptr, 0));
  fdb::EmptyFuture f1 = tr.watch(key("foo"));

  CHECK(wait_future(f1) == 1034); // watches_disabled
}

TEST_CASE("fdb_transaction_watch reset") {
  // Resetting (or destroying) an uncommitted transaction should cause watches
  // created by the transaction to fail with a transaction_cancelled error.
  fdb::Transaction tr(db);
  fdb::EmptyFuture f1 = tr.watch(key("foo"));
  tr.reset();
  CHECK(wait_future(f1) == 1025); // transaction_cancelled
}

TEST_CASE("fdb_transaction_watch max watches") {
  int64_t max_watches = 3;
  fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_MAX_WATCHES,
                                    (const uint8_t *)&max_watches, 8));

  auto event = std::make_shared<FdbEvent>();

  fdb::Transaction tr(db);
  while (1) {
    fdb::EmptyFuture f1 = tr.watch(key("a"));
    fdb::EmptyFuture f2 = tr.watch(key("b"));
    fdb::EmptyFuture f3 = tr.watch(key("c"));
    fdb::EmptyFuture f4 = tr.watch(key("d"));
    fdb::EmptyFuture f5 = tr.commit();

    fdb_error_t err = wait_future(f5);
    if (err) {
      fdb::EmptyFuture f6 = tr.on_error(err);
      fdb_check(wait_future(f6));
      continue;
    }

    // Callbacks will be triggered with operation_cancelled errors once the
    // too_many_watches error fires, as the other futures will go out of scope
    // and be cleaned up. The future which too_many_watches occurs on is
    // nondeterministic, so each future is checked.
    fdb_check(f1.set_callback(
        +[](FDBFuture *f, void *param) {
          fdb_error_t err = fdb_future_get_error(f);
          if (err != 1101) { // operation_cancelled
            CHECK(err == 1032); // too_many_watches
          }
          auto *event = static_cast<std::shared_ptr<FdbEvent> *>(param);
          (*event)->set();
          delete event;
        }, new std::shared_ptr<FdbEvent>(event)));
    fdb_check(f2.set_callback(
        +[](FDBFuture *f, void *param) {
          fdb_error_t err = fdb_future_get_error(f);
          if (err != 1101) { // operation_cancelled
            CHECK(err == 1032); // too_many_watches
          }
          auto *event = static_cast<std::shared_ptr<FdbEvent> *>(param);
          (*event)->set();
          delete event;
        }, new std::shared_ptr<FdbEvent>(event)));
    fdb_check(f3.set_callback(
        +[](FDBFuture *f, void *param) {
          fdb_error_t err = fdb_future_get_error(f);
          if (err != 1101) { // operation_cancelled
            CHECK(err == 1032); // too_many_watches
          }
          auto *event = static_cast<std::shared_ptr<FdbEvent> *>(param);
          (*event)->set();
          delete event;
        }, new std::shared_ptr<FdbEvent>(event)));
    fdb_check(f4.set_callback(
        +[](FDBFuture *f, void *param) {
          fdb_error_t err = fdb_future_get_error(f);
          if (err != 1101) { // operation_cancelled
            CHECK(err == 1032); // too_many_watches
          }
          auto *event = static_cast<std::shared_ptr<FdbEvent> *>(param);
          (*event)->set();
          delete event;
        }, new std::shared_ptr<FdbEvent>(event)));

    event->wait();
    break;
  }

  // Reset available number of watches.
  max_watches = 10000;
  fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_MAX_WATCHES,
                                    (const uint8_t *)&max_watches, 8));
}

TEST_CASE("fdb_transaction_watch") {
  insert_data(db, create_data({ { "foo", "foo" } }));

  struct Context {
    FdbEvent event;
  };
  Context context;

  fdb::Transaction tr(db);
  while (1) {
    fdb::EmptyFuture f1 = tr.watch(key("foo"));
    fdb::EmptyFuture f2 = tr.commit();

    fdb_error_t err = wait_future(f2);
    if (err) {
      fdb::EmptyFuture f3 = tr.on_error(err);
      fdb_check(wait_future(f3));
      continue;
    }

    fdb_check(f1.set_callback(
        +[](FDBFuture *, void *param) {
          auto *context = static_cast<Context *>(param);
          context->event.set();
        },
        &context));

    // Update value for key "foo" to trigger the watch.
    insert_data(db, create_data({ { "foo", "bar" } }));
    context.event.wait();
    break;
  }
}

TEST_CASE("fdb_transaction_cancel") {
  // Cannot use transaction after cancelling it...
  fdb::Transaction tr(db);
  tr.cancel();
  fdb::ValueFuture f1 = tr.get("foo", /* snapshot */ false);
  CHECK(wait_future(f1) == 1025); // transaction_cancelled

  // ... until the transaction has been reset.
  tr.reset();
  fdb::ValueFuture f2 = tr.get("foo", /* snapshot */ false);
  fdb_check(wait_future(f2));
}

TEST_CASE("fdb_transaction_add_conflict_range") {
  bool success = false;

  bool retry = true;
  while (retry) {
    fdb::Transaction tr(db);
    while (1) {
      fdb::Int64Future f1 = tr.get_read_version();

      fdb_error_t err = wait_future(f1);
      if (err) {
        fdb::EmptyFuture f2 = tr.on_error(err);
        fdb_check(wait_future(f2));
        continue;
      }
      break;
    }

    fdb::Transaction tr2(db);
    while (1) {
      fdb_check(tr2.add_conflict_range(key("a"), strinc(key("a")),
                                       FDB_CONFLICT_RANGE_TYPE_WRITE));
      fdb::EmptyFuture f1 = tr2.commit();

      fdb_error_t err = wait_future(f1);
      if (err) {
        fdb::EmptyFuture f2 = tr2.on_error(err);
        fdb_check(wait_future(f2));
        continue;
      }
      break;
    }

    while (1) {
      fdb_check(tr.add_conflict_range(key("a"), strinc(key("a")),
                                       FDB_CONFLICT_RANGE_TYPE_READ));
      fdb_check(tr.add_conflict_range(key("a"), strinc(key("a")),
                                       FDB_CONFLICT_RANGE_TYPE_WRITE));
      fdb::EmptyFuture f1 = tr.commit();

      fdb_error_t err = wait_future(f1);
      if (err == 1020) { // not_committed
        // Test should pass if transactions conflict.
        success = true;
        retry = false;
      } else if (err) {
        fdb::EmptyFuture f2 = tr.on_error(err);
        fdb_check(wait_future(f2));
        retry = true;
      } else {
        // If the transaction succeeded, something went wrong.
        CHECK(false);
        retry = false;
      }
      break;
    }
  }

  // Double check that failure was achieved and the loop wasn't just broken out
  // of.
  CHECK(success);
}

TEST_CASE("special-key-space valid transaction ID") {
  auto value = get_value("\xff\xff/tracing/a/transaction_id", /* snapshot */ false, {});
  REQUIRE(value.has_value());
  uint64_t transaction_id = std::stoul(value.value());
  CHECK(transaction_id > 0);
}

TEST_CASE("special-key-space custom transaction ID") {
  fdb::Transaction tr(db);
  fdb_check(tr.set_option(FDB_TR_OPTION_SPECIAL_KEY_SPACE_ENABLE_WRITES,
                          nullptr, 0));
  while (1) {
    tr.set("\xff\xff/tracing/a/transaction_id", std::to_string(ULONG_MAX));
    fdb::ValueFuture f1 = tr.get("\xff\xff/tracing/a/transaction_id",
                                 /* snapshot */ false);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));

    REQUIRE(out_present);
    uint64_t transaction_id = std::stoul(std::string(val, vallen));
    CHECK(transaction_id == ULONG_MAX);
    break;
  }
}

TEST_CASE("special-key-space set transaction ID after write") {
  fdb::Transaction tr(db);
  fdb_check(tr.set_option(FDB_TR_OPTION_SPECIAL_KEY_SPACE_ENABLE_WRITES,
                          nullptr, 0));
  while (1) {
    tr.set(key("foo"), "bar");
    tr.set("\xff\xff/tracing/a/transaction_id", "0");
    fdb::ValueFuture f1 = tr.get("\xff\xff/tracing/a/transaction_id",
                                 /* snapshot */ false);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));

    REQUIRE(out_present);
    uint64_t transaction_id = std::stoul(std::string(val, vallen));
    CHECK(transaction_id != 0);
    break;
  }
}

TEST_CASE("special-key-space set token after write") {
  fdb::Transaction tr(db);
  fdb_check(tr.set_option(FDB_TR_OPTION_SPECIAL_KEY_SPACE_ENABLE_WRITES,
                          nullptr, 0));
  while (1) {
    tr.set(key("foo"), "bar");
    tr.set("\xff\xff/tracing/a/token", "false");
    fdb::ValueFuture f1 = tr.get("\xff\xff/tracing/a/token",
                                 /* snapshot */ false);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));

    REQUIRE(out_present);
    uint64_t token = std::stoul(std::string(val, vallen));
    CHECK(token != 0);
    break;
  }
}

TEST_CASE("special-key-space valid token") {
  auto value = get_value("\xff\xff/tracing/a/token", /* snapshot */ false, {});
  REQUIRE(value.has_value());
  uint64_t token = std::stoul(value.value());
  CHECK(token > 0);
}

TEST_CASE("special-key-space disable tracing") {
  fdb::Transaction tr(db);
  fdb_check(tr.set_option(FDB_TR_OPTION_SPECIAL_KEY_SPACE_ENABLE_WRITES,
                          nullptr, 0));
  while (1) {
    tr.set("\xff\xff/tracing/a/token", "false");
    fdb::ValueFuture f1 = tr.get("\xff\xff/tracing/a/token",
                                 /* snapshot */ false);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));

    REQUIRE(out_present);
    uint64_t token = std::stoul(std::string(val, vallen));
    CHECK(token == 0);
    break;
  }
}

TEST_CASE("FDB_DB_OPTION_TRANSACTION_TRACE_DISABLE") {
  fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_TRANSACTION_TRACE_DISABLE, nullptr, 0));

  auto value = get_value("\xff\xff/tracing/a/token", /* snapshot */ false, {});
  REQUIRE(value.has_value());
  uint64_t token = std::stoul(value.value());
  CHECK(token == 0);

  fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_TRANSACTION_TRACE_ENABLE, nullptr, 0));
}

TEST_CASE("FDB_DB_OPTION_TRANSACTION_TRACE_DISABLE enable tracing for transaction") {
  fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_TRANSACTION_TRACE_DISABLE, nullptr, 0));

  fdb::Transaction tr(db);
  fdb_check(tr.set_option(FDB_TR_OPTION_SPECIAL_KEY_SPACE_ENABLE_WRITES,
                          nullptr, 0));
  while (1) {
    tr.set("\xff\xff/tracing/a/token", "true");
    fdb::ValueFuture f1 = tr.get("\xff\xff/tracing/a/token",
                                 /* snapshot */ false);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    int out_present;
    char *val;
    int vallen;
    fdb_check(f1.get(&out_present, (const uint8_t **)&val, &vallen));

    REQUIRE(out_present);
    uint64_t token = std::stoul(std::string(val, vallen));
    CHECK(token > 0);
    break;
  }

  fdb_check(fdb_database_set_option(db, FDB_DB_OPTION_TRANSACTION_TRACE_ENABLE, nullptr, 0));
}

TEST_CASE("special-key-space tracing get range") {
  std::string tracingBegin = "\xff\xff/tracing/a/";
  std::string tracingEnd = "\xff\xff/tracing/a0";

  fdb::Transaction tr(db);
  fdb_check(tr.set_option(FDB_TR_OPTION_SPECIAL_KEY_SPACE_ENABLE_WRITES,
                          nullptr, 0));
  while (1) {
    fdb::KeyValueArrayFuture f1 = tr.get_range(
        FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(
          (const uint8_t *)tracingBegin.c_str(),
          tracingBegin.size()
        ),
        FDB_KEYSEL_LAST_LESS_OR_EQUAL(
          (const uint8_t *)tracingEnd.c_str(),
          tracingEnd.size()
        ) + 1, /* limit */ 0, /* target_bytes */ 0,
        /* FDBStreamingMode */ FDB_STREAMING_MODE_WANT_ALL, /* iteration */ 0,
        /* snapshot */ false, /* reverse */ 0);

    fdb_error_t err = wait_future(f1);
    if (err) {
      fdb::EmptyFuture f2 = tr.on_error(err);
      fdb_check(wait_future(f2));
      continue;
    }

    FDBKeyValue const *out_kv;
    int out_count;
    int out_more;
    fdb_check(f1.get(&out_kv, &out_count, &out_more));

    CHECK(!out_more);
    CHECK(out_count == 2);

    CHECK(std::string((char *)out_kv[0].key, out_kv[0].key_length) == tracingBegin + "token");
    CHECK(std::stoul(std::string((char *)out_kv[0].value, out_kv[0].value_length)) > 0);
    CHECK(std::string((char *)out_kv[1].key, out_kv[1].key_length) == tracingBegin + "transaction_id");
    CHECK(std::stoul(std::string((char *)out_kv[1].value, out_kv[1].value_length)) > 0);
    break;
  }
}

TEST_CASE("fdb_error_predicate") {
  CHECK(fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 1007)); // transaction_too_old
  CHECK(fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 1020)); // not_committed
  CHECK(fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 1038)); // database_locked

  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 1036)); // accessed_unreadable
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2000)); // client_invalid_operation
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2004)); // key_outside_legal_range
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2005)); // inverted_range
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2006)); // invalid_option_value
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2007)); // invalid_option
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2011)); // version_invalid
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2020)); // transaction_invalid_version
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2023)); // transaction_read_only
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2100)); // incompatible_protocol_version
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2101)); // transaction_too_large
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2102)); // key_too_large
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2103)); // value_too_large
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2108)); // unsupported_operation
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 2200)); // api_version_unset
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 4000)); // unknown_error
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, 4001)); // internal_error

  CHECK(fdb_error_predicate(FDB_ERROR_PREDICATE_MAYBE_COMMITTED, 1021)); // commit_unknown_result

  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_MAYBE_COMMITTED, 1000)); // operation_failed
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_MAYBE_COMMITTED, 1004)); // timed_out
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_MAYBE_COMMITTED, 1025)); // transaction_cancelled
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_MAYBE_COMMITTED, 1038)); // database_locked
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_MAYBE_COMMITTED, 1101)); // operation_cancelled
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_MAYBE_COMMITTED, 2002)); // commit_read_incomplete

  CHECK(fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, 1007)); // transaction_too_old
  CHECK(fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, 1020)); // not_committed
  CHECK(fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, 1038)); // database_locked

  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, 1021)); // commit_unknown_result
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, 1025)); // transaction_cancelled
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, 1031)); // transaction_timed_out
  CHECK(!fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, 1040)); // proxy_memory_limit_exceeded
}

TEST_CASE("block_from_callback") {
  fdb::Transaction tr(db);
  fdb::ValueFuture f1 = tr.get("foo", /*snapshot*/ true);
  struct Context {
    FdbEvent event;
    fdb::Transaction *tr;
  };
  Context context;
  context.tr = &tr;
  fdb_check(f1.set_callback(
      +[](FDBFuture *, void *param) {
        auto *context = static_cast<Context *>(param);
        fdb::ValueFuture f2 = context->tr->get("bar", /*snapshot*/ true);
        fdb_error_t error = f2.block_until_ready();
        if (error) {
          CHECK(error == /*blocked_from_network_thread*/ 2026);
        }
        context->event.set();
      },
      &context));
  context.event.wait();
}

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cout << "Unit tests for the FoundationDB C API.\n"
              << "Usage: fdb_c_unit_tests /path/to/cluster_file key_prefix"
              << std::endl;
    return 1;
  }

  doctest::Context context;

  fdb_check(fdb_select_api_version(700));
  fdb_check(fdb_setup_network());
  std::thread network_thread{ &fdb_run_network };

  db = fdb_open_database(argv[1]);
  clusterFilePath = std::string(argv[1]);
  prefix = argv[2];
  int res = context.run();
  fdb_database_destroy(db);

  if (context.shouldExit()) {
    fdb_check(fdb_stop_network());
    network_thread.join();
    return res;
  }
  fdb_check(fdb_stop_network());
  network_thread.join();

  return res;
}
