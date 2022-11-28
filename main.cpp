#include "wdul/console.hpp"
#include "wdul/fs.hpp"
#include "wdul/strconv.hpp"
#include <algorithm>
#include <vector>
#include <array>

namespace program
{
	// Standard I/O.
	class console
	{
	public:
		// No copy/move.
		console(console const&) = delete;
		console(console&&) = delete;
		console& operator=(console const&) = delete;
		console& operator=(console&&) = delete;

		// Initialises standard input/output handles. Sets the console mode.
		console();

		// Restores the console mode.
		~console();

		// Writes the spcified string to standard output.
		console& write(std::wstring_view const Sv)
		{
			mOutput.write(Sv);
			return *this;
		}

		// Reads from standard input to the specified string.
		// The previous contents of the specified string are not cleared.
		// Does not affect the last-read string.
		void read(std::wstring& Str);

		// Sets the last-read string to the string read from standard input.
		std::wstring const& read()
		{
			mLastRead.clear();
			read(mLastRead);
			return mLastRead;
		}

		auto const& output_handle() const noexcept { return mOutput; }
		auto const& input_handle() const noexcept { return mInput; }

		// Returns the last-read string.
		auto const& last_read() const noexcept { return mLastRead; }

	private:
		wdul::console_output_handle mOutput;
		wdul::console_input_handle mInput;
		wdul::console_output_mode mPrevOutputMode;
		wdul::console_input_mode mPrevInputMode;
		static constexpr std::size_t mWchBufferSize = 128;
		wchar_t mWchBuffer[mWchBufferSize];
		std::wstring mLastRead;
	};

	console::console() :
		mOutput(wdul::get_std_handle(wdul::std_handle_id::output)),
		mInput(wdul::get_std_handle(wdul::std_handle_id::input))
	{
		wdul::leave_uninitialized(mWchBuffer);

		mPrevOutputMode = mOutput.get_mode();
		mPrevInputMode = mInput.get_mode();

		// Allows virtual terminal sequences. See https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences.
		mOutput.set_mode(mPrevOutputMode | wdul::console_output_mode::enable_virtual_terminal_processing);
	}

	console::~console()
	{
		try
		{
			mOutput.write(L"\x1b[0m");
			mOutput.set_mode(mPrevOutputMode);
			mInput.set_mode(mPrevInputMode);
		}
		catch (std::exception const& e)
		{
			MessageBoxA(nullptr, e.what(), "Failed to restore console IO mode", MB_ICONERROR);
		}
	}

	void console::read(std::wstring& Str)
	{
		mOutput.write(L"\x1b[0m> ");
		std::uint32_t numRead;
		do
		{
			numRead = mInput.read(mWchBuffer);
			Str.append(mWchBuffer, numRead);
		} while (numRead == std::size(mWchBuffer));
		if (Str.ends_with(L"\r\n"))
		{
			Str.resize(numRead - 2);
		}
	}

	// Gets a boolean input from the user. The last-read string (that is, the string returned by console::last_read) is modified.
	// Returns true if the user inputs yes.
	// Returns false if the user inputs no.
	bool ask_yesno(console& Console)
	{
		while (true)
		{
			Console.read();
			if (Console.last_read() == L"y" || Console.last_read() == L"yes")
			{
				return true;
			}
			else if (Console.last_read() == L"n" || Console.last_read() == L"no")
			{
				return false;
			}
			Console.write(L"Invalid input. Enter yes or no.\n");
		}
	}

	void echo_command_line(console& Cons, int const ArgCount, wchar_t** const Args)
	{
		for (int i = 0; i < ArgCount; ++i)
		{
			Cons.write(L"Argument ").write(std::to_wstring(i)).write(L": \"").write(Args[i]).write(L"\"\n");
		}
	}

	// A sorted array of acceptable command-line arguments.
	inline constexpr std::wstring_view argument_names[] =
	{
		L"dir",
		L"ext",
		L"note",
		L"notef",
		L"recurse",
		L"replace",
		L"syntax",
		L"verbose",
	};
	static_assert(std::is_sorted(std::begin(argument_names), std::end(argument_names)));

	// Compile-time utility to get the longest string in the argument_names array.
	consteval std::size_t get_max_argument_name_length()
	{
		std::size_t maxSize = 0;
		for (std::wstring_view const* p = argument_names; p != argument_names + std::size(argument_names); ++p)
		{
			if (p->size() > maxSize)
			{
				maxSize = p->size();
			}
		}
		return maxSize;
	}

