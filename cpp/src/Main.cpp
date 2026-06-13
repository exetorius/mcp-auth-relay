// mcp-auth-relay — C++ implementation
// Lightweight MCP relay with bearer token injection, first-run setup,
// OS startup registration, and /relay-* commands.
//
// Build: cmake -B build -S . && cmake --build build --config Release
// Run:   ./mcp-auth-relay  (reads config.json from the same directory as the executable)

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#  define IS_TTY (_isatty(_fileno(stdin)))
#else
#  include <unistd.h>
#  define IS_TTY (isatty(STDIN_FILENO))
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct Config
{
    std::string bearer_token;
    std::string upstream_host     = "127.0.0.1";
    int         upstream_port     = 8088;
    int         proxy_port        = 8089;
    std::string manifest_path;
    std::string integration;
    std::string server_name       = "mcp-auth-relay";
    std::string instructions;
    bool        startup_asked     = false;
    bool        startup_registered = false;
};

static fs::path g_config_path;

static Config load_config(const fs::path& path)
{
    Config cfg;
    if (!fs::exists(path)) return cfg;
    try
    {
        std::ifstream f(path);
        auto j = json::parse(f);
        cfg.bearer_token        = j.value("bearer_token",        "");
        cfg.upstream_host       = j.value("upstream_host",       "127.0.0.1");
        cfg.upstream_port       = j.value("upstream_port",       8088);
        cfg.proxy_port          = j.value("proxy_port",          8089);
        cfg.integration         = j.value("integration",         "");
        cfg.server_name         = j.value("server_name",         "mcp-auth-relay");
        cfg.instructions        = j.value("instructions",        "");
        cfg.startup_asked       = j.value("startup_asked",       false);
        cfg.startup_registered  = j.value("startup_registered",  false);

        std::string raw_path = j.value("manifest_path", "");
#if defined(_WIN32)
        char expanded[MAX_PATH] = {};
        if (!raw_path.empty() && ExpandEnvironmentStringsA(raw_path.c_str(), expanded, MAX_PATH))
            cfg.manifest_path = expanded;
        else
            cfg.manifest_path = raw_path;
#else
        if (!raw_path.empty() && raw_path[0] == '~')
        {
            const char* home = std::getenv("HOME");
            cfg.manifest_path = std::string(home ? home : "") + raw_path.substr(1);
        }
        else cfg.manifest_path = raw_path;
#endif
    }
    catch (...) {}
    return cfg;
}

