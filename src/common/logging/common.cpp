// yabridge: a Wine plugin bridge
// Copyright (C) 2020-2022 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "common.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>

/**
 * The environment variable indicating whether to log to a file. Will log to
 * STDERR if not specified.
 */
constexpr char logging_file_environment_variable[] = "YABRIDGE_DEBUG_FILE";

/**
 * The verbosity of the logging, defaults to `Logger::Verbosity::basic`.
 *
 * @see Logger::Verbosity
 */
constexpr char logging_verbosity_environment_variable[] =
    "YABRIDGE_DEBUG_LEVEL";

/**
 * The `YABRIDGE_DEBUG_LEVEL` flag for enabling editor tracing.
 */
constexpr char editor_tracing_flag[] = "+editor";

Logger::Logger(std::shared_ptr<std::ostream> stream,
               Verbosity verbosity_level,
               bool editor_tracing,
               std::string prefix,
               bool prefix_timestamp)
    : verbosity_(verbosity_level),
      editor_tracing_(editor_tracing),
      stream_(stream),
      prefix_(prefix),
      prefix_timestamp_(prefix_timestamp) {}

Logger Logger::create_from_environment(std::string prefix,
                                       std::shared_ptr<std::ostream> stream,
                                       bool prefix_timestamp) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* file_path_env = getenv(logging_file_environment_variable);
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* verbosity_env = getenv(logging_verbosity_environment_variable);
    std::string file_path = file_path_env ? std::string(file_path_env) : "";
    std::string verbosity = verbosity_env ? std::string(verbosity_env) : "";

    // Editor debug tracing is an optional flag that can be added to any debug
    // level (and technically it will also work fine if it's the only option,
    // but you're not supposed to do that ;))
    const bool editor_tracing = verbosity.ends_with(editor_tracing_flag);
    if (editor_tracing) {
        verbosity =
            verbosity.substr(0, verbosity.size() - strlen(editor_tracing_flag));
    }

    // Default to `Verbosity::basic` if the environment variable has not
    // been set or if it is not an integer.
    Verbosity verbosity_level;
    try {
        verbosity_level = static_cast<Verbosity>(std::stoi(verbosity));
    } catch (const std::invalid_argument&) {
        verbosity_level = Verbosity::basic;
    }

    if (!stream) {
        // If `file` points to a valid location then use create/truncate the
        // file and write all of the logs there, otherwise use STDERR
        const auto log_file = std::make_shared<std::ofstream>(
            file_path, std::fstream::out | std::fstream::app);
        if (log_file->is_open()) {
            stream = log_file;
        } else {
            // For STDERR we sadly can't just use `std::cerr`. In the group
            // process we need to capture all output generated by the process
            // itself, and the only way to do this is by reopening the STDERR
            // and STDOUT streams to a pipe. Luckily `/dev/stderr` stays
            // unaffected, so we can still write there without causing infinite
            // loops.
            stream = std::make_shared<std::ofstream>(
                "/dev/stderr", std::fstream::out | std::fstream::app);
        }
    }

    return Logger(stream, verbosity_level, editor_tracing, prefix,
                  prefix_timestamp);
}

Logger Logger::create_wine_stderr() {
    // We're logging directly to `std::cerr` instead of to `/dev/stderr` because
    // we want the STDERR redirection from the group host processes to still
    // function here
    return create_from_environment(
        "", std::shared_ptr<std::ostream>(&std::cerr, [](auto*) {}), false);
}

Logger Logger::create_exception_logger() {
#ifdef __WINE__
    return Logger::create_wine_stderr();
#else
    return Logger::create_from_environment("[error] ");
#endif
}

void Logger::log(const std::string& message) {
    std::ostringstream formatted_message;

    if (prefix_timestamp_) {
        const auto current_time = std::chrono::system_clock::now();
        const time_t timestamp =
            std::chrono::system_clock::to_time_t(current_time);

        // How did C++ manage to get time formatting libraries without a way to
        // actually get a timestamp in a threadsafe way? `localtime_r` in C++ is
        // not portable but luckily we only have to support GCC anyway.
        std::tm tm;
        localtime_r(&timestamp, &tm);

        formatted_message << std::put_time(&tm, "%T") << " ";
    }

    formatted_message << prefix_;
    formatted_message << message;
    // Flushing a stringstream doesn't do anything, but we need to put a
    // linefeed in this string stream rather writing it sprightly to the output
    // stream to prevent two messages from being put on the same row
    formatted_message << std::endl;

    *stream_ << formatted_message.str() << std::flush;
}
