@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%.") do set "ROOT=%%~fI"

set "CONAN_DIR=%ROOT%\conan"
set "CACHE_DIR=%CONAN_DIR%\cache"
set "PROFILE_OVERRIDE=%CONAN_DIR%\profile-override"
set "DEPS_DIR=%ROOT%\third-party"
set "BASE_RECIPE_DIR=%CONAN_DIR%\base-recipe"

set "CONAN_EXE="
set "CMAKE_EXE="
set "NINJA_EXE="
set "DO_EXPORT="
set "INSTALL_PROFILE="
set "BUILD_PROFILE="
set "TOOLCHAIN_PATH="
set "NATIVE_PROFILE=native"
set "BUILD_ENV_PROFILE=win-x64"
set "BUILD_TYPE=Release"
set "DEPS_ONLY="

:parse_args
if "%~1"=="" goto :after_args
if /I "%~1"=="-h" goto :usage_ok
if /I "%~1"=="--help" goto :usage_ok
if /I "%~1"=="setup" (
    if defined DO_EXPORT (
        echo error: setup was specified more than once.
        goto :usage_error
    )
    set "DO_EXPORT=1"
    shift
    goto :parse_setup_args
)
if /I "%~1"=="build" (
    if "%~2"=="" (
        echo error: build requires a profile name.
        goto :usage_error
    )
    if defined BUILD_PROFILE (
        echo error: build was specified more than once.
        goto :usage_error
    )
    set "INSTALL_PROFILE=%~2"
    set "BUILD_PROFILE=%~2"
    shift
    shift
    goto :parse_build_args
)

echo error: unknown command: %~1
goto :usage_error

:parse_setup_args
if "%~1"=="" goto :after_args
if /I "%~1"=="-h" goto :usage_ok
if /I "%~1"=="--help" goto :usage_ok
if /I "%~1"=="--toolchain" (
    if "%~2"=="" (
        echo error: --toolchain requires a toolchain path.
        goto :usage_error
    )
    set "TOOLCHAIN_PATH=%~2"
    shift
    shift
    goto :parse_setup_args
)
if /I "%~1"=="--cache" (
    if "%~2"=="" (
        echo error: --cache requires a cache path.
        goto :usage_error
    )
    set "CACHE_DIR=%~f2"
    shift
    shift
    goto :parse_setup_args
)

echo error: unknown setup argument: %~1
goto :usage_error

:parse_build_args
if "%~1"=="" goto :after_args
if /I "%~1"=="-h" goto :usage_ok
if /I "%~1"=="--help" goto :usage_ok
if /I "%~1"=="--toolchain" (
    if "%~2"=="" (
        echo error: --toolchain requires a toolchain path.
        goto :usage_error
    )
    set "TOOLCHAIN_PATH=%~2"
    shift
    shift
    goto :parse_build_args
)
if /I "%~1"=="--cache" (
    if "%~2"=="" (
        echo error: --cache requires a cache path.
        goto :usage_error
    )
    set "CACHE_DIR=%~f2"
    shift
    shift
    goto :parse_build_args
)
if /I "%~1"=="--deps-only" (
    set "DEPS_ONLY=1"
    shift
    goto :parse_build_args
)
if /I "%~1"=="--debug" (
    set "BUILD_TYPE=Debug"
    shift
    goto :parse_build_args
)

echo error: unknown build argument: %~1
goto :usage_error

:after_args
if not defined DO_EXPORT if not defined INSTALL_PROFILE goto :usage_error

if not defined TOOLCHAIN_PATH goto :toolchain_checked
if not exist "%TOOLCHAIN_PATH%\." (
    echo error: toolchain path does not exist: %TOOLCHAIN_PATH%
    exit /b 1
)
:toolchain_checked

if not defined INSTALL_PROFILE goto :profile_mode_checked
call :validate_profile_mode
if errorlevel 1 exit /b 1
:profile_mode_checked

set "CONAN_HOME=%CACHE_DIR%"
if not exist "%CACHE_DIR%\." mkdir "%CACHE_DIR%" || exit /b 1

if defined TOOLCHAIN_PATH (
    set "CONAN_EXE=%TOOLCHAIN_PATH%\tool\conan\bin\conan.exe"
) else (
    set "CONAN_EXE=conan"
)

call :check_executable "%CONAN_EXE%" "Conan executable"
if errorlevel 1 (
    echo        Pass --toolchain or make conan available on PATH.
    exit /b 1
)

if not defined BUILD_PROFILE goto :cmake_checked
if defined DEPS_ONLY goto :cmake_checked
if defined TOOLCHAIN_PATH (
    set "CMAKE_EXE=%TOOLCHAIN_PATH%\tool\cmake\bin\cmake.exe"
    set "NINJA_EXE=%TOOLCHAIN_PATH%\tool\ninja\bin\ninja.exe"
) else (
    set "CMAKE_EXE=cmake"
    set "NINJA_EXE=ninja"
)
call :check_executable "%CMAKE_EXE%" "CMake executable"
if errorlevel 1 (
    echo        Pass --toolchain or make cmake available on PATH.
    exit /b 1
)
call :check_executable "%NINJA_EXE%" "Ninja executable"
if errorlevel 1 (
    echo        Pass --toolchain or make ninja available on PATH.
    exit /b 1
)
:cmake_checked

pushd "%ROOT%" || exit /b 1

set "BOOTSTRAP_RESULT=0"
if not defined DO_EXPORT goto :after_export
call :export_recipes
if errorlevel 1 set "BOOTSTRAP_RESULT=1"

