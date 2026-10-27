#!/usr/bin/env bash

set -u

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)" || exit 1
root="$script_dir"

conan_dir="$root/conan"
cache_dir="$conan_dir/cache"
profile_override="$conan_dir/profile-override"
base_recipe_dir="$conan_dir/base-recipe"
deps_dir="$root/third-party"

conan_exe=""
cmake_exe=""
ninja_exe=""
do_export=""
install_profile=""
build_profile=""
toolchain_path=""
native_profile="native"
build_env_profile=""
build_type="Release"
deps_only=""

usage() {
    cat <<EOF
Usage:
  $(basename "$0") setup [--toolchain <toolchain_path>] [--cache <path>]
  $(basename "$0") build <profile> [--toolchain <toolchain_path>] [--cache <path>] [--deps-only] [--debug]

Build mode defaults to Release. Pass --debug to use Debug.
Pass --cache to set CONAN_HOME explicitly; the default is conan/cache.
Setup and build use <toolchain_path>/tool/conan/bin/conan when --toolchain is set, otherwise PATH.
Without --toolchain, build requires profile native and uses host profile default + conan/profile-override and build profile default.
With --toolchain, build uses <toolchain_path>/conan/<profile> + conan/profile-override and an OS build profile; profile native is not allowed.
conan build uses <toolchain_path>/tool/cmake/bin/cmake when --toolchain is set, otherwise PATH cmake.
Build and generator directories are build/<profile>-<build-type>.
Pass --deps-only to stop after conan install.
EOF
}

usage_error() {
    usage
    exit 2
}

parse_toolchain_arg() {
    if (($# < 2)); then
        echo "error: --toolchain requires a toolchain path." >&2
        usage_error
    fi
    toolchain_path="$2"
}

parse_cache_arg() {
    if (($# < 2)); then
        echo "error: --cache requires a cache path." >&2
        usage_error
    fi
    cache_dir="$2"
}

parse_setup_args() {
    while (($#)); do
        case "$1" in
            -h|--help)
                usage
                exit 0
                ;;
            --toolchain)
                parse_toolchain_arg "$@"
                shift 2
                ;;
            --cache)
                parse_cache_arg "$@"
                shift 2
                ;;
            *)
                echo "error: unknown setup argument: $1" >&2
                usage_error
                ;;
        esac
    done
}

parse_build_args() {
    while (($#)); do
        case "$1" in
            -h|--help)
                usage
                exit 0
                ;;
            --toolchain)
                parse_toolchain_arg "$@"
                shift 2
                ;;
            --cache)
                parse_cache_arg "$@"
                shift 2
                ;;
            --deps-only)
                deps_only=1
                shift
                ;;
            --debug)
                build_type="Debug"
                shift
                ;;
            *)
                echo "error: unknown build argument: $1" >&2
                usage_error
                ;;
        esac
    done
}

