#pragma once
// .cutproj = versioned JSON. Serialization is deterministic so save -> load -> save is
// byte-identical (verified by tests).
#include "core/model.h"

#include <string>

namespace cut {

// projectPath is used to compute media paths relative to the project file.
std::string serializeProject(const Project& p, const std::string& projectPath);
bool deserializeProject(const std::string& json, Project& out, std::string& error);

bool saveProjectFile(const Project& p, const std::string& path, std::string& error);  // atomic replace
bool loadProjectFile(const std::string& path, Project& out, std::string& error);

// Marks assets offline when their file is missing or changed (size/mtime); tries relPath first.
// Returns the number of offline assets.
int resolveMediaPaths(Project& p, const std::string& projectPath);

// Schema migration hook: upgrades older documents in place. Returns false if too new.
bool migrateProjectJson(void* jsonObject, int fromVersion, std::string& error);

// Text style <-> JSON (title style presets).
std::string textStyleToJson(const TextStyle& s);
bool textStyleFromJson(const std::string& json, TextStyle& out);
// Effect <-> JSON (effect presets). fromJson fails for unknown effect types.
std::string effectToJson(const Effect& e);
bool effectFromJson(const std::string& json, Effect& out);

// File helpers (UTF-8 paths).
bool readFileUtf8(const std::string& path, std::string& out);
bool writeFileAtomic(const std::string& path, const std::string& data);
bool fileStat(const std::string& path, int64_t& size, int64_t& mtime);

} // namespace cut
