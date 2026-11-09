import App.UnattendedRun;
import Core;
import Core.imgui;
import Core.SDL;
import File;
import Script;
import Entity;
import UI;

// ScriptEditorSim: the real Script Editor panel with simulated keyboard input and no window, GPU or engine. See
// Docs/Context/ScriptEditorSim/CONTEXT.md for the key syntax and the commands.

namespace
{
	const char* const c_defaultDocument =
		"//@@dsl 1\n"
		"//@\n"
		"//@function Update(float deltaSeconds)\n"
		"//@\t\n"
		"//@end\n"
		"//@@end\n";

	// ---- Clipboard: a private string, so a simulated copy never reaches the real one ----
	oc::string g_clipboard;
	const char* getClipboard(ImGuiContext*) { return g_clipboard.c_str(); }
	void setClipboard(ImGuiContext*, const char* text) { g_clipboard = text; }

	// ---- Keys ----
	struct SimKey
	{
		SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
		int keycode = 0;
		bool shift = false;
		bool ctrl = false;
		bool alt = false;
	};

	// The inverse of the editor's charFromKeycode (US QWERTY): the physical key and shift state that type `c`.
	SimKey keyFromChar(char c)
	{
		static constexpr oc::string_view shifted = "!@#$%^&*()_+{}|:\"<>?~";
		static constexpr oc::string_view unshifted = "1234567890-=[]\\;',./`";
		SimKey key;
		const size_t shiftIndex = shifted.find(c);
		if (shiftIndex != oc::string_view::npos)
		{
			key.shift = true;
			key.keycode = unshifted[shiftIndex];
		}
		else if (c >= 'A' && c <= 'Z')
		{
			key.shift = true;
			key.keycode = c - 'A' + 'a';
		}
		else
		{
			key.keycode = c;
		}
		return key;
	}

	bool namedKey(oc::string_view name, SimKey& key)
	{
		struct Named { const char* name; SDL_Scancode scancode; };
		static constexpr Named table[] = {
			{ "enter", SDL_SCANCODE_RETURN }, { "bs", SDL_SCANCODE_BACKSPACE }, { "del", SDL_SCANCODE_DELETE }, { "esc", SDL_SCANCODE_ESCAPE },
			{ "tab", SDL_SCANCODE_TAB }, { "up", SDL_SCANCODE_UP }, { "down", SDL_SCANCODE_DOWN }, { "left", SDL_SCANCODE_LEFT },
			{ "right", SDL_SCANCODE_RIGHT }, { "home", SDL_SCANCODE_HOME }, { "end", SDL_SCANCODE_END }, { "pgup", SDL_SCANCODE_PAGEUP },
			{ "pgdn", SDL_SCANCODE_PAGEDOWN },
		};
		for (const Named& entry : table)
			if (name == entry.name)
			{
				key.scancode = entry.scancode;
				key.keycode = 0;
				return true;
			}
		if (name.size() == 1)
		{
			key.keycode = name[0];
			if (name[0] >= 'a' && name[0] <= 'z')
				key.scancode = static_cast<SDL_Scancode>(SDL_SCANCODE_A + (name[0] - 'a'));
			return true;
		}
		return false;
	}

	SDL_Event toEvent(const SimKey& key)
	{
		SDL_Event event{};
		event.type = SDL_EVENT_KEY_DOWN;
		event.key.scancode = key.scancode;
		event.key.key = static_cast<SDL_Keycode>(key.keycode);
		event.key.mod = static_cast<SDL_Keymod>((key.shift ? SDL_KMOD_SHIFT : 0) | (key.ctrl ? SDL_KMOD_CTRL : 0) | (key.alt ? 0x0100u : 0u));
		event.key.down = true;
		return event;
	}

