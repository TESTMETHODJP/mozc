// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "engine/data_loader.h"

#include <cstdint>
#include <memory>
#include <string>

#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "base/file/temp_dir.h"
#include "base/file_util.h"
#include "base/hash.h"
#include "data_manager/data_manager.h"
#include "data_manager/data_manager_interface.h"
#include "protocol/engine_builder.pb.h"
#include "testing/gmock.h"
#include "testing/gunit.h"
#include "testing/mozctest.h"

namespace mozc {
namespace {

constexpr absl::string_view kMockMagicNumber = "MOCK";

struct Param {
  EngineReloadRequest::EngineType type;
  std::string predictor_name;
};

class DataLoaderTest : public testing::TestWithTempUserProfile,
                       public ::testing::WithParamInterface<Param> {
 protected:
  DataLoaderTest()
      : mock_data_path_(
            testing::GetSourcePath({MOZC_SRC_COMPONENTS("data_manager"),
                                    "testing", "mock_mozc.data"})) {
    LOG(INFO) << mock_data_path_;
  }

  void Clear() {
    loader_.Clear();
    request_.Clear();
  }

  const std::string mock_data_path_;
  DataLoader loader_;
  EngineReloadRequest request_;
};

TEST_P(DataLoaderTest, BasicTest) {
  {
    // Test request without install.
    request_.set_engine_type(GetParam().type);
    request_.set_file_path(mock_data_path_);
    request_.set_magic_number(kMockMagicNumber);

    const uint64_t id = loader_.RegisterRequest(request_);
    DataLoader::ResponseFuture response_future = loader_.Build(id);

    response_future.Wait();
    const DataLoader::Response &response = response_future.Get();

    DataManager data_manager;
    data_manager.InitFromFile(mock_data_path_, kMockMagicNumber);
    absl::string_view expected_version = data_manager.GetDataVersion();
    std::string expected_filename = data_manager.GetFilename().value();

    EXPECT_EQ(response.response.status(), EngineReloadResponse::RELOAD_READY);
    EXPECT_EQ(response.id, id);
    ASSERT_TRUE(response.modules);

    const DataManagerInterface &response_data_manager =
        response.modules->GetDataManager();
    EXPECT_EQ(response_data_manager.GetDataVersion(), expected_version);
    EXPECT_TRUE(response_data_manager.GetFilename());
    EXPECT_EQ(response_data_manager.GetFilename().value(), expected_filename);
    EXPECT_EQ(response.response.request().engine_type(), GetParam().type);
  }

  Clear();

  {
    // Test request with install.  Since the requested file is copied,
    // |mock_data_path_| is copied to a temporary file.
    TempDirectory temp_dir = testing::MakeTempDirectoryOrDie();
    const std::string src_path =
        FileUtil::JoinPath({temp_dir.path(), "src.data"});
    ASSERT_OK(FileUtil::CopyFile(mock_data_path_, src_path));

    const std::string install_path =
        FileUtil::JoinPath({temp_dir.path(), "dst.data"});
    request_.set_engine_type(GetParam().type);
    request_.set_file_path(src_path);
    request_.set_install_location(install_path);
    request_.set_magic_number(kMockMagicNumber);
    const uint64_t id = loader_.RegisterRequest(request_);

    DataLoader::ResponseFuture response_future = loader_.Build(id);
    response_future.Wait();
    const DataLoader::Response &response = response_future.Get();

    DataManager data_manager;
    data_manager.InitFromFile(src_path, kMockMagicNumber);
    absl::string_view expected_version = data_manager.GetDataVersion();
    std::string expected_filename = data_manager.GetFilename().value();

    EXPECT_EQ(response.response.status(), EngineReloadResponse::RELOAD_READY);
    EXPECT_EQ(response.id, id);
    ASSERT_TRUE(response.modules);

    const DataManagerInterface &response_data_manager =
        response.modules->GetDataManager();
    EXPECT_EQ(response_data_manager.GetDataVersion(), expected_version);
    EXPECT_TRUE(response_data_manager.GetFilename());
    EXPECT_EQ(response_data_manager.GetFilename().value(), expected_filename);

    // Verify |src_path| was copied.
    EXPECT_OK(FileUtil::FileExists(src_path));
    EXPECT_OK(FileUtil::FileExists(install_path));
  }
}

TEST_P(DataLoaderTest, AsyncBuildRepeatedly) {
  // Calls RegisterRequest multiple times.
  // Makes sure that the last request is processed.
  TempDirectory temp_dir = testing::MakeTempDirectoryOrDie();
  std::string last_path;
  uint64_t latest_id = 0;

  // Sending the duplicated request three times.
  // They are all ignored. i.e., latest_id is obtained after dedup.
  for (int trial = 0; trial < 3; ++trial) {
    for (int i = 0; i < 32; ++i) {
      // Test request without install.
      request_.set_engine_type(GetParam().type);
      last_path = FileUtil::JoinPath(
          {temp_dir.path(), absl::StrCat("src_", i, ".data")});
      ASSERT_OK(FileUtil::CopyFile(mock_data_path_, last_path));
      request_.set_file_path(last_path);
      request_.set_magic_number(kMockMagicNumber);
      latest_id = loader_.RegisterRequest(request_);
    }
  }

  DataLoader::ResponseFuture response_future = loader_.Build(latest_id);

  response_future.Wait();
  const DataLoader::Response &response = response_future.Get();

  DataManager data_manager;
  data_manager.InitFromFile(last_path, kMockMagicNumber);
  absl::string_view expected_version = data_manager.GetDataVersion();
  std::string expected_filename = data_manager.GetFilename().value();

  EXPECT_EQ(response.response.status(), EngineReloadResponse::RELOAD_READY);
  EXPECT_EQ(response.response.request().file_path(), last_path);
  ASSERT_TRUE(response.modules);

  const DataManagerInterface &response_data_manager =
      response.modules->GetDataManager();
  EXPECT_EQ(response_data_manager.GetDataVersion(), expected_version);
  EXPECT_TRUE(response_data_manager.GetFilename());
  EXPECT_EQ(response_data_manager.GetFilename().value(), expected_filename);
  EXPECT_EQ(response.id, latest_id);
}

TEST_P(DataLoaderTest, AsyncBuildWithoutInstall) {
  // Request preparation without install.
  request_.set_engine_type(GetParam().type);
  request_.set_file_path(mock_data_path_);
  request_.set_magic_number(kMockMagicNumber);
  const uint64_t id = loader_.RegisterRequest(request_);

  DataLoader::ResponseFuture response_future = loader_.Build(id);

  response_future.Wait();
  const DataLoader::Response &response = response_future.Get();

  DataManager data_manager;
  data_manager.InitFromFile(mock_data_path_, kMockMagicNumber);
  absl::string_view expected_version = data_manager.GetDataVersion();
  std::string expected_filename = data_manager.GetFilename().value();

  EXPECT_EQ(response.response.status(), EngineReloadResponse::RELOAD_READY);
  ASSERT_TRUE(response.modules);

  const DataManagerInterface &response_data_manager =
      response.modules->GetDataManager();
  EXPECT_EQ(response_data_manager.GetDataVersion(), expected_version);
  EXPECT_TRUE(response_data_manager.GetFilename());
  EXPECT_EQ(response_data_manager.GetFilename().value(), expected_filename);
  EXPECT_EQ(response.id, id);
}

TEST_P(DataLoaderTest, AsyncBuildWithInstall) {
  TempDirectory temp_dir = testing::MakeTempDirectoryOrDie();
  const std::string tmp_src = FileUtil::JoinPath({temp_dir.path(), "src.data"});
  const std::string install_path =
      FileUtil::JoinPath({temp_dir.path(), "dst.data"});

  // Since requested file is copied, copy |mock_data_path_| to a temporary
  // file.
  ASSERT_OK(FileUtil::CopyFile(mock_data_path_, tmp_src));

  // Request preparation with install.
  request_.set_engine_type(GetParam().type);
  request_.set_file_path(tmp_src);
  request_.set_install_location(install_path);
  request_.set_magic_number(kMockMagicNumber);
  const uint64_t id = loader_.RegisterRequest(request_);

  DataLoader::ResponseFuture response_future = loader_.Build(id);

  response_future.Wait();
  const DataLoader::Response &response = response_future.Get();

  // Builder should be ready now.
  EXPECT_EQ(response.response.status(), EngineReloadResponse::RELOAD_READY);

  // |tmp_src| should be copied to |install_path|.
  ASSERT_OK(FileUtil::FileExists(tmp_src));
  ASSERT_OK(FileUtil::FileExists(install_path));

  DataManager data_manager;
  data_manager.InitFromFile(tmp_src, kMockMagicNumber);
  absl::string_view expected_version = data_manager.GetDataVersion();
  std::string expected_filename = data_manager.GetFilename().value();

  ASSERT_TRUE(response.modules);

  const DataManagerInterface &response_data_manager =
      response.modules->GetDataManager();
  EXPECT_EQ(response_data_manager.GetDataVersion(), expected_version);
  EXPECT_TRUE(response_data_manager.GetFilename());
  EXPECT_EQ(response_data_manager.GetFilename().value(), expected_filename);
  EXPECT_EQ(response.id, id);
}

TEST_P(DataLoaderTest, FailureCaseDataBroken) {
  // Test the case where input file is invalid.
  request_.set_engine_type(GetParam().type);
  request_.set_file_path(testing::GetSourceFileOrDie(
      {MOZC_SRC_COMPONENTS("engine"), "data_loader_test.cc"}));
  request_.set_magic_number(kMockMagicNumber);
  const uint64_t id = loader_.RegisterRequest(request_);

  DataLoader::ResponseFuture response_future = loader_.Build(id);

  response_future.Wait();
  const DataLoader::Response &response = response_future.Get();

  EXPECT_EQ(response.response.status(), EngineReloadResponse::DATA_BROKEN);
  EXPECT_FALSE(response.modules);
  EXPECT_EQ(response.id, id);
}

TEST_P(DataLoaderTest, InvalidId) {
  // Test the case where input file is invalid.
  request_.set_engine_type(GetParam().type);
  request_.set_file_path(mock_data_path_);
  request_.set_magic_number(kMockMagicNumber);
  const uint64_t id =
      loader_.RegisterRequest(request_) + 1;  // + 1 to make invalid id.

  DataLoader::ResponseFuture response_future = loader_.Build(id);

  response_future.Wait();
  const DataLoader::Response &response = response_future.Get();

  EXPECT_EQ(response.response.status(), EngineReloadResponse::DATA_MISSING);
  EXPECT_FALSE(response.modules);
  EXPECT_EQ(response.id, id);
}

TEST_P(DataLoaderTest, FailureCaseFileDoesNotExist) {
  // Test the case where input file doesn't exist.
  request_.set_engine_type(GetParam().type);
  request_.set_file_path("file_does_not_exist");
  request_.set_magic_number(kMockMagicNumber);

  const uint64_t id = loader_.RegisterRequest(request_);
  DataLoader::ResponseFuture response_future = loader_.Build(id);

  response_future.Wait();
  const DataLoader::Response &response = response_future.Get();

  EXPECT_EQ(response.response.status(), EngineReloadResponse::MMAP_FAILURE);
  EXPECT_FALSE(response.modules);
  EXPECT_EQ(response.id, id);
}

TEST_P(DataLoaderTest, RegisterRequestTest) {
  Clear();

  auto id = [&](absl::string_view file_path, int32_t priority) {
    EngineReloadRequest request;
    request.set_engine_type(GetParam().type);
    request.set_file_path(file_path);
    request.set_priority(priority);
    return Fingerprint(request.SerializeAsString());
  };

  auto register_request = [&](absl::string_view file_path, int32_t priority) {
    EngineReloadRequest request;
    request.set_engine_type(GetParam().type);
    request.set_file_path(file_path);
    request.set_priority(priority);
    return loader_.RegisterRequest(request);
  };

  auto unregister_request = [&](absl::string_view file_path, int32_t priority) {
    return loader_.ReportLoadFailure(id(file_path, priority));
  };

  // Register request.
  constexpr const int32_t kPHigh = 0;
  constexpr const int32_t kPLow = 5;

  EXPECT_EQ(id("foo", kPLow), register_request("foo", kPLow));
  EXPECT_EQ(id("bar", kPLow), register_request("bar", kPLow));
  EXPECT_EQ(id("foo", kPLow), register_request("foo", kPLow));
  EXPECT_EQ(id("bar", kPHigh), register_request("bar", kPHigh));
  EXPECT_EQ(id("bar", kPHigh),
            register_request("buzz", kPLow));  // buzz>foo>bar
  EXPECT_EQ(id("foo", kPHigh), register_request("foo", kPHigh));
  EXPECT_EQ(id("bar", kPHigh), register_request("bar", kPHigh));
  EXPECT_EQ(id("bar", kPHigh), register_request("foo", kPLow));  // foo>buzz>bar
  EXPECT_EQ(id("bar", kPHigh), register_request("bar", kPLow));  // bar>foo>buzz
  EXPECT_EQ(id("buzz", kPHigh), register_request("buzz", kPHigh));

  // Unregister.
  EXPECT_EQ(id("bar", kPHigh), unregister_request("buzz", kPHigh));
  EXPECT_EQ(id("bar", kPHigh), unregister_request("foo", kPHigh));
  EXPECT_EQ(id("bar", kPHigh), unregister_request("foo", kPHigh));
  EXPECT_EQ(id("bar", kPLow), unregister_request("bar", kPHigh));
  EXPECT_EQ(id("bar", kPLow), unregister_request("buzz", kPHigh));
  EXPECT_EQ(id("bar", kPLow), unregister_request("foo", kPLow));
  EXPECT_EQ(id("bar", kPLow), unregister_request("foo", kPHigh));
  EXPECT_EQ(id("bar", kPLow), unregister_request("bar", kPHigh));
  EXPECT_EQ(id("buzz", kPLow), unregister_request("bar", kPLow));
  EXPECT_EQ(0, unregister_request("buzz", kPLow));
}

INSTANTIATE_TEST_SUITE_P(
    DataLoaderTest, DataLoaderTest,
    ::testing::Values(Param{EngineReloadRequest::DESKTOP, "DefaultPredictor"},
                      Param{EngineReloadRequest::MOBILE, "MobilePredictor"}),
    [](const ::testing::TestParamInfo<Param> &info) -> std::string {
      return info.param.predictor_name;
    });

}  // namespace
}  // namespace mozc
