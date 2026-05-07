#include "mi.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>

// --- GDB/MI wire protocol ---

static std::string mi_unescape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            continue;
        }
        char c = s[++i];
        switch (c) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default:  out.push_back(c);    break;
        }
    }
    return out;
}

static std::string mi_quote(const std::string &s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

static bool parse_token(std::string_view line, int &token, size_t &pos)
{
    pos = 0;
    while (pos < line.size() && isdigit((unsigned char)line[pos])) pos++;
    if (pos == 0) {
        token = 0;
        return false;
    }
    auto [ptr, ec] = std::from_chars(line.data(), line.data() + pos, token);
    return ec == std::errc{} && ptr == line.data() + pos;
}

static bool is_gdb_prompt(std::string_view line)
{
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        line.remove_suffix(1);
    return line == "(gdb)";
}

static void append_output_line(Debugger *dbg, const std::string &line)
{
    dbg->output += line;
    dbg->output.push_back('\n');
}

static std::string extract_cstring(std::string_view text, std::string_view key)
{
    std::string needle(key);
    needle += "=\"";
    size_t p = text.find(needle);
    if (p == std::string_view::npos) return {};
    p += needle.size();

    std::string raw;
    bool escape = false;
    for (size_t i = p; i < text.size(); i++) {
        char c = text[i];
        if (escape) {
            raw.push_back('\\');
            raw.push_back(c);
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else if (c == '"') {
            return mi_unescape(raw);
        } else {
            raw.push_back(c);
        }
    }
    return {};
}

static int extract_int(std::string_view text, std::string_view key, int fallback = 0)
{
    std::string s = extract_cstring(text, key);
    if (s.empty()) {
        std::string needle(key);
        needle += '=';
        size_t p = text.find(needle);
        if (p == std::string_view::npos) return fallback;
        p += needle.size();
        size_t e = p;
        while (e < text.size() && isdigit((unsigned char)text[e])) e++;
        s.assign(text.substr(p, e - p));
    }
    int v = fallback;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} ? v : fallback;
}

static size_t find_matching(std::string_view text, size_t open_pos)
{
    char open = text[open_pos];
    char close = open == '{' ? '}' : ']';
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (size_t i = open_pos; i < text.size(); i++) {
        char c = text[i];
        if (in_string) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == open) depth++;
        else if (c == close && --depth == 0) return i;
    }
    return std::string_view::npos;
}

static std::vector<std::string_view> extract_objects(std::string_view text)
{
    std::vector<std::string_view> objects;
    size_t p = 0;
    while ((p = text.find('{', p)) != std::string_view::npos) {
        size_t e = find_matching(text, p);
        if (e == std::string_view::npos) break;
        objects.push_back(text.substr(p, e - p + 1));
        p = e + 1;
    }
    return objects;
}

static void log_message(Debugger *dbg, bool sent,
                        const std::string &summary,
                        const std::string &raw)
{
    Debugger::DapMessage m;
    m.sent = sent;
    m.summary = summary;
    m.raw = raw;
    m.timestamp = platform_now_iso8601();
    dbg->dap_log.push_back(std::move(m));
}

void mi_command(Debugger *dbg, const std::string &command,
                const std::string &operation)
{
    int token = dbg->next_seq++;
    std::string line = std::to_string(token) + command;
    std::string wire = line + "\n";
    if (!operation.empty()) dbg->pending_mi_ops[token] = operation;
    log_message(dbg, true, "-> " + command, line);
    if (dbg->pipe_write)
        dbg->pipe_write(dbg->platform, wire.data(), (int)wire.size());
}

void mi_continue(Debugger *dbg)
{
    mi_command(dbg, "-exec-continue", "continue");
    dbg->state = DapState::RUNNING;
    dbg->status = "Running";
}

void mi_interrupt(Debugger *dbg)
{
    mi_command(dbg, "-exec-interrupt", "interrupt");
}

void mi_next(Debugger *dbg)
{
    mi_command(dbg, "-exec-next", "next");
    dbg->state = DapState::RUNNING;
    dbg->status = "Running";
}

void mi_step(Debugger *dbg)
{
    mi_command(dbg, "-exec-step", "step");
    dbg->state = DapState::RUNNING;
    dbg->status = "Running";
}

void mi_finish(Debugger *dbg)
{
    mi_command(dbg, "-exec-finish", "finish");
    dbg->state = DapState::RUNNING;
    dbg->status = "Running";
}