	// ---- Software renderer: the ImDrawData as a PNG, so the layout can be inspected without a GPU ----
	void rasterTriangle(oc::vector<uint8>& rgba, int width, int height, const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c,
		ImTextureData* tex, const ImVec4& clip)
	{
		const float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
		if (area == 0.0f)
			return;
		const int minX = oc::max(static_cast<int>(oc::min(oc::min(a.pos.x, b.pos.x), c.pos.x)), oc::max(static_cast<int>(clip.x), 0));
		const int minY = oc::max(static_cast<int>(oc::min(oc::min(a.pos.y, b.pos.y), c.pos.y)), oc::max(static_cast<int>(clip.y), 0));
		const int maxX = oc::min(static_cast<int>(std::ceil(oc::max(oc::max(a.pos.x, b.pos.x), c.pos.x))), oc::min(width, static_cast<int>(clip.z)));
		const int maxY = oc::min(static_cast<int>(std::ceil(oc::max(oc::max(a.pos.y, b.pos.y), c.pos.y))), oc::min(height, static_cast<int>(clip.w)));
		// ImGui gives every primitive this editor draws one colour on all of its vertices.
		const float colR = static_cast<float>((a.col >> 0) & 0xFF), colG = static_cast<float>((a.col >> 8) & 0xFF),
			colB = static_cast<float>((a.col >> 16) & 0xFF), colA = static_cast<float>((a.col >> 24) & 0xFF) / 255.0f;
		for (int y = minY; y < maxY; ++y)
			for (int x = minX; x < maxX; ++x)
			{
				const float px = x + 0.5f, py = y + 0.5f;
				const float w0 = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / area;
				const float w1 = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) / area;
				const float w2 = 1.0f - w0 - w1;
				if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
					continue;
				float r = 1.0f, g = 1.0f, bl = 1.0f, alpha = colA;
				if (tex != nullptr && tex->Pixels != nullptr)
				{
					const int u = oc::clamp(static_cast<int>((w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x) * tex->Width), 0, tex->Width - 1);
					const int v = oc::clamp(static_cast<int>((w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y) * tex->Height), 0, tex->Height - 1);
					const unsigned char* texel = static_cast<const unsigned char*>(tex->GetPixelsAt(u, v));
					if (tex->BytesPerPixel == 4)
					{
						r = texel[0] / 255.0f;
						g = texel[1] / 255.0f;
						bl = texel[2] / 255.0f;
						alpha *= texel[3] / 255.0f;
					}
					else
					{
						alpha *= texel[0] / 255.0f;
					}
				}
				uint8* pixel = &rgba[(static_cast<size_t>(y) * width + x) * 4];
				pixel[0] = static_cast<uint8>(colR * r * alpha + pixel[0] * (1.0f - alpha));
				pixel[1] = static_cast<uint8>(colG * g * alpha + pixel[1] * (1.0f - alpha));
				pixel[2] = static_cast<uint8>(colB * bl * alpha + pixel[2] * (1.0f - alpha));
			}
	}

	bool writeScreenshot(const ImDrawData& drawData, int width, int height, const oc::string& path)
	{
		oc::vector<uint8> rgba(static_cast<size_t>(width) * height * 4, 255);
		for (const ImDrawList* list : drawData.CmdLists)
			for (const ImDrawCmd& cmd : list->CmdBuffer)
			{
				if (cmd.UserCallback != nullptr)
					continue;
				ImTextureData* tex = reinterpret_cast<ImTextureData*>(cmd.GetTexID());
				const ImDrawIdx* indices = list->IdxBuffer.Data + cmd.IdxOffset;
				const ImDrawVert* verts = list->VtxBuffer.Data + cmd.VtxOffset;
				for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3)
					rasterTriangle(rgba, width, height, verts[indices[i]], verts[indices[i + 1]], verts[indices[i + 2]], tex, cmd.ClipRect);
			}
		return ImageIO::writePngRgba8(path, static_cast<uint32>(width), static_cast<uint32>(height), oc::span<const uint8>(rgba.data(), rgba.size()), /*allowMainThread*/ true);
	}

	// ---- The headless ImGui frame ----
	class Session
	{
	public:
		Session(int width, int height) : m_width(width), m_height(height)
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
			io.DeltaTime = 1.0f / 60.0f;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
			platform.Platform_GetClipboardTextFn = getClipboard;
			platform.Platform_SetClipboardTextFn = setClipboard;
			ImGui::StyleColorsDark();
		}

		void frame(ScriptEditor& editor)
		{
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
			if (m_frame++ == 0)
				ImGui::SetNextWindowFocus(); // once: focusing each frame would raise this window over the candidate popup
			ImGui::Begin("Script Editor", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);
			editor.render();
			ImGui::End();
			ImGui::Render();
			serviceTextures();
		}

		// Each change gets its own frame: ImGui spreads events of different kinds over frames anyway, and a press must be
		// seen at its position.
		void mouseTo(ScriptEditor& editor, int line)
		{
			float x = 0.0f, y = 0.0f;
			editor.linePosition(line, x, y);
			ImGui::GetIO().AddMousePosEvent(x, y);
			frame(editor);
		}

		void mouseButton(ScriptEditor& editor, bool down)
		{
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
			frame(editor);
		}

		void shiftKey(ScriptEditor& editor, bool down)
		{
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, down);
			frame(editor);
		}

		bool screenshot(ScriptEditor& editor, const oc::string& path)
		{
			frame(editor); // an auto-sized popup measures itself on the frame before it can be placed
			frame(editor);
			return writeScreenshot(*ImGui::GetDrawData(), m_width, m_height, path);
		}

	private:
		// A software "upload": the rasterizer reads the texture's CPU pixels through the pointer stored as the id.
		void serviceTextures()
		{
			for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
			{
				if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)
				{
					tex->SetTexID(reinterpret_cast<ImTextureID>(tex));
					tex->SetStatus(ImTextureStatus_OK);
				}
				else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
				{
					tex->SetTexID(0);
					tex->SetStatus(ImTextureStatus_Destroyed);
				}
			}
		}

		int m_width, m_height;
		int m_frame = 0;
	};

	// ---- The key program ----
	struct Options
	{
		bool trace = false;
		bool burst = false; // every key of the program into ONE frame: fast typing, between two widget passes
		int context = -1;
		oc::string pngPath;
		oc::string outPath; // the final document as .dsl text, for DslCompiler --compile
	};

	// Plain characters press their key, {name} a named key: {enter} {bs} {del} {esc} {tab} {up} {down} {left} {right}
	// {home} {end} {pgup} {pgdn}, with the modifiers c-, s- and a- ({c-s}, {s-tab}, {a-down}) and a repeat ({bs*5}). The mouse
	// is {mdown:<line>} {mmove:<line>} {mup} {sclick:<line>}. The control commands are {show} (print the state) and
	// {png:path}; '{{' types a '{'. Line breaks in a program mean nothing.
	bool runProgram(const oc::string& program, ScriptEditor& editor, Session& session, const Options& options)
	{
		const auto press = [&](const SimKey& key, const oc::string& label)
		{
			// A keyboard sends each held modifier as a key press of its own, before the key it modifies.
			const auto pressModifier = [&](bool held, SDL_Scancode scancode)
			{
				if (!held)
					return;
				SimKey modifier = key;
				modifier.scancode = scancode;
				modifier.keycode = 0;
				editor.handleKeyEvent(toEvent(modifier));
				if (!options.burst)
					session.frame(editor);
			};
			pressModifier(key.shift, SDL_SCANCODE_LSHIFT);
			pressModifier(key.ctrl, SDL_SCANCODE_LCTRL);
			pressModifier(key.alt, SDL_SCANCODE_LALT);
			editor.handleKeyEvent(toEvent(key));
			if (!options.burst)
				session.frame(editor);
			if (options.trace)
				printf("%-9s %s\n", label.c_str(), editor.describeStateShort().c_str());
		};

		for (size_t pos = 0; pos < program.size(); ++pos)
		{
			const char c = program[pos];
			if (c == '\n' || c == '\r')
				continue;
			if (c != '{' || (pos + 1 < program.size() && program[pos + 1] == '{'))
			{
				if (c == '{')
					++pos;
				press(keyFromChar(c), oc::string(1, c));
				continue;
			}
			const size_t close = program.find('}', pos);
			if (close == oc::string::npos)
			{
				printf("error: unclosed { at %zu\n", pos);
				return false;
			}
			oc::string token = program.substr(pos + 1, close - pos - 1);
			pos = close;
			if (token == "show")
			{
				session.frame(editor);
				printf("%s", editor.describeState(options.context).c_str());
				continue;
			}
			if (token.rfind("png:", 0) == 0)
			{
				session.screenshot(editor, token.substr(4));
				continue;
			}
			if (token == "wait-compile")
			{
				// What the App's main loop does with a save, then the wait for the verdict.
				session.frame(editor);
				Globals::scriptHost.handleScriptReloadRequests(editor.takeReloadRequests());
				while (Globals::scriptHost.isCompileCheckRunning())
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				session.frame(editor);
				continue;
			}
			if (token.rfind("mdown:", 0) == 0 || token.rfind("mmove:", 0) == 0 || token.rfind("sclick:", 0) == 0)
			{
				const int line = std::atoi(token.c_str() + token.find(':') + 1);
				if (token[0] == 's')
					session.shiftKey(editor, true);
				session.mouseTo(editor, line);
				if (token[0] != 'm' || token[1] == 'd')
					session.mouseButton(editor, true);
				if (token[0] == 's')
				{
					session.mouseButton(editor, false);
					session.shiftKey(editor, false);
				}
			}
			else if (token == "mup")
			{
				session.mouseButton(editor, false);
			}
			if (token.rfind("mdown:", 0) == 0 || token.rfind("mmove:", 0) == 0 || token.rfind("sclick:", 0) == 0 || token == "mup")
			{
				if (options.trace)
					printf("%-9s %s\n", ("{" + token + "}").c_str(), editor.describeStateShort().c_str());
				continue;
			}
			int repeat = 1;
			if (const size_t star = token.find('*'); star != oc::string::npos)
			{
				repeat = std::atoi(token.c_str() + star + 1);
				token.resize(star);
			}
			SimKey key;
			oc::string_view name = token;
			while (name.size() > 2 && name[1] == '-')
			{
				if (name[0] == 'c') key.ctrl = true;
				else if (name[0] == 's') key.shift = true;
				else if (name[0] == 'a') key.alt = true;
				name.remove_prefix(2);
			}
			if (!namedKey(name, key))
			{
				printf("error: unknown key {%s}\n", token.c_str());
				return false;
			}
			for (int i = 0; i < repeat; ++i)
				press(key, "{" + token + "}");
		}
		return true;
	}

	// ---- Monkey: random keys, watching for a crash or a document that no longer loads ----
	int runMonkey(int keyCount, uint32 seed, ScriptEditor& editor, Session& session)
	{
		static constexpr oc::string_view typed = "abcdefghijklmnopqrstuvwxyz abcdefgifor 0123456789.,()+-*/%<>=!&|\"#_ ";
		static const char* const named[] = { "{enter}", "{bs}", "{bs}", "{bs}", "{esc}", "{tab}", "{up}", "{down}", "{left}", "{right}", "{home}", "{end}", "{c-c}", "{c-v}", "{c-x}", "{del}", "{c-z}", "{c-y}", "{a-up}", "{a-up}", "{a-down}", "{a-down}", "{s-up}", "{s-down}" };
		const Options options{};
		std::mt19937 random(seed);
		oc::string log;
		for (int i = 0; i < keyCount; ++i)
		{
			const oc::string token = random() % 3 == 0 ? oc::string(named[random() % std::size(named)]) : oc::string(1, typed[random() % typed.size()]);
			log += token;
			// Written before the key runs: a crash leaves the exact program that reproduces it.
			FileSystem::writeFileStr("Local/ScriptEditorSim-monkey.keys", log, /*allowMainThread*/ true);
			if (!runProgram(token, editor, session, options))
				return 1;
			oc::string error;
			if (!editor.documentLoads(error))
			{
				printf("round trip FAILED after %d keys (seed %u): %s\nkeys: %s\n%s\n", i + 1, seed, error.c_str(), log.c_str(), editor.documentText().c_str());
				return 1;
			}
		}
		printf("monkey OK: %d keys, seed %u\n", keyCount, seed);
		return 0;
	}

	// Raw DSL as in DslCompiler, with "\n" and "\t" spelled out so one line holds a whole document.
	oc::string wrapRawDsl(const char* raw)
	{
		oc::string document = "//@@dsl 1\n//@";
		for (const char* c = raw; *c != '\0'; ++c)
		{
			if (c[0] == '\\' && c[1] == 'n') { document += "\n//@"; ++c; }
			else if (c[0] == '\\' && c[1] == 't') { document += '\t'; ++c; }
			else document += *c;
		}
		return document + "\n//@@end\n";
	}

	// ---- Check: the regression file, lines of "<key program> ==> <line the document must then hold>" ----
	int runCheck(const oc::string& path, ScriptEditor& editor, Session& session)
	{
		std::istringstream in(oc::toStd(FileSystem::readFileStr(path)));
		int failures = 0, cases = 0;
		oc::string document;
		for (oc::string line; oc::getline(in, line); )
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.rfind("doc ", 0) == 0)
			{
				document = wrapRawDsl(line.c_str() + 4);
				continue;
			}
			const size_t arrow = line.find(" ==> ");
			if (arrow == oc::string::npos)
				continue;
			++cases;
			oc::string error;
			if (!editor.loadText(document, error))
			{
				printf("error: %s\n", error.c_str());
				return 1;
			}
			session.frame(editor);
			const oc::string program = line.substr(0, arrow);
			const oc::string expected = line.substr(arrow + 5);
			runProgram(program, editor, session, Options{});
			session.frame(editor);
			// The document with its indentation gone and " ;; " between the lines, so a case can require several in a row.
			oc::string text = " ;; ";
			std::istringstream documentLines(oc::toStd(editor.documentText()));
			for (oc::string documentLine; oc::getline(documentLines, documentLine); )
			{
				size_t start = documentLine.rfind("//@", 0) == 0 ? 3 : 0;
				while (start < documentLine.size() && documentLine[start] == '\t')
					++start;
				text += documentLine.substr(start) + " ;; ";
			}
			if (text.find(" ;; " + expected + " ;; ") == oc::string::npos)
			{
				++failures;
				printf("FAIL %s\n  wanted: %s\n  state:  %s\n", program.c_str(), expected.c_str(), editor.describeStateShort().c_str());
			}
		}
		printf("%d of %d cases pass\n", cases - failures, cases);
		return failures > 0 ? 1 : 0;
	}

	int usage()
	{
		printf("usage: ScriptEditorSim [--doc <path>] [--text <dsl lines>] [--keys <program>] [--keys-file <path>] [--trace] [--burst]\n"
			"                       [--context <n>] [--png <path>] [--out <path>] [--save-as <path>] [--size <w>x<h>] [--monkey <keys> [--seed <n>]] [--check <path>]\n");
		fflush(nullptr);
		return 2;
	}
}

