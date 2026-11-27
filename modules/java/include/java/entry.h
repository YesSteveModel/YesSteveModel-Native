#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "err.h"
#include "java/class.h"
#include "java/type.h"
#include "log.h"

namespace ysm::java {
template <std::size_t N>
struct FixedString {
    char value[N]{};

    consteval FixedString() = default;

    consteval FixedString(const char (&text)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = text[i];
        }
    }

    [[nodiscard]] constexpr std::size_t Size() const noexcept {
        return N - 1;
    }

    [[nodiscard]] constexpr const char* c_str() const noexcept {
        return value;
    }

    [[nodiscard]] constexpr std::string_view View() const noexcept {
        return {value, Size()};
    }

    template <std::size_t Pos, std::size_t Count>
    [[nodiscard]] consteval auto Substr() const {
        static_assert(Pos + Count <= N - 1);

        FixedString<Count + 1> result;
        for (std::size_t i = 0; i < Count; ++i) {
            result.value[i] = value[Pos + i];
        }
        result.value[Count] = '\0';
        return result;
    }
};

template <std::size_t N>
FixedString(const char (&)[N]) -> FixedString<N>;

namespace internal {
struct DefaultFallback {};

template <typename>
inline constexpr bool kDependentFalse = false;

template <std::size_t... Ns>
consteval auto Concat(const FixedString<Ns>&... parts) {
    constexpr std::size_t kSize = (0 + ... + (Ns - 1)) + 1;
    FixedString<kSize> result;
    std::size_t offset = 0;

    auto append = [&]<std::size_t M>(const FixedString<M>& part) {
        for (std::size_t i = 0; i + 1 < M; ++i) {
            result.value[offset++] = part.value[i];
        }
    };
    (append(parts), ...);

    result.value[offset] = '\0';
    return result;
}

template <FixedString Desc>
consteval std::size_t MethodSeparator() {
    return Desc.View().find(';');
}

template <FixedString Desc>
consteval std::size_t ArgumentBegin() {
    const auto separator = MethodSeparator<Desc>();
    if (separator == std::string_view::npos) {
        return std::string_view::npos;
    }
    return Desc.View().find('(', separator + 1);
}

template <FixedString Desc>
consteval std::size_t ArgumentEnd() {
    const auto begin = ArgumentBegin<Desc>();
    if (begin == std::string_view::npos) {
        return std::string_view::npos;
    }
    return Desc.View().find(')', begin + 1);
}

template <FixedString Desc>
consteval bool IsEntryDescriptor() {
    constexpr auto desc = Desc.View();
    constexpr auto separator = MethodSeparator<Desc>();
    constexpr auto argument_begin = ArgumentBegin<Desc>();
    constexpr auto argument_end = ArgumentEnd<Desc>();
    return desc.size() >= 6 && desc.front() == 'L' &&
           separator != std::string_view::npos && separator > 1 &&
           argument_begin != std::string_view::npos &&
           argument_begin > separator + 1 &&
           argument_end != std::string_view::npos &&
           argument_end > argument_begin && argument_end + 1 < desc.size();
}

template <FixedString Desc>
consteval bool MethodStartsWithN() {
    constexpr auto separator = MethodSeparator<Desc>();
    constexpr auto argument_begin = ArgumentBegin<Desc>();
    if constexpr (separator == std::string_view::npos ||
                  argument_begin == std::string_view::npos) {
        return false;
    } else {
        return argument_begin > separator + 1 &&
               Desc.View()[separator + 1] == 'n';
    }
}

template <FixedString Desc>
struct EntryDescriptor {
    static_assert(IsEntryDescriptor<Desc>(),
                  "Illegal JNI entry descriptor");
    static_assert(MethodStartsWithN<Desc>(),
                  "JNI entry method name must start with 'n'");

    static constexpr auto kDesc = Desc.View();
    static constexpr auto kMethodSeparator = MethodSeparator<Desc>();
    static constexpr auto kArgumentBegin = ArgumentBegin<Desc>();
    static constexpr auto kArgumentEnd = ArgumentEnd<Desc>();