// Reader thread: reads line-oriented MI records and pushes complete lines.
void reader_thread_func(Debugger *dbg)
{
    std::string buf;
    char tmp[4096];

    while (dbg->reader_running.load()) {
        int n = dbg->pipe_read(dbg->platform, tmp, sizeof(tmp));
        if (n <= 0) {
            dbg->reader_running.store(false);
            break;
        }
        buf.append(tmp, (size_t)n);
        for (;;) {
            size_t nl = buf.find('\n');
            if (nl == std::string::npos) break;
            std::string line = buf.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buf.erase(0, nl + 1);
            if (line.empty()) continue;
            std::lock_guard<std::mutex> lock(dbg->queue_mutex);
            dbg->inbox.push_back(std::move(line));
        }
    }
}

void stdout_thread_func(Debugger *dbg)
{
    char tmp[4096];
    while (dbg->stdout_running.load()) {
        int n = dbg->stdout_read(dbg->platform, tmp, sizeof(tmp));
        if (n < 0) continue;
        if (n == 0) break;
        std::lock_guard<std::mutex> lock(dbg->queue_mutex);
        dbg->stdout_pending.append(tmp, (size_t)n);
    }
    dbg->stdout_running.store(false);
}

// --- Source file cache ---

SourceFile *get_source(Debugger *dbg, const std::string &path)
{
    std::string norm = platform_realpath(path);
    for (auto &sf : dbg->sources) {
        if (sf.path == norm) return &sf;
    }

    std::string content = platform_read_file(norm.c_str());
    if (content.empty()) return nullptr;

    SourceFile sf;
    sf.path = norm;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) {
            sf.lines.push_back(content.substr(pos));
            break;
        }
        size_t end = (nl > pos && content[nl - 1] == '\r') ? nl - 1 : nl;
        sf.lines.push_back(content.substr(pos, end - pos));
        pos = nl + 1;
    }
    dbg->sources.push_back(std::move(sf));
    return &dbg->sources.back();
}

// --- Breakpoint helpers ---

static std::string breakpoint_location(const std::string &path, int line)
{
    return path + ":" + std::to_string(line);
}

void send_breakpoints_for_file(Debugger *dbg, const std::string &path)
{
    for (auto &bp : dbg->breakpoints) {
        if (bp.path != path) continue;
        if (bp.id > 0) {
            mi_command(dbg, "-break-delete " + std::to_string(bp.id),
                       "break-delete");
            bp.id = 0;
        }
    }

    for (auto &bp : dbg->breakpoints) {
        if (bp.path != path || !bp.enabled) continue;
        std::string loc = breakpoint_location(path, bp.line);
        int token = dbg->next_seq;
        mi_command(dbg, "-break-insert " + mi_quote(loc), "break-insert");
        dbg->pending_bps[token] = path + "\t" + std::to_string(bp.line);
    }

    if (dbg->run_to_path == path && dbg->run_to_line > 0) {
        std::string loc = breakpoint_location(path, dbg->run_to_line);
        mi_command(dbg, "-break-insert -t " + mi_quote(loc), "break-insert");
    }
}

void send_exception_breakpoints(Debugger *)
{
}

void toggle_breakpoint(Debugger *dbg, const std::string &path, int line)
{
    std::string norm = platform_realpath(path);
    for (auto it = dbg->breakpoints.begin(); it != dbg->breakpoints.end(); ++it) {
        if (it->path == norm && it->line == line) {
            int id = it->id;
            dbg->breakpoints.erase(it);
            if (id > 0 && dbg->state != DapState::IDLE &&
                dbg->state != DapState::TERMINATED) {
                mi_command(dbg, "-break-delete " + std::to_string(id),
                           "break-delete");
            }
            return;
        }
    }

    LineBreakpoint bp;
    bp.path = norm;
    bp.line = line;
    SourceFile *sf = get_source(dbg, path);
    if (sf && line >= 1 && line <= (int)sf->lines.size())
        bp.line_text = sf->lines[(size_t)(line - 1)];
    dbg->breakpoints.push_back(bp);
    if (dbg->state != DapState::IDLE && dbg->state != DapState::TERMINATED)
        send_breakpoints_for_file(dbg, norm);
}

// Returns: 0 = no breakpoint, 1 = enabled, 2 = disabled
int has_breakpoint(Debugger *dbg, const std::string &path, int line)
{
    for (auto &bp : dbg->breakpoints) {
        if (bp.path == path && bp.line == line)
            return bp.enabled ? 1 : 2;
    }
    return 0;
}