:after_export
if not "%BOOTSTRAP_RESULT%"=="0" goto :finish
if not defined INSTALL_PROFILE goto :finish
if defined DEPS_ONLY (
    call :install "%INSTALL_PROFILE%"
    if errorlevel 1 set "BOOTSTRAP_RESULT=1"
    goto :finish
)
if not defined BUILD_PROFILE goto :finish
call :build "%BUILD_PROFILE%"
if errorlevel 1 set "BOOTSTRAP_RESULT=1"

:finish
popd
exit /b %BOOTSTRAP_RESULT%

:check_executable
if exist "%~1" exit /b 0
where "%~1" >nul 2>nul
if not errorlevel 1 exit /b 0
echo error: %~2 does not exist or is not on PATH: %~1
exit /b 1

:validate_profile_mode
if /I "%INSTALL_PROFILE%"=="%NATIVE_PROFILE%" (
    if defined TOOLCHAIN_PATH (
        echo error: profile %NATIVE_PROFILE% cannot be used with --toolchain.
        exit /b 1
    )
    exit /b 0
)
if not defined TOOLCHAIN_PATH (
    echo error: without --toolchain, build profile must be %NATIVE_PROFILE%.
    exit /b 1
)
exit /b 0

:export_recipes
if not exist "%BASE_RECIPE_DIR%\conanfile.py" (
    echo error: base recipe does not exist: %BASE_RECIPE_DIR%
    exit /b 1
)
if not exist "%DEPS_DIR%\." (
    echo error: dependency recipes directory does not exist: %DEPS_DIR%
    exit /b 1
)

call :export_recipe "%BASE_RECIPE_DIR%"
if errorlevel 1 exit /b 1

set "FOUND_RECIPE="
for /f "delims=" %%D in ('dir /b /ad /on "%DEPS_DIR%" 2^>nul') do (
    if exist "%DEPS_DIR%\%%D\conanfile.py" (
        set "FOUND_RECIPE=1"
        call :export_recipe "%DEPS_DIR%\%%D"
        if errorlevel 1 exit /b 1
    )
)
if not defined FOUND_RECIPE (
    echo error: no dependency recipes found in %DEPS_DIR%
    exit /b 1
)
exit /b 0

:export_recipe
call :run_conan export "%~1" --user=ysm --channel=stable
exit /b %ERRORLEVEL%

:run_conan_with_profile
if not exist "%PROFILE_OVERRIDE%" (
    echo error: project profile override does not exist: %PROFILE_OVERRIDE%
    exit /b 1
)
if defined TOOLCHAIN_PATH (
    if not exist "%TOOLCHAIN_PATH%\conan\%~1" (
        echo error: toolchain host profile does not exist: %TOOLCHAIN_PATH%\conan\%~1
        exit /b 1
    )
    if not exist "%TOOLCHAIN_PATH%\conan\%BUILD_ENV_PROFILE%" (
        echo error: toolchain build profile does not exist: %TOOLCHAIN_PATH%\conan\%BUILD_ENV_PROFILE%
        exit /b 1
    )
    call :run_conan %~2 "%ROOT%" -pr:h "%TOOLCHAIN_PATH%\conan\%~1" -pr:h "%PROFILE_OVERRIDE%" -pr:b "%TOOLCHAIN_PATH%\conan\%BUILD_ENV_PROFILE%" --no-remote --build=missing -s build_type=%BUILD_TYPE% -c:h "user.target:profile=%~1" --core-conf core.graph:compatibility_mode=optimized
) else (
    call :run_conan profile detect -e
    if errorlevel 1 exit /b 1
    call :run_conan %~2 "%ROOT%" -pr:h default -pr:h "%PROFILE_OVERRIDE%" -s:h compiler.runtime=static -pr:b default --no-remote --build=missing -s build_type=%BUILD_TYPE% -c:h "user.target:profile=%NATIVE_PROFILE%" --core-conf core.graph:compatibility_mode=optimized
)
exit /b %ERRORLEVEL%

:install
call :run_conan_with_profile "%~1" install
if errorlevel 1 exit /b %ERRORLEVEL%
call :run_conan cache clean --build
exit /b %ERRORLEVEL%

:build
call :run_conan_with_profile "%~1" build
exit /b %ERRORLEVEL%

:run_conan
echo + "%CONAN_EXE%" %*
if exist "%CONAN_EXE%" (
    call "%CONAN_EXE%" %*
) else (
    call %CONAN_EXE% %*
)
exit /b %ERRORLEVEL%

:usage_ok
call :usage
exit /b 0

:usage_error
call :usage
exit /b 2

:usage
echo Usage:
echo   %~nx0 setup [--toolchain ^<toolchain_path^>] [--cache ^<path^>]
echo   %~nx0 build ^<profile^> [--toolchain ^<toolchain_path^>] [--cache ^<path^>] [--deps-only] [--debug]
echo.
echo Build mode defaults to Release. Pass --debug to use Debug.
echo Pass --cache to set CONAN_HOME explicitly; the default is conan/cache.
echo Setup and build use ^<toolchain_path^>/tool/conan/bin/conan.exe when --toolchain is set, otherwise PATH.
echo Without --toolchain, build requires profile %NATIVE_PROFILE% and uses host profile default + conan/profile-override and build profile default.
echo With --toolchain, build uses ^<toolchain_path^>/conan/^<profile^> + conan/profile-override and a win-x64 build profile; profile %NATIVE_PROFILE% is not allowed.
echo conan build uses ^<toolchain_path^>/tool/cmake/bin/cmake.exe when --toolchain is set, otherwise PATH cmake.
echo Build and generator directories are build/^<profile^>-^<build-type^>.
echo Pass --deps-only to stop after conan install.
exit /b 0