    static constexpr auto kClassName =
        Desc.template Substr<1, kMethodSeparator - 1>();
    static constexpr auto kMethodName =
        Desc.template Substr<kMethodSeparator + 1,
                             kArgumentBegin - kMethodSeparator - 1>();
    static constexpr auto kSignature =
        Desc.template Substr<kArgumentBegin, Desc.Size() - kArgumentBegin>();
    static constexpr auto kReturnDescriptor =
        Desc.template Substr<kArgumentEnd + 1,
                             Desc.Size() - kArgumentEnd - 1>();

    static constexpr std::size_t kSimpleClassBegin = [] {
        constexpr auto name = kClassName.View();
        constexpr auto separator = name.rfind('/');
        if constexpr (separator == std::string_view::npos) {
            return std::size_t{0};
        } else {
            return separator + 1;
        }
    }();
    static constexpr auto kSimpleClassName =
        kClassName.template Substr<kSimpleClassBegin,
                                   kClassName.Size() - kSimpleClassBegin>();
    static constexpr auto kLoggingScopeName =
        Concat(kSimpleClassName, FixedString{"."}, kMethodName);
};

consteval std::size_t DescriptorTypeEnd(std::string_view desc,
                                        std::size_t begin,
                                        bool allow_void = false) {
    if (begin >= desc.size()) {
        return std::string_view::npos;
    }

    switch (desc[begin]) {
        case 'V':
            return allow_void ? begin + 1 : std::string_view::npos;
        case 'Z':
        case 'B':
        case 'C':
        case 'S':
        case 'I':
        case 'J':
        case 'F':
        case 'D':
            return begin + 1;
        case '[':
            return DescriptorTypeEnd(desc, begin + 1, false);
        case 'L': {
            const auto end = desc.find(';', begin + 1);
            if (end == std::string_view::npos || end == begin + 1) {
                return std::string_view::npos;
            }
            for (auto i = begin + 1; i < end; ++i) {
                if (desc[i] == '.' || desc[i] == '[' || desc[i] == ';') {
                    return std::string_view::npos;
                }
            }
            return end + 1;
        }
        default:
            return std::string_view::npos;
    }
}

template <FixedString Desc>
struct EntrySignature {
    using Descriptor = EntryDescriptor<Desc>;

    static consteval std::size_t CountArguments() {
        std::size_t count = 0;
        auto offset = Descriptor::kArgumentBegin + 1;
        while (offset < Descriptor::kArgumentEnd) {
            offset = DescriptorTypeEnd(Desc.View(), offset);
            if (offset == std::string_view::npos ||
                offset > Descriptor::kArgumentEnd) {
                return std::string_view::npos;
            }
            ++count;
        }
        return offset == Descriptor::kArgumentEnd ? count
                                                  : std::string_view::npos;
    }

    static constexpr auto kArgumentCount = CountArguments();
    static_assert(kArgumentCount != std::string_view::npos,
                  "Illegal JNI argument descriptor");

    template <std::size_t Index>
    static consteval std::size_t ArgumentOffset() {
        static_assert(Index < kArgumentCount,
                      "JNI argument index is out of range");
        auto offset = Descriptor::kArgumentBegin + 1;
        for (std::size_t i = 0; i < Index; ++i) {
            offset = DescriptorTypeEnd(Desc.View(), offset);
        }
        return offset;
    }

    static constexpr auto kReturnOffset = Descriptor::kArgumentEnd + 1;
    static constexpr auto kReturnEnd =
        DescriptorTypeEnd(Desc.View(), kReturnOffset, true);
    static_assert(kReturnEnd == Desc.Size(),
                  "Illegal JNI return descriptor");
};

template <FixedString Desc, std::size_t Pos>
struct DescriptorJniType {
    static constexpr auto kDesc = Desc.View();
    static constexpr auto kCode = kDesc[Pos];