	enum class argument_name_id : std::uint8_t { unknown };

	// Returns a unique value for the given argument name.
	// Returns argument_name_id::unknown if the argument name was not found in the argument_names array.
	[[nodiscard]] constexpr argument_name_id find_argument_name_id(std::wstring_view const String)
	{
		auto it = std::lower_bound(std::begin(argument_names), std::end(argument_names), String);
		if (it == std::end(argument_names) || *it != String)
		{
			return argument_name_id::unknown;
		}
		return static_cast<argument_name_id>((it - std::begin(argument_names)) + 1);
	}

	// Holds a source and destination directory name.
	struct directory_argument
	{
		std::wstring src;
		std::wstring dst;
	};

	// Core program methods, manages program state.
	class instance
	{
	public:
		// No copy/move.
		instance(instance const&) = delete;
		instance(instance&&) = delete;
		instance& operator=(instance const&) = delete;
		instance& operator=(instance&&) = delete;

		instance()
		{
			mCons.write(L"\x1b[1;4;32mcopynotice (Source Code Notice Writer) v1.0.1\x1b[0;90m\nA tool by Will Daisey\x1b[0m\n");
		}

		void report_exception(std::wstring_view const What)
		{
			mCons.write(L"\x1b[0;1;31m").write(What).write(L"\n");
		}

		// Converts the command line to program arguments.
		// Returns true on success, false on failure.
		[[nodiscard]] bool init(int const ArgC, _In_reads_(ArgC) wchar_t** const ArgV);

		// Executes the program.
		void execute();

	private:
		void target_directories(directory_argument const& Directories, std::vector<directory_argument>& NewDirectories);

		// Iterates through the Directories.src directory, targeting files with the given extension, then calls
		// program::create_file for each target file.
		// Returns the number of files created.
		std::uint32_t iterate(directory_argument const& Directories, std::wstring_view const Extension);

		// Creates the output file, writes the notice to it, and copies the rest of the source file to it.
		// Returns true if and only if a file was created.
		bool create_file(directory_argument const& Directories, std::wstring_view const Fname);

		console mCons;
		bool mRecurse = false;
		bool mVerbose = false;
		bool mReplace = false;
		char8_t mCommentPrefix[16] = u8"// "; // null-terminated
		std::vector<directory_argument> mDirectories;
		std::vector<std::wstring> mExtensions;
		std::u8string mNotice;
		bool mAlwaysOverwriteFiles = false;
	};
}

int wmain(int ArgCount, wchar_t** Args)
{
	try
	{
		program::instance instance;
		try
		{
			if (!instance.init(ArgCount, Args))
			{
				return 1;
			}
			instance.execute();
			return 0;
		}
		catch (std::exception const& e)
		{
			instance.report_exception(wdul::utf8_to_utf16(reinterpret_cast<char8_t const*>(e.what())));
			return 1;
		}
	}
	catch (std::exception const& e)
	{
		MessageBoxA(nullptr, e.what(), "copynotice error", MB_ICONERROR);
		return 1;
	}
}

