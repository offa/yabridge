// yabridge: a Wine VST bridge
// Copyright (C) 2020  Robbert van der Helm
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

#include "plugin-bridge.h"

#include <boost/asio/read_until.hpp>
#include <boost/process/env.hpp>
#include <boost/process/io.hpp>
#include <boost/process/start_dir.hpp>
#include <iostream>

// Generated inside of build directory
#include <src/common/config/config.h>
#include <src/common/config/version.h>

#include "../common/communication.h"
#include "../common/events.h"

namespace bp = boost::process;
// I'd rather use std::filesystem instead, but Boost.Process depends on
// boost::filesystem
namespace fs = boost::filesystem;

intptr_t dispatch_proxy(AEffect*, int, int, intptr_t, void*, float);
void process_proxy(AEffect*, float**, float**, int);
void process_replacing_proxy(AEffect*, float**, float**, int);
void setParameter_proxy(AEffect*, int, float);
float getParameter_proxy(AEffect*, int);

/**
 * Fetch the bridge instance stored in an unused pointer from a VST plugin. This
 * is sadly needed as a workaround to avoid using globals since we need free
 * function pointers to interface with the VST C API.
 */
PluginBridge& get_bridge_instance(const AEffect& plugin) {
    return *static_cast<PluginBridge*>(plugin.ptr3);
}

// TODO: It would be nice to have a better way to encapsulate the small
//       differences in behavior when using plugin groups, i.e. everywhere where
//       we check for `config.group.has_value()`

