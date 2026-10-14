#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The cartridge save as one file. The core reports every write; the copy is
// kept in memory and written out once writes go quiet, so a game saving many
// small blocks touches the disk once.
class SaveFile {
  public:
    void Open(std::string path);
    std::vector<uint8_t> Load() const;
    void Write(const uint8_t* data, uint32_t length, uint64_t nowMs);
    void FlushIfIdle(uint64_t nowMs);
    void Flush();

  private:
    std::string path_;
    std::vector<uint8_t> data_;
    uint64_t lastWriteMs_ = 0;
    bool dirty_ = false;
};