[[nodiscard]] bool program::instance::init(int const ArgC, _In_reads_(ArgC) wchar_t** const ArgV)
{
	if (ArgC < 2)
	{
		mCons.write(
			L"\x1b[1;31mNo arguments specified.\n\n"
			"\x1b[0;1;4mAvailable arguments:\x1b[24m\n"
			"\x1b[1m/dir       \x1b[34m[src] [dst]\x1b[0m src: A path to a directory to search. dst: A path to a directory to place output files. You may use this argument multiple times.\n"
			"\x1b[1m/ext       \x1b[34m[name]\x1b[0m A target file extension. You may use this argument multiple times.\n"
			"\x1b[1m/note      \x1b[34m[str]\x1b[0m Specifies the notice to write into the output files.\n"
			"\x1b[1m/notef     \x1b[34m[name]\x1b[0m Specifies the name of a text file which contains the notice to write into the output files.\n"
			"\x1b[1m/recurse   \x1b[0mSearches through subdirectories.\n"
			"\x1b[1m/verbose   \x1b[0mLogs extended information.\n"
			"\x1b[1m/syntax    \x1b[34m[prefix] \x1b[0mSets the prefix used to indicate a comment. By default, the comment prefix is set to \"// \".\n"
			"\x1b[1m/replace   \x1b[0mIf a comment is already present at the beginning of a source file, it is replaced.\n"
			"\x1b[90mArguments \x1b[0m/note\x1b[90m and \x1b[0m/notef\x1b[90m are mutually exclusive.\n"
			"\n\x1b[1mExample:\x1b[0m copynotice /dir \"program\\code\" \"temp\" /note \"Written by John Doe.\" /ext \"h\" /ext \"c\" /verbose\n\n"
		);
		return false;
	}
	wchar_t argName[get_max_argument_name_length()];
	for (int argIdx = 1; argIdx < ArgC; ++argIdx)
	{
		wchar_t* arg = ArgV[argIdx];
		if (arg[0] != L'/')
		{
			mCons.write(L"\x1b[1;31mError: Expected argument name, got \"").write(arg).write(L"\". Argument names must start with a forward slash.\n");
			return false;
		}
		std::size_t argNameLen = 0;
		for (std::size_t argChIdx = 1; arg[argChIdx] != L'\0' && argNameLen < std::size(argName); ++argChIdx, ++argNameLen)
		{
			argName[argNameLen] = arg[argChIdx];
		}
		switch (find_argument_name_id(std::wstring_view(argName, argNameLen)))
		{
		case argument_name_id::unknown:
			mCons.write(L"\x1b[1;31mError: Unknown argument \"").write(std::wstring_view(argName, argNameLen)).write(L"\"\n");
			return false;

		case find_argument_name_id(L"dir"):
			if (++argIdx == ArgC || ArgV[argIdx][0] == L'/')
			{
				mCons.write(L"\x1b[1;31mError: Argument \"dir\": expected 2 subarguments, got 0.\n");
				return false;
			}
			{
				auto& dir = mDirectories.emplace_back();
				dir.src = ArgV[argIdx];
				if (dir.src.ends_with(L"\\") || dir.src.ends_with(L"/"))
				{
					mCons.write(L"\x1b[1;31mError: Argument \"dir\": subargument 1 (src): do not use a trailing slash.\n");
					return false;
				}
				if (++argIdx == ArgC || ArgV[argIdx][0] == L'/')
				{
					mCons.write(L"\x1b[1;31mError: Argument \"dir\": expected 2 subarguments, got 1.\n");
					return false;
				}
				dir.dst = ArgV[argIdx];
				if (dir.dst.empty())
				{
					mCons.write(L"\x1b[1;31mError: Argument \"dir\": subargument 2 (dst): argument cannot be empty.\n");
					return false;
				}
				if (dir.dst.ends_with(L"\\") || dir.dst.ends_with(L"/"))
				{
					mCons.write(L"\x1b[1;31mError: Argument \"dir\": subargument 2 (dst): do not use a trailing slash.\n");
					return false;
				}
				if (dir.src.find_first_of(L"<>:\"|?*") != dir.src.npos)
				{
					mCons.write(L"\x1b[1;31mError: Argument \"dir\": subargument \"").write(dir.src).write(L"\" contains an illegal character.\n");
					return false;
				}
				if (dir.dst.find_first_of(L"<>:\"|?*") != dir.dst.npos)
				{
					mCons.write(L"\x1b[1;31mError: Argument \"dir\": subargument \"").write(dir.dst).write(L"\" contains an illegal character.\n");
					return false;
				}
			}
			break;

		case find_argument_name_id(L"ext"):
			if (++argIdx == ArgC || ArgV[argIdx][0] == L'/')
			{
				mCons.write(L"\x1b[1;31mError: Argument \"ext\" must be followed by a subargument.\n");
				return false;
			}
			{
				auto const extensionLen = std::char_traits<wchar_t>::length(ArgV[argIdx]);
				if (extensionLen > 15)
				{
					mCons.write(L"\x1b[1;31mError: Argument \"ext\": extension too long.\n");
					return false;
				}
				if (extensionLen == 0)
				{
					mCons.write(L"\x1b[1;31mError: Argument \"ext\": extension cannot be blank.\n");
					return false;
				}
				auto& extension = mExtensions.emplace_back(ArgV[argIdx]);
				if (extension.find_first_of(L"<>:\"/\\|?*.") != extension.npos)
				{
					mCons.write(L"\x1b[1;31mError: Extension \"").write(extension).write(L"\" contains an illegal character.\n");
					return false;
				}
			}
			break;

		case find_argument_name_id(L"note"):
			if (++argIdx == ArgC || ArgV[argIdx][0] == L'/')
			{
				mCons.write(L"\x1b[1;31mError: Argument \"note\" must be followed by a subargument.\n");
				return false;
			}
			if (!mNotice.empty())
			{
				mCons.write(L"\x1b[1;31mError: Notice string already supplied.\n");
				return false;
			}
			mNotice = wdul::utf16_to_utf8(ArgV[argIdx]);
			break;

		case find_argument_name_id(L"notef"):
			if (++argIdx == ArgC || ArgV[argIdx][0] == L'/')
			{
				mCons.write(L"\x1b[1;31mError: Argument \"notef\" must be followed by a subargument.\n");
				return false;
			}
			if (!mNotice.empty())
			{
				mCons.write(L"\x1b[1;31mError: Notice string already supplied.\n");
				return false;
			}
			{
				auto f = wdul::fopen(ArgV[argIdx], wdul::file_open_mode::open_existing, FILE_FLAG_SEQUENTIAL_SCAN, wdul::generic_access::read, wdul::file_share_mode::read);
				auto const size = wdul::fgetsize(f.get());
				mNotice.resize(static_cast<std::uint32_t>(size));
				wdul::check_bool(ReadFile(f.get(), &mNotice[0], static_cast<std::uint32_t>(size), nullptr, nullptr));
			}
			break;

		case find_argument_name_id(L"recurse"):
			if (mRecurse)
			{
				mCons.write(L"\x1b[1;31mError: /recurse already set.\n");
				return false;
			}
			mRecurse = true;
			break;

		case find_argument_name_id(L"verbose"):
			if (mVerbose)
			{
				mCons.write(L"\x1b[1;31mError: /verbose already set.\n");
				return false;
			}
			mVerbose = true;
			mCons.write(L"\x1b[90m");
			echo_command_line(mCons, ArgC, ArgV);
			break;

		case find_argument_name_id(L"syntax"):
		{
			if (++argIdx == ArgC || ArgV[argIdx][0] == L'/')
			{
				mCons.write(L"\x1b[1;31mError: Argument \"syntax\" must be followed by a subargument.\n");
				return false;
			}
			auto subargument = wdul::utf16_to_utf8(ArgV[argIdx]);
			auto const maxCommentLength = sizeof(mCommentPrefix) - 1; // -1 for null terminator
			if (subargument.size() > maxCommentLength)
			{
				mCons.write(L"\x1b[1;31mError: Argument \"syntax\": subargument cannot exceed ").write(std::to_wstring(maxCommentLength).data()).write(L" characters.\n");
				return false;
			}
			if (subargument.empty())
			{
				mCons.write(L"\x1b[1;31mError: Argument \"syntax\": subargument must not be blank.\n");
				return false;
			}
			std::memcpy(mCommentPrefix, subargument.data(), subargument.size() + 1 /*+ 1 for null terminator*/);
			break;
		}

		case find_argument_name_id(L"replace"):
			if (mReplace)
			{
				mCons.write(L"\x1b[1;31mError: /replace already set.\n");
				return false;
			}
			mReplace = true;
			break;

		default:
			mCons.write(L"\x1b[1;31mError: Behaviour not implemented for argument \"").write(std::wstring_view(argName, argNameLen)).write(L"\"\n");
			return false;
		}
	}

	std::vector<directory_argument> newDirectories;
	for (auto const& directories : mDirectories)
	{
		target_directories(directories, newDirectories);
	}

	for (auto const& newDirectoryArg : newDirectories)
	{
		bool alreadyTargeted = false;
		for (auto const& directories : mDirectories)
		{
			if (directories.src == newDirectoryArg.src)
			{
				mCons.write(L"\x1b[90mDirectory \x1b[33m\"").write(newDirectoryArg.src).write(L"\"\x1b[90m is already targeted.\n");
				alreadyTargeted = true;
			}
		}
		if (!alreadyTargeted) mDirectories.push_back(newDirectoryArg);
	}

	for (auto const& directories : mDirectories)
	{
		if (!CreateDirectoryW(directories.dst.data(), nullptr))
		{
			auto const lastError = GetLastError();
			if (lastError != ERROR_ALREADY_EXISTS)
			{
				wdul::throw_win32(lastError, "Could not create output directory. Ensure intermediate directories exist");
			}
		}
		else
		{
			mCons.write(L"\x1b[90mCreated output directory \x1b[33m\"").write(directories.dst).write(L"\"\x1b[0m\n");
		}
	}

	return true;
}