    using type = typename decltype([] consteval {
        if constexpr (kCode == 'Z') {
            return std::type_identity<jboolean>{};
        } else if constexpr (kCode == 'B') {
            return std::type_identity<jbyte>{};
        } else if constexpr (kCode == 'C') {
            return std::type_identity<jchar>{};
        } else if constexpr (kCode == 'S') {
            return std::type_identity<jshort>{};
        } else if constexpr (kCode == 'I') {
            return std::type_identity<jint>{};
        } else if constexpr (kCode == 'J') {
            return std::type_identity<jlong>{};
        } else if constexpr (kCode == 'F') {
            return std::type_identity<jfloat>{};
        } else if constexpr (kCode == 'D') {
            return std::type_identity<jdouble>{};
        } else if constexpr (kCode == 'V') {
            return std::type_identity<void>{};
        } else if constexpr (kCode == 'L') {
            constexpr auto end = DescriptorTypeEnd(kDesc, Pos);
            constexpr auto object = kDesc.substr(Pos, end - Pos);
            if constexpr (object == "Ljava/lang/String;") {
                return std::type_identity<jstring>{};
            } else if constexpr (object == "Ljava/lang/Class;") {
                return std::type_identity<jclass>{};
            } else if constexpr (object == "Ljava/lang/Throwable;") {
                return std::type_identity<jthrowable>{};
            } else {
                return std::type_identity<jobject>{};
            }
        } else if constexpr (kCode == '[') {
            constexpr auto element = kDesc[Pos + 1];
            if constexpr (element == 'Z') {
                return std::type_identity<jbooleanArray>{};
            } else if constexpr (element == 'B') {
                return std::type_identity<jbyteArray>{};
            } else if constexpr (element == 'C') {
                return std::type_identity<jcharArray>{};
            } else if constexpr (element == 'S') {
                return std::type_identity<jshortArray>{};
            } else if constexpr (element == 'I') {
                return std::type_identity<jintArray>{};
            } else if constexpr (element == 'J') {
                return std::type_identity<jlongArray>{};
            } else if constexpr (element == 'F') {
                return std::type_identity<jfloatArray>{};
            } else if constexpr (element == 'D') {
                return std::type_identity<jdoubleArray>{};
            } else {
                return std::type_identity<jobjectArray>{};
            }
        } else {
            static_assert(kDependentFalse<decltype(kCode)>,
                          "Unsupported JNI descriptor type");
        }
    }())::type;
};

template <typename... Args>
struct EntryArgumentList {};

template <FixedString Desc, std::size_t... Indices>
consteval auto MakeEntryArgumentList(std::index_sequence<Indices...>) {
    return EntryArgumentList<
        typename DescriptorJniType<
            Desc, EntrySignature<Desc>::template ArgumentOffset<Indices>()>::
            type...>{};
}

template <FixedString Desc>
using EntryArgumentListT = decltype(MakeEntryArgumentList<Desc>(
    std::make_index_sequence<EntrySignature<Desc>::kArgumentCount>{}));

template <FixedString Desc>
using EntryReturnT = typename DescriptorJniType<
    Desc, EntrySignature<Desc>::kReturnOffset>::type;

template <FixedString Desc, bool Hot = false>
using EntryImplementationResultT = std::conditional_t<Hot, EntryReturnT<Desc>, std::conditional_t<
    std::is_same_v<EntryReturnT<Desc>, void> ||
        std::is_same_v<EntryReturnT<Desc>, jboolean>,
    absl::Status, absl::StatusOr<EntryReturnT<Desc>>>>;

template <FixedString Desc, bool Hot, typename Arguments = EntryArgumentListT<Desc>>
struct EntryImplementationPointer;

template <FixedString Desc, bool Hot, typename... Args>
struct EntryImplementationPointer<Desc, Hot, EntryArgumentList<Args...>> {
    using type = EntryImplementationResultT<Desc, Hot> (*)(JNIEnv_*, Args...) noexcept(Hot);
};

template <FixedString Desc, bool Hot = false>
using EntryImplementationPointerT =
    typename EntryImplementationPointer<Desc, Hot>::type;

struct EntryRegistration {
    CStringView class_name;
    CStringView method_name;
    CStringView signature;
    void* func;
};

struct EntryNode {
    EntryRegistration registration;
    EntryNode* next = nullptr;
};

enum class EntryRegistryPhase {
    kCollecting,
    kBinding,
    kBound,
    kFailed,
};

class EntryRegistry final {
   public:
    constexpr EntryRegistry() noexcept = default;

