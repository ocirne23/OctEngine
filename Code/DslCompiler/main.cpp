import Core;
import File;
import Script;
import Entity;

// DslCompiler: command-line DSL -> .dsl compile. See CONTEXT.md next to this file for usage and the full
// language reference. Parses DSL text, transpiles it, and writes the dual-purpose .dsl file (generated C++ +
// "//@"-commented DSL block) -- the exact file the Script Editor's Save produces. Only registerScriptDslBindings
// runs (pure registration); no engine system is initialized, and main() exits via _Exit because the init_seg
// global dtors assume the NORMAL engine teardown order (renderer globals touch Vulkan on destruction) and
// fail-fast without it.
//
// Input is either an existing .dsl (detected by a "//@@dsl" line: loaded directly, then re-saved normalized
// with the C++ regenerated) or RAW DSL text: code lines plain with TAB indentation, directives spelled with a
// single '@' ("@require ..."/"@data ..."/"@event ..."), no markers. The wrap step inserts "//@" at the start
// of EVERY line -- which is exactly what turns "@require" into the stored "//@@require" form -- and brackets
// the result with the "//@@dsl 1"/"//@@end" markers ScriptLoader::load reads. Do not write "@dsl"/"@end"
// marker lines in raw input; the wrapper owns those.
static int compileDsl(const std::string& inputPath, const std::string& outputPath)
{
	registerScriptDslBindings();

	DSL document;
	std::vector<std::unique_ptr<DSLSymbol>> builtins;
	Globals::scriptBindings.build(document.sidebar, builtins);

	// A console tool: everything runs on its one (main) thread by design.
	const FileSystem::AllowMainThreadIO allowIo;
	if (!FileSystem::exists(inputPath))
	{
		std::cout << "error: cannot open '" << inputPath << "'\n";
		return 1;
	}
	std::istringstream in(FileSystem::readFileStr(inputPath));
	std::vector<std::string> lines;
	bool alreadyWrapped = false;
	for (std::string raw; std::getline(in, raw); )
	{
		if (!raw.empty() && raw.back() == '\r')
			raw.pop_back();
		alreadyWrapped = alreadyWrapped || raw.rfind("//@@dsl", 0) == 0;
		lines.push_back(std::move(raw));
	}
	std::string loadPath = inputPath;
	std::string tempPath;
	int lineShift = 0;
	if (!alreadyWrapped)
	{
		std::string wrapped = "//@@dsl 1\n";
		lineShift = 1; // the inserted header shifts every line number the loader reports by one
		for (const std::string& line : lines)
			wrapped += "//@" + line + "\n";
		wrapped += "//@@end\n";
		tempPath = outputPath + ".wrap.tmp";
		if (!FileSystem::writeFileStr(tempPath, wrapped))
		{
			std::cout << "error: cannot write '" << tempPath << "'\n";
			return 1;
		}
		loadPath = tempPath;
	}

	const ScriptLoader::LoadResult result = ScriptLoader::load(document, loadPath, builtins, Globals::scriptBindings);
	if (!tempPath.empty())
		FileSystem::remove(tempPath);
	if (!result.success)
	{
		std::string error = result.error; // "<path>(<line>): <what>"
		if (!tempPath.empty() && error.rfind(tempPath + "(", 0) == 0)
		{
			const size_t close = error.find(')');
			const int line = std::atoi(error.c_str() + tempPath.size() + 1);
			error = inputPath + "(" + std::to_string(std::max(1, line - lineShift)) + ")"
				+ (close != std::string::npos ? error.substr(close + 1) : std::string());
		}
		std::cout << "error: " << error << "\n";
		return 1;
	}

	// load() already ran checkContainerMutations, so save()'s own re-check cannot be what fails here.
	if (!ScriptLoader::save(document, outputPath, Transpiler::transpile(document, Globals::scriptBindings)))
	{
		std::cout << "error: cannot write '" << outputPath << "'\n";
		return 1;
	}
	std::cout << "wrote " << outputPath << "\n";
	return 0;
}

// --compile: run the written .dsl through the REAL ScriptHost pipeline (vcvars64 + cl into
// Assets/Local/Scripts, then LoadLibrary) -- the exact path F6 takes, so a green result here means the script
// loads in the engine as-is. cl's own error log prints via Log (stdout in a console build). Side benefit: the
// produced DLL is the same file the engine would build, mtime-stamped, so the engine can skip recompiling it.
// A fresh process has no "previous build" to fall back to, which makes a non-empty dllPath the exact
// "compiled AND exported entry points" signal (see ScriptHost::getOrLoad's failure paths).
static int compileOutput(const std::string& outputPath)
{
	const ScriptModule* module = Globals::scriptHost.getOrLoad(outputPath, /*forceRecompile*/ true);
	if (module == nullptr || module->dllPath.empty())
	{
		std::cout << "error: compile failed for '" << outputPath << "' (cl output above)\n";
		return 1;
	}
	std::cout << "compiled OK: " << module->dllPath << "\n  entry points:";
	if (module->onSpawn != nullptr)        std::cout << " OnSpawn";
	if (module->update != nullptr)         std::cout << " Update";
	if (module->onDestroy != nullptr)      std::cout << " OnDestroy";
	if (module->onEvent != nullptr)        std::cout << " OnEvent";
	if (module->onPhysicsEvent != nullptr) std::cout << " OnPhysicsEvent";
	std::cout << "\n  script data: " << module->dataSize << " bytes, required components mask: 0x"
		<< std::hex << module->requiredComponents << std::dec;
	if (!module->eventNames.empty())
	{
		std::cout << ", events:";
		for (const std::string& name : module->eventNames)
			std::cout << " " << name;
	}
	std::cout << "\n";
	return 0;
}

int main(int argc, char* argv[])
{
	Globals::profiler.endStaticInit();
	FileSystem::initialize(); // relative paths resolve against Assets/, like everywhere else in the engine

	bool checkCompile = false;
	std::vector<std::string> paths;
	for (int i = 1; i < argc; ++i)
	{
		const std::string_view arg = argv[i];
		if (arg == "--compile" || arg == "-c")
			checkCompile = true;
		else
			paths.push_back(std::string(arg));
	}
	if (paths.empty() || paths.size() > 2)
	{
		std::cout << "usage: DslCompiler <input> [output.dsl] [--compile]\n"
			"  input:     raw DSL text (see Code/DslCompiler/CONTEXT.md) or an existing .dsl\n"
			"  output:    defaults to the input path with a .dsl extension\n"
			"  --compile: after writing, cl-compile + load the output via ScriptHost (the F6 pipeline)\n"
			"  relative paths resolve against Assets/\n";
		std::cout.flush();
		std::_Exit(1);
	}
	const std::string& input = paths[0];
	const std::string output = paths.size() > 1 ? paths[1]
		: FileSystem::replaceExtension(input, ".dsl");

	int code = compileDsl(input, output);
	if (code == 0 && checkCompile)
		code = compileOutput(output);
	// Skip the global dtors entirely -- nothing was initialized (see the comment on compileDsl).
	std::cout.flush();
	std::_Exit(code);
}