void program::instance::target_directories(directory_argument const& Directories, std::vector<directory_argument>& NewDirectories)
{
	mCons.write(L"\x1b[90mTarget directory: \x1b[33m\"").write(Directories.src).write(
		L"\"\x1b[90m. Output directory: \x1b[33m\"").write(Directories.dst).write(L"\"\x1b[0m\n");

	if (!mRecurse)
	{
		return;
	}

	// Recursively add subdirectories to the NewDirectories vector:

	WIN32_FIND_DATAW findData;
	std::wstring searchString = Directories.src + (Directories.src.empty() ? L"*" : L"\\*");

	auto findHandle = wdul::find_file_handle(FindFirstFileW(searchString.data(), &findData));
	if (!findHandle)
	{
		// FindFirstFile shouldn't fail with ERROR_NO_MORE_FILES because of the current and parent directories ("." and "..").
		wdul::throw_last_error("FindFirstFile failed");
	}

	do
	{
		if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			// Skip non-directories.
			continue;
		}
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
		{
			// Skip hidden directories.
			continue;
		}
		if (findData.cFileName[0] == L'.' && (findData.cFileName[1] == L'\0' || (findData.cFileName[1] == L'.' && findData.cFileName[2] == L'\0')))
		{
			// Skip the current & parent directories.
			continue;
		}

		directory_argument newDirectory = Directories;
		if (!newDirectory.src.empty()) newDirectory.src += L'\\';
		newDirectory.src += findData.cFileName;
		newDirectory.dst += L'\\';
		newDirectory.dst += findData.cFileName;
		NewDirectories.push_back(newDirectory);
		target_directories(newDirectory, NewDirectories);

	} while (FindNextFileW(findHandle.get(), &findData));

	auto const errorCode = GetLastError();
	if (errorCode != ERROR_NO_MORE_FILES)
	{
		wdul::throw_win32(errorCode, "FindNextFile failed");
	}
}

