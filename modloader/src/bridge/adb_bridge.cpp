// modloader/src/bridge/adb_bridge.cpp
// TCP socket server on 127.0.0.1:19420 — JSON command protocol
// No root required — accessible via `adb forward tcp:19420 tcp:19420`

#include "modloader/adb_bridge.h"
#include "modloader/lua_engine.h"
#include "modloader/mod_loader.h"
#include "modloader/pak_mounter.h"
#include "modloader/lua_dump_generator.h"
#include "modloader/reflection_walker.h"
#include "modloader/process_event_hook.h"
#include "modloader/pe_trace.h"
#include "modloader/class_rebuilder.h"
#include "modloader/object_monitor.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/paths.h"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

using json = nlohmann::json;

namespace adb_bridge
{

    static int s_server_fd = -1;
    static std::atomic<bool> s_running{false};
    static pthread_t s_server_thread;
    static std::chrono::steady_clock::time_point s_start_time;

    // ═══ Custom command registry ════════════════════════════════════════════
    static std::unordered_map<std::string, CommandHandler> s_custom_commands;
    static std::mutex s_command_mutex;

    void register_command(const std::string &name, CommandHandler handler)
    {
        std::lock_guard<std::mutex> lock(s_command_mutex);
        s_custom_commands[name] = handler;
        logger::log_info("ADB", "Registered custom command: '%s'", name.c_str());
    }

    std::vector<std::string> get_registered_commands()
    {
        std::lock_guard<std::mutex> lock(s_command_mutex);
        std::vector<std::string> names;
        for (const auto &pair : s_custom_commands)
        {
            names.push_back(pair.first);
        }
        return names;
    }

    // ═══ JSON response helpers ══════════════════════════════════════════════
    static std::string ok_response(const json &result)
    {
        json resp;
        resp["ok"] = true;
        resp["result"] = result;
        return resp.dump() + "\n";
    }

    static std::string error_response(const std::string &msg)
    {
        json resp;
        resp["ok"] = false;
        resp["error"] = msg;
        return resp.dump() + "\n";
    }

    // ═══ Command handlers ═══════════════════════════════════════════════════

    static std::string handle_list_mods()
    {
        auto &mods = mod_loader::get_all_mods();
        json result = json::array();
        for (const auto &m : mods)
        {
            json entry;
            entry["name"] = m.name;
            entry["status"] = (m.status == mod_loader::ModStatus::LOADED) ? "loaded" : (m.status == mod_loader::ModStatus::ERRORED) ? "errored"
                                                                                                                                    : "failed";
            entry["errors"] = m.error_count;
            if (!m.error.empty())
                entry["last_error"] = m.error;
            result.push_back(entry);
        }
        return ok_response(result);
    }

    static std::string handle_reload_mod(const json &payload)
    {
        if (!payload.contains("name"))
            return error_response("missing 'name' parameter");
        std::string name = payload["name"];
        bool ok = mod_loader::reload_mod(name);
        return ok ? ok_response("reloaded: " + name) : error_response("reload failed: " + name);
    }

    static std::string handle_load_mod(const json &payload)
    {
        if (!payload.contains("name"))
            return error_response("missing 'name' parameter");
        std::string name = payload["name"];
        bool ok = mod_loader::load_mod(name);
        return ok ? ok_response("loaded: " + name) : error_response("load failed: " + name);
    }