while (($#)); do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        setup)
            if [[ -n "$do_export" ]]; then
                echo "error: setup was specified more than once." >&2
                usage_error
            fi
            do_export=1
            shift
            parse_setup_args "$@"
            break
            ;;
        build)
            if (($# < 2)); then
                echo "error: build requires a profile name." >&2
                usage_error
            fi
            if [[ -n "$build_profile" ]]; then
                echo "error: build was specified more than once." >&2
                usage_error
            fi
            install_profile="$2"
            build_profile="$2"
            shift 2
            parse_build_args "$@"
            break
            ;;
        *)
            echo "error: unknown command: $1" >&2
            usage_error
            ;;
    esac
done

if [[ -z "$do_export" && -z "$install_profile" && -z "$build_profile" ]]; then
    usage_error
fi

if [[ -n "$toolchain_path" && ! -d "$toolchain_path" ]]; then
    echo "error: toolchain path does not exist: $toolchain_path" >&2
    exit 1
fi

validate_profile_mode() {
    [[ -n "$install_profile" ]] || return 0

    if [[ "$install_profile" == "$native_profile" ]]; then
        if [[ -n "$toolchain_path" ]]; then
            echo "error: profile $native_profile cannot be used with --toolchain." >&2
            return 1
        fi
        return 0
    fi

    if [[ -z "$toolchain_path" ]]; then
        echo "error: without --toolchain, build profile must be $native_profile." >&2
        return 1
    fi
}

validate_profile_mode || exit 1

check_executable() {
    local exe="$1"
    local label="$2"

    if [[ "$exe" == */* || "$exe" == *\\* ]]; then
        if [[ -x "$exe" ]]; then
            return 0
        fi
    elif command -v -- "$exe" >/dev/null 2>&1; then
        return 0
    fi

    echo "error: $label does not exist or is not on PATH: $exe" >&2
    return 1
}

detect_build_env_profile() {
    case "$(uname -s 2>/dev/null || echo unknown)" in
        MINGW*|MSYS*|CYGWIN*)
            build_env_profile="win-x64"
            ;;
        Darwin*)
            build_env_profile="macos-arm64"
            ;;
        Linux*)
            build_env_profile="linux-x64"
            ;;
        *)
            echo "error: unsupported build operating system: $(uname -s 2>/dev/null || echo unknown)" >&2
            return 1
            ;;
    esac
}

if [[ -n "$toolchain_path" && -n "$build_profile" ]]; then
    detect_build_env_profile || exit 1
fi

mkdir -p -- "$cache_dir" || exit 1
cache_dir="$(cd -- "$cache_dir" && pwd -P)" || exit 1
export CONAN_HOME="$cache_dir"

if [[ -n "$toolchain_path" ]]; then
    conan_exe="$toolchain_path/tool/conan/bin/conan"
else
    conan_exe="conan"
fi

if ! check_executable "$conan_exe" "Conan executable"; then
    echo "       Pass --toolchain or make conan available on PATH." >&2
    exit 1
fi

if [[ -n "$build_profile" && -z "$deps_only" ]]; then
    if [[ -n "$toolchain_path" ]]; then
        cmake_exe="$toolchain_path/tool/cmake/bin/cmake"
        ninja_exe="$toolchain_path/tool/ninja/bin/ninja"
    else
        cmake_exe="cmake"
        ninja_exe="ninja"
    fi

    if ! check_executable "$cmake_exe" "CMake executable"; then
        echo "       Pass --toolchain or make cmake available on PATH." >&2
        exit 1
    fi
    if ! check_executable "$ninja_exe" "Ninja executable"; then
        echo "       Pass --toolchain or make ninja available on PATH." >&2
        exit 1
    fi
fi

run_conan() {
    printf '+'
    printf ' %q' "$conan_exe" "$@"
    printf '\n'
    "$conan_exe" "$@"
}

export_recipe() {
    run_conan export "$1" --user=ysm --channel=stable
}

export_recipes() {
    if [[ ! -f "$base_recipe_dir/conanfile.py" ]]; then
        echo "error: base recipe does not exist: $base_recipe_dir" >&2
        exit 1
    fi
    if [[ ! -d "$deps_dir" ]]; then
        echo "error: dependency recipes directory does not exist: $deps_dir" >&2
        exit 1
    fi

    export_recipe "$base_recipe_dir" || return 1

    local found_recipe=""
    local recipe_dir
    for recipe_dir in "$deps_dir"/*; do
        [[ -d "$recipe_dir" ]] || continue
        [[ -f "$recipe_dir/conanfile.py" ]] || continue

        found_recipe=1
        export_recipe "$recipe_dir" || return 1
    done

    if [[ -z "$found_recipe" ]]; then
        echo "error: no dependency recipes found in $deps_dir" >&2
        return 1
    fi
}

run_conan_with_profile() {
    local conan_command="$1"
    local profile="$2"

    if [[ ! -f "$profile_override" ]]; then
        echo "error: project profile override does not exist: $profile_override" >&2
        return 1
    fi

    if [[ -n "$toolchain_path" ]]; then
        if [[ ! -f "$toolchain_path/conan/$profile" ]]; then
            echo "error: toolchain host profile does not exist: $toolchain_path/conan/$profile" >&2
            return 1
        fi
        if [[ ! -f "$toolchain_path/conan/$build_env_profile" ]]; then
            echo "error: toolchain build profile does not exist: $toolchain_path/conan/$build_env_profile" >&2
            return 1
        fi

        run_conan "$conan_command" "$root" \
            -pr:h "$toolchain_path/conan/$profile" \
            -pr:h "$profile_override" \
            -pr:b "$toolchain_path/conan/$build_env_profile" \
            --no-remote \
            --build=missing \
            -s "build_type=$build_type" \
            -c:h "user.target:profile=$profile" \
            --core-conf core.graph:compatibility_mode=optimized
    else
        run_conan profile detect -e || return $?
        run_conan "$conan_command" "$root" \
            -pr:h default \
            -pr:h "$profile_override" \
            -pr:b default \
            --no-remote \
            --build=missing \
            -s "build_type=$build_type" \
            -c:h "user.target:profile=$native_profile" \
            --core-conf core.graph:compatibility_mode=optimized
    fi
}

install_with_profile() {
    run_conan_with_profile install "$1" || return $?
    run_conan cache clean --build
}

build_with_profile() {
    run_conan_with_profile build "$1"
}

cd -- "$root" || exit 1

result=0
if [[ -n "$do_export" ]]; then
    export_recipes || result=1
fi

if [[ "$result" == 0 && -n "$install_profile" ]]; then
    if [[ -n "$deps_only" ]]; then
        install_with_profile "$install_profile" || result=1
    elif [[ -n "$build_profile" ]]; then
        build_with_profile "$build_profile" || result=1
    fi
fi

exit "$result"
