module Procedural;

import Core;
import :Diffusion.Json;

namespace Procedural::Diffusion
{
	namespace
	{
		class Parser
		{
		public:
			Parser(oc::string_view text) : m_s(text) {}

			bool parseValue(JsonValue& out)
			{
				if (++m_depth > 64)
					return fail("nesting too deep");
				const bool ok = parseValueInner(out);
				--m_depth;
				return ok;
			}

			bool atEnd()
			{
				skipWs();
				return m_i >= m_s.size();
			}

			oc::string error;

		private:
			bool fail(oc::string_view msg)
			{
				if (error.empty())
					error = oc::string(msg) + " at offset " + oc::to_string(m_i);
				return false;
			}

			void skipWs()
			{
				while (m_i < m_s.size())
				{
					const char c = m_s[m_i];
					if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
						m_i++;
					else
						break;
				}
			}

			bool literal(oc::string_view lit)
			{
				if (m_s.substr(m_i, lit.size()) != lit)
					return fail("bad literal");
				m_i += lit.size();
				return true;
			}

			bool parseString(oc::string& out)
			{
				if (m_i >= m_s.size() || m_s[m_i] != '"')
					return fail("expected string");
				m_i++;
				out.clear();
				while (m_i < m_s.size())
				{
					const char c = m_s[m_i++];
					if (c == '"')
						return true;
					if (c != '\\')
					{
						out.push_back(c);
						continue;
					}
					if (m_i >= m_s.size())
						return fail("truncated escape");
					const char e = m_s[m_i++];
					switch (e)
					{
					case '"':  out.push_back('"');  break;
					case '\\': out.push_back('\\'); break;
					case '/':  out.push_back('/');  break;
					case 'b':  out.push_back('\b'); break;
					case 'f':  out.push_back('\f'); break;
					case 'n':  out.push_back('\n'); break;
					case 'r':  out.push_back('\r'); break;
					case 't':  out.push_back('\t'); break;
					case 'u':
					{
						// These config files are ASCII; decode the code unit but only keep the low byte
						// rather than pretending to do full UTF-16 -> UTF-8. Nothing here needs it.
						if (m_i + 4 > m_s.size())
							return fail("truncated \\u escape");
						uint32 cp = 0;
						for (int32 k = 0; k < 4; k++)
						{
							const char h = m_s[m_i + k];
							cp <<= 4;
							if (h >= '0' && h <= '9') cp |= (uint32)(h - '0');
							else if (h >= 'a' && h <= 'f') cp |= (uint32)(h - 'a' + 10);
							else if (h >= 'A' && h <= 'F') cp |= (uint32)(h - 'A' + 10);
							else return fail("bad hex in \\u escape");
						}
						m_i += 4;
						if (cp < 0x80)
							out.push_back((char)cp);
						else
							return fail("non-ASCII \\u escape is not supported");
						break;
					}
					default: return fail("bad escape");
					}
				}
				return fail("unterminated string");
			}

			bool parseNumber(JsonValue& out)
			{
				const size_t start = m_i;
				if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+'))
					m_i++;
				while (m_i < m_s.size())
				{
					const char c = m_s[m_i];
					if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
						m_i++;
					else
						break;
				}
				if (m_i == start)
					return fail("expected number");

				// from_chars is locale-independent, unlike strtod — these files use '.' regardless of the
				// user's locale, so this must not depend on it.
				double v = 0.0;
				const char* first = m_s.data() + start;
				const char* last = m_s.data() + m_i;
				const std::from_chars_result r = std::from_chars(first, last, v);
				if (r.ec != std::errc() || r.ptr != last)
					return fail("malformed number");
				out.type = JsonValue::EType::Number;
				out.number = v;
				return true;
			}