    EntryRegistry(const EntryRegistry&) = delete;
    EntryRegistry& operator=(const EntryRegistry&) = delete;
    EntryRegistry(EntryRegistry&&) = delete;
    EntryRegistry& operator=(EntryRegistry&&) = delete;

    void Link(EntryNode* node) noexcept;
    [[nodiscard]] absl::Status BeginBinding();
    void FinishBinding(bool success) noexcept;

    [[nodiscard]] constexpr const EntryNode* entries() const noexcept {
        return head_;
    }

    [[nodiscard]] constexpr EntryRegistryPhase phase() const noexcept {
        return phase_;
    }

   private:
    EntryNode* head_ = nullptr;
    EntryRegistryPhase phase_ = EntryRegistryPhase::kCollecting;
};

void LinkEntry(EntryNode* node) noexcept;
[[nodiscard]] const EntryNode* RegisteredEntries() noexcept;
[[nodiscard]] absl::StatusOr<std::vector<const EntryRegistration*>>
CollectAndValidateEntries(const EntryNode* head);

template <typename R, FixedString Desc>
consteval bool ReturnTypeMatchesDescriptor() {
    constexpr auto ret = EntryDescriptor<Desc>::kReturnDescriptor.View();
    if constexpr (JavaObjectType<R>) {
        return (ret.size() >= 2 && ret.front() == 'L' &&
                ret.back() == ';') ||
               (!ret.empty() && ret.front() == '[');
    } else if constexpr (std::is_same_v<R, jboolean>) {
        return ret == "Z";
    } else if constexpr (std::is_same_v<R, jbyte>) {
        return ret == "B";
    } else if constexpr (std::is_same_v<R, jchar>) {
        return ret == "C";
    } else if constexpr (std::is_same_v<R, jshort>) {
        return ret == "S";
    } else if constexpr (std::is_same_v<R, jint> ||
                         std::is_same_v<R, int>) {
        return ret == "I";
    } else if constexpr (std::is_same_v<R, jlong>) {
        return ret == "J";
    } else if constexpr (std::is_same_v<R, jfloat>) {
        return ret == "F";
    } else if constexpr (std::is_same_v<R, jdouble>) {
        return ret == "D";
    } else {
        return false;
    }
}

template <typename R, auto Fallback>
constexpr R FailureValue() {
    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(Fallback)>,
                                 DefaultFallback>) {
        return R{};
    } else {
        static_assert(std::is_convertible_v<decltype(Fallback), R>,
                      "JNI fallback must be convertible to the return type");
        return static_cast<R>(Fallback);
    }
}

template <FixedString Desc, typename Callable>
bool InvokeStatus(Callable&& func) {
    using Descriptor = EntryDescriptor<Desc>;
    LoggingScope scope{Descriptor::kLoggingScopeName.View(), true};
    try {
        auto status = std::invoke(std::forward<Callable>(func));
        if (status.ok()) [[likely]] {
            return true;
        }
        YSM_LOG_DEBUG_EVERY_N_SEC(
            5, "Native operation failed with status: {}."sv, status.ToString());
    } catch (const std::exception& ex) {
        YSM_LOG_DEBUG_EVERY_N_SEC(
            5, "Native operation threw an exception: {}."sv, ex.what());
    } catch (...) {
        YSM_LOG_DEBUG_EVERY_N_SEC(
            5, "Native operation threw an unknown exception."sv);
    }
    return false;
}

