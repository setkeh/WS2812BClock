#pragma once

// Starts capturing log lines into the send queue. Call this as early as
// possible -- before WiFi -- so the boot sequence is captured too. It touches
// no network state, so it is safe before the TCP/IP stack exists.
void logship_init(void);

// Starts actually sending. Must be called after the TCP/IP stack is up (that
// is, after WiFi has been initialised): resolving the collector's name goes
// through lwip, and lwip asserts if its mailbox does not exist yet. Lines
// logged between logship_init() and here wait in the queue.
void logship_start(void);

// If the previous boot ended in a crash, log the stored core dump's summary
// (faulting task, PC, exception cause, backtrace). Call it once the network
// is up so the report actually reaches the collector.
void logship_report_coredump(void);