PluginBridge::PluginBridge(audioMasterCallback host_callback)
    : config(Configuration::load_for(get_this_file_location())),
      vst_plugin_path(find_vst_plugin()),
      vst_plugin_arch(find_vst_architecture(vst_plugin_path)),
      vst_host_path(find_vst_host(vst_plugin_arch, config.group.has_value())),
      // All the fields should be zero initialized because
      // `Vst2PluginInstance::vstAudioMasterCallback` from Bitwig's plugin
      // bridge will crash otherwise
      plugin(),
      io_context(),
      socket_endpoint(generate_plugin_endpoint().string()),
      socket_acceptor(io_context, socket_endpoint),
      host_vst_dispatch(io_context),
      host_vst_dispatch_midi_events(io_context),
      vst_host_callback(io_context),
      host_vst_parameters(io_context),
      host_vst_process_replacing(io_context),
      host_callback_function(host_callback),
      logger(Logger::create_from_environment(
          create_logger_prefix(socket_endpoint.path()))),
      wine_version(get_wine_version()),
      wine_stdout(io_context),
      wine_stderr(io_context) {
    log_init_message();
    launch_vst_host();

    // Print the Wine host's STDOUT and STDERR streams to the log file. This
    // should be done before trying to accept the sockets as otherwise we will
    // miss all output.
    async_log_pipe_lines(wine_stdout, wine_stdout_buffer, "[Wine STDOUT] ");
    async_log_pipe_lines(wine_stderr, wine_stderr_buffer, "[Wine STDERR] ");
    wine_io_handler = std::thread([&]() { io_context.run(); });

#ifndef USE_WINEDBG
    // If the Wine process fails to start, then nothing will connect to the
    // sockets and we'll be hanging here indefinitely. To prevent this, we'll
    // periodically poll whether the Wine process is still running, and throw
    // when it is not. The alternative would be to rewrite this to using
    // `async_accept`, Boost.Asio timers, and another IO context, but I feel
    // like this a much simpler solution.
    std::thread([&]() {
        using namespace std::literals::chrono_literals;

        while (true) {
            if (finished_accepting_sockets) {
                return;
            }

            // When using regular individually hosted plugins we can simply
            // check whether the process is still running, but Boost.Process
            // does not allow you to do the same thing for a process that's not
            // a child if this process. When using plugin groups we'll have to
            // manually check whether the PID returned by the group host process
            // is still active.
            if (config.group.has_value()) {
                if (kill(vst_host_pid, 0) != 0) {
                    logger.log(
                        "The group host process has exited unexpectedly. Check "
                        "the output above for more information.");
                    std::terminate();
                }
            } else {
                if (!vst_host.running()) {
                    logger.log(
                        "The Wine process failed to start. Check the output "
                        "above for more information.");
                    std::terminate();
                }
            }

            std::this_thread::sleep_for(1s);
        }
    }).detach();
#endif

    // It's very important that these sockets are connected to in the same
    // order in the Wine VST host
    socket_acceptor.accept(host_vst_dispatch);
    socket_acceptor.accept(host_vst_dispatch_midi_events);
    socket_acceptor.accept(vst_host_callback);
    socket_acceptor.accept(host_vst_parameters);
    socket_acceptor.accept(host_vst_process_replacing);
    finished_accepting_sockets = true;

    // There's no need to keep the socket endpoint file around after accepting
    // all the sockets, and RAII won't clean these files up for us
    socket_acceptor.close();
    fs::remove(socket_endpoint.path());

    // Set up all pointers for our `AEffect` struct. We will fill this with data
    // from the VST plugin loaded in Wine at the end of this constructor.
    plugin.ptr3 = this;
    plugin.dispatcher = dispatch_proxy;
    plugin.process = process_proxy;
    plugin.setParameter = setParameter_proxy;
    plugin.getParameter = getParameter_proxy;
    plugin.processReplacing = process_replacing_proxy;

    // For our communication we use simple threads and blocking operations
    // instead of asynchronous IO since communication has to be handled in
    // lockstep anyway
    host_callback_handler = std::thread([&]() {
        try {
            while (true) {
                // TODO: Think of a nicer way to structure this and the similar
                //       handler in `Vst2Bridge::handle_dispatch_midi_events`
                receive_event(
                    vst_host_callback, std::pair<Logger&, bool>(logger, false),
                    [&](Event& event) {
                        // MIDI events sent from the plugin back to the host are
                        // a special case here. They have to sent during the
                        // `processReplacing()` function or else the host will
                        // ignore them. Because of this we'll temporarily save
                        // any MIDI events we receive here, and then we'll
                        // actually send them to the host at the end of the
                        // `process_replacing()` function.
                        if (event.opcode == audioMasterProcessEvents) {
                            std::lock_guard lock(incoming_midi_events_mutex);

                            incoming_midi_events.push_back(
                                std::get<DynamicVstEvents>(event.payload));
                            EventResult response{1, nullptr, std::nullopt};

                            return response;
                        } else {
                            return passthrough_event(
                                &plugin, host_callback_function)(event);
                        }
                    });
            }
        } catch (const boost::system::system_error&) {
            // This happens when the sockets got closed because the plugin
            // is being shut down
        }
    });

    // Read the plugin's information from the Wine process. This can only be
    // done after we started accepting host callbacks as the plugin will likely
    // call these during its initialization. We reuse the `dispatcher()` socket
    // for this since this has to be done only once.
    const auto initialization_data =
        read_object<EventResult>(host_vst_dispatch);
    const auto initialized_plugin =
        std::get<AEffect>(initialization_data.payload);

    update_aeffect(plugin, initialized_plugin);
}

class DispatchDataConverter : DefaultDataConverter {
   public:
    DispatchDataConverter(std::vector<uint8_t>& chunk_data,
                          AEffect& plugin,
                          VstRect& editor_rectangle)
        : chunk(chunk_data), plugin(plugin), rect(editor_rectangle) {}

