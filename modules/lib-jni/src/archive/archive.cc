#include <proxy/proxy.h>
#include <pystring.h>

#include <codec/7z.h>
#include <codec/archive.h>
#include <codec/zip.h>
#include <err.h>
#include <java/array.h>
#include <java/entry.h>
#include <java/opaque_ptr.h>
#include <java/string.h>
#include <legacy/bundle.h>
#include <log.h>

#include "java/buffer.h"
#include "string_cvt.h"

namespace ysm::lib::archive {
namespace {
PRO_DEF_MEM_DISPATCH(MemList, List);
PRO_DEF_MEM_DISPATCH(MemGetFile, GetFile);

struct ArchiveFacade : pro::facade_builder
    ::add_convention<MemList,
                     absl::StatusOr<jobjectArray>(JNIEnv_* env, jstring path,
                                                  jint type)>
    ::add_convention<MemGetFile,
                     absl::StatusOr<jobject>(JNIEnv_* env, jstring file_name,
                                             jboolean dry_run)>
    ::support_destruction<pro::constraint_level::nontrivial>
    ::build {};

using ArchiveProxy = pro::proxy<ArchiveFacade>;

template <typename ArchiveType>
struct Wrapper {
    codec::Archive<ArchiveType, typename ArchiveType::EntryType> archive;  // msvc 连这都推导不出
    BufferManaged cache;

    explicit Wrapper(const fs::path& path) : archive(path) {}

    absl::StatusOr<jobjectArray> List(JNIEnv_* env, jstring path_obj,
                                      jint type) {
        std::string_view path = decltype(archive)::kRoot;
        std::string path_holder;
        if (path_obj != nullptr) {
            path_holder = java::StrToU8(env, path_obj);
            path = path_holder;
        }
        jobjectArray result;
        if (type == 0) {
            result = java::ObjectArray(
                         env, java::StrType(env), archive.Files(path),
                         [](auto env, auto& str) {
                             return java::U8ToStr(env, std::string(str));
                         })
                         .release();
        } else {
            result = java::ObjectArray(
                         env, java::StrType(env), archive.Directories(path),
                         [](auto env, auto& str) {
                             return java::U8ToStr(env, std::string(str));
                         })
                         .release();
        }
        YSM_LOG_DEBUG("Listed archive path \"{}\": type={}, entries={}", path,
                      type, env->GetArrayLength(result));
        return result;
    }

    absl::StatusOr<jobject> GetFile(JNIEnv_* env, jstring file_name_obj,
                                    jboolean dry_run) {
        YSM_ASSERT(file_name_obj,
                   absl::InvalidArgumentError("file name is null"sv));
        auto file_name = java::StrToU8(env, file_name_obj);
        auto entry = archive[file_name];
        if (entry) {
            if (dry_run) {
                // 随便返回个东西，只要不是 null 就行
                YSM_LOG_DEBUG("Found archive entry \"{}\" (dry run)",
                              file_name);
                return file_name_obj;
            }
            YSM_RETURN_IF_ERROR(archive.Extract(entry.value(), cache));
            YSM_LOG_DEBUG("Extracted archive entry \"{}\": {} bytes",
                          file_name, cache.size());
            return env->NewDirectByteBuffer(
                cache.data(), static_cast<jlong>(cache.size()));
        }
        YSM_LOG_DEBUG("Archive entry not found: \"{}\"", file_name);
        return nullptr;
    }

    absl::Status Ok() { return archive.Ok(); }
};

template <typename ArchiveType>
absl::StatusOr<jlong> Allocate(const fs::path& path) {
    auto ptr = std::make_unique<Wrapper<ArchiveType>>(path);
    auto status = ptr->Ok();
    if (status.ok()) {
        return java::MakeOpaquePtr<ArchiveProxy>(std::move(ptr));
    }
    YSM_LOG_DEBUG("Failed to open archive \"{}\": {}", PathToU8(path),
                  status.message());
    return status;
}
}  // namespace

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/NativeArchive;nCreate(Ljava/lang/String;)J",
    (archive_path)) {
    YSM_ASSERT(archive_path,
               absl::InvalidArgumentError("path is null"sv));
    auto path = java::StrToPath(env, archive_path);
    if (path.has_extension()) {
        auto ext = pystring::lower(PathToU8(path.extension()));
        absl::StatusOr<jlong> result = jlong{0};
        if (ext == ".zip"sv) {
            result = Allocate<codec::Zip>(path);
        } else if (ext == ".7z"sv) {
            result = Allocate<codec::SevenZip>(path);
        } else if (ext == ".ysm"sv) {
            result = Allocate<legacy::bundle::LegacyBundle>(path);
        }
        if (result.ok() && *result != 0) {
            YSM_LOG_DEBUG("Opened archive: path=\"{}\", format={}",
                          PathToU8(path), ext);
        }
        return result;
    }
    YSM_LOG_DEBUG("Unsupported archive path: \"{}\"", PathToU8(path));
    return jlong{0};
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/NativeArchive;nList(JLjava/lang/String;I)[Ljava/lang/String;",
    (ptr, path, type)) {
    YSM_DECLARE_OR_RETURN(archive, java::CastOpaquePtr<ArchiveProxy>(ptr));
    return (*archive)->List(env, path, type);
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/NativeArchive;nGetFile(JLjava/lang/String;Z)Ljava/lang/Object;",
    (ptr, file_name, dry_run)) {
    YSM_DECLARE_OR_RETURN(archive, java::CastOpaquePtr<ArchiveProxy>(ptr));
    return (*archive)->GetFile(env, file_name, dry_run);
}
}  // namespace ysm::lib::archive
