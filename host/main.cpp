#include "network_manager.hpp"
#include <iostream>
#include "transfer_client.hpp"
#include <filesystem>

int main() {
  std::cout << "WarpPoint host discovery test\n";
  NetworkManager nm;
  auto peers = nm.discoverPeers(2000);
  std::cout << "Found " << peers.size() << " peers\n";
  for (auto &p : peers) {
    std::cout << "Peer: " << p.addr << ":" << p.port << "\n";
  }
  if (peers.size() && std::filesystem::exists("sample_payload.bin")) {
    std::cout << "Found sample_payload.bin, attempting transfer to first peer\n";
    bool ok = pushFile(peers[0].addr, peers[0].port ? peers[0].port : 40000, "sample_payload.bin", "received.bin");
    std::cout << (ok ? "Transfer succeeded\n" : "Transfer failed\n");
  } else {
    std::cout << "No sample payload or no peers, exiting.\n";
  }
  return 0;
}