    EventPayload read(const int opcode,
                      const int index,
                      const intptr_t value,
                      const void* data) {
        // There are some events that need specific structs that we can't simply
        // serialize as a string because they might contain null bytes
        switch (opcode) {
            case effOpen:
                // This should not be needed, but some improperly coded plugins
                // such as the Roland Cloud plugins will initialize part of
                // their `AEffect` only after the host calls `effOpen`, instead
                // of during the initialization.
                return WantsAEffectUpdate{};
                break;
            case effEditGetRect:
                return WantsVstRect();
                break;
            case effEditOpen:
                // The host will have passed us an X11 window handle in the void
                // pointer. In the Wine VST host we'll create a Win32 window,
                // ask the plugin to embed itself in that and then embed that
                // window into this X11 window handle.
                return reinterpret_cast<size_t>(data);
                break;
            case effGetChunk:
                return WantsChunkBuffer();
                break;
            case effSetChunk: {
                const uint8_t* chunk_data = static_cast<const uint8_t*>(data);

                // When the host passes a chunk it will use the value parameter
                // to tell us its length
                return std::vector<uint8_t>(chunk_data, chunk_data + value);
            } break;
            case effProcessEvents:
                return DynamicVstEvents(*static_cast<const VstEvents*>(data));
                break;
            case effGetInputProperties:
            case effGetOutputProperties:
                // In this case we can't simply pass an empty marker struct
                // because the host can have already populated this field with
                // data (or at least Bitwig does this)
                return *static_cast<const VstIOProperties*>(data);
                break;
            case effGetParameterProperties:
                return *static_cast<const VstParameterProperties*>(data);
                break;
            case effGetMidiKeyName:
                return *static_cast<const VstMidiKeyName*>(data);
                break;
            case effSetSpeakerArrangement:
            case effGetSpeakerArrangement:
                // This is the output speaker configuration, the `read_value()`
                // method below reads the input speaker configuration
                return DynamicSpeakerArrangement(
                    *static_cast<const VstSpeakerArrangement*>(data));
                break;
            // Any VST host I've encountered has properly zeroed out these their
            // string buffers, but we'll add a list of opcodes that should
            // return a string just in case `DefaultDataConverter::read()` can't
            // figure it out.
            case effGetProgramName:
            case effGetParamLabel:
            case effGetParamDisplay:
            case effGetParamName:
            case effGetProgramNameIndexed:
            case effGetEffectName:
            case effGetVendorString:
            case effGetProductString:
            case effShellGetNextPlugin:
                return WantsString{};
                break;
            default:
                return DefaultDataConverter::read(opcode, index, value, data);
                break;
        }
    }

    std::optional<EventPayload> read_value(const int opcode,
                                           const intptr_t value) {
        switch (opcode) {
            case effSetSpeakerArrangement:
            case effGetSpeakerArrangement:
                // These two events are special in that they pass a pointer to
                // the output speaker configuration through the `data`
                // parameter, but then they also pass a pointer to the input
                // speaker configuration through the `value` parameter. This is
                // the only event that does this.
                return DynamicSpeakerArrangement(
                    *static_cast<const VstSpeakerArrangement*>(
                        reinterpret_cast<void*>(value)));
                break;
            default:
                return DefaultDataConverter::read_value(opcode, value);
                break;
        }
    }

    void write(const int opcode, void* data, const EventResult& response) {
        switch (opcode) {
            case effOpen: {
                // Update our `AEffect` object one last time for improperly
                // coded late initialing plugins. Hopefully the host will see
                // that the object is updated because these plugins don't send
                // any notification about this.
                const auto updated_plugin = std::get<AEffect>(response.payload);
                update_aeffect(plugin, updated_plugin);
            } break;
            case effEditGetRect: {
                // Either the plugin will have returned (a pointer to) their
                // editor dimensions, or they will not have written anything.
                if (std::holds_alternative<std::nullptr_t>(response.payload)) {
                    return;
                }

                const auto new_rect = std::get<VstRect>(response.payload);
                rect = new_rect;

                *static_cast<VstRect**>(data) = &rect;
            } break;
            case effGetChunk: {
                // Write the chunk data to some publically accessible place in
                // `PluginBridge` and write a pointer to that struct to the data
                // pointer
                const auto buffer =
                    std::get<std::vector<uint8_t>>(response.payload);
                chunk.assign(buffer.begin(), buffer.end());

                *static_cast<uint8_t**>(data) = chunk.data();
            } break;
            case effGetInputProperties:
            case effGetOutputProperties: {
                // These opcodes pass the plugin some empty struct through the
                // data parameter that the plugin then fills with flags and
                // other data to describe an input or output channel.
                const auto properties =
                    std::get<VstIOProperties>(response.payload);

                *static_cast<VstIOProperties*>(data) = properties;
            } break;
            case effGetParameterProperties: {
                // Same as the above
                const auto properties =
                    std::get<VstParameterProperties>(response.payload);

                *static_cast<VstParameterProperties*>(data) = properties;
            } break;
            case effGetMidiKeyName: {
                // Ditto
                const auto properties =
                    std::get<VstMidiKeyName>(response.payload);

                *static_cast<VstMidiKeyName*>(data) = properties;
            } break;
            case effGetSpeakerArrangement: {
                // The plugin will have updated the objects passed by the host
                // with its preferred output speaker configuration if it
                // supports this. The same thing happens for the input speaker
                // configuration in `write_value()`.
                auto speaker_arrangement =
                    std::get<DynamicSpeakerArrangement>(response.payload);

                // Reconstruct a dynamically sized `VstSpeakerArrangement`
                // object to a buffer, and write back the results to the data
                // parameter.
                VstSpeakerArrangement* output =
                    static_cast<VstSpeakerArrangement*>(data);
                std::vector<uint8_t> reconstructed_object =
                    speaker_arrangement.as_raw_data();
                std::copy(reconstructed_object.begin(),
                          reconstructed_object.end(),
                          reinterpret_cast<uint8_t*>(output));
            } break;
            default:
                DefaultDataConverter::write(opcode, data, response);
                break;
        }
    }

