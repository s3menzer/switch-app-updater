#include <switch.h>

#include "updater.hpp"

#include <cstdio>
#include <ctime>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <string>
#include <unistd.h>

#ifndef SWITCH_UPDATER_APPLICATION_NAME
#define SWITCH_UPDATER_APPLICATION_NAME "application"
#endif

#ifndef SWITCH_UPDATER_CURRENT_VERSION
#define SWITCH_UPDATER_CURRENT_VERSION "0.0.0"
#endif

#ifndef SWITCH_UPDATER_MANIFEST_URL
#define SWITCH_UPDATER_MANIFEST_URL ""
#endif

namespace
{
	PrintConsole *g_pConsole = nullptr;
	FILE *g_pTraceFile = nullptr;
	bool g_DebugLogging = false;

	std::string directoryOf(const char *path)
	{
		if(!path || !path[0])
			return ".";

		const std::string value(path);
		const std::size_t slash = value.find_last_of("/\\");
		if(slash == std::string::npos)
			return ".";
		if(slash == 0)
			return "/";
		return value.substr(0, slash);
	}

	std::string ownExecutablePath(int argc, char **argv)
	{
		// hbmenu passes the absolute path of the selected NRO as argv[1].
		// argv[0] is the loader/program name and may point to the SD root.
		const char *pathArgument = nullptr;
		if(argc > 1 && argv && argv[1] && argv[1][0])
			pathArgument = argv[1];
		else if(argc > 0 && argv && argv[0] && argv[0][0])
			pathArgument = argv[0];

		if(pathArgument)
		{
			const std::string argumentPath(pathArgument);
			// Keep SD-device paths and other absolute paths unchanged.
			if(argumentPath[0] == '/' ||
				argumentPath.find(':') != std::string::npos)
				return argumentPath;

			// Resolve both a bare filename and a relative path such as
			// "teeworlds/teeworlds.nro" before changing directories.
			char currentDirectory[1024];
			if(getcwd(currentDirectory, sizeof(currentDirectory)))
				return std::string(currentDirectory) + "/" + argumentPath;

			return argumentPath;
		}

		return {};
	}

	bool setWorkingDirectory(const std::string &path)
	{
		if(path.empty() || path == ".")
			return false;
		return chdir(path.c_str()) == 0;
	}

	bool debugLoggingEnabled()
	{
		std::ifstream input("settings07.cfg");
		if(!input)
			return false;

		std::string line;
		while(std::getline(input, line))
		{
			char command[64];
			int value = 0;
			if(std::sscanf(line.c_str(), " %63s %d", command, &value) == 2 &&
				std::strcmp(command, "debug") == 0)
				return value != 0;
		}
		return false;
	}

	bool traceFileIsFromToday()
	{
		struct stat fileInfo;
		if(stat("teeworlds.txt", &fileInfo) != 0)
			return false;

		const std::time_t currentTime = std::time(nullptr);
		std::tm currentDate;
		std::tm traceDate;
		if(!localtime_r(&currentTime, &currentDate) ||
			!localtime_r(&fileInfo.st_mtime, &traceDate))
			return false;

		return currentDate.tm_year == traceDate.tm_year &&
			currentDate.tm_yday == traceDate.tm_yday;
	}

	void printLine(const char *message)
	{
		if(!g_DebugLogging || !message)
			return;

		std::printf("%s\n", message);
		if(g_pConsole)
			consoleUpdate(g_pConsole);
		if(g_pTraceFile)
		{
			std::fprintf(g_pTraceFile, "%s\n", message);
			std::fflush(g_pTraceFile);
		}
	}

	bool waitForLaunchButton()
	{
		printLine("Updater finished. Press A to start game or B to exit.");

		padConfigureInput(1, HidNpadStyleSet_NpadFullCtrl);
		PadState pad;
		padInitializeDefault(&pad);

		u64 previousButtons = 0;
		while(appletMainLoop())
		{
			padUpdate(&pad);
			const u64 buttons = padGetButtons(&pad);
			const u64 pressed = buttons & ~previousButtons;
			previousButtons = buttons;

			if(pressed & HidNpadButton_B)
				return false;
			if(pressed != 0)
				return true;

			if(g_pConsole)
				consoleUpdate(g_pConsole);
			svcSleepThread(10000000ULL);
		}

		return false;
	}

	void updateProgress(const char *message, void *)
	{
		char line[1024];
		std::snprintf(
			line,
			sizeof(line),
			"[updater] %s",
			message ? message : "");
		printLine(line);
	}

	const char *resultName(updater::Result result)
	{
		switch(result)
		{
		case updater::Result::NoUpdate: return "no update";
		case updater::Result::Updated: return "updated";
		case updater::Result::Failed: return "failed";
		}
		return "unknown";
	}

	void closeDiagnostics()
	{
		if(g_pConsole)
		{
			consoleUpdate(g_pConsole);
			consoleExit(g_pConsole);
			g_pConsole = nullptr;
		}
		if(g_pTraceFile)
		{
			std::fflush(g_pTraceFile);
			std::fclose(g_pTraceFile);
			g_pTraceFile = nullptr;
		}
	}

	bool fileExists(const std::string &path)
	{
		struct stat fileInfo;
		return stat(path.c_str(), &fileInfo) == 0;
	}
}

