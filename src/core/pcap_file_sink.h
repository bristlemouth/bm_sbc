#pragma once

/// @file pcap_file_sink.h
/// @brief Linux-specific pcap file sink.
///
/// Opens a file and provides the PcapWriteCb that writes to it.
/// Thread-safe: a mutex protects all writes so that RX and TX
/// callbacks from different threads are serialised.

#ifdef __cplusplus
extern "C" {
#endif

/// Open a pcap file for writing and initialise the pcap stream.
///
/// Writes the pcap global header immediately.
///
/// @param path  File path to create/overwrite.
/// @return 0 on success, -1 on failure.
int pcap_file_sink_open(const char *path);

/// Flush and close the pcap file.
void pcap_file_sink_close(void);

#ifdef __cplusplus
}
#endif