    intptr_t return_value(const int opcode, const intptr_t original) {
        return DefaultDataConverter::return_value(opcode, original);
    }

    void write_value(const int opcode,
                     intptr_t value,
                     const EventResult& response) {
        switch (opcode) {
            case effGetSpeakerArrangement: {
                // Same as the above, but now for the input speaker
                // configuration object under the `value` pointer
                auto speaker_arrangement =
                    std::get<DynamicSpeakerArrangement>(response.payload);

                VstSpeakerArrangement* output =
                    static_cast<VstSpeakerArrangement*>(
                        reinterpret_cast<void*>(value));
                std::vector<uint8_t> reconstructed_object =
                    speaker_arrangement.as_raw_data();
                std::copy(reconstructed_object.begin(),
                          reconstructed_object.end(),
                          reinterpret_cast<uint8_t*>(output));
            } break;
            default:
                return DefaultDataConverter::write_value(opcode, value,
                                                         response);
                break;
        }
    }

   private:
    std::vector<uint8_t>& chunk;
    AEffect& plugin;
    VstRect& rect;
};

intptr_t PluginBridge::dispatch(AEffect* /*plugin*/,
                                int opcode,
                                int index,
                                intptr_t value,
                                void* data,
                                float option) {
    // HACK: Ardour 5.X has a bug in its VST implementation where it calls the
    //       plugin's dispatcher before the plugin has even finished
    //       initializing. This has been fixed back in 2018, but there has not
    //       been a release that contains the fix yet. This should be removed
    //       once Ardour 6.0 gets released.
    //       https://tracker.ardour.org/view.php?id=7668
    if (BOOST_UNLIKELY(plugin.magic == 0)) {
        logger.log_event(true, opcode, index, value, nullptr, option,
                         std::nullopt);
        logger.log(
            "   WARNING: The host has dispatched an event before the plugin "
            "has finished initializing, ignoring the event. (are we running "
            "Ardour 5.X?)");
        logger.log_event_response(true, opcode, 0, nullptr, std::nullopt);
        return 0;
    }

    DispatchDataConverter converter(chunk_data, plugin, editor_rectangle);

    switch (opcode) {
        case effClose: {
            // Allow the plugin to handle its own shutdown, and then terminate
            // the process. Because terminating the Wine process will also
            // forcefully close all open sockets this will also terminate our
            // handler thread.
            intptr_t return_value = 0;
            try {
                // TODO: Add some kind of timeout?
                return_value =
                    send_event(host_vst_dispatch, dispatch_mutex, converter,
                               std::pair<Logger&, bool>(logger, true), opcode,
                               index, value, data, option);
            } catch (const boost::system::system_error& a) {
                // Thrown when the socket gets closed because the VST plugin
                // loaded into the Wine process crashed during shutdown
                logger.log("The plugin crashed during shutdown, ignoring");
            }

            // Don't terminate group host processes. They will shut down
            // automatically after all plugins have exited.
            if (!config.group.has_value()) {
                vst_host.terminate();
            } else {
                // Manually the dispatch socket will cause the host process to
                // terminate
                host_vst_dispatch.close();
            }

            // The `stop()` method will cause the IO context to just drop all of
            // its work immediately and not throw any exceptions that would have
            // been caused by pipes and sockets being closed.
            io_context.stop();

            // These threads should now be finished because we've forcefully
            // terminated the Wine process, interupting their socket operations
            if (group_host_connect_handler.joinable()) {
                // This thread is only used when using plugin groups
                group_host_connect_handler.join();
            }
            host_callback_handler.join();
            wine_io_handler.join();

            delete this;

            return return_value;
        }; break;
        case effProcessEvents:
            // Because of limitations of the Win32 API we have to use a seperate
            // thread and socket to pass MIDI events. Otherwise plugins will
            // stop receiving MIDI data when they have an open dropdowns or
            // message box.
            return send_event(host_vst_dispatch_midi_events,
                              dispatch_midi_events_mutex, converter,
                              std::pair<Logger&, bool>(logger, true), opcode,
                              index, value, data, option);
            break;
        case effCanDo: {
            const std::string query(static_cast<const char*>(data));

            // NOTE: If the plugins returns `0xbeefXXXX` to this query, then
            //       REAPER will pass a libSwell handle rather than an X11
            //       window ID to `effEditOpen`. This is of course not going to
            //       work when the GUI is handled using Wine so we'll ignore it.
            if (query == "hasCockosViewAsConfig") {
                logger.log_event(true, opcode, index, value, query, option,
                                 std::nullopt);
                logger.log(
                    "   The host requests libSwell GUI support which is not "
                    "supported using Wine, ignoring the request.");
                logger.log_event_response(true, opcode, -1, nullptr,
                                          std::nullopt);
                return -1;
            }
        } break;
    }

    // We don't reuse any buffers here like we do for audio processing. This
    // would be useful for chunk data, but since that's only needed when saving
    // and loading plugin state it's much better to have bitsery or our
    // receiving function temporarily allocate a large enough buffer rather than
    // to have a bunch of allocated memory sitting around doing nothing.
    return send_event(host_vst_dispatch, dispatch_mutex, converter,
                      std::pair<Logger&, bool>(logger, true), opcode, index,
                      value, data, option);
}

