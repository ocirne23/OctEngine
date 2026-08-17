export module Core.Log;

import Core;

export namespace Log
{
	enum class Level : uint8
	{
		Verbose = 0,
		Info    = 1,
		Warning = 2,
		Error   = 3,
	};

	struct Message
	{
		Level       level;
		oc::string text;
	};

	constexpr uint32 MaxMessages = 4096;

	// ---- storage ------------------------------------------------------------

	namespace detail
	{
		inline std::mutex              g_mutex;
		inline oc::deque<Message>     g_messages;
		inline uint32                  g_revision = 0;   // incremented on every add
	}

	// ---- write API ----------------------------------------------------------

	inline void log(Level level, oc::string_view text)
	{
		std::lock_guard lock(detail::g_mutex);
		if (detail::g_messages.size() >= MaxMessages)
			detail::g_messages.pop_front();
		detail::g_messages.push_back({ level, oc::string(text) });
		++detail::g_revision;
		printf("[%s] %.*s\n", level == Level::Verbose ? "V" : level == Level::Info ? "I" : level == Level::Warning ? "W" : "E",
			static_cast<int>(text.size()), text.data());
	}

	inline void verbose(oc::string_view text) { log(Level::Verbose, text); }
	inline void info   (oc::string_view text) { log(Level::Info,    text); }
	inline void warning(oc::string_view text) { log(Level::Warning, text); }
	inline void error  (oc::string_view text) { log(Level::Error,   text); }

	// ---- read API (call from UI thread only) --------------------------------

	// Returns a snapshot copy and the current revision number.
	inline uint32 getRevision()
	{
		std::lock_guard lock(detail::g_mutex);
		return detail::g_revision;
	}

	inline oc::vector<Message> getMessages()
	{
		std::lock_guard lock(detail::g_mutex);
		return { detail::g_messages.begin(), detail::g_messages.end() };
	}

	inline void clear()
	{
		std::lock_guard lock(detail::g_mutex);
		detail::g_messages.clear();
		++detail::g_revision;
	}
}
