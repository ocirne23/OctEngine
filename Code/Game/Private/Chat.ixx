export module Game:Chat;

import Core;
import Core.Log;
import Entity;  // NetworkManager
import Network; // NetWriter/NetReader
import UI;      // ChatView (the ChatPanel's snapshot)

// The multiplayer TEXT CHAT model: the message log every instance keeps, fed by ONE network
// event — "ChM" [string text] (reliable ch1). A sent line is fireNetworkEvent'd: the local fire
// lands it in our own log (sender = our clientId), the transport carries it to the server, which
// relays it to every other client re-attributed to the real sender (NetworkManager never trusts
// a client-written identity). Single player fires locally only, so the box still works offline.
// Lives through the lobby AND the match (the lobby's conversation stays visible in-game); reset
// on exit-to-menu. Main-thread only (NetworkManager contract); a plain stack object in main().
export class ChatSystem final
{
public:

	static constexpr size_t c_maxMessages = 100;
	static constexpr size_t c_maxTextLength = ChatPanel::c_maxTextLength;
	// The event-filter cap: varint length + text (the Game layer's Gq* filter repeats this number).
	static constexpr size_t c_maxEventBytes = 256;

	static bool handlesEvent(oc::string_view name) { return name == "ChM"; }
	// For the server-side event filters (lobby + game): a client may send chat lines this long.
	static bool allowsEvent(oc::string_view name, oc::span<const uint8> data)
	{
		return handlesEvent(name) && data.size() <= c_maxEventBytes;
	}

	void reset()
	{
		m_messages.clear();
		++m_generation;
	}

	// The line the player typed (main thread, from the ChatPanel's takeOutgoing).
	void send(oc::string_view text)
	{
		if (text.empty())
			return;
		if (text.size() > c_maxTextLength)
			text = text.substr(0, c_maxTextLength);
		uint8 buffer[c_maxEventBytes];
		NetWriter writer(buffer);
		writer.writeString(text);
		if (writer.overflowed())
			return;
		Globals::networkManager.fireNetworkEvent("ChM", writer.data()); // local fire = our own log entry
	}

	// main's dispatcher routes "ChM" here (main thread: chat events are only fired from main and
	// received in receive()).
	void handleNetEvent()
	{
		NetReader reader(Globals::networkManager.currentEventData());
		const oc::string_view text = reader.readString();
		if (reader.overflowed() || text.empty())
			return;
		Message message;
		message.clientId = Globals::networkManager.currentEventSender();
		message.text = oc::string(text.substr(0, c_maxTextLength));
		for (char& c : message.text) // the text came off the wire: keep it printable
			if ((unsigned char)c < 0x20)
				c = ' ';
		if (m_messages.size() >= c_maxMessages)
			m_messages.erase(m_messages.begin());
		m_messages.push_back(oc::move(message));
		++m_generation;
	}

	// Bumps on every log change — main pushes a fresh view only then (see main.cpp).
	uint32 generation() const { return m_generation; }

	ChatView view() const
	{
		ChatView v;
		const uint32 self = Globals::networkManager.localClientId();
		v.messages.reserve(m_messages.size());
		for (const Message& message : m_messages)
			v.messages.push_back({ message.clientId, message.clientId == self, message.text });
		return v;
	}

private:

	struct Message
	{
		uint32 clientId = 0;
		oc::string text;
	};

	oc::vector<Message> m_messages;
	uint32 m_generation = 1;
};
