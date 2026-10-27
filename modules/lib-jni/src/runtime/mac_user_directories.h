#pragma once

#include <absl/status/statusor.h>
#include <jni.h>

namespace ysm::lib::runtime {

absl::StatusOr<jobjectArray> QueryMacUserDirectories(JNIEnv_* env);

}  // namespace ysm::lib::runtime