static void save_config_key(const std::string& key, const json& value)
{
    json j = json::object();
    if (fs::exists(g_config_path))
    {
        try { std::ifstream f(g_config_path); j = json::parse(f); }
        catch (...) {}
    }
    j[key] = value;
    std::ofstream f(g_config_path);
    f << j.dump(2);
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

static json load_manifest(const std::string& manifest_path)
{
    if (manifest_path.empty()) return json::array();
    try
    {
        std::ifstream f(manifest_path);
        if (!f.is_open()) return json::array();
        return json::parse(f);
    }
    catch (...) { return json::array(); }
}

// ---------------------------------------------------------------------------
// Integration pack
// ---------------------------------------------------------------------------

static json        g_hints;
static json        g_synthetic_tools = json::array();
static std::string g_instructions;

static std::string load_integration(const fs::path& repo_root, const std::string& name, Config& cfg)
{
    g_hints          = json::object();
    g_synthetic_tools = json::array();

    if (name.empty()) return "";

    fs::path pack = repo_root / "integrations" / name;
    if (!fs::exists(pack))
        return "Integration pack '" + name + "' not found at " + pack.string();

    std::vector<std::string> parts;

    fs::path hints_path = pack / "hints.json";
    if (fs::exists(hints_path))
    {
        try
        {
            std::ifstream f(hints_path);
            g_hints = json::parse(f);
            parts.push_back(std::to_string(g_hints.size()) + " hints");
        }
        catch (const std::exception& e) { parts.push_back(std::string("hints ERROR: ") + e.what()); }
    }

    fs::path synth_path = pack / "synthetic_tools.json";
    if (fs::exists(synth_path))
    {
        try
        {
            std::ifstream f(synth_path);
            g_synthetic_tools = json::parse(f);
            parts.push_back(std::to_string(g_synthetic_tools.size()) + " synthetic tools");
        }
        catch (const std::exception& e) { parts.push_back(std::string("synthetic_tools ERROR: ") + e.what()); }
    }

    fs::path instr_path = pack / "instructions.md";
    if (fs::exists(instr_path) && cfg.instructions.empty())
    {
        try
        {
            std::ifstream f(instr_path);
            std::ostringstream ss; ss << f.rdbuf();
            cfg.instructions = ss.str();
            g_instructions   = cfg.instructions;
            parts.push_back("instructions (" + std::to_string(cfg.instructions.size()) + " bytes)");
        }
        catch (const std::exception& e) { parts.push_back(std::string("instructions ERROR: ") + e.what()); }
    }

    std::string result = "Integration '" + name + "' loaded";
    if (!parts.empty())
    {
        result += " — ";
        for (size_t i = 0; i < parts.size(); ++i)
            result += (i ? ", " : "") + parts[i];
    }
    return result;
}

// ---------------------------------------------------------------------------
// Hint injection
// ---------------------------------------------------------------------------

static json apply_hints(const json& tools)
{
    if (g_hints.is_null() || g_hints.empty()) return tools;
    json result = json::array();
    for (auto tool : tools)
    {
        std::string name = tool.value("name", "");
        if (g_hints.contains(name))
            tool["description"] = tool.value("description", "") + g_hints[name].get<std::string>();
        result.push_back(tool);
    }
    return result;
}

// ---------------------------------------------------------------------------
// OS startup registration
// ---------------------------------------------------------------------------

static fs::path g_exe_path;

static std::pair<bool, std::string> register_startup()
{
    std::string exe = g_exe_path.string();

#if defined(_WIN32)
    std::string cmd =
        "schtasks /Create /TN \"mcp-auth-relay\" /TR \"\\\"" + exe + "\\\"\" "
        "/SC ONLOGON /RL HIGHEST /F";
    int r = std::system(cmd.c_str());
    if (r == 0)
        return {true, "Registered via Task Scheduler — relay will start automatically at login."};
    return {false, "Task Scheduler registration failed. Try running as administrator."};

#elif defined(__APPLE__)
    fs::path plist_dir  = fs::path(std::getenv("HOME")) / "Library" / "LaunchAgents";
    fs::path plist_path = plist_dir / "com.mcp-auth-relay.plist";
    fs::create_directories(plist_dir);
    std::ofstream f(plist_path);
    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      << "<plist version=\"1.0\"><dict>\n"
      << "  <key>Label</key><string>com.mcp-auth-relay</string>\n"
      << "  <key>ProgramArguments</key><array><string>" << exe << "</string></array>\n"
      << "  <key>RunAtLoad</key><true/>\n"
      << "  <key>KeepAlive</key><true/>\n"
      << "</dict></plist>\n";
    f.close();
    std::system(("launchctl load " + plist_path.string()).c_str());
    return {true, "Registered via launchd — relay will start automatically at login."};

#else
    fs::path svc_dir  = fs::path(std::getenv("HOME")) / ".config" / "systemd" / "user";
    fs::path svc_path = svc_dir / "mcp-auth-relay.service";
    fs::create_directories(svc_dir);
    std::ofstream f(svc_path);
    f << "[Unit]\nDescription=mcp-auth-relay\nAfter=network.target\n\n"
      << "[Service]\nExecStart=" << exe << "\nRestart=on-failure\n\n"
      << "[Install]\nWantedBy=default.target\n";
    f.close();
    std::system("systemctl --user daemon-reload");
    int r = std::system("systemctl --user enable mcp-auth-relay");
    if (r == 0)
        return {true, "Registered via systemd user service — relay will start automatically at login."};
    return {false, "systemd registration failed."};
#endif
}

static std::pair<bool, std::string> unregister_startup()
{
#if defined(_WIN32)
    int r = std::system("schtasks /Delete /TN \"mcp-auth-relay\" /F");
    return r == 0
        ? std::make_pair(true,  std::string("Removed from Task Scheduler."))
        : std::make_pair(false, std::string("Could not remove — may not have been registered."));

#elif defined(__APPLE__)
    fs::path plist = fs::path(std::getenv("HOME")) / "Library" / "LaunchAgents" / "com.mcp-auth-relay.plist";
    std::system(("launchctl unload " + plist.string()).c_str());
    fs::remove(plist);
    return {true, "Removed from launchd."};

#else
    std::system("systemctl --user disable mcp-auth-relay");
    fs::remove(fs::path(std::getenv("HOME")) / ".config" / "systemd" / "user" / "mcp-auth-relay.service");
    return {true, "Removed from systemd."};
#endif
}

// ---------------------------------------------------------------------------
// Setup menu
// ---------------------------------------------------------------------------

static Config* g_cfg_ptr = nullptr;

static void run_setup_menu()
{
    std::cout << "\n"
              << "  ╔══════════════════════════════════════════╗\n"
              << "  ║          mcp-auth-relay  setup           ║\n"
              << "  ╚══════════════════════════════════════════╝\n\n";

    if (g_cfg_ptr->startup_registered)
    {
        std::cout << "  Startup with OS: ENABLED\n\n"
                  << "  1. Disable startup with OS\n"
                  << "  2. Done\n\n"
                  << "  Enter choice [1-2]: " << std::flush;
        std::string choice; std::getline(std::cin, choice);
        if (choice == "1")
        {
            auto [ok, msg] = unregister_startup();
            std::cout << "\n  " << msg << "\n\n";
            save_config_key("startup_registered", false);
            save_config_key("startup_asked",      true);
            g_cfg_ptr->startup_registered = false;
        }
        return;
    }

    std::cout
        << "  How would you like to start the relay?\n\n"
        << "  1. Start with OS  (recommended)\n"
        << "     Relay starts automatically at login — no manual step needed.\n\n"
        << "  2. Start manually each time\n"
        << "     Run mcp-auth-relay when you need it.\n\n"
        << "  3. Ask me next time\n"
        << "     Start now, prompt again on next launch.\n\n"
        << "  Enter choice [1-3]: " << std::flush;

    std::string choice;
    std::getline(std::cin, choice);
    std::cout << "\n";

    if (choice == "1")
    {
        auto [ok, msg] = register_startup();
        std::cout << "  " << msg << "\n";
        if (!ok) std::cout << "  You may need to run as administrator and try again.\n";
        save_config_key("startup_asked",      true);
        save_config_key("startup_registered", ok);
        g_cfg_ptr->startup_registered = ok;
    }
    else if (choice == "2")
    {
        std::cout << "  Got it — starting manually. Type /relay-setup to change this later.\n";
        save_config_key("startup_asked",      true);
        save_config_key("startup_registered", false);
    }
    else
    {
        std::cout << "  OK — will ask again next time.\n";
        save_config_key("startup_asked",      false);
        save_config_key("startup_registered", false);
    }
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

static fs::path g_repo_root;

static void cmd_status(const Config& cfg)
{
    std::cout << "\n  mcp-auth-relay\n"
              << "  ----------------------------------------\n"
              << "  Listening:    http://127.0.0.1:" << cfg.proxy_port << "/mcp\n"
              << "  Upstream:     http://" << cfg.upstream_host << ":" << cfg.upstream_port << "/mcp\n"
              << "  Token:        " << (cfg.bearer_token.empty() ? "NOT SET" : "set") << "\n"
              << "  Manifest:     " << (cfg.manifest_path.empty() ? "not configured" : cfg.manifest_path) << "\n";
    if (!cfg.manifest_path.empty())
        std::cout << "  Tools:        " << load_manifest(cfg.manifest_path).size()
                  << " from manifest + " << g_synthetic_tools.size() << " synthetic\n";
    if (!cfg.integration.empty())
        std::cout << "  Integration:  " << cfg.integration
                  << " (" << g_hints.size() << " hints, " << g_synthetic_tools.size() << " synthetic tools)\n";
    else
        std::cout << "  Integration:  none — type /relay-packs to install one\n";
    std::cout << "  Startup:      " << (cfg.startup_registered ? "enabled" : "manual") << "\n\n";
}

static void cmd_reload(Config& cfg)
{
    cfg = load_config(g_config_path);
    auto status = load_integration(g_repo_root, cfg.integration, cfg);
    std::cout << "  Reloaded. " << (status.empty() ? "No integration." : status) << "\n\n";
}

static void command_loop(Config& cfg)
{
    std::string line;
    while (std::getline(std::cin, line))
    {
        // trim
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();

        if (line.empty()) continue;

        if (line == "/relay-quit" || line == "/relay-exit")
        {
            std::cout << "[relay] Relay stopped.\n";
            std::exit(0);
        }
        else if (line == "/relay-setup")  { run_setup_menu(); }
        else if (line == "/relay-status") { cmd_status(cfg); }
        else if (line == "/relay-reload") { cmd_reload(cfg); }
        else if (line == "/relay-packs")
        {
            std::cout << "  /relay-packs: use 'python proxy.py' for interactive pack installation,\n"
                      << "  or manually clone a pack into the integrations/ folder.\n\n";
        }
        else
        {
            std::cout << "  Unknown command '" << line << "'.\n"
                      << "  Available: /relay-setup /relay-packs /relay-status /relay-reload /relay-quit\n\n";
        }
    }
}

// ---------------------------------------------------------------------------
// Forward to upstream
// ---------------------------------------------------------------------------

struct ForwardResult { bool success; std::string body; };

static ForwardResult forward_to_upstream(const std::string& request_body, const Config& cfg)
{
    httplib::Client client(cfg.upstream_host, cfg.upstream_port);
    client.set_connection_timeout(2);
    client.set_read_timeout(120);

    httplib::Headers headers = {
        {"X-MCP-Auth-Relay", "true"},
        {"Connection",       "close"},
    };
    if (!cfg.bearer_token.empty())
        headers.emplace("Authorization", "Bearer " + cfg.bearer_token);

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        auto r = client.Post("/mcp", headers, request_body, "application/json");
        if (r && r->status == 200) return {true, r->body};
        if (r && r->status != 200) return {false, r->body};
    }
    return {false, ""};
}

static json upstream_error_response(const json& req_id, const std::string& tool_name, const std::string& upstream_msg)
{
    std::string text = upstream_msg.empty()
        ? "Upstream server is not running.\nPlease start your MCP server, then retry '" + tool_name + "'."
        : "Upstream server rejected the request: " + upstream_msg + "\n"
          "Check that bearer_token in config.json matches the upstream server's expected token.";
    return {{"jsonrpc","2.0"},{"id",req_id},
            {"result",{{"content",json::array({{{"type","text"},{"text",text}}})},{"isError",true}}}};
}

// ---------------------------------------------------------------------------
// CORS
// ---------------------------------------------------------------------------

static void add_cors(httplib::Response& res)
{
    res.set_header("Access-Control-Allow-Origin",  "*");
    res.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, MCP-Protocol-Version, mcp-session-id");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    g_exe_path   = fs::path(argv[0]).parent_path() / fs::path(argv[0]).filename();
    fs::path exe_dir  = fs::path(argv[0]).parent_path();
    g_repo_root  = exe_dir.parent_path(); // bin/ -> mcp-auth-relay/
    g_config_path = exe_dir / "config.json";

    bool first_run = !fs::exists(g_config_path);

    Config cfg = load_config(g_config_path);
    g_cfg_ptr  = &cfg;
    auto intg_status = load_integration(g_repo_root, cfg.integration, cfg);

    // Startup log
    if (cfg.bearer_token.empty())
        std::cout << "[relay] WARNING: bearer_token is empty — upstream requests will be unauthenticated.\n";
    else
        std::cout << "[relay] Bearer token loaded.\n";

    if (!cfg.manifest_path.empty())
    {
        auto m = load_manifest(cfg.manifest_path);
        if (m.empty())
            std::cout << "[relay] Manifest not found at " << cfg.manifest_path << " — tools/list will be empty until upstream writes it.\n";
        else
            std::cout << "[relay] Manifest: " << m.size() << " tools loaded.\n";
    }

    if (!intg_status.empty()) std::cout << "[relay] " << intg_status << "\n";

    std::cout << "[relay] mcp-auth-relay started — listening on http://127.0.0.1:" << cfg.proxy_port << "/mcp\n";
    std::cout << "[relay] Forwarding to upstream at http://" << cfg.upstream_host << ":" << cfg.upstream_port << "/mcp\n";

    // First-run / startup setup (only in interactive terminal)
    bool needs_setup = first_run || !cfg.startup_asked;
    if (needs_setup && IS_TTY)
        run_setup_menu();
    else if (cfg.integration.empty())
        std::cout << "\n  No integration pack loaded. Type /relay-packs for options.\n\n";

    // Start stdin command loop in background thread (TTY only)
    if (IS_TTY)
        std::thread([&cfg]() { command_loop(cfg); }).detach();

    // HTTP server
    httplib::Server svr;

    svr.Options("/mcp", [](const httplib::Request&, httplib::Response& res) {
        add_cors(res); res.status = 200;
    });

    svr.Get("/mcp", [](const httplib::Request& req, httplib::Response& res) {
        if (req.get_header_value("Accept").find("text/event-stream") == std::string::npos)
        {
            add_cors(res);
            res.set_content("mcp-auth-relay running", "text/plain");
            return;
        }
        std::cout << "[relay] SSE stream opened\n";
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection",    "keep-alive");
        add_cors(res);
        res.set_chunked_content_provider(
            "text/event-stream",
            [](size_t, httplib::DataSink& sink) {
                static const std::string hb = ": heartbeat\n\n";
                if (!sink.write(hb.c_str(), hb.size())) return false;
                std::this_thread::sleep_for(std::chrono::seconds(15));
                return true;
            },
            [](bool) { std::cout << "[relay] SSE stream closed\n"; }
        );
    });

    svr.Post("/mcp", [&cfg](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);
        res.set_header("Content-Type", "application/json");

        json rpc;
        try { rpc = json::parse(req.body); }
        catch (...) { res.status = 400; res.set_content(R"({"error":"Invalid JSON"})", "application/json"); return; }

        const auto method = rpc.value("method", "");
        const auto req_id = rpc.contains("id") ? rpc["id"] : json(nullptr);

        if (method == "initialize")
        {
            auto client_version = rpc.value("params/protocolVersion"_json_pointer, std::string("2024-11-05"));
            std::cout << "[relay] initialize (protocol " << client_version << ")\n";
            const std::string& instr = cfg.instructions.empty()
                ? std::string("MCP relay active. Upstream: http://") + cfg.upstream_host + ":"
                  + std::to_string(cfg.upstream_port) + "/mcp."
                : cfg.instructions;
            res.set_content(json({{"jsonrpc","2.0"},{"id",req_id},{"result",{
                {"protocolVersion", client_version},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", cfg.server_name}, {"version","1.0.0"}, {"instructions", instr}}}
            }}}).dump(), "application/json");
            return;
        }

        if (method == "notifications/initialized") { res.status = 202; return; }

        if (method == "tools/list")
        {
            auto tools = apply_hints(load_manifest(cfg.manifest_path));
            for (auto& t : g_synthetic_tools) tools.push_back(t);
            std::cout << "[relay] tools/list -> " << tools.size() << " tools\n";
            res.set_content(
                json({{"jsonrpc","2.0"},{"id",req_id},{"result",{{"tools",tools}}}}).dump(),
                "application/json");
            return;
        }

        auto [success, body] = forward_to_upstream(req.body, cfg);
        if (success)
        {
            std::cout << "[relay] " << method << " -> upstream\n";
            res.set_content(body, "application/json");
        }
        else
        {
            std::cout << "[relay] " << method << " -> upstream unreachable\n";
            if (method == "tools/call")
            {
                auto tool_name = rpc.value("/params/name"_json_pointer, std::string("unknown"));
                res.set_content(upstream_error_response(req_id, tool_name, body).dump(), "application/json");
            }
            else
                res.set_content(json({{"jsonrpc","2.0"},{"id",req_id},{"result",json::object()}}).dump(), "application/json");
        }
    });

    svr.listen("127.0.0.1", cfg.proxy_port);
    return 0;
}
