#pragma once

#include "dcmake.hpp"

#include <string>

void mi_command(Debugger *dbg, const std::string &command,
                const std::string &operation = {});
void mi_start_session(Debugger *dbg);
void mi_continue(Debugger *dbg);
void mi_interrupt(Debugger *dbg);
void mi_next(Debugger *dbg);
void mi_step(Debugger *dbg);
void mi_finish(Debugger *dbg);

void process_messages(Debugger *dbg);
void reader_thread_func(Debugger *dbg);
void stdout_thread_func(Debugger *dbg);

SourceFile *get_source(Debugger *dbg, const std::string &path);
void open_source(Debugger *dbg, const std::string &path);
void toggle_breakpoint(Debugger *dbg, const std::string &path, int line);
int has_breakpoint(Debugger *dbg, const std::string &path, int line);
void send_breakpoints_for_file(Debugger *dbg, const std::string &path);
void send_exception_breakpoints(Debugger *dbg);
void relocate_breakpoints(Debugger *dbg, const std::string &path);
void fetch_variables(Debugger *dbg, int64_t ref);