int main(int argc, char **argv)
{
	fsdevMountSdmc();

	// Keep all updater artifacts next to the launcher and game NROs. This
	// avoids hard-coding an SD-card path and makes the launcher portable to
	// another /switch/<application> directory.
	const std::string executablePath = ownExecutablePath(argc, argv);
	std::string appDirectory = directoryOf(executablePath.c_str());
	const bool workingDirectorySet =
		appDirectory == "." || setWorkingDirectory(appDirectory);

	g_DebugLogging = debugLoggingEnabled();
	if(g_DebugLogging)
	{
		g_pTraceFile = std::fopen(
			"teeworlds.txt",
			traceFileIsFromToday() ? "a" : "w");
		g_pConsole = consoleInit(nullptr);
	}
	else
	{
		nxlinkStdio();
	}

	socketInitializeDefault();

	printLine("Teeworlds Switch launcher starting");
	char pathMessage[1024];
	std::snprintf(
		pathMessage,
		sizeof(pathMessage),
		"launcher path=%s app directory=%s cwd=%s",
		executablePath.empty() ? "<unknown>" : executablePath.c_str(),
		appDirectory.c_str(),
		workingDirectorySet ? "set" : "unchanged");
	printLine(pathMessage);
	char currentDirectoryMessage[1024];
	char currentDirectory[512];
	if(getcwd(currentDirectory, sizeof(currentDirectory)))
	{
		std::snprintf(
			currentDirectoryMessage,
			sizeof(currentDirectoryMessage),
			"launcher current directory=%s",
			currentDirectory);
		printLine(currentDirectoryMessage);
	}

	std::string gamePath;
	if(appDirectory != ".")
		gamePath = appDirectory + "/game.nro";
	else
	{
		char currentDirectory[512];
		if(getcwd(currentDirectory, sizeof(currentDirectory)))
			gamePath = std::string(currentDirectory) + "/game.nro";
		else
			gamePath = "./game.nro";
	}

	updater::Config config;
	config.applicationName = SWITCH_UPDATER_APPLICATION_NAME;
	config.configFileBaseName = SWITCH_UPDATER_APPLICATION_NAME;
	config.currentVersion = SWITCH_UPDATER_CURRENT_VERSION;
	// The launcher binary is intentionally independent of the game build
	// number. The manifest hashes decide whether the launcher itself changed.
	config.currentBuild = 0;
	config.defaultManifestUrl = SWITCH_UPDATER_MANIFEST_URL;
	config.executablePath = executablePath.empty()
		? "teeworlds.nro"
		: executablePath;
	config.targetPath = gamePath;
	config.platform = "switch";
	config.deferExecutableUpdate = true;
	config.networkAlreadyInitialized = true;
	config.logProgress = g_DebugLogging;
	config.progressCallback = updateProgress;

	char configMessage[512];
	std::snprintf(
		configMessage,
		sizeof(configMessage),
		"launcher: version=%s build=%d manifest=%s game=%s",
		config.currentVersion.c_str(),
		config.currentBuild,
		config.defaultManifestUrl.c_str(),
		config.targetPath.c_str());
	printLine(configMessage);

	const updater::Result result = updater::run(config);
	char resultMessage[128];
	std::snprintf(
		resultMessage,
		sizeof(resultMessage),
		"launcher updater result=%s",
		resultName(result));
	printLine(resultMessage);

	// updater::run mounts the SD card again. That can reset the process cwd,
	// so restore it immediately before handing control to game.nro.
	const bool workingDirectoryRestored =
		appDirectory == "." || setWorkingDirectory(appDirectory);
	char restoredDirectoryMessage[1024];
	std::snprintf(
		restoredDirectoryMessage,
		sizeof(restoredDirectoryMessage),
		"launcher working directory restored=%s",
		workingDirectoryRestored ? "yes" : "no");
	printLine(restoredDirectoryMessage);

	// Keep all updater paths anchored to the same directory as the launcher,
	// even if the loader supplied an unexpected working directory.
	printLine("Updater target paths resolved next to the launcher");
	char gamePathMessage[1024];
	const bool gameAvailable = fileExists(gamePath);
	std::snprintf(
		gamePathMessage,
		sizeof(gamePathMessage),
		"Game path: %s (exists=%s)",
		gamePath.c_str(),
		gameAvailable ? "yes" : "no");
	printLine(gamePathMessage);
	if(!gameAvailable)
		printLine("game.nro is missing; the launcher cannot chain-load it");

	if(g_DebugLogging && !waitForLaunchButton())
	{
		printLine("Launcher cancelled before starting game.nro");
		closeDiagnostics();
		socketExit();
		return 0;
	}

	if(!gameAvailable)
	{
		closeDiagnostics();
		socketExit();
		nxlinkStdio();
		std::printf(
			"Could not start %s: file does not exist\n",
			gamePath.c_str());
		return 1;
	}
	closeDiagnostics();
	socketExit();

	/*
	 * hbloader starts the next NRO with its own default working directory.
	 * Pass the absolute game path as the first application argument as well
	 * as setting the launcher's cwd. The game can then restore the same
	 * directory before it opens relative data and user files.
	 */
	const Result launchResult =
		envSetNextLoad(gamePath.c_str(), gamePath.c_str());
	if(R_FAILED(launchResult))
	{
		nxlinkStdio();
		std::printf(
			"Could not start %s (0x%x)\n",
			gamePath.c_str(),
			launchResult);
		return 1;
	}

	return 0;
}