void program::instance::execute()
{
	std::uint32_t filesCreated = 0;
	mCons.write(L"\x1b[0m");
	for (auto const& directories : mDirectories)
	{
		for (auto const& extension : mExtensions)
		{
			filesCreated += iterate(directories, extension);
		}
	}
	mCons.write(L"\x1b[32;1mDone. Created ").write(std::to_wstring(filesCreated)).write(L" file(s)\x1b[0m\n");
}

std::uint32_t program::instance::iterate(directory_argument const& Directories, std::wstring_view const Extension)
{
	std::wstring targetFilename;

	// Build the target path and file name.
	targetFilename = Directories.src;
	if (!Directories.src.empty()) targetFilename += L'\\';

	// Append the asterisk wildcard to signify any file name, then append the extension.
	targetFilename += L"*.";
	targetFilename += Extension;

	if (mVerbose)
	{
		mCons.write(L"\n\x1b[32mExecuting for target: \x1b[33m\"").write(targetFilename).write(L"\"\n");
	}

	WIN32_FIND_DATAW findData;
	wdul::find_file_handle findHandle(FindFirstFileW(targetFilename.data(), &findData));
	if (!findHandle)
	{
		auto const error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND)
		{
			mCons.write(L"\x1b[32mCould not find a target file for target: \x1b[33m\"").write(targetFilename).write(L"\"\n");
			return 0;
		}
		wdul::throw_win32(error, "FindFirstFile failed");
	}

	std::uint32_t filesCreated = 0;
	do
	{
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
		{
			continue;
		}
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			continue;
		}
		if (create_file(Directories, findData.cFileName))
		{
			++filesCreated;
		}
	} while (FindNextFileW(findHandle.get(), &findData));

	auto const errorCode = GetLastError();
	if (errorCode == ERROR_NO_MORE_FILES)
	{
		mCons.write(L"\x1b[32;1mFinished target \"").write(targetFilename).write(L"\": Created ").write(std::to_wstring(filesCreated)).write(L" file(s)\x1b[0m\n");
		return filesCreated;
	}
	wdul::throw_win32(errorCode, "Failed to find the next target file");
}