    static std::string handle_exec_lua(const json &payload)
    {
        if (!payload.contains("code"))
            return error_response("missing 'code' parameter");
        std::string code = payload["code"];

        // Lua is NOT thread-safe. exec_lua runs on the bridge thread but the Lua state
        // is owned by the game thread (ProcessEvent hooks fire there). We must queue the
        // execution onto the game thread and block until the result is ready.
        struct ExecContext
        {
            std::mutex mtx;
            std::condition_variable cv;
            std::atomic<bool> done{false};
            std::atomic<bool> cancelled{false}; // set when bridge times out
            lua_engine::ExecResult result;
        };
        auto ctx = std::make_shared<ExecContext>();

        pe_hook::queue_game_thread([ctx, code]()
                                   {
        // CRITICAL: If the bridge already timed out and the caller disconnected,
        // skip execution to avoid running stale/dangerous code on the game thread.
        if (ctx->cancelled.load(std::memory_order_acquire)) {
            logger::log_warn("ADB", "exec_lua: skipping cancelled/timed-out command");
            return;
        }

        // Use instruction limit for bridge exec_lua to prevent infinite loops/recursion
        // from freezing the game thread. 10M instructions is plenty for any reasonable
        // bridge command but catches runaway code before it can block the game.
        ctx->result = lua_engine::exec_string(code, "=adb", 10'000'000);
        {
            std::lock_guard<std::mutex> lk(ctx->mtx);
            ctx->done.store(true, std::memory_order_release);
        }
        ctx->cv.notify_one(); });

        // Wait up to 8 seconds for the game thread to process it
        {
            std::unique_lock<std::mutex> lk(ctx->mtx);
            if (!ctx->cv.wait_for(lk, std::chrono::seconds(8), [&]
                                  { return ctx->done.load(); }))
            {
                // Mark as cancelled so the queued lambda skips execution
                ctx->cancelled.store(true, std::memory_order_release);
                logger::log_warn("ADB", "exec_lua: timed out, marking queued command as cancelled");
                return error_response("exec_lua timed out (game thread did not process within 8s)");
            }
        }

        if (ctx->result.success)
        {
            return ok_response(ctx->result.output.empty() ? "ok" : ctx->result.output);
        }
        return error_response(ctx->result.error);
    }

    static std::string handle_list_hooks()
    {
        auto stats = pe_hook::get_func_stats();
        json result = json::array();
        for (const auto &fs : stats)
        {
            json entry;
            entry["path"] = fs.name;
            entry["call_count"] = fs.call_count;
            result.push_back(entry);
        }
        return ok_response(result);
    }

    static std::string handle_dump_sdk()
    {
        // Re-walk GUObjectArray to capture newly loaded classes, then regenerate SDK
        int files = sdk_gen::regenerate();
        json result;
        result["classes"] = sdk_gen::class_count();
        result["structs"] = sdk_gen::struct_count();
        result["enums"] = sdk_gen::enum_count();
        result["files_written"] = files;
        result["cxx_header_path"] = paths::cxx_header_dir();
        result["lua_types_path"] = paths::lua_types_dir();
        result["legacy_sdk_path"] = paths::sdk_dir();
        result["note"] = "Re-walked GUObjectArray and regenerated SDK (CXXHeaderDump + Lua + Legacy)";
        return ok_response(result);
    }

    static std::string handle_object_count()
    {
        int32_t live = object_monitor::get_live_count();
        int32_t last_walk = reflection::get_object_count();
        json result;
        result["live_object_count"] = live;
        result["last_walk_count"] = last_walk;
        result["growth_since_walk"] = live - last_walk;
        result["classes_known"] = reflection::get_classes().size();
        result["structs_known"] = reflection::get_structs().size();
        result["enums_known"] = reflection::get_enums().size();
        result["auto_dumps"] = object_monitor::auto_dump_count();
        return ok_response(result);
    }

    static std::string handle_mount_pak(const json &payload)
    {
        if (!payload.contains("path") && !payload.contains("name"))
        {
            return error_response("missing 'path' or 'name' parameter");
        }
        std::string pak = payload.contains("path") ? payload["path"].get<std::string>()
                                                   : payload["name"].get<std::string>();
        bool ok = pak_mounter::mount(pak);
        return ok ? ok_response("mounted: " + pak) : error_response("mount failed: " + pak);
    }

    static std::string handle_list_paks()
    {
        auto &paks = pak_mounter::get_all();
        json result = json::array();
        for (const auto &p : paks)
        {
            json entry;
            entry["name"] = p.name;
            entry["path"] = p.path;
            entry["mounted"] = p.mounted;
            entry["size_mb"] = p.file_size / (1024.0 * 1024.0);
            if (!p.error.empty())
                entry["error"] = p.error;
            result.push_back(entry);
        }
        return ok_response(result);
    }

    static std::string handle_log_tail(const json &payload)
    {
        int lines = 50;
        if (payload.contains("lines"))
            lines = payload["lines"];
        auto tail = logger::get_tail(lines);
        json result = json::array();
        for (const auto &line : tail)
        {
            result.push_back(line);
        }
        return ok_response(result);
    }

