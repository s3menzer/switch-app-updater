#include "manifest.hpp"

#include "third_party/json-parser/json.h"

#include <cstring>
#include <limits>
#include <string>

namespace updater
{
namespace detail
{
namespace
{
	const json_value *jsonObjectValue(const json_value *object, const char *name)
	{
		if(!object || object->type != json_object)
			return nullptr;

		for(unsigned i = 0; i < object->u.object.length; ++i)
		{
			const json_object_entry &entry = object->u.object.values[i];
			if(entry.name && std::string(entry.name, entry.name_length) == name)
				return entry.value;
		}
		return nullptr;
	}

	bool jsonStringValue(
		const json_value *value,
		std::string &output,
		bool required)
	{
		if(!value)
			return !required;
		if(value->type != json_string)
			return false;

		output.assign(value->u.string.ptr, value->u.string.length);
		return !required || !output.empty();
	}

	bool jsonBuildValue(const json_value *value, int &build)
	{
		if(!value)
			return false;

		if(value->type == json_integer)
		{
			if(value->u.integer < 0 ||
				value->u.integer > static_cast<json_int_t>(std::numeric_limits<int>::max()))
				return false;
			build = static_cast<int>(value->u.integer);
			return true;
		}
		if(value->type == json_double)
		{
			if(value->u.dbl < 0.0 ||
				value->u.dbl > static_cast<double>(std::numeric_limits<int>::max()) ||
				value->u.dbl != static_cast<double>(static_cast<int>(value->u.dbl)))
				return false;
			build = static_cast<int>(value->u.dbl);
			return true;
		}
		return false;
	}

	bool parseJsonManifest(
		const std::string &document,
		Manifest &manifest,
		std::string &error)
	{
		json_settings settings;
		std::memset(&settings, 0, sizeof(settings));
		char parseError[json_error_max] = {0};
		json_value *root = json_parse_ex(
			&settings,
			document.c_str(),
			document.size(),
			parseError);
		if(!root)
		{
			error = parseError[0] ? parseError : "invalid JSON";
			return false;
		}

		Manifest parsed;
		bool valid = root->type == json_object;
		const json_value *value = nullptr;
		if(valid)
			valid = jsonStringValue(
				jsonObjectValue(root, "version"),
				parsed.version,
				true);
		if(!valid)
			error = "manifest is missing a non-empty string 'version'";

		if(valid)
		{
			value = jsonObjectValue(root, "platform");
			if(value && !jsonStringValue(value, parsed.platform, false))
			{
				error = "manifest has an invalid 'platform'";
				valid = false;
			}
		}

		if(valid)
		{
			value = jsonObjectValue(root, "build");
			if(value)
			{
				if(!jsonBuildValue(value, parsed.build))
				{
					error = "manifest has an invalid 'build'";
					valid = false;
				}
				else
					parsed.hasBuild = true;
			}
		}

		if(valid)
		{
			value = jsonObjectValue(root, "files");
			if(value)
			{
				if(value->type != json_array || value->u.array.length == 0)
				{
					error = "'files' must be a non-empty array";
					valid = false;
				}
				else
				{
					for(unsigned i = 0; i < value->u.array.length; ++i)
					{
						const json_value *entry = value->u.array.values[i];
						UpdateFile file;
						if(!entry || entry->type != json_object ||
							!jsonStringValue(
								jsonObjectValue(entry, "path"),
								file.path,
								true) ||
							!jsonStringValue(
								jsonObjectValue(entry, "download"),
								file.downloadUrl,
								true))
						{
							error =
								"each file needs non-empty string 'path' and 'download'";
							valid = false;
							break;
						}
						const json_value *hash =
							jsonObjectValue(entry, "sha256");
						if(hash && !jsonStringValue(hash, file.expectedHash, false))
						{
							error = "file has an invalid 'sha256'";
							valid = false;
							break;
						}
						parsed.files.push_back(file);
					}
				}
			}
			else
			{
				UpdateFile file;
				if(!jsonStringValue(
						jsonObjectValue(root, "download"),
						file.downloadUrl,
						true))
				{
					error = "manifest needs 'files' or a 'download' string";
					valid = false;
				}
				else
				{
					const json_value *hash = jsonObjectValue(root, "sha256");
					if(hash && !jsonStringValue(hash, file.expectedHash, false))
					{
						error = "manifest has an invalid 'sha256'";
						valid = false;
					}
					parsed.files.push_back(file);
				}
			}
		}

		json_value_free(root);
		if(!valid)
			return false;

		manifest = parsed;
		error.clear();
		return true;
	}

}

bool parseManifest(const std::string &document, Manifest &manifest, std::string &error)
{
	return parseJsonManifest(document, manifest, error);
}

bool parseConfig(const std::string &document, std::string &manifestUrl)
{
	json_settings settings;
	std::memset(&settings, 0, sizeof(settings));
	char parseError[json_error_max] = {0};
	json_value *root = json_parse_ex(
		&settings,
		document.c_str(),
		document.size(),
		parseError);
	if(!root)
		return false;

	const json_value *url = jsonObjectValue(root, "manifest_url");
	if(!url)
		url = jsonObjectValue(root, "url");
	const bool valid = jsonStringValue(url, manifestUrl, true) &&
		(manifestUrl.compare(0, 7, "http://") == 0 ||
			manifestUrl.compare(0, 8, "https://") == 0);
	json_value_free(root);
	return valid;
}
}
}
