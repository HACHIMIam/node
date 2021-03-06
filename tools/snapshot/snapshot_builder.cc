#include "snapshot_builder.h"
#include <iostream>
#include <sstream>
#include "node_internals.h"
#include "node_main_instance.h"
#include "node_v8_platform-inl.h"

namespace node {

using v8::Context;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::Locker;
using v8::SnapshotCreator;
using v8::StartupData;

template <typename T>
void WriteVector(std::stringstream* ss, const T* vec, size_t size) {
  for (size_t i = 0; i < size; i++) {
    *ss << std::to_string(vec[i]) << (i == size - 1 ? '\n' : ',');
  }
}

std::string FormatBlob(v8::StartupData* blob,
                       const std::vector<size_t>& isolate_data_indexes) {
  std::stringstream ss;

  ss << R"(#include <cstddef>
#include "node_main_instance.h"
#include "v8.h"

// This file is generated by tools/snapshot. Do not edit.

namespace node {

static const char blob_data[] = {
)";
  WriteVector(&ss, blob->data, blob->raw_size);
  ss << R"(};

static const int blob_size = )"
     << blob->raw_size << R"(;
static v8::StartupData blob = { blob_data, blob_size };
)";

  ss << R"(v8::StartupData* NodeMainInstance::GetEmbeddedSnapshotBlob() {
  return &blob;
}

static const std::vector<size_t> isolate_data_indexes {
)";
  WriteVector(&ss, isolate_data_indexes.data(), isolate_data_indexes.size());
  ss << R"(};

const std::vector<size_t>* NodeMainInstance::GetIsolateDataIndexes() {
  return &isolate_data_indexes;
}
}  // namespace node
)";

  return ss.str();
}

std::string SnapshotBuilder::Generate(
    const std::vector<std::string> args,
    const std::vector<std::string> exec_args) {
  // TODO(joyeecheung): collect external references and set it in
  // params.external_references.
  std::vector<intptr_t> external_references = {
      reinterpret_cast<intptr_t>(nullptr)};
  Isolate* isolate = Isolate::Allocate();
  per_process::v8_platform.Platform()->RegisterIsolate(isolate,
                                                       uv_default_loop());
  NodeMainInstance* main_instance = nullptr;
  std::string result;

  {
    std::vector<size_t> isolate_data_indexes;
    SnapshotCreator creator(isolate, external_references.data());
    {
      main_instance =
          NodeMainInstance::Create(isolate,
                                   uv_default_loop(),
                                   per_process::v8_platform.Platform(),
                                   args,
                                   exec_args);
      HandleScope scope(isolate);
      creator.SetDefaultContext(Context::New(isolate));
      isolate_data_indexes = main_instance->isolate_data()->Serialize(&creator);

      size_t index = creator.AddContext(NewContext(isolate));
      CHECK_EQ(index, NodeMainInstance::kNodeContextIndex);
    }

    // Must be out of HandleScope
    StartupData blob =
        creator.CreateBlob(SnapshotCreator::FunctionCodeHandling::kClear);
    CHECK(blob.CanBeRehashed());
    // Must be done while the snapshot creator isolate is entered i.e. the
    // creator is still alive.
    main_instance->Dispose();
    result = FormatBlob(&blob, isolate_data_indexes);
    delete blob.data;
  }

  per_process::v8_platform.Platform()->UnregisterIsolate(isolate);
  return result;
}
}  // namespace node
