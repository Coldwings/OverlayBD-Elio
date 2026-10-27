// Local publication intents; not part of exchanged upstream data/index format.
#pragma once
#include <cstdint>
#include <string>

namespace obd::format::hybrid {
// Blocking metadata operations. Caller holds exclusive data/index ownership
// and runs these off scheduler workers. Recovery precedes normal header parsing.
// Recognize appended sealed metadata only outside live DATA ownership and
// after validating identity, layout, and logical mappings against the sidecar.
bool is_sealed_pair(int data_fd, int index_fd);
bool recover_transaction(int data_fd, int index_fd, int directory_fd,
                         const std::string& name, bool writable);
void grow_transaction(int data_fd, int index_fd, int directory_fd,
                      const std::string& name, uint64_t virtual_size);
void seal_transaction(int data_fd, int index_fd, int directory_fd,
                      const std::string& name, const std::string& user_tag);

#ifdef OBD_TEST_HOOKS
// 1=intent synced, 2=grow data synced, 3=grow index synced,
// 4=seal index written, 5=seal trailer written, 6=seal data synced,
// 7=intent unlinked before directory sync, 8=seal marker written before sync.
// Optional process exit uses code86.
void transaction_fault_for_test(int stage, bool exit_process = false);
#endif
}  // namespace obd::format::hybrid