template <JavaType R, FixedString Desc, typename Callable>
R InvokeStatusOr(Callable&& func, R fallback) {
    using Descriptor = EntryDescriptor<Desc>;
    LoggingScope scope{Descriptor::kLoggingScopeName.View(), true};
    try {
        auto result = std::invoke(std::forward<Callable>(func));
        if (result.ok()) [[likely]] {
            return std::move(result).value();
        }
        YSM_LOG_DEBUG_EVERY_N_SEC(
            5, "Native operation failed with status: {}."sv, result.status().ToString());
    } catch (const std::exception& ex) {
        YSM_LOG_DEBUG_EVERY_N_SEC(
            5, "Native operation threw an exception: {}."sv, ex.what());
    } catch (...) {
        YSM_LOG_DEBUG_EVERY_N_SEC(
            5, "Native operation threw an unknown exception."sv);
    }
    return fallback;
}

template <auto Impl, FixedString Desc,
          auto Fallback = DefaultFallback{}, typename Signature = decltype(Impl)>
struct EntryThunk {
    static_assert(kDependentFalse<Signature>,
                  "JNI implementation must return absl::Status or "
                  "absl::StatusOr<R> and take JNIEnv_* as its first argument");
};

template <auto Impl, FixedString Desc, auto Fallback, typename R, typename... Args>
struct EntryThunk<Impl, Desc, Fallback, R (*)(JNIEnv_*, Args...) noexcept> {
    static R JNICALL Invoke(JNIEnv_* env, jclass, Args... args) noexcept {
        return Impl(env, args...);
    }
};

template <auto Impl, FixedString Desc, auto Fallback, typename... Args>
struct EntryThunk<Impl, Desc, Fallback,
                  absl::Status (*)(JNIEnv_*, Args...)> {
    using Descriptor = EntryDescriptor<Desc>;

    static_assert((JavaType<Args> && ...),
                  "JNI arguments must satisfy java::JavaType");
    static_assert(
        std::is_same_v<std::remove_cvref_t<decltype(Fallback)>,
                       DefaultFallback>,
        "absl::Status JNI entries do not accept a custom fallback");

    static constexpr auto kReturnDescriptor =
        Descriptor::kReturnDescriptor.View();
    static_assert(kReturnDescriptor == "V" || kReturnDescriptor == "Z",
                  "absl::Status JNI entries require a void or boolean "
                  "descriptor return type");

    using JniReturn =
        std::conditional_t<kReturnDescriptor == "Z", jboolean, void>;

    static JniReturn JNICALL Invoke(JNIEnv_* env, jclass, Args... args) {
        const bool ok =
            InvokeStatus<Desc>([&] { return Impl(env, args...); });
        if constexpr (std::is_same_v<JniReturn, jboolean>) {
            return ok ? JNI_TRUE : JNI_FALSE;
        }
    }
};

template <auto Impl, FixedString Desc, auto Fallback, JavaType R,
          typename... Args>
struct EntryThunk<Impl, Desc, Fallback,
                  absl::StatusOr<R> (*)(JNIEnv_*, Args...)> {
    using Descriptor = EntryDescriptor<Desc>;

    static_assert((JavaType<Args> && ...),
                  "JNI arguments must satisfy java::JavaType");
    static_assert(ReturnTypeMatchesDescriptor<R, Desc>(),
                  "absl::StatusOr<R> does not match the descriptor return "
                  "type");

    static R JNICALL Invoke(JNIEnv_* env, jclass, Args... args) {
        return InvokeStatusOr<R, Desc>([&] { return Impl(env, args...); },
                                       FailureValue<R, Fallback>());
    }
};
}  // namespace internal

template <auto Impl, FixedString Desc,
          auto Fallback = internal::DefaultFallback{}>
class EntryRegistrar final {
   public:
    EntryRegistrar() noexcept : node_{CreateRegistration(), nullptr} {
        internal::LinkEntry(&node_);
    }

    EntryRegistrar(const EntryRegistrar&) = delete;
    EntryRegistrar& operator=(const EntryRegistrar&) = delete;
    EntryRegistrar(EntryRegistrar&&) = delete;
    EntryRegistrar& operator=(EntryRegistrar&&) = delete;

