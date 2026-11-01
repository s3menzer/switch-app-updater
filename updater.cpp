#include "updater.hpp"
#include "manifest.hpp"

#include <switch.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace updater
{
    namespace
    {
        const int NetworkTimeoutSeconds = 8;

        struct Url
        {
            std::string host;
            std::string port;
            std::string path;
        };

        struct HttpResponse
        {
            int statusCode = 0;
            std::vector<unsigned char> body;
        };

        typedef int SocketHandle;
        const SocketHandle InvalidSocket = -1;

        struct Progress
        {
            Progress(ProgressCallback callback, void* userData, bool enabled)
                : callback_(callback), userData_(userData), enabled_(enabled)
            {
            }

            void report(const std::string& message) const
            {
                if(!enabled_)
                {
                    return;
                }
                std::fprintf(stderr, "[update] %s\n", message.c_str());
                if (callback_ != nullptr)
                {
                    callback_(message.c_str(), userData_);
                }
            }

            void reportError(const std::string& message) const
            {
                if(!enabled_)
                {
                    return;
                }
                std::fprintf(stderr, "[update] ERROR: %s\n", message.c_str());
                if (callback_ != nullptr)
                {
                    const std::string callbackMessage = "ERROR: " + message;
                    callback_(callbackMessage.c_str(), userData_);
                }
            }

        private:
            ProgressCallback callback_;
            void* userData_;
            bool enabled_;
        };

        std::string trim(const std::string& value)
        {
            std::size_t first = 0;
            while (first < value.size() &&
                   std::isspace(static_cast<unsigned char>(value[first])) != 0)
            {
                ++first;
            }

            std::size_t last = value.size();
            while (last > first &&
                   std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
            {
                --last;
            }
            return value.substr(first, last - first);
        }

        std::string lowerCase(const std::string& value)
        {
            std::string result = value;
            for (char& character : result)
            {
                character = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(character)));
            }
            return result;
        }

        bool readTextFile(const std::string& path, std::string& output)
        {
            std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
            if (!input)
            {
                return false;
            }

            std::ostringstream contents;
            contents << input.rdbuf();
            output = contents.str();
            return true;
        }

        bool fileExists(const std::string& path)
        {
            std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
            return static_cast<bool>(input);
        }

        bool isSafeRelativePath(const std::string& path)
        {
            if (path.empty() ||
                path[0] == '/' ||
                path.find('\\') != std::string::npos ||
                path.find(':') != std::string::npos)
            {
                return false;
            }

            std::size_t segmentStart = 0;
            while (segmentStart <= path.size())
            {
                const std::size_t separator = path.find('/', segmentStart);
                const std::size_t segmentLength =
                    separator == std::string::npos
                        ? path.size() - segmentStart
                        : separator - segmentStart;
                const std::string segment =
                    path.substr(segmentStart, segmentLength);
                if (segment.empty() || segment == "." || segment == "..")
                {
                    return false;
                }
                if (separator == std::string::npos)
                {
                    break;
                }
                segmentStart = separator + 1;
            }
            return true;
        }

        std::string directoryOf(const std::string& path)
        {
            const std::size_t slash = path.find_last_of("/\\");
            if (slash == std::string::npos)
            {
                return ".";
            }
            if (slash == 0)
            {
                return "/";
            }
            return path.substr(0, slash);
        }

        std::string configPath(const Config& config)
        {
            const std::string baseName = config.configFileBaseName.empty()
                ? config.applicationName
                : config.configFileBaseName;
            return directoryOf(config.executablePath) + "/" + baseName + "-update.json";
        }

        std::string normalizedPath(const std::string& path)
        {
            std::string result = path;
            while (result.compare(0, 2, "./") == 0)
                result.erase(0, 2);
            return result.empty() ? "." : result;
        }

        bool samePath(const std::string& left, const std::string& right)
        {
            return !left.empty() && !right.empty() &&
                normalizedPath(left) == normalizedPath(right);
        }

        std::string configuredManifestUrl(const Config& config)
        {
            std::string document;
            std::string url;
            if (readTextFile(configPath(config), document) &&
                detail::parseConfig(document, url))
                return url;

            return config.defaultManifestUrl;
        }

        bool parseHttpUrl(const std::string& value, Url& url)
        {
            if (value.compare(0, 7, "http://") != 0)
            {
                return false;
            }

            const std::size_t authorityStart = 7;
            const std::size_t pathStart = value.find('/', authorityStart);
            const std::string authority = value.substr(
                authorityStart,
                pathStart == std::string::npos
                    ? std::string::npos
                    : pathStart - authorityStart);
            if (authority.empty())
            {
                return false;
            }

            const std::size_t portSeparator = authority.rfind(':');
            if (portSeparator != std::string::npos &&
                authority.find(']') == std::string::npos)
            {
                url.host = authority.substr(0, portSeparator);
                url.port = authority.substr(portSeparator + 1);
            }
            else
            {
                url.host = authority;
                url.port = "80";
            }

            url.path = pathStart == std::string::npos
                ? "/"
                : value.substr(pathStart);
            return !url.host.empty() && !url.port.empty();
        }

        void closeSocket(SocketHandle socket)
        {
            if (socket == InvalidSocket)
            {
                return;
            }
            close(socket);
        }

        void setSocketTimeout(SocketHandle socket)
        {
            timeval timeout{};
            timeout.tv_sec = NetworkTimeoutSeconds;
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        }

        class NetworkSession
        {
        public:
            explicit NetworkSession(bool networkAlreadyInitialized)
                : active_(false),
                  networkAlreadyInitialized_(networkAlreadyInitialized)
            {
            }

            bool start()
            {
                if(networkAlreadyInitialized_)
                {
                    return true;
                }
                if (R_FAILED(socketInitializeDefault()))
                {
                    return false;
                }
                active_ = true;
                return true;
            }

            ~NetworkSession()
            {
                if (!active_)
                {
                    return;
                }
                socketExit();
            }

        private:
            bool active_;
            bool networkAlreadyInitialized_;
        };

        bool sendAll(SocketHandle socket, const std::string& request)
        {
            std::size_t sent = 0;
            while (sent < request.size())
            {
                const std::size_t remaining = request.size() - sent;
                const int chunk = static_cast<int>(
                    std::min<std::size_t>(remaining, 16 * 1024));
                const int result = ::send(
                    socket,
                    request.data() + sent,
                    chunk,
                    0);
                if (result <= 0)
                {
                    return false;
                }
                sent += static_cast<std::size_t>(result);
            }
            return true;
        }

        bool findHeaderEnd(const std::vector<unsigned char>& response,
                           std::size_t& headerEnd)
        {
            for (std::size_t index = 3; index < response.size(); ++index)
            {
                if (response[index - 3] == '\r' &&
                    response[index - 2] == '\n' &&
                    response[index - 1] == '\r' &&
                    response[index] == '\n')
                {
                    headerEnd = index + 1;
                    return true;
                }
            }
            return false;
        }

        bool decodeChunkedBody(const std::vector<unsigned char>& encoded,
                               std::vector<unsigned char>& decoded)
        {
            const unsigned char crlf[] = {'\r', '\n'};
            std::size_t position = 0;
            while (position < encoded.size())
            {
                const std::vector<unsigned char>::const_iterator lineBegin =
                    encoded.begin() + position;
                const std::vector<unsigned char>::const_iterator lineEndIterator =
                    std::search(
                        lineBegin,
                        encoded.end(),
                        crlf,
                        crlf + sizeof(crlf));
                if (lineEndIterator == encoded.end())
                {
                    return false;
                }

                const std::size_t lineEnd =
                    static_cast<std::size_t>(lineEndIterator - encoded.begin());
                const std::string sizeText(
                    encoded.begin() + position,
                    encoded.begin() + lineEnd);
                const unsigned long chunkSize = std::strtoul(
                    sizeText.c_str(),
                    nullptr,
                    16);
                position = lineEnd + 2;
                if (chunkSize == 0)
                {
                    return true;
                }

                if (chunkSize > encoded.size() - position ||
                    encoded.size() - position - chunkSize < 2)
                {
                    return false;
                }

                decoded.insert(
                    decoded.end(),
                    encoded.begin() + position,
                    encoded.begin() + position + chunkSize);
                position += chunkSize;
                if (encoded[position] != '\r' || encoded[position + 1] != '\n')
                {
                    return false;
                }
                position += 2;
            }
            return false;
        }

        bool httpGet(const std::string& urlString,
                     const std::string& userAgent,
                     HttpResponse& response,
                     const Progress& progress,
                     bool networkAlreadyInitialized)
        {
            Url url;
            if (!parseHttpUrl(urlString, url))
            {
                progress.reportError(
                    "unsupported or invalid HTTP URL: " + urlString);
                return false;
            }

            progress.report("HTTP URL: " + urlString);
            progress.report(
                "HTTP endpoint: " + url.host + ":" + url.port + url.path);
            progress.report("Initializing network connection...");
            NetworkSession network(networkAlreadyInitialized);
            if (!network.start())
            {
                progress.reportError("network initialization failed");
                return false;
            }

            progress.report("Resolving update server " + url.host + "...");
            addrinfo hints{};
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_family = AF_UNSPEC;
            addrinfo* addresses = nullptr;
            if (getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &addresses) != 0)
            {
                progress.reportError("could not resolve " + url.host);
                return false;
            }

            SocketHandle connected = InvalidSocket;
            for (addrinfo* address = addresses;
                 address != nullptr;
                 address = address->ai_next)
            {
                SocketHandle candidate = static_cast<SocketHandle>(
                    socket(address->ai_family, address->ai_socktype, address->ai_protocol));
                if (candidate == InvalidSocket)
                {
                    continue;
                }

                setSocketTimeout(candidate);
                progress.report("Connecting to update server " + url.host + ":" + url.port + "...");
                if (::connect(candidate, address->ai_addr, address->ai_addrlen) == 0)
                {
                    connected = candidate;
                    break;
                }
                closeSocket(candidate);
            }
            freeaddrinfo(addresses);

            if (connected == InvalidSocket)
            {
                progress.reportError(
                    "could not connect to " + url.host + ":" + url.port);
                return false;
            }

            const std::string request =
                "GET " + url.path + " HTTP/1.1\r\n"
                "Host: " + url.host + "\r\n"
                "Connection: close\r\n"
                // The manifest is mutable.  Do not allow a proxy or a
                // server-side cache to make a freshly published build
                // number invisible to the updater.
                "Cache-Control: no-cache, no-store, max-age=0\r\n"
                "Pragma: no-cache\r\n"
                "User-Agent: " + userAgent + "\r\n"
                "\r\n";

            progress.report("Requesting " + url.path + "...");
            progress.report("Receiving server response...");
            bool success = sendAll(connected, request);
            std::vector<unsigned char> rawResponse;
            unsigned char buffer[16 * 1024];
            while (success)
            {
                const int count = static_cast<int>(
                    recv(connected, reinterpret_cast<char*>(buffer), sizeof(buffer), 0));
                if (count == 0)
                {
                    break;
                }
                if (count < 0)
                {
                    success = false;
                    break;
                }
                rawResponse.insert(rawResponse.end(), buffer, buffer + count);
            }
            closeSocket(connected);

            if (!success)
            {
                progress.reportError("HTTP read failed");
                return false;
            }

            std::size_t headerEnd = 0;
            if (!findHeaderEnd(rawResponse, headerEnd))
            {
                progress.reportError("invalid HTTP response");
                return false;
            }

            const std::string headers(
                rawResponse.begin(),
                rawResponse.begin() + headerEnd);
            const std::size_t firstSpace = headers.find(' ');
            if (firstSpace == std::string::npos)
            {
                return false;
            }
            response.statusCode = std::atoi(headers.c_str() + firstSpace + 1);
            if (response.statusCode < 200 || response.statusCode >= 300)
            {
                std::ostringstream message;
                message << "server returned HTTP " << response.statusCode;
                progress.reportError(message.str());
                return false;
            }

            std::vector<unsigned char> body(
                rawResponse.begin() + headerEnd,
                rawResponse.end());
            const std::string lowerHeaders = lowerCase(headers);
            if (lowerHeaders.find("transfer-encoding: chunked") != std::string::npos)
            {
                return decodeChunkedBody(body, response.body);
            }

            response.body.swap(body);
            {
                std::ostringstream message;
                message << "HTTP response status=" << response.statusCode
                        << " body_bytes=" << response.body.size();
                progress.report(message.str());
            }
            return true;
        }

        class Sha256
        {
        public:
            Sha256()
                : dataLength_(0), bitLength_(0)
            {
                state_[0] = 0x6a09e667;
                state_[1] = 0xbb67ae85;
                state_[2] = 0x3c6ef372;
                state_[3] = 0xa54ff53a;
                state_[4] = 0x510e527f;
                state_[5] = 0x9b05688c;
                state_[6] = 0x1f83d9ab;
                state_[7] = 0x5be0cd19;
            }

            void update(const unsigned char* input, std::size_t length)
            {
                for (std::size_t index = 0; index < length; ++index)
                {
                    data_[dataLength_++] = input[index];
                    if (dataLength_ == 64)
                    {
                        transform();
                        bitLength_ += 512;
                        dataLength_ = 0;
                    }
                }
            }

            std::string final()
            {
                std::size_t index = dataLength_;
                data_[index++] = 0x80;
                while (index < 56)
                {
                    data_[index++] = 0;
                }

                if (dataLength_ >= 56)
                {
                    while (index < 64)
                    {
                        data_[index++] = 0;
                    }
                    transform();
                    std::fill(data_, data_ + 56, 0);
                }

                bitLength_ += static_cast<unsigned long long>(dataLength_) * 8;
                for (int offset = 0; offset < 8; ++offset)
                {
                    data_[63 - offset] = static_cast<unsigned char>(
                        bitLength_ >> (offset * 8));
                }
                transform();

                std::ostringstream output;
                output << std::hex << std::setfill('0');
                for (std::size_t stateIndex = 0; stateIndex < 8; ++stateIndex)
                {
                    output << std::setw(8) << state_[stateIndex];
                }
                return output.str();
            }

        private:
            static unsigned int rotateRight(unsigned int value, unsigned int count)
            {
                return (value >> count) | (value << (32 - count));
            }

            void transform()
            {
                static const unsigned int constants[64] =
                {
                    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
                    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
                    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
                    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
                    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
                    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
                    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
                    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
                    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
                    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
                    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
                    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
                    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
                };

                unsigned int schedule[64];
                for (int index = 0; index < 16; ++index)
                {
                    schedule[index] =
                        (static_cast<unsigned int>(data_[index * 4]) << 24) |
                        (static_cast<unsigned int>(data_[index * 4 + 1]) << 16) |
                        (static_cast<unsigned int>(data_[index * 4 + 2]) << 8) |
                        static_cast<unsigned int>(data_[index * 4 + 3]);
                }

                for (int index = 16; index < 64; ++index)
                {
                    const unsigned int smallSigma0 =
                        rotateRight(schedule[index - 15], 7) ^
                        rotateRight(schedule[index - 15], 18) ^
                        (schedule[index - 15] >> 3);
                    const unsigned int smallSigma1 =
                        rotateRight(schedule[index - 2], 17) ^
                        rotateRight(schedule[index - 2], 19) ^
                        (schedule[index - 2] >> 10);
                    schedule[index] = schedule[index - 16] + smallSigma0 +
                        schedule[index - 7] + smallSigma1;
                }

                unsigned int a = state_[0];
                unsigned int b = state_[1];
                unsigned int c = state_[2];
                unsigned int d = state_[3];
                unsigned int e = state_[4];
                unsigned int f = state_[5];
                unsigned int g = state_[6];
                unsigned int h = state_[7];

                for (int index = 0; index < 64; ++index)
                {
                    const unsigned int bigSigma1 =
                        rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
                    const unsigned int choice = (e & f) ^ ((~e) & g);
                    const unsigned int temporary1 =
                        h + bigSigma1 + choice + constants[index] + schedule[index];
                    const unsigned int bigSigma0 =
                        rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
                    const unsigned int majority = (a & b) ^ (a & c) ^ (b & c);
                    const unsigned int temporary2 = bigSigma0 + majority;

                    h = g;
                    g = f;
                    f = e;
                    e = d + temporary1;
                    d = c;
                    c = b;
                    b = a;
                    a = temporary1 + temporary2;
                }

                state_[0] += a;
                state_[1] += b;
                state_[2] += c;
                state_[3] += d;
                state_[4] += e;
                state_[5] += f;
                state_[6] += g;
                state_[7] += h;
            }

            unsigned char data_[64]{};
            std::size_t dataLength_;
            unsigned long long bitLength_;
            unsigned int state_[8]{};
        };

        std::string sha256(const std::vector<unsigned char>& data)
        {
            Sha256 hash;
            if (!data.empty())
            {
                hash.update(data.data(), data.size());
            }
            return hash.final();
        }

        bool sha256File(const std::string& path, std::string& output)
        {
            std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
            if(!input)
                return false;

            input.seekg(0, std::ios::end);
            const std::streamoff length = input.tellg();
            if(length < 0)
                return false;
            input.seekg(0, std::ios::beg);

            std::vector<unsigned char> data(static_cast<std::size_t>(length));
            if(!data.empty())
                input.read(
                    reinterpret_cast<char *>(data.data()),
                    static_cast<std::streamsize>(data.size()));
            if(!input && !input.eof())
                return false;

            output = sha256(data);
            return true;
        }

        std::vector<int> versionParts(const std::string& version)
        {
            std::vector<int> result;
            int number = 0;
            bool hasNumber = false;
            for (char character : version)
            {
                if (character >= '0' && character <= '9')
                {
                    number = number * 10 + (character - '0');
                    hasNumber = true;
                }
                else if (hasNumber)
                {
                    result.push_back(number);
                    number = 0;
                    hasNumber = false;
                }
            }
            if (hasNumber)
            {
                result.push_back(number);
            }
            while (result.size() < 3)
            {
                result.push_back(0);
            }
            return result;
        }

        int compareVersions(const std::string& left,
                            const std::string& right)
        {
            const std::vector<int> leftParts = versionParts(left);
            const std::vector<int> rightParts = versionParts(right);
            for (std::size_t index = 0; index < 3; ++index)
            {
                if (leftParts[index] != rightParts[index])
                {
                    return leftParts[index] > rightParts[index] ? 1 : -1;
                }
            }
            return 0;
        }

        std::string defaultTargetPath(const Config& config)
        {
            if(!config.executablePath.empty())
                return directoryOf(config.executablePath) + "/" +
                    config.applicationName + ".nro";
            return config.applicationName + ".nro";
        }

        bool writeBinaryFile(const std::string& path,
                             const std::vector<unsigned char>& data)
        {
            std::ofstream output(
                path.c_str(),
                std::ios::out | std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return false;
            }
            if (!data.empty())
            {
                output.write(
                    reinterpret_cast<const char*>(data.data()),
                    static_cast<std::streamsize>(data.size()));
            }
            return static_cast<bool>(output);
        }

        bool replaceExistingFile(const std::string& sourcePath,
                                 const std::string& targetPath)
        {
            if (std::rename(sourcePath.c_str(), targetPath.c_str()) == 0)
            {
                return true;
            }

            // Some filesystem implementations do not overwrite an existing
            // destination with rename(). Move the old file aside, install the
            // new one, and restore the old file if the second rename fails.
            if (!fileExists(targetPath))
            {
                return false;
            }

            const std::string backupPath = targetPath + ".backup";
            std::remove(backupPath.c_str());
            if (std::rename(targetPath.c_str(), backupPath.c_str()) != 0)
            {
                return false;
            }

            if (std::rename(sourcePath.c_str(), targetPath.c_str()) == 0)
            {
                std::remove(backupPath.c_str());
                return true;
            }

            // Best effort rollback so a failed update never leaves the
            // application without its executable.
            std::rename(backupPath.c_str(), targetPath.c_str());
            return false;
        }

        bool writePendingFileList(const std::string& path,
                                  const std::vector<std::string>& targetPaths)
        {
            std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc);
            if (!output)
            {
                return false;
            }

            for (const std::string& targetPath : targetPaths)
            {
                output << targetPath << '\n';
            }
            return static_cast<bool>(output);
        }

        bool installPendingUpdate(const std::string& targetPath,
                                  const Progress& progress)
        {
            const std::string pendingPath = targetPath + ".pending";
            if (!fileExists(pendingPath))
            {
                return false;
            }

            progress.report("Found staged update file: " + pendingPath);
            if (replaceExistingFile(pendingPath, targetPath))
            {
                progress.report("Installed staged update file: " + targetPath);
                return true;
            }
            std::ostringstream message;
            message << "could not install staged update file " << targetPath
                    << " (errno=" << errno << ": " << std::strerror(errno) << ")";
            progress.reportError(message.str());
            return false;
        }

        bool installPendingUpdates(const std::string& targetPath,
                                   const Progress& progress)
        {
            const std::string pendingListPath = targetPath + ".pending-files";
            if (!fileExists(pendingListPath))
            {
                return installPendingUpdate(targetPath, progress);
            }

            progress.report("Found staged multi-file update list: " + pendingListPath);
            std::string pendingList;
            if (!readTextFile(pendingListPath, pendingList))
            {
                progress.reportError("could not read staged update list: " + pendingListPath);
                return false;
            }

            std::vector<std::string> targetPaths;
            std::istringstream input(pendingList);
            std::string line;
            while (std::getline(input, line))
            {
                line = trim(line);
                if (!line.empty())
                {
                    targetPaths.push_back(line);
                }
            }
            if (targetPaths.empty())
            {
                progress.reportError("staged update list is empty: " + pendingListPath);
                return false;
            }

            // Verify the complete transaction before replacing anything. This
            // prevents one missing pending file from turning a multi-file
            // update into a different update on every subsequent launch.
            for (const std::string& pendingTargetPath : targetPaths)
            {
                const std::string pendingPath = pendingTargetPath + ".pending";
                if (!fileExists(pendingPath))
                {
                    progress.reportError("staged update file is missing: " + pendingPath);
                    return false;
                }
            }

            for (const std::string& pendingTargetPath : targetPaths)
            {
                const std::string pendingPath = pendingTargetPath + ".pending";
                progress.report("Installing staged update file: " + pendingTargetPath);
                if (!replaceExistingFile(pendingPath, pendingTargetPath))
                {
                    std::ostringstream message;
                    message << "could not install staged update file "
                            << pendingTargetPath
                            << " (errno=" << errno << ": "
                            << std::strerror(errno) << ")";
                    progress.reportError(message.str());
                    return false;
                }
                progress.report("Installed staged update file: " + pendingTargetPath);
            }

            if (std::remove(pendingListPath.c_str()) != 0)
            {
                std::ostringstream message;
                message << "could not remove staged update list "
                        << pendingListPath
                        << " (errno=" << errno << ": "
                        << std::strerror(errno) << ")";
                progress.reportError(message.str());
                return false;
            }
            progress.report("Installed all files from staged update list.");
            return true;
        }

        void removePendingFiles(const std::vector<std::string>& targetPaths)
        {
            for (const std::string& targetPath : targetPaths)
            {
                std::remove((targetPath + ".download").c_str());
                std::remove((targetPath + ".pending").c_str());
            }
        }
    }

    Result run(const Config& config)
    {
        const Progress progress(
            config.progressCallback,
            config.progressUserData,
            config.logProgress);

        progress.report("Mounting SD card...");
        fsdevMountSdmc();

        const std::string targetPath = config.targetPath.empty()
            ? defaultTargetPath(config)
            : config.targetPath;
        progress.report("Checking for a previously staged update...");
        // A pending update was downloaded by a previous launch.  Install it
        // before doing another network check.  The running process may still
        // contain the old build's compile-time number, so checking the
        // manifest again here would immediately download the same artifact
        // for a second time.
        const std::string pendingPath = targetPath + ".pending";
        const std::string pendingListPath = targetPath + ".pending-files";
        const std::string executablePath = config.executablePath;
        const bool hasSeparateExecutable =
            config.deferExecutableUpdate &&
            !executablePath.empty() &&
            !samePath(executablePath, targetPath);
        const bool installedExecutableUpdate =
            hasSeparateExecutable &&
            installPendingUpdates(executablePath, progress);
        const bool installedTargetUpdate =
            installPendingUpdates(targetPath, progress);
        if (installedExecutableUpdate || installedTargetUpdate)
        {
            progress.report("Installed a previously staged update.");
            return Result::Updated;
        }
        const bool executablePending =
            hasSeparateExecutable &&
            (fileExists(executablePath + ".pending") ||
             fileExists(executablePath + ".pending-files"));
        if (fileExists(pendingPath) || fileExists(pendingListPath) ||
            executablePending)
        {
            progress.reportError(
                "could not install staged update; keeping pending files");
            return Result::Failed;
        }

        progress.report("Reading update configuration...");
        const std::string manifestUrl = configuredManifestUrl(config);
        progress.report("Manifest URL: " + manifestUrl);
        progress.report("Downloading update manifest...");

        HttpResponse manifestResponse;
        if (!httpGet(
                manifestUrl,
                config.applicationName + "/" + config.currentVersion,
                manifestResponse,
                progress,
                config.networkAlreadyInitialized))
        {
            progress.reportError("update check failed; starting current version");
            return Result::Failed;
        }

        progress.report("Parsing update manifest...");
        const std::string manifest(
            manifestResponse.body.begin(),
            manifestResponse.body.end());
        detail::Manifest updateManifest;
        std::string parseError;
        if (!detail::parseManifest(manifest, updateManifest, parseError))
        {
            progress.reportError("invalid update manifest: " + parseError);
            return Result::Failed;
        }
        const std::string& remoteVersion = updateManifest.version;
        const std::string& platform = updateManifest.platform;
        const std::vector<detail::UpdateFile>& updateFiles = updateManifest.files;
        const int remoteBuild = updateManifest.build;
        const bool hasRemoteBuild = updateManifest.hasBuild;

        {
            std::ostringstream message;
            message << "manifest: version=" << remoteVersion
                    << " build=";
            if (hasRemoteBuild)
            {
                message << remoteBuild;
            }
            else
            {
                message << "<none>";
            }
            message << " platform=" << (platform.empty() ? "<none>" : platform)
                    << " files=" << updateFiles.size();
            progress.report(message.str());
        }

        const std::string currentPlatform =
            config.platform.empty() ? "switch" : config.platform;
        if (!platform.empty() && platform != currentPlatform)
        {
            progress.reportError(
                "manifest is for platform " + platform +
                ", current platform is " + currentPlatform);
            return Result::NoUpdate;
        }

        const int versionComparison = compareVersions(remoteVersion, config.currentVersion);
        const bool newerVersion = versionComparison > 0;
        // Build numbers are monotonically increasing across the Switch
        // builds.  They must therefore be considered independently of the
        // marketing version: a rebuild with the same (or even accidentally
        // stale) version still needs to update when its build is newer.
        const bool newerBuild =
            hasRemoteBuild && remoteBuild > config.currentBuild;
        if (!newerVersion && !newerBuild)
        {
            std::ostringstream message;
            message << "current version " << config.currentVersion
                    << " build " << config.currentBuild
                    << " is up to date (remote version " << remoteVersion
                    << " build " << remoteBuild << ")";
            progress.report(message.str());
            return Result::NoUpdate;
        }

        {
            std::ostringstream message;
            message << "New version " << remoteVersion
                    << " build " << remoteBuild << " is available.";
            progress.report(message.str());
        }
        {
            std::ostringstream message;
            message << "downloading version " << remoteVersion
                    << " build " << remoteBuild
                    << " (" << updateFiles.size() << " file"
                    << (updateFiles.size() == 1 ? "" : "s") << ")";
            progress.report(message.str());
        }
        const std::string targetDirectory = directoryOf(targetPath);
        std::vector<std::string> targetPaths;
        for (const detail::UpdateFile& file : updateFiles)
        {
            const std::string fileTargetPath = file.path.empty()
                ? targetPath
                : targetDirectory + "/" + file.path;
            if (!file.path.empty() && !isSafeRelativePath(file.path))
            {
                progress.reportError("manifest contains an unsafe file path: " + file.path);
                return Result::Failed;
            }
            if (file.downloadUrl.empty())
            {
                progress.reportError("manifest contains a file without a download URL");
                return Result::Failed;
            }
            if (std::find(
                    targetPaths.begin(),
                    targetPaths.end(),
                    fileTargetPath) != targetPaths.end())
            {
                progress.reportError("manifest contains duplicate file path: " + fileTargetPath);
                return Result::Failed;
            }
            targetPaths.push_back(fileTargetPath);
        }

        struct DownloadedFile
        {
            std::string targetPath;
            std::string temporaryPath;
        };
        std::vector<DownloadedFile> downloadedFiles;
        for (std::size_t index = 0; index < updateFiles.size(); ++index)
        {
            const detail::UpdateFile& file = updateFiles[index];
            const std::string fileTargetPath = targetPaths[index];
            const std::string temporaryPath = fileTargetPath + ".download";

            if(!file.expectedHash.empty())
            {
                std::string localHash;
                if(sha256File(fileTargetPath, localHash) &&
                    lowerCase(trim(file.expectedHash)) == localHash)
                {
                    progress.report(
                        "File is already current: " + fileTargetPath);
                    continue;
                }
            }

            std::ostringstream message;
            message << "Downloading file " << (index + 1)
                    << "/" << updateFiles.size() << ": "
                    << (file.path.empty() ? fileTargetPath : file.path);
            progress.report(message.str());

            HttpResponse artifactResponse;
            if (!httpGet(
                    file.downloadUrl,
                    config.applicationName + "/" + config.currentVersion,
                    artifactResponse,
                    progress,
                    config.networkAlreadyInitialized))
            {
                removePendingFiles(targetPaths);
                progress.reportError("download failed; starting current version");
                return Result::Failed;
            }

            progress.report("Verifying downloaded file...");
            if (!file.expectedHash.empty() &&
                lowerCase(trim(file.expectedHash)) != sha256(artifactResponse.body))
            {
                removePendingFiles(targetPaths);
                progress.reportError("checksum mismatch; refusing update");
                return Result::Failed;
            }

            progress.report("Writing downloaded file...");
            std::remove(temporaryPath.c_str());
            if (!writeBinaryFile(temporaryPath, artifactResponse.body))
            {
                removePendingFiles(targetPaths);
                progress.reportError("could not write " + temporaryPath);
                return Result::Failed;
            }
            downloadedFiles.push_back({fileTargetPath, temporaryPath});
        }

        if(downloadedFiles.empty())
        {
            progress.report("All manifest files are already current.");
            return Result::NoUpdate;
        }

        std::vector<std::string> stagedPaths;
        bool installedFile = false;
        bool stagedExecutable = false;
        for(const DownloadedFile& file : downloadedFiles)
        {
            const bool isRunningExecutable =
                config.deferExecutableUpdate &&
                samePath(file.targetPath, executablePath);
            if(isRunningExecutable)
            {
                const std::string pendingExecutablePath =
                    file.targetPath + ".pending";
                std::remove(pendingExecutablePath.c_str());
                if(std::rename(
                        file.temporaryPath.c_str(),
                        pendingExecutablePath.c_str()) != 0)
                {
                    removePendingFiles(targetPaths);
                    progress.reportError(
                        "could not stage running executable " +
                        file.targetPath);
                    return Result::Failed;
                }
                stagedExecutable = true;
                progress.report(
                    "Staged launcher update for the next launcher start: " +
                    file.targetPath);
                continue;
            }

            progress.report("Installing update file: " + file.targetPath);
            if(replaceExistingFile(file.temporaryPath, file.targetPath))
            {
                installedFile = true;
                progress.report("Installed update file: " + file.targetPath);
                continue;
            }

            // A non-running file should normally be replaceable immediately.
            // Keep the old pending-list fallback for filesystems that reject
            // replacing an existing file atomically.
            const std::string pendingFilePath = file.targetPath + ".pending";
            std::remove(pendingFilePath.c_str());
            if(std::rename(
                    file.temporaryPath.c_str(),
                    pendingFilePath.c_str()) == 0)
            {
                stagedPaths.push_back(file.targetPath);
                progress.report(
                    "Staged update file for the next launcher start: " +
                    file.targetPath);
                continue;
            }

            removePendingFiles(targetPaths);
            progress.reportError(
                "could not install or stage downloaded update at " +
                file.targetPath);
            return Result::Failed;
        }

        if(!stagedPaths.empty() &&
            !writePendingFileList(pendingListPath, stagedPaths))
        {
            removePendingFiles(targetPaths);
            std::remove(pendingListPath.c_str());
            progress.reportError("could not write pending file list");
            return Result::Failed;
        }

        {
            std::ostringstream message;
            message << "updated version " << remoteVersion
                    << " build " << remoteBuild
                    << " (" << updateFiles.size() << " file"
                    << (updateFiles.size() == 1 ? "" : "s") << ")";
            if(installedFile)
                message << "; files installed";
            if(stagedExecutable || !stagedPaths.empty())
                message << "; deferred files staged";
            message << "; starting game without restarting launcher";
            progress.report(message.str());
        }
        return Result::Updated;
    }
}
