#pragma once
// A command channel over the USB serial port, so a browser can talk to the Orb.
//
// Why this exists: a public website cannot reach a device on someone's home WiFi, and
// should not be able to. WebSerial is the one path that works, and it works because the
// owner physically plugs the cable in and grants permission per device. That makes the
// USB port, not the network, the front door for Studio.
//
// Two constraints shape the protocol:
//
// 1. This port is already a debug console. Firmware prints freely to Serial, and that
//    output interleaves with anything we send. So every reply is framed with a sentinel
//    prefix (RESP_TAG) and terminated by a newline: the browser scans lines, ignores
//    everything that is not tagged, and never has to guess whether a line is log spam or
//    an answer. Requests are tagged too (REQ_TAG), which keeps a human poking at the
//    console from tripping a theme switch by typing a bare word.
//
// 2. Commands are plain space-separated words, not JSON. Replies are JSON because the
//    browser wants structure, but parsing JSON on the device would mean linking a parser
//    for the sake of three verbs. Emitting is cheap; consuming is not.
//
// Reads are non-blocking and bounded. poll() drains only what is already buffered and
// returns, because it runs in the same loop() that services the knob, and input latency
// is the thing this project keeps having to defend.
#include <stdint.h>
#include <stddef.h>

namespace orb_link {

// Framing. Both include the trailing space, so a line is only a command if it starts with
// the tag AND a separator: "?orb" alone is not a request, and a log line that happens to
// begin with "!orbit" is not a reply.
constexpr const char *REQ_TAG  = "?orb ";
constexpr const char *RESP_TAG = "!orb ";

// Bumped when the reply shape changes in a way an older browser client would misread.
// The browser checks this on hello and refuses rather than guessing.
constexpr int PROTOCOL_VERSION = 1;

// What the device calls itself. The USB descriptor cannot say this (the ESP32-S3 enumerates
// through its ROM USB-Serial-JTAG block, whose strings are fixed in silicon), so identity
// is asserted here, one layer up, where we actually control it.
constexpr const char *PRODUCT_NAME = "The Orb";

// Called when the host asks to switch themes. Returns true if the slug was accepted and a
// switch is now pending. Supplied by main.cpp so this module stays out of the reboot
// sequencing, which has to be deferred past the reply or the host never sees it.
void setThemeRequestHook(bool (*hook)(const char *slug));

void begin();

// True while a file transfer is mid-flight. loop() uses this to spend more of each pass
// draining the port: during an install the screen is showing the update overlay anyway,
// so rendering behind it is work nobody can see, and it was costing most of the transfer
// rate (measured 18 KB/s idle vs 5 KB/s while the radar rendered).
bool transferActive();

// Drain and dispatch whatever the host has sent. Cheap when idle: one availableForRead
// check. Safe to call every loop.
void poll();

}   // namespace orb_link