    static std::string handle_get_stats()
    {
        auto elapsed = std::chrono::steady_clock::now() - s_start_time;
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

        json result;
        result["uptime_seconds"] = secs;
        result["mods_loaded"] = mod_loader::loaded_count();
        result["mods_failed"] = mod_loader::failed_count();
        result["mods_total"] = mod_loader::total_count();
        result["classes_known"] = reflection::get_classes().size();
        result["structs_known"] = reflection::get_structs().size();
        result["enums_known"] = reflection::get_enums().size();
        result["live_object_count"] = object_monitor::get_live_count();
        result["last_walk_count"] = reflection::get_object_count();
        result["auto_dumps"] = object_monitor::auto_dump_count();

        auto stats = pe_hook::get_func_stats();
        int64_t total_calls = 0;
        for (const auto &fs : stats)
            total_calls += fs.call_count;
        result["total_hook_calls"] = total_calls;
        result["active_hooks"] = stats.size();

        return ok_response(result);
    }

    static std::string handle_find_object(const json &payload)
    {
        if (!payload.contains("name"))
            return error_response("missing 'name' parameter");
        std::string name = payload["name"];
        ue::UObject *obj = reflection::find_object_by_name(name);
        if (!obj)
            return error_response("object not found: " + name);

        json result;
        result["name"] = reflection::get_short_name(obj);
        result["full_name"] = reflection::get_full_name(obj);
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "0x%lX", (unsigned long)reinterpret_cast<uintptr_t>(obj));
        result["address"] = std::string(addr_buf);

