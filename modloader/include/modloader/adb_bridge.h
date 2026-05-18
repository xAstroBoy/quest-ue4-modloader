#pragma once
// modloader/include/modloader/adb_bridge.h
// TCP socket server on 127.0.0.1:19420 — ADB command bridge
// No root required. JSON protocol.

#include <string>
#include <functional>
#include <vector>

namespace adb_bridge {

constexpr int ADB_BRIDGE_PORT = 19420;

// Command handler type: takes args string, returns response string
using CommandHandler = std::function<std::string(const std::string&)>;

// Start the TCP server on a background thread
bool start();

// Stop the server
void stop();

// Check if the server is running
bool is_running();

// True while a bridge command is actively executing on the game thread.
// Used by Lua bindings to reject crash-prone bulk scans from live bridge calls.
bool is_game_thread_command_active();

// Register a custom command handler (from Lua mods via RegisterCommand)
void register_command(const std::string& name, CommandHandler handler);

// Get names of all registered custom commands
std::vector<std::string> get_registered_commands();

} // namespace adb_bridge