			bool parseValueInner(JsonValue& out)
			{
				skipWs();
				if (m_i >= m_s.size())
					return fail("unexpected end of input");

				switch (m_s[m_i])
				{
				case 'n':
					if (!literal("null"))
						return false;
					out.type = JsonValue::EType::Null;
					return true;
				case 't':
					if (!literal("true"))
						return false;
					out.type = JsonValue::EType::Bool;
					out.boolean = true;
					return true;
				case 'f':
					if (!literal("false"))
						return false;
					out.type = JsonValue::EType::Bool;
					out.boolean = false;
					return true;
				case '"':
					out.type = JsonValue::EType::String;
					return parseString(out.str);
				case '[':
				{
					m_i++;
					out.type = JsonValue::EType::Array;
					skipWs();
					if (m_i < m_s.size() && m_s[m_i] == ']')
					{
						m_i++;
						return true;
					}
					for (;;)
					{
						JsonValue v;
						if (!parseValue(v))
							return false;
						out.arr.push_back(oc::move(v));
						skipWs();
						if (m_i >= m_s.size())
							return fail("unterminated array");
						if (m_s[m_i] == ',')
						{
							m_i++;
							continue;
						}
						if (m_s[m_i] == ']')
						{
							m_i++;
							return true;
						}
						return fail("expected ',' or ']'");
					}
				}
				case '{':
				{
					m_i++;
					out.type = JsonValue::EType::Object;
					skipWs();
					if (m_i < m_s.size() && m_s[m_i] == '}')
					{
						m_i++;
						return true;
					}
					for (;;)
					{
						skipWs();
						oc::string key;
						if (!parseString(key))
							return false;
						skipWs();
						if (m_i >= m_s.size() || m_s[m_i] != ':')
							return fail("expected ':'");
						m_i++;
						JsonValue v;
						if (!parseValue(v))
							return false;
						out.obj.emplace_back(oc::move(key), oc::move(v));
						skipWs();
						if (m_i >= m_s.size())
							return fail("unterminated object");
						if (m_s[m_i] == ',')
						{
							m_i++;
							continue;
						}
						if (m_s[m_i] == '}')
						{
							m_i++;
							return true;
						}
						return fail("expected ',' or '}'");
					}
				}
				default:
					return parseNumber(out);
				}
			}

			oc::string_view m_s;
			size_t m_i = 0;
			int32 m_depth = 0;
		};
	}

	const JsonValue* JsonValue::find(oc::string_view key) const
	{
		if (type != EType::Object)
			return nullptr;
		for (const auto& [k, v] : obj)
			if (k == key)
				return &v;
		return nullptr;
	}

	bool JsonValue::asFloatArray(oc::vector<float>& out, int32 expectedCount) const
	{
		if (type != EType::Array)
			return false;
		if (expectedCount >= 0 && (int32)arr.size() != expectedCount)
			return false;
		out.clear();
		out.reserve(arr.size());
		for (const JsonValue& v : arr)
		{
			if (v.type != EType::Number)
				return false;
			out.push_back((float)v.number);
		}
		return true;
	}

	bool JsonValue::asFloatArray2D(oc::vector<float>& out, int32 expectedRows, int32 expectedCols) const
	{
		if (type != EType::Array || (int32)arr.size() != expectedRows)
			return false;
		out.clear();
		out.reserve((size_t)expectedRows * expectedCols);
		for (const JsonValue& row : arr)
		{
			oc::vector<float> r;
			if (!row.asFloatArray(r, expectedCols))
				return false;
			out.insert(out.end(), r.begin(), r.end());
		}
		return true;
	}

	bool JsonValue::parse(oc::string_view text, JsonValue& out, oc::string& error)
	{
		// Tolerate a UTF-8 BOM: HuggingFace-hosted json can carry one and it isn't whitespace.
		if (text.size() >= 3 && (uint8)text[0] == 0xEF && (uint8)text[1] == 0xBB && (uint8)text[2] == 0xBF)
			text.remove_prefix(3);

		Parser p(text);
		out = JsonValue{};
		if (!p.parseValue(out))
		{
			error = p.error;
			return false;
		}
		if (!p.atEnd())
		{
			error = "trailing content after top-level value";
			return false;
		}
		return true;
	}
}