int main(int argc, char* argv[])
{
	installUnattendedFailureHandling();
	Globals::profiler.endStaticInit();
	FileSystem::initialize();
	const FileSystem::AllowMainThreadIO allowIo;
	registerScriptDslBindings();

	Options options;
	oc::string document = c_defaultDocument;
	oc::string program;
	oc::string checkPath;
	oc::string savePath;
	int width = 1280, height = 800, monkeyKeys = 0;
	uint32 seed = 1;
	for (int i = 1; i < argc; ++i)
	{
		const oc::string_view arg = argv[i];
		const bool hasValue = i + 1 < argc;
		if (arg == "--trace")
			options.trace = true;
		else if (arg == "--burst")
			options.burst = true;
		else if (arg == "--doc" && hasValue)
			document = FileSystem::readFileStr(argv[++i]);
		else if (arg == "--text" && hasValue)
			document = wrapRawDsl(argv[++i]);
		else if (arg == "--keys" && hasValue)
			program += argv[++i];
		else if (arg == "--keys-file" && hasValue)
			program += FileSystem::readFileStr(argv[++i]);
		else if (arg == "--context" && hasValue)
			options.context = std::atoi(argv[++i]);
		else if (arg == "--png" && hasValue)
			options.pngPath = argv[++i];
		else if (arg == "--save-as" && hasValue)
			savePath = argv[++i];
		else if (arg == "--out" && hasValue)
			options.outPath = argv[++i];
		else if (arg == "--check" && hasValue)
			checkPath = argv[++i];
		else if (arg == "--monkey" && hasValue)
			monkeyKeys = std::atoi(argv[++i]);
		else if (arg == "--seed" && hasValue)
			seed = static_cast<uint32>(std::atoi(argv[++i]));
		else if (arg == "--size" && hasValue)
		{
			const oc::string_view size = argv[++i];
			width = std::atoi(size.data());
			if (const size_t x = size.find('x'); x != oc::string_view::npos)
				height = std::atoi(size.data() + x + 1);
		}
		else
			_Exit(usage());
	}

	int result = 0;
	Session session(width, height);
	static ScriptEditor editor;
	oc::string error;
	session.frame(editor); // builds the bindings
	if (!editor.loadText(document, error))
	{
		printf("error: %s\n", error.c_str());
		fflush(nullptr);
		_Exit(1);
	}
	if (!savePath.empty())
		editor.setDocumentPath(savePath);
	session.frame(editor);
	session.frame(editor); // the first frames after a load: focus settles, m_formatted is current

	if (!checkPath.empty())
	{
		result = runCheck(checkPath, editor, session);
	}
	else if (monkeyKeys > 0)
	{
		result = runMonkey(monkeyKeys, seed, editor, session);
	}
	else
	{
		if (!runProgram(program, editor, session, options))
			result = 1;
		session.frame(editor);
		printf("%s", editor.describeState(options.context).c_str());
		if (!editor.documentLoads(error))
		{
			printf("ROUND TRIP FAILED: %s\n", error.c_str());
			result = 1;
		}
		if (!options.pngPath.empty() && !session.screenshot(editor, options.pngPath))
			printf("error: cannot write %s\n", options.pngPath.c_str());
		if (!options.outPath.empty() && !FileSystem::writeFileStr(options.outPath, editor.documentText(), /*allowMainThread*/ true))
			printf("error: cannot write %s\n", options.outPath.c_str());
	}
	fflush(nullptr);
	_Exit(result); // global dtors assume the engine's teardown order, see DslCompiler
}
