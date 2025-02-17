#!/usr/bin/env bash
#
# Patch the VST3 SDK and replace all MSVC-isms so it can be compiled with
# winegcc. We do it this way instead of modifying the SDK directly so we don't
# have to fork multiple repositories and keep them up to date.
# If anyone knows a better way to get the SDK to compile with Win32 supports
# under winegcc without having to modify it then please let me know, because I'd
# rather not have to do this.
#
# Usage:
# patch-vst3-sdk.sh <sdk_directory>

set -euo pipefail

sdk_directory=$1
if [[ -z $sdk_directory ]]; then
  echo "Usage:"
  echo "patch-vst3-sdk.sh <sdk_directory>"
  exit 1
fi

# Use the proper libc functions instead of the MSVC intrinsics. These are also
# used in `fstring.cpp`, but there we will patch the entire file to use the
# standard POSIX/GCC string formatting facilities.
sed -i 's/\b_vsnprintf\b/vsnprintf/g;s/\b_snprintf\b/snprintf/g' "$sdk_directory/base/source/fdebug.cpp"

# Use the attributes and types from GCC
sed -i 's/defined(__MINGW32__)/defined(__WINE__)/g' "$sdk_directory/pluginterfaces/base/ftypes.h"

# There are some more places where the SDK includes better compatibility with
# GCC that we can use
# NOTE: We should **not** define __MINGW32__ globally, since that also breaks
#       Wine's headers in headache inducing ways
sed -i 's/defined(__MINGW32__)/defined(__WINE__)/g' "$sdk_directory/public.sdk/source/common/systemclipboard_win32.cpp"

# Use the string manipulation functions from the C standard library
sed -i 's/\bSMTG_OS_WINDOWS\b/0/g;s/\bSMTG_OS_LINUX\b/1/g' "$sdk_directory/base/source/fstring.cpp"
sed -i 's/\bSMTG_OS_WINDOWS\b/0/g;s/\bSMTG_OS_LINUX\b/1/g' "$sdk_directory/pluginterfaces/base/fstrdefs.h"
sed -i 's/\bSMTG_OS_WINDOWS\b/0/g;s/\bSMTG_OS_LINUX\b/1/g' "$sdk_directory/pluginterfaces/base/ustring.cpp"

# `Windows.h` expects `wchar_t`, and the above defines will cause us to use
# `char16_t` for string literals. This replacement targets a very specific line,
# so if the SDK gets updated, this fails, and we're getting a ton of `wchar_t`
# related compile errors, that's why. The previous sed call will have replaced
# `SMTG_OS_WINDOWS` with a 0 here.
sed -i 's/^	#if 0$/	#if __WINE__/' "$sdk_directory/pluginterfaces/base/fstrdefs.h"

# We'll need some careful replacements in the Linux definitions in `fstring.cpp`
# to use `wchar_t` instead of `char16_t`.
replace_char16() {
  local needle=$1
  local filename=$2

  wchar_version=${needle//char16_t/wchar_t}
  sed -i "s/^$needle$/#ifdef __WINE__\\
    $wchar_version\\
#else\\
    \0\\
#endif/" "$filename"
}

replace_char16 "using ConverterFacet = std::codecvt_utf8_utf16<char16_t>;" "$sdk_directory/base/source/fstring.cpp"
replace_char16 "using Converter = std::wstring_convert<ConverterFacet, char16_t>;" "$sdk_directory/base/source/fstring.cpp"
replace_char16 "using Converter = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>;" "$sdk_directory/pluginterfaces/base/ustring.cpp"

# Don't try adding `std::u8string` to an `std::vector<std::string>`. MSVC
# probably coerces them, but GCC doesn't
sed -i 's/\bgeneric_u8string\b/generic_string/g' "$sdk_directory/public.sdk/source/vst/hosting/module_win32.cpp"

# libstdc++fs doesn't work under Winelib, for whatever reason that might be.
# We'll patch the Win32 module loading to use `ghc::filesystem` instead.
sed -i 's/^#include <\(experimental\/\)\?filesystem>$/#include <ghc\/filesystem.hpp>/' "$sdk_directory/public.sdk/source/vst/hosting/module_win32.cpp"
sed -i 's/^namespace filesystem = std\(::experimental\)\?::filesystem;$/namespace filesystem = ghc::filesystem;/' "$sdk_directory/public.sdk/source/vst/hosting/module_win32.cpp"

# Wine uses the narrow versions of everything by default, and in Unity builds
# we need to explicitly use the UTF-16 version here.
sed -i 's/\bIShellLink\*/IShellLinkW*/g' "$sdk_directory/public.sdk/source/vst/hosting/module_win32.cpp"

# Meson requires this program to output something, or else it will error out
# when trying to encode the empty output
echo "Successfully patched '$sdk_directory' for winegcc compatibility"
