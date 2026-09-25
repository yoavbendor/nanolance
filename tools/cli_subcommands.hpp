// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

/// The `nanolance` CLI's subcommands, each of which is also still its own binary.
///
/// Four tools with four naming conventions (`arrowipc2lance`, `nlance2table`, `nlance_info`,
/// `nlance_stitch`) is a discoverability tax: nothing tells you the other three exist. They are now
/// subcommands of one `nanolance` binary, so `nanolance --help` teaches the whole surface.
///
/// Each implementation file defines its subcommand here and keeps its own `main` when built as a
/// standalone target -- see `NANOLANCE_CLI_SUBCOMMAND` at the bottom of each. One implementation,
/// two front doors; the old names keep working for anything that already scripts them.
///
/// `argv[0]` is the program name the subcommand should print in its usage text; the dispatcher
/// passes "nanolance <sub>".

int nanolance_cli_import(int argc, char** argv);  ///< arrowipc2lance: Arrow IPC stream -> Lance
int nanolance_cli_info(int argc, char** argv);    ///< nlance_info: fragment/row statistics
int nanolance_cli_cat(int argc, char** argv);     ///< nlance2table: dataset -> CSV/NDJSON
int nanolance_cli_stitch(int argc, char** argv);  ///< nlance_stitch: many datasets -> one