bool program::instance::create_file(directory_argument const& Directories, std::wstring_view const Fname)
{
	auto srcPath = Directories.src;
	if (!srcPath.empty())
	{
		// If the source directory path is not empty, append a backslash.
		srcPath += L'\\';
	}
	srcPath += Fname;

	// Open the source file for reading.
	if (mVerbose) mCons.write(L" \x1b[90mOpening  \x1b[33m\"").write(srcPath).write(L"\"\x1b[90m... ");
	auto srcFile = wdul::fopen(srcPath.data(), wdul::file_open_mode::open_existing, 0, wdul::generic_access::read, wdul::file_share_mode::read);
	if (mVerbose) mCons.write(L"\x1b[90mDone.\x1b[0m\n");

	// Destination directory cannot be empty.
	auto dstPath = Directories.dst;
	dstPath += L'\\';
	dstPath += Fname;

	if (!mAlwaysOverwriteFiles)
	{
		// Ask before overwriting files.
		if (wdul::fexists(dstPath.data()))
		{
			mCons.write(L"\x1b[94mFile \x1b[93m\"").write(dstPath).write(L"\"\x1b[94m already exists.\x1b[0m\n");
			mCons.write(L"Do you want to overwrite this file and future files? (y/n)\n");
			if (ask_yesno(mCons))
			{
				mAlwaysOverwriteFiles = true;
			}
			else
			{
				return false;
			}
		}
	}

	// Create the destination file for writing.
	if (mVerbose) mCons.write(L" \x1b[90mCreating \x1b[33m\"").write(dstPath).write(L"\"\x1b[90m... ");
	auto dstFile = wdul::fopen(dstPath.data(), wdul::file_open_mode::create_always, FILE_ATTRIBUTE_NORMAL, wdul::generic_access::write, wdul::file_share_mode::read);
	if (mVerbose) mCons.write(L"\x1b[90mDone.\x1b[0m\n");

	std::uint8_t readBuffer[64]; // temporary storage buffer for reading.

	// Store the first line of the source file in firstCodeLine.
	std::u8string firstCodeLine;
	if (!wdul::freadline(srcFile.get(), firstCodeLine, sizeof(readBuffer), readBuffer))
	{
		mCons.write(L"\x1b[1;31mSource file ").write(Fname).write(L" is empty.\x1b[0m\n");
		return true;
	}

	if (firstCodeLine.starts_with(mCommentPrefix)) // If the first line of the source file is a comment:
	{
		if (mReplace)
		{
			// If mReplace is set, read past the comment and any consecutive comments in the source file.
			// firstCodeLine will be set to the first non-comment code line.
			while (wdul::freadline(srcFile.get(), firstCodeLine, sizeof(readBuffer), readBuffer))
			{
				if (!firstCodeLine.starts_with(mCommentPrefix))
				{
					break;
				}
			}
		}
	}

	char8_t newline[] = { u8'\r', u8'\n' };
	auto const commentPrefixLen = static_cast<std::uint32_t>(std::char_traits<char8_t>::length(mCommentPrefix));
	std::uint32_t noticeOffset = 0, commentEndPos;

	do
	{
		using namespace std::string_view_literals; // for the sv operator for constructing a string_view.

		// Set commentEndPos to the position of the first new line found.
		auto const findResult = mNotice.find(u8"\r\n"sv, noticeOffset);
		commentEndPos = static_cast<std::uint32_t>(findResult);
		if (findResult == std::u8string_view::npos)
		{
			commentEndPos = static_cast<std::uint32_t>(mNotice.size());
		}

		// Write the comment prefix to the destination file.
		wdul::fwrite(dstFile.get(), commentPrefixLen, mCommentPrefix);
		// Write a line of the notice to the destination file.
		wdul::fwrite(dstFile.get(), commentEndPos - noticeOffset, &mNotice[noticeOffset]);
		// Write a new line to the destination file.
		wdul::fwrite(dstFile.get(), sizeof(newline), newline);

		// Set noticeOffset to the next line.
		noticeOffset = commentEndPos + 2;
	} while (commentEndPos != static_cast<std::uint32_t>(mNotice.size()));

	// Write the previously stored first line of the source file to the destination file.
	wdul::fwrite(dstFile.get(), static_cast<std::uint32_t>(firstCodeLine.size()), firstCodeLine.data());
	wdul::fwrite(dstFile.get(), sizeof(newline), newline);

	// Write the rest of the source file to the destination file.
	std::uint32_t readSize;
	while ((readSize = wdul::fread(srcFile.get(), sizeof(readBuffer), readBuffer)) != 0)
	{
		wdul::fwrite(dstFile.get(), readSize, readBuffer);
	}

	dstFile.close();
	return true;
}
