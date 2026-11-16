// The game's textures for Game texture nodes, read from the player's own files beside the editor: which object types
// each level holds (indexed once in the background and cached, as reading every level takes a while), the textures one
// type draws with, and views of them for the picker's thumbnails and the preview.
#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/game_archive.h"
#include "shader_editor/shader_graph.h"

namespace kameo::shader_editor {

class EditorHost;

class GameTextureLibrary {
 public:
  ~GameTextureLibrary();
  // `host` makes the thumbnails and must outlive the library.
  explicit GameTextureLibrary(EditorHost& host) : host_(host) {}

  // Which object types each level holds, read in the background.
  void StartIndex();
  bool Indexing() const { return indexing_.valid(); }
  size_t IndexedFiles() const { return indexed_files_.load(); }
  size_t FilesToIndex() const { return files_to_index_.load(); }
  struct Found {
    std::string type;
    std::string archive;
  };
  // Types whose name holds `filter` (any case), at most `limit` of them.
  std::vector<Found> Find(const std::string& filter, size_t limit);

  // The textures one type draws with, read in the background: null until they are, empty when it has none.
  const std::vector<GameTextureRef>* TexturesOf(const std::string& archive, const std::string& type);
  // A thumbnail of one texture, made once its pixels are read; 0 until then, or when it can't be read.
  ImTextureID View(const GameTextureRef& texture);

 private:
  std::shared_ptr<const game_archive::Archive> Open(const std::string& archive);
  void Collect();

  EditorHost& host_;
  std::mutex mutex_;
  // The index: archive file -> its types.
  std::map<std::string, std::vector<std::string>> types_;
  std::future<std::map<std::string, std::vector<std::string>>> indexing_;
  std::atomic<size_t> indexed_files_ = 0;
  std::atomic<size_t> files_to_index_ = 0;
  // Read on a thread of their own; a few archives stay open, as one type's textures are often read one after another.
  std::map<std::string, std::future<std::vector<GameTextureRef>>> reading_types_;
  std::map<std::string, std::vector<GameTextureRef>> type_textures_;
  std::map<std::string, std::future<std::shared_ptr<game_archive::Texture>>> reading_pixels_;
  std::map<std::string, ImTextureID> views_;
  std::vector<std::pair<std::string, std::shared_ptr<const game_archive::Archive>>> open_;
};

}  // namespace kameo::shader_editor