void PluginBridge::process_replacing(AEffect* /*plugin*/,
                                     float** inputs,
                                     float** outputs,
                                     int sample_frames) {
    // The inputs and outputs arrays should be `[num_inputs][sample_frames]` and
    // `[num_outputs][sample_frames]` floats large respectfully.
    std::vector<std::vector<float>> input_buffers(
        plugin.numInputs, std::vector<float>(sample_frames));
    for (int channel = 0; channel < plugin.numInputs; channel++) {
        std::copy(inputs[channel], inputs[channel] + sample_frames + 1,
                  input_buffers[channel].begin());
    }

    const AudioBuffers request{input_buffers, sample_frames};
    write_object(host_vst_process_replacing, request, process_buffer);

    // Write the results back to the `outputs` arrays
    const auto response =
        read_object<AudioBuffers>(host_vst_process_replacing, process_buffer);

    assert(response.buffers.size() == static_cast<size_t>(plugin.numOutputs));
    for (int channel = 0; channel < plugin.numOutputs; channel++) {
        std::copy(response.buffers[channel].begin(),
                  response.buffers[channel].end(), outputs[channel]);
    }

    // Plugins are allowed to send MIDI events during processing using a host
    // callback. These have to be processed during the actual
    // `processReplacing()` function or else the host will ignore them. To
    // prevent these events from getting delayed by a sample we'll process them
    // after the plugin is done processing audio rather than during the time
    // we're still waiting on the plugin.
    std::lock_guard lock(incoming_midi_events_mutex);
    for (DynamicVstEvents& events : incoming_midi_events) {
        host_callback_function(&plugin, audioMasterProcessEvents, 0, 0,
                               &events.as_c_events(), 0.0);
    }

    incoming_midi_events.clear();
}

float PluginBridge::get_parameter(AEffect* /*plugin*/, int index) {
    logger.log_get_parameter(index);

    const Parameter request{index, std::nullopt};
    ParameterResult response;

    // Prevent race conditions from `getParameter()` and `setParameter()` being
    // called at the same time since  they share the same socket
    {
        std::lock_guard lock(parameters_mutex);
        write_object(host_vst_parameters, request);
        response = read_object<ParameterResult>(host_vst_parameters);
    }

    logger.log_get_parameter_response(response.value.value());

    return response.value.value();
}

