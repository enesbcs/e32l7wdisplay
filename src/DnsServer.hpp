// Captive-portal DNS server for the AP ("hotspot") mode.
//
// Answers every DNS A query with the AP's own IP (172.218.28.1), so
// phones/laptops detect a captive portal ("Sign in to network") instead of
// reporting "connected, no internet". UDP port 53, lwIP sockets.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the DNS responder on UDP/53 bound to INADDR_ANY. Safe to call from
// the AP startup path; each call (re)starts a fresh responder task.
void dns_captive_start(void);

// Stop the responder (deletes the task and socket).
void dns_captive_stop(void);

#ifdef __cplusplus
}
#endif