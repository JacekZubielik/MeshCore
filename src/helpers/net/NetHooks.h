#pragma once
// Lightweight hook so the shared CLI (MyMesh::handleCommand) can route net-config
// commands WITHOUT depending on the full NetServices header (WiFi.h / PubSubClient).
// Defined in NetServices.cpp; safe no-op if NetServices was never begun.
//
// Returns true if the command was one of ours (wifi.* / mqtt.* / net.* / net status|apply)
// and `reply` was filled — the caller should then stop and not pass it to CommonCLI.

#include <stddef.h>

bool net_handle_cli(const char* command, char* reply, size_t reply_len);