void PluginBridge::set_parameter(AEffect* /*plugin*/, int index, float value) {
    logger.log_set_parameter(index, value);

    const Parameter request{index, value};
    ParameterResult response;

    {
        std::lock_guard lock(parameters_mutex);
        write_object(host_vst_parameters, request);

        response = read_object<ParameterResult>(host_vst_parameters);
    }

    logger.log_set_parameter_response();

    // This should not contain any values and just serve as an acknowledgement
    assert(!response.value.has_value());
}

void PluginBridge::async_log_pipe_lines(patched_async_pipe& pipe,
                                        boost::asio::streambuf& buffer,
                                        std::string prefix) {
    boost::asio::async_read_until(
        pipe, buffer, '\n',
        [&, prefix](const boost::system::error_code& error, size_t) {
            // When we get an error code then that likely means that the pipe
            // has been clsoed and we have reached the end of the file
            if (error.failed()) {
                return;
            }

            std::string line;
            std::getline(std::istream(&buffer), line);
            logger.log(prefix + line);

            async_log_pipe_lines(pipe, buffer, prefix);
        });
}

void PluginBridge::launch_vst_host() {
    const bp::environment host_env = set_wineprefix();

#ifndef USE_WINEDBG
    const std::vector<std::string> host_command{vst_host_path.string()};
#else
    // This is set up for KDE Plasma. Other desktop environments and window
    // managers require some slight modifications to spawn a detached terminal
    // emulator.
    const std::vector<std::string> host_command{"/usr/bin/kstart5",
                                                "konsole",
                                                "--",
                                                "-e",
                                                "winedbg",
                                                "--gdb",
                                                vst_host_path.string() + ".so"};
#endif

#ifndef USE_WINEDBG
    const fs::path plugin_path = vst_plugin_path;
    const fs::path starting_dir = fs::current_path();
#else
    // winedbg has no reliable way to escape spaces, so we'll start the process
    // in the plugin's directory
    const fs::path plugin_path = vst_plugin_path.filename();
    const fs::path starting_dir = vst_plugin_path.parent_path();

    if (plugin_path.string().find(' ') != std::string::npos) {
        logger.log("Warning: winedbg does not support paths containing spaces");
    }
#endif
    const fs::path socket_path = socket_endpoint.path();

    if (!config.group.has_value()) {
        vst_host =
            bp::child(host_command, plugin_path, socket_path,
                      bp::env = host_env, bp::std_out = wine_stdout,
                      bp::std_err = wine_stderr, bp::start_dir = starting_dir);
        return;
    }

    // When using plugin groups, we'll first try to connect to an existing group
    // host process and ask it to host our plugin. If no such process exists,
    // then we'll start a new process. In the event that two yabridge instances
    // simultaneously try to start a new group process for the same group, then
    // the last process to connect to the socket will terminate gracefully and
    // the first process will handle the connections for both yabridge
    // instances.
    fs::path wine_prefix = host_env.at("WINEPREFIX").to_string();
    if (host_env.at("WINEPREFIX").empty()) {
        // Fall back to `~/.wine` if this has not been set or detected. This
        // would happen if the plugin's .dll file is not inside of a Wine
        // prefix. If this happens, then the Wine instance will be launched in
        // the default Wine prefix, so we should reflect that here.
        wine_prefix = fs::path(host_env.at("HOME").to_string()) / ".wine";
    }

    const fs::path group_socket_path = generate_group_endpoint(
        config.group.value(), wine_prefix, vst_plugin_arch);

    try {
        // Request the existing group host process to host our plugin, and store
        // the PID of that process so we'll know if it has crashed
        boost::asio::local::stream_protocol::socket group_socket(io_context);
        group_socket.connect(group_socket_path.string());

        write_object(group_socket,
                     GroupRequest{plugin_path.string(), socket_path.string()});
        const auto response = read_object<GroupResponse>(group_socket);

        vst_host_pid = response.pid;
    } catch (const boost::system::system_error&) {
        // In case we could not connect to the socket, then we'll start a
        // new group host process. This process is detached immediately
        // because it should run independently of this yabridge instance as
        // it will likely outlive it.
        vst_host =
            bp::child(host_command, group_socket_path, bp::env = host_env,
                      bp::std_out = wine_stdout, bp::std_err = wine_stderr,
                      bp::start_dir = starting_dir);
        vst_host_pid = vst_host.id();
        vst_host.detach();

        // We now want to connect to the socket the in the exact same way as
        // above. The only problem is that it may take some time for the
        // process to start depending on Wine's current state. We'll defer
        // this to a thread so we can finish the rest of the startup in the
        // meantime.
        group_host_connect_handler = std::thread([&, group_socket_path,
                                                  plugin_path, socket_path]() {
            using namespace std::literals::chrono_literals;

            // TODO: Replace this polling with inotify when encapsulating
            //       the different host launch behaviors
            while (vst_host.running()) {
                std::this_thread::sleep_for(20ms);

                try {
                    // This is the exact same connection sequence as above
                    boost::asio::local::stream_protocol::socket group_socket(
                        io_context);
                    group_socket.connect(group_socket_path.string());

                    write_object(group_socket,
                                 GroupRequest{plugin_path.string(),
                                              socket_path.string()});
                    const auto response =
                        read_object<GroupResponse>(group_socket);

                    // If two group processes started at the same time, than the
                    // first one will be the one to respond to the host request
                    vst_host_pid = response.pid;
                    return;
                } catch (const boost::system::system_error&) {
                }
            }
        });
    }
}