static std::string_view strip(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

void relocate_breakpoints(Debugger *dbg, const std::string &path)
{
    SourceFile *sf = get_source(dbg, path);
    if (!sf) return;

    std::vector<LineBreakpoint *> bps;
    for (auto &bp : dbg->breakpoints) {
        if (bp.path == path && !bp.line_text.empty()) bps.push_back(&bp);
    }
    std::sort(bps.begin(), bps.end(),
              [](auto *a, auto *b) { return a->line < b->line; });

    int num_lines = (int)sf->lines.size();
    int min_line = 1;
    constexpr int max_search = 100;

    for (auto *bp : bps) {
        std::string_view target = strip(bp->line_text);
        int best = -1;
        for (int delta = 0; delta <= max_search; delta++) {
            int candidates[2] = {bp->line + delta, bp->line - delta};
            for (int c : candidates) {
                if (c < min_line || c > num_lines) continue;
                if (strip(sf->lines[(size_t)(c - 1)]) == target) {
                    best = c;
                    goto found;
                }
            }
        }
    found:
        if (best > 0) {
            bp->line = best;
            min_line = best + 1;
        } else {
            if (bp->line < min_line) bp->line = min_line;
            min_line = bp->line + 1;
        }
    }
}

void open_source(Debugger *dbg, const std::string &path)
{
    std::string norm = platform_realpath(path);
    for (auto &os : dbg->open_sources) {
        if (os.path == norm) {
            os.focus = true;
            return;
        }
    }
    OpenSource os;
    os.path = norm;
    os.focus = true;
    dbg->open_sources.push_back(std::move(os));
}

void fetch_variables(Debugger *, int64_t)
{
}

static void mark_changed_variables(std::vector<DapVariable> &prev,
                                   std::vector<DapVariable> &next)
{
    std::unordered_map<std::string, std::string> old_vals;
    for (auto &v : prev)
        old_vals[v.name] = v.value;
    for (auto &v : next) {
        auto it = old_vals.find(v.name);
        v.changed = (it == old_vals.end() || it->second != v.value);
    }
}

static void parse_stack(Debugger *dbg, std::string_view line)
{
    std::vector<StackFrame> stack;
    for (std::string_view obj : extract_objects(line)) {
        if (obj.find("level=") == std::string_view::npos ||
            obj.find("func=") == std::string_view::npos)
            continue;
        StackFrame f;
        f.id = extract_int(obj, "level", (int)stack.size());
        f.name = extract_cstring(obj, "func");
        f.source_path = extract_cstring(obj, "fullname");
        if (f.source_path.empty()) f.source_path = extract_cstring(obj, "file");
        if (!f.source_path.empty()) f.source_path = platform_realpath(f.source_path);
        f.line = extract_int(obj, "line", 0);
        stack.push_back(std::move(f));
    }

    dbg->stack = std::move(stack);
    if (!dbg->stack.empty()) {
        auto &top = dbg->stack[0];
        if (!top.source_path.empty()) {
            dbg->current_source = get_source(dbg, top.source_path);
            dbg->current_line = top.line;
            dbg->scroll_to_line = true;
            open_source(dbg, top.source_path);
        }
    }
    dbg->state = DapState::STOPPED;
    dbg->status = "Paused";
    mi_command(dbg, "-stack-list-variables --all-values", "stack-list-variables");
}

static void parse_variables(Debugger *dbg, std::string_view line)
{
    std::vector<DapVariable> vars;
    for (std::string_view obj : extract_objects(line)) {
        if (obj.find("name=") == std::string_view::npos) continue;
        DapVariable v;
        v.name = extract_cstring(obj, "name");
        v.value = extract_cstring(obj, "value");
        v.type = extract_cstring(obj, "type");
        if (!v.name.empty()) vars.push_back(std::move(v));
    }

    DapScope scope;
    scope.name = "Locals";
    scope.fetched = true;
    scope.variables = std::move(vars);

    if (!dbg->scopes.empty())
        mark_changed_variables(dbg->scopes[0].variables, scope.variables);
    dbg->scopes.clear();
    dbg->scopes.push_back(std::move(scope));
}

static void finish_break_insert(Debugger *dbg, int token, std::string_view line)
{
    auto it = dbg->pending_bps.find(token);
    if (it == dbg->pending_bps.end()) return;

    std::string key = std::move(it->second);
    dbg->pending_bps.erase(it);
    size_t tab = key.rfind('\t');
    if (tab == std::string::npos) return;
    std::string path = key.substr(0, tab);
    int line_num = std::atoi(key.c_str() + tab + 1);
    int id = extract_int(line, "number", 0);

    for (auto &bp : dbg->breakpoints) {
        if (bp.path == path && bp.line == line_num) {
            bp.id = id;
            int actual = extract_int(line, "line", 0);
            if (actual > 0) bp.line = actual;
            break;
        }
    }
}

static void handle_result(Debugger *dbg, int token, std::string_view line)
{
    std::string op;
    auto it = dbg->pending_mi_ops.find(token);
    if (it != dbg->pending_mi_ops.end()) {
        op = std::move(it->second);
        dbg->pending_mi_ops.erase(it);
    }

    if (line.find("^error") != std::string_view::npos) {
        std::string msg = extract_cstring(line, "msg");
        dbg->status = msg.empty() ? "GDB error" : "GDB error: " + msg;
        return;
    }
    if (line.find("^running") != std::string_view::npos) {
        dbg->state = DapState::RUNNING;
        dbg->status = "Running";
        return;
    }

    if (op == "stack-list-frames") {
        parse_stack(dbg, line);
    } else if (op == "stack-list-variables") {
        parse_variables(dbg, line);
    } else if (op == "break-insert") {
        finish_break_insert(dbg, token, line);
    }
}

static void handle_async(Debugger *dbg, std::string_view line)
{
    if (line.starts_with("*running")) {
        dbg->state = DapState::RUNNING;
        dbg->status = "Running";
        return;
    }
    if (line.starts_with("*stopped")) {
        std::string reason = extract_cstring(line, "reason");

        // Inferior is gone. ggdb runs a fresh GDB per debuggee, so we
        // ask GDB to quit; the reader thread will see EOF and the
        // shared teardown path returns us to IDLE with this status.
        if (reason.starts_with("exited")) {
            std::string status_msg;
            if (reason == "exited-normally") {
                status_msg = "Exited normally";
            } else if (reason == "exited") {
                std::string code = extract_cstring(line, "exit-code");
                status_msg = code.empty() ? "Exited"
                                          : "Exited (code " + code + ")";
            } else if (reason == "exited-signalled") {
                std::string sig = extract_cstring(line, "signal-name");
                status_msg = sig.empty() ? "Exited (signalled)"
                                         : "Exited (" + sig + ")";
            } else {
                status_msg = "Exited (" + reason + ")";
            }
            dbg->status = std::move(status_msg);
            dbg->state = DapState::TERMINATED;
            mi_command(dbg, "-gdb-exit", "gdb-exit");
            return;
        }

        dbg->status = reason.empty() ? "Paused" : "Paused (" + reason + ")";
        mi_command(dbg, "-stack-list-frames", "stack-list-frames");

        if (dbg->run_to_line) {
            std::string path = std::move(dbg->run_to_path);
            dbg->run_to_line = 0;
            send_breakpoints_for_file(dbg, path);
        }
    }
}

static bool handle_stream(Debugger *dbg, char kind, std::string_view line)
{
    if (line.size() < 3 || line[1] != '"') return false;
    std::string text = mi_unescape(line.substr(2, line.size() - 3));
    if (kind == '@') dbg->output += text;
    return true;
}

void mi_start_session(Debugger *dbg)
{
    mi_command(dbg, "-gdb-set mi-async on", "gdb-set");
#ifdef _WIN32
    mi_command(dbg, "-gdb-set new-console on", "gdb-set");
#endif
    if (!dbg->inferior_tty_path.empty()) {
        mi_command(dbg, "-inferior-tty-set " + mi_quote(dbg->inferior_tty_path),
                   "inferior-tty-set");
    }

    std::vector<std::string> files;
    for (auto &bp : dbg->breakpoints) {
        bool found = false;
        for (auto &f : files) {
            if (f == bp.path) { found = true; break; }
        }
        if (!found) files.push_back(bp.path);
    }
    for (auto &f : files) {
        relocate_breakpoints(dbg, f);
        send_breakpoints_for_file(dbg, f);
    }

    mi_command(dbg, dbg->pause_at_entry ? "-exec-run --start" : "-exec-run",
               "run");
}

void process_messages(Debugger *dbg)
{
    std::vector<std::string> messages;
    {
        std::lock_guard<std::mutex> lock(dbg->queue_mutex);
        messages.swap(dbg->inbox);
        if (!dbg->stdout_pending.empty()) {
            dbg->output += dbg->stdout_pending;
            dbg->stdout_pending.clear();
        }
    }

    for (auto &raw : messages) {
        if (is_gdb_prompt(raw)) continue;
        log_message(dbg, false, "<- " + raw.substr(0, raw.find(',')), raw);

        int token = 0;
        size_t pos = 0;
        parse_token(raw, token, pos);
        if (pos >= raw.size()) {
            append_output_line(dbg, raw);
            continue;
        }

        char kind = raw[pos];
        std::string_view body(raw.data() + pos, raw.size() - pos);
        if (kind == '^') {
            handle_result(dbg, token, body);
        } else if (kind == '*') {
            handle_async(dbg, body);
        } else if (kind == '=' || kind == '+') {
            // Async notifications are protocol metadata, not inferior output.
        } else if (kind == '~' || kind == '@' || kind == '&') {
            if (!handle_stream(dbg, kind, body))
                append_output_line(dbg, raw);
        } else {
            // On Windows especially, inferior stdout can arrive as raw text
            // interleaved with MI records instead of as @ target-stream output.
            append_output_line(dbg, raw);
        }
    }

    if (!dbg->reader_running.load() && dbg->state != DapState::IDLE) {
        dcmake_stop(dbg);
    }
}