   private:
    using Descriptor = internal::EntryDescriptor<Desc>;
    using Thunk = internal::EntryThunk<Impl, Desc, Fallback>;

    static internal::EntryRegistration CreateRegistration() noexcept {
        auto func = &Thunk::Invoke;
        static_assert(sizeof(func) == sizeof(void*));
        return {
            CStringView{Descriptor::kClassName.c_str(),
                        Descriptor::kClassName.Size()},
            CStringView{Descriptor::kMethodName.c_str(),
                        Descriptor::kMethodName.Size()},
            CStringView{Descriptor::kSignature.c_str(),
                        Descriptor::kSignature.Size()},
            reinterpret_cast<void*>(func),
        };
    }

    internal::EntryNode node_;
};

#define YSM_JNI_ENTRY_INTERNAL_UNPAREN(...) __VA_ARGS__
#define YSM_JNI_ENTRY_INTERNAL_PARAMETER(name) , auto name
#define YSM_JNI_ENTRY_INTERNAL_COUNT_PARAMETER(name) +1

#define YSM_JNI_ENTRY_INTERNAL_PARAMETERS(names)                         \
    YSM_PP_FOR_EACH(YSM_JNI_ENTRY_INTERNAL_PARAMETER,                    \
                    YSM_JNI_ENTRY_INTERNAL_UNPAREN names)

#define YSM_JNI_ENTRY_INTERNAL_PARAMETER_COUNT(names)                    \
    (0 YSM_PP_FOR_EACH(YSM_JNI_ENTRY_INTERNAL_COUNT_PARAMETER,           \
                       YSM_JNI_ENTRY_INTERNAL_UNPAREN names))

#define YSM_JNI_ENTRY_INTERNAL_FUNCTION(id)                              \
    YSM_MACROS_CONCAT_NAME(YsmJniEntryImplementation, id)
#define YSM_JNI_ENTRY_INTERNAL_REGISTRAR(id)                             \
    YSM_MACROS_CONCAT_NAME(kYsmJniEntryRegistrar, id)

#define YSM_JNI_ENTRY_INTERNAL_SIGNATURE(desc, names, id, hot)           \
    static auto YSM_JNI_ENTRY_INTERNAL_FUNCTION(id)(                     \
        JNIEnv_* env YSM_JNI_ENTRY_INTERNAL_PARAMETERS(names)) noexcept(hot) \
        ->::ysm::java::internal::EntryImplementationResultT<desc, hot>

#define YSM_JNI_ENTRY_INTERNAL_IMPL(desc, names, id, hot, ...)           \
    static_assert(                                                       \
        ::ysm::java::internal::EntrySignature<desc>::kArgumentCount ==   \
            YSM_JNI_ENTRY_INTERNAL_PARAMETER_COUNT(names),               \
        "JNI descriptor argument count does not match the parameter "    \
        "name count");                                                  \
    YSM_JNI_ENTRY_INTERNAL_SIGNATURE(desc, names, id, hot);              \
    [[maybe_unused]] static ::ysm::java::EntryRegistrar<                 \
        static_cast<                                                     \
            ::ysm::java::internal::EntryImplementationPointerT<desc, hot>>( \
            &YSM_JNI_ENTRY_INTERNAL_FUNCTION(id)),                       \
        desc __VA_OPT__(,) __VA_ARGS__>                                  \
        YSM_JNI_ENTRY_INTERNAL_REGISTRAR(id);                            \
    YSM_JNI_ENTRY_INTERNAL_SIGNATURE(desc, names, id, hot)

#define YSM_JNI_ENTRY(desc, names, ...)                                  \
    YSM_JNI_ENTRY_INTERNAL_IMPL(desc, names, __COUNTER__, false, __VA_ARGS__)

// Hot entries own their exception boundary so they can preserve committed work.
#define YSM_JNI_HOT_ENTRY(desc, names)                                   \
    YSM_JNI_ENTRY_INTERNAL_IMPL(desc, names, __COUNTER__, true)

absl::Status BindEntry(JNIEnv_* env);
}  // namespace ysm::java