        ue::UClass *cls = ue::uobj_get_class(obj);
        if (cls)
        {
            result["class"] = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(cls));
        }

        return ok_response(result);
    }

    static std::string handle_find_class(const json &payload)
    {
        if (!payload.contains("name"))
            return error_response("missing 'name' parameter");
        std::string name = payload["name"];
        ue::UClass *cls = reflection::find_class_ptr(name);
        if (!cls)
            return error_response("class not found: " + name);

        std::string class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(cls));
        auto *rc = rebuilder::rebuild(class_name);

        json result;
        result["name"] = class_name;
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "0x%lX", (unsigned long)reinterpret_cast<uintptr_t>(cls));
        result["address"] = std::string(addr_buf);

        if (rc)
        {
            result["properties"] = rc->all_properties.size();
            result["functions"] = rc->all_functions.size();
            result["instances"] = rc->instance_count();
            result["parent"] = rc->parent_name;
        }

        return ok_response(result);
    }

    // ═══ Dispatch command ═══════════════════════════════════════════════════
    static std::string dispatch(const std::string &raw_json)
    {
        json cmd = json::parse(raw_json, nullptr, false);
        if (cmd.is_discarded())
        {
            return error_response("invalid JSON");
        }

        if (!cmd.contains("cmd"))
            return error_response("missing 'cmd' field");

        std::string command = cmd["cmd"];
        logger::log_info("ADB", "Command: %s", command.c_str());

        if (command == "list_mods")
            return handle_list_mods();
        if (command == "reload_mod")
            return handle_reload_mod(cmd);
        if (command == "load_mod")
            return handle_load_mod(cmd);
        if (command == "exec_lua")
            return handle_exec_lua(cmd);
        if (command == "list_hooks")
            return handle_list_hooks();
        if (command == "dump_sdk")
            return handle_dump_sdk();
        if (command == "redump")
            return handle_dump_sdk();
        if (command == "mount_pak")
            return handle_mount_pak(cmd);
        if (command == "list_paks")
            return handle_list_paks();
        if (command == "log_tail")
            return handle_log_tail(cmd);
        if (command == "get_stats")
            return handle_get_stats();
        if (command == "find_object")
            return handle_find_object(cmd);
        if (command == "find_class")
            return handle_find_class(cmd);
        if (command == "object_count")
            return handle_object_count();

        // ═══ PE Trace commands ═══════════════════════════════════════════════
        if (command == "pe_trace_start")
        {
            std::string filter;
            if (cmd.contains("filter"))
                filter = cmd["filter"].get<std::string>();
            pe_trace::start(filter);
            return ok_response("tracing started" + (filter.empty() ? std::string("") : " (filter: " + filter + ")"));
        }
        if (command == "pe_trace_stop")
        {
            pe_trace::stop();
            return ok_response(pe_trace::status());
        }
        if (command == "pe_trace_clear")
        {
            pe_trace::clear();
            return ok_response("trace data cleared");
        }
        if (command == "pe_trace_status")
        {
            return ok_response(pe_trace::status());
        }
        if (command == "pe_trace_top")
        {
            int n = 50;
            if (cmd.contains("n"))
                n = cmd["n"].get<int>();
            if (cmd.contains("lines"))
                n = cmd["lines"].get<int>();
            return ok_response(pe_trace::top(n));
        }
        if (command == "pe_trace_dump")
        {
            std::string result = pe_trace::dump_to_file();
            return ok_response(result);
        }

        // simple ping for console connectivity
        if (command == "ping")
        {
            return ok_response("pong");
        }

        // ═══ exec_console — execute a UE console command via bridge ═════════
        // Usage: {"cmd": "exec_console", "command": "stat fps"}
        // This runs the command via PlayerController::ConsoleCommand (ProcessEvent)
        // which works even in shipping builds (no FExec::Exec guard).
        if (command == "exec_console")
        {
            if (!cmd.contains("command"))
                return error_response("exec_console: missing 'command' field");
            std::string console_cmd = cmd["command"].get<std::string>();

            // exec_console must run on the game thread (PlayerController access is game-thread only)
            // Queue it and wait for result
            std::string exec_result;
            std::mutex mtx;
            std::condition_variable cv;
            bool done = false;

            pe_hook::queue_game_thread([&]()
                                       {
                // Find PlayerController
                ue::UObject* pc = nullptr;
                static const char* pc_classes[] = {
                    "BP_PlayerController_C", "VR4PlayerController_BP_C",
                    "PFXPlayerController", "PlayerController", nullptr
                };
                for (int i = 0; pc_classes[i] && !pc; i++) {
                    auto* rc = rebuilder::rebuild(pc_classes[i]);
                    if (rc) pc = rc->get_first_instance();
                }
                if (!pc) pc = reflection::find_first_instance("PlayerController");

                if (!pc) {
                    std::lock_guard<std::mutex> lk(mtx);
                    exec_result = "ERROR: no PlayerController";
                    done = true;
                    cv.notify_one();
                    return;
                }

                auto* func = pe_hook::resolve_func_path("PlayerController:ConsoleCommand");
                if (!func) {
                    std::lock_guard<std::mutex> lk(mtx);
                    exec_result = "ERROR: ConsoleCommand not found";
                    done = true;
                    cv.notify_one();
                    return;
                }

                uint16_t parms_size = ue::ufunc_get_parms_size(func);
                std::vector<uint8_t> parms(parms_size > 0 ? parms_size : 128, 0);

                std::u16string u16cmd(console_cmd.begin(), console_cmd.end());
                u16cmd.push_back(u'\0');
                struct FStr { const char16_t* data; int32_t num, max; };
                FStr fs;
                fs.data = u16cmd.c_str();
                fs.num = static_cast<int32_t>(u16cmd.size());
                fs.max = fs.num;
                std::memcpy(parms.data(), &fs, sizeof(FStr));

                auto pe = pe_hook::get_original();
                if (!pe) pe = symbols::ProcessEvent;
                if (pe) {
                    pe(pc, func, parms.data());
                    std::lock_guard<std::mutex> lk(mtx);
                    exec_result = "OK: executed '" + console_cmd + "'";
                } else {
                    std::lock_guard<std::mutex> lk(mtx);
                    exec_result = "ERROR: ProcessEvent not available";
                }
                done = true;
                cv.notify_one(); });

            // Wait up to 8 seconds for game thread to execute
            {
                std::unique_lock<std::mutex> lk(mtx);
                if (!cv.wait_for(lk, std::chrono::seconds(8), [&]
                                 { return done; }))
                {
                    return error_response("exec_console timed out");
                }
            }

            logger::log_info("ADB", "exec_console: %s -> %s", console_cmd.c_str(), exec_result.c_str());
            return ok_response(exec_result);
        }

        // dump_console_commands — dump all known console commands/CVars
        if (command == "dump_console_commands")
        {
            json result = json::array();
            const auto &classes = reflection::get_classes();
            for (const auto &ci : classes)
            {
                for (const auto &fi : ci.functions)
                {
                    if (fi.flags & 0x00000200)
                    { // FUNC_Exec
                        json entry;
                        entry["class"] = ci.name;
                        entry["name"] = fi.name;
                        entry["full"] = ci.name + ":" + fi.name;
                        entry["params"] = fi.num_parms;
                        result.push_back(entry);
                    }
                }
            }
            logger::log_info("ADB", "dump_console_commands: %d commands", (int)result.size());
            return ok_response(result);
        }

        // dump_symbols — writes all ELF symbols from libUE4.so to a file
        if (command == "dump_symbols")
        {
            std::string out_path = paths::data_dir() + "/symbol_dump.txt";
            int count = symbols::dump_symbols(out_path);
            if (count > 0)
            {
                json result;
                result["symbols"] = count;
                result["path"] = out_path;
                return ok_response(result);
            }
            return error_response("symbol dump failed — see log");
        }

        // log_stream is handled specially in the client loop
        if (command == "log_stream")
            return ""; // sentinel

        // help: list all built-in and custom commands
        if (command == "help")
        {
            json result = json::array();
            // list built-ins hardcoded as in dispatch table
            std::vector<std::string> built = {
                "list_mods", "reload_mod", "load_mod", "exec_lua", "list_hooks",
                "dump_sdk", "mount_pak", "list_paks", "log_tail", "get_stats",
                "find_object", "find_class", "object_count", "dump_symbols",
                "exec_console", "dump_console_commands",
                "pe_trace_start", "pe_trace_stop", "pe_trace_clear", "pe_trace_status",
                "pe_trace_top", "pe_trace_dump", "ping"};
            for (auto &b : built)
                result.push_back(b);
            // then append custom
            {
                std::lock_guard<std::mutex> lock(s_command_mutex);
                for (const auto &pair : s_custom_commands)
                {
                    result.push_back(pair.first);
                }
            }
            return ok_response(result);
        }

        // Check custom commands registered by Lua mods
        // These callbacks invoke Lua functions — must run on game thread
        {
            CommandHandler handler;
            std::string args;
            {
                std::lock_guard<std::mutex> lock(s_command_mutex);
                auto it = s_custom_commands.find(command);
                if (it != s_custom_commands.end())
                {
                    handler = it->second;
                    if (cmd.contains("args"))
                    {
                        args = cmd["args"].get<std::string>();
                    }
                }
            }
            if (handler)
            {
                struct CmdContext
                {
                    std::mutex mtx;
                    std::condition_variable cv;
                    std::atomic<bool> done{false};
                    std::atomic<bool> cancelled{false};
                    std::string result;
                };
                auto ctx = std::make_shared<CmdContext>();

                pe_hook::queue_game_thread([ctx, handler, args]()
                                           {
                if (ctx->cancelled.load(std::memory_order_acquire)) {
                    logger::log_warn("ADB", "Custom command: skipping cancelled/timed-out");
                    return;
                }
                ctx->result = handler(args);
                {
                    std::lock_guard<std::mutex> lk(ctx->mtx);
                    ctx->done.store(true, std::memory_order_release);
                }
                ctx->cv.notify_one(); });

                std::unique_lock<std::mutex> lk(ctx->mtx);
                if (!ctx->cv.wait_for(lk, std::chrono::seconds(8), [&]
                                      { return ctx->done.load(); }))
                {
                    ctx->cancelled.store(true, std::memory_order_release);
                    return error_response("command '" + command + "' timed out (game thread did not process within 8s)");
                }
                return ok_response(ctx->result);
            }
        }

        return error_response("unknown command: " + command);
    }

    // ═══ Handle a single client connection ══════════════════════════════════
    static void handle_client(int client_fd)
    {
        char buf[8192];
        std::string accumulated;

        while (s_running)
        {
            ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
            if (n <= 0)
                break;

            buf[n] = '\0';
            accumulated += buf;

            // Process complete lines (newline-delimited JSON)
            size_t pos;
            while ((pos = accumulated.find('\n')) != std::string::npos)
            {
                std::string line = accumulated.substr(0, pos);
                accumulated = accumulated.substr(pos + 1);

                if (line.empty())
                    continue;

                // Check for log_stream command
                {
                    json cmd = json::parse(line, nullptr, false);
                    if (!cmd.is_discarded() && cmd.contains("cmd") && cmd["cmd"] == "log_stream")
                    {
                        // Stream logs until client disconnects
                        logger::log_info("ADB", "Starting log stream for client");
                        logger::add_stream_socket(client_fd);

                        // Send existing tail first
                        auto tail = logger::get_tail(100);
                        if (!tail.empty())
                        {
                            send(client_fd, tail.c_str(), tail.size(), MSG_NOSIGNAL);
                        }

                        // Block until client disconnects — logger will push new lines via the stream socket
                        char drain[256];
                        while (s_running)
                        {
                            ssize_t r = recv(client_fd, drain, sizeof(drain), 0);
                            if (r <= 0)
                                break;
                        }

                        logger::remove_stream_socket(client_fd);
                        logger::log_info("ADB", "Log stream client disconnected");
                        close(client_fd);
                        return;
                    }
                }

                std::string response = dispatch(line);
                if (!response.empty())
                {
                    send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
                }
            }

            // If there is no newline yet, try processing accumulated as a single command
            if (!accumulated.empty() && accumulated.find('{') != std::string::npos && accumulated.find('}') != std::string::npos)
            {
                std::string response = dispatch(accumulated);
                accumulated.clear();
                if (!response.empty())
                {
                    send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
                }
            }
        }

        close(client_fd);
    }

    // ═══ Server thread ══════════════════════════════════════════════════════
    static void *server_thread_fn(void *arg)
    {
        (void)arg;

        while (s_running)
        {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            int client_fd = accept(s_server_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0)
            {
                if (s_running)
                {
                    logger::log_error("ADB", "accept() failed: %s", strerror(errno));
                }
                continue;
            }

            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
            logger::log_info("ADB", "Client connected from %s:%d",
                             client_ip, ntohs(client_addr.sin_port));

            // Handle client in a detached thread
            pthread_t client_thread;
            int *fd_copy = new int(client_fd);
            pthread_create(&client_thread, nullptr, [](void *arg) -> void *
                           {
            int fd = *reinterpret_cast<int*>(arg);
            delete reinterpret_cast<int*>(arg);
            handle_client(fd);
            return nullptr; }, fd_copy);
            pthread_detach(client_thread);
        }

        return nullptr;
    }

    // ═══ Public API ═════════════════════════════════════════════════════════
    bool start()
    {
        if (s_running)
            return true;

        s_start_time = std::chrono::steady_clock::now();

        s_server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (s_server_fd < 0)
        {
            logger::log_error("ADB", "socket() failed: %s", strerror(errno));
            return false;
        }

        int opt = 1;
        setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(ADB_BRIDGE_PORT);

        if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            logger::log_error("ADB", "bind() failed on port %d: %s", ADB_BRIDGE_PORT, strerror(errno));
            close(s_server_fd);
            s_server_fd = -1;
            return false;
        }

        if (listen(s_server_fd, 4) < 0)
        {
            logger::log_error("ADB", "listen() failed: %s", strerror(errno));
            close(s_server_fd);
            s_server_fd = -1;
            return false;
        }

        s_running = true;
        pthread_create(&s_server_thread, nullptr, server_thread_fn, nullptr);

        logger::log_info("ADB", "Bridge listening on 127.0.0.1:%d", ADB_BRIDGE_PORT);
        return true;
    }

    void stop()
    {
        s_running = false;
        if (s_server_fd >= 0)
        {
            shutdown(s_server_fd, SHUT_RDWR);
            close(s_server_fd);
            s_server_fd = -1;
        }
        logger::log_info("ADB", "Bridge stopped");
    }

    bool is_running()
    {
        return s_running;
    }

} // namespace adb_bridge