void PluginBridge::log_init_message() {
    std::stringstream init_msg;

    init_msg << "Initializing yabridge version " << yabridge_git_version
             << std::endl;
    init_msg << "host:         '" << vst_host_path.string() << "'" << std::endl;
    init_msg << "plugin:       '" << vst_plugin_path.string() << "'"
             << std::endl;
    init_msg << "socket:       '" << socket_endpoint.path() << "'" << std::endl;
    init_msg << "wine prefix:  '"
             << find_wineprefix().value_or("<default>").string() << "'"
             << std::endl;
    init_msg << "wine version: '" << wine_version << "'" << std::endl;
    init_msg << std::endl;

    // Print the path to the currently loaded configuration file and all
    // settings in use. Printing the matched glob pattern could also be useful
    // but it'll be very noisy and it's likely going to be clear from the shown
    // values anyways.
    init_msg << "config from:  '"
             << config.matched_file.value_or("<defaults>").string() << "'"
             << std::endl;
    init_msg << "hosting mode: '";
    if (config.group.has_value()) {
        init_msg << "plugin group \"" << config.group.value() << "\"";
    } else {
        init_msg << "individually";
    }
    if (vst_plugin_arch == PluginArchitecture::vst_32) {
        init_msg << ", 32-bit";
    } else {
        init_msg << ", 64-bit";
    }
    init_msg << "'" << std::endl;
    init_msg << std::endl;

    // Include a list of enabled compile-tiem features, mostly to make debug
    // logs more useful
    init_msg << "Enabled features:" << std::endl;
#ifdef USE_BITBRIDGE
    init_msg << "- bitbridge support" << std::endl;
#endif
#ifdef USE_WINEDBG
    init_msg << "- winedbg" << std::endl;
#endif
#if !(defined(USE_BITBRIDGE) || defined(USE_WINEDBG))
    init_msg << "  <none>" << std::endl;
#endif
    init_msg << std::endl;

    for (std::string line = ""; std::getline(init_msg, line);) {
        logger.log(line);
    }
}

// The below functions are proxy functions for the methods defined in
// `Bridge.cpp`

intptr_t dispatch_proxy(AEffect* plugin,
                        int opcode,
                        int index,
                        intptr_t value,
                        void* data,
                        float option) {
    return get_bridge_instance(*plugin).dispatch(plugin, opcode, index, value,
                                                 data, option);
}

void process_proxy(AEffect* plugin,
                   float** inputs,
                   float** outputs,
                   int sample_frames) {
    return get_bridge_instance(*plugin).process_replacing(
        plugin, inputs, outputs, sample_frames);
}

void process_replacing_proxy(AEffect* plugin,
                             float** inputs,
                             float** outputs,
                             int sample_frames) {
    return get_bridge_instance(*plugin).process_replacing(
        plugin, inputs, outputs, sample_frames);
}

void setParameter_proxy(AEffect* plugin, int index, float value) {
    return get_bridge_instance(*plugin).set_parameter(plugin, index, value);
}

float getParameter_proxy(AEffect* plugin, int index) {
    return get_bridge_instance(*plugin).get_parameter(plugin, index);
}