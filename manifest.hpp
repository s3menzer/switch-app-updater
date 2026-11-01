#pragma once

#include <string>
#include <vector>

namespace updater
{
namespace detail
{
	struct UpdateFile
	{
		// Empty when the manifest describes a single-file update.
		std::string path;
		std::string downloadUrl;
		std::string expectedHash;
	};

	struct Manifest
	{
		std::string version;
		std::string platform;
		std::vector<UpdateFile> files;
		int build = 0;
		bool hasBuild = false;
	};

	bool parseManifest(const std::string &document, Manifest &manifest, std::string &error);
	bool parseConfig(const std::string &document, std::string &manifestUrl);
}
}
