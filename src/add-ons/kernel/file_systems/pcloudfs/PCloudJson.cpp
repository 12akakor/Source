/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PCloudJson.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>


static const int32 kMaxJsonDepth = 64;
static const char kArrayCountField[] = "_count";


status_t
PCloudJson::Parse(const char* text, size_t length, BMessage& into)
{
	if (text == NULL || length == 0)
		return B_BAD_VALUE;

	Parser parser(text, length);
	return parser.Parse(into);
}


status_t
PCloudJson::Parse(const BString& text, BMessage& into)
{
	return Parse(text.String(), text.Length(), into);
}


bool
PCloudJson::GetString(const BMessage& message, const char* name,
	BString& value)
{
	return message.FindString(name, &value) == B_OK;
}


bool
PCloudJson::GetInt64(const BMessage& message, const char* name, int64& value)
{
	return message.FindInt64(name, &value) == B_OK;
}


bool
PCloudJson::GetBool(const BMessage& message, const char* name, bool& value)
{
	return message.FindBool(name, &value) == B_OK;
}


bool
PCloudJson::Has(BMessage& message, const char* name)
{
	return message.HasField(name);
}


int32
PCloudJson::ArrayCount(const BMessage& array)
{
	int32 count = 0;
	if (array.FindInt32(kArrayCountField, &count) != B_OK)
		return 0;
	return count;
}


status_t
PCloudJson::ArrayItem(BMessage& array, int32 index, BMessage& item)
{
	BString name;
	name.SetToFormat("%d", index);
	type_code type;
	int32 count = 0;
	if (array.GetFieldInfo(name.String(), &type, &count) != B_OK)
		return B_BAD_VALUE;
	if (type != B_MESSAGE_TYPE)
		return B_BAD_VALUE;
	return array.FindMessage(name.String(), 0, &item);
}


// #pragma mark - Parser


PCloudJson::Parser::Parser(const char* text, size_t length)
	:
	fText(text),
	fLength(length),
	fPosition(0),
	fDepth(0)
{
}


status_t
PCloudJson::Parser::Parse(BMessage& into)
{
	_SkipWhitespace();
	if (!_ParseValue(into, NULL))
		return B_BAD_VALUE;
	_SkipWhitespace();
	if (!_AtEnd())
		return B_BAD_VALUE;
	return B_OK;
}


bool
PCloudJson::Parser::_ParseValue(BMessage& parent, const char* name)
{
	if (fDepth >= kMaxJsonDepth || _AtEnd())
		return false;

	fDepth++;
	bool result = false;
	char character = _Peek();

	if (character == '{') {
		BMessage object;
		result = _ParseObject(object);
		if (result && name != NULL)
			result = parent.AddMessage(name, &object) == B_OK;
	} else if (character == '[') {
		BMessage array;
		result = _ParseArray(array);
		if (result && name != NULL)
			result = parent.AddMessage(name, &array) == B_OK;
	} else if (character == '"') {
		BString value;
		result = _ParseString(value);
		if (result && name != NULL)
			result = parent.AddString(name, value.String()) == B_OK;
	} else if (character == '-' || isdigit((unsigned char)character)) {
		result = _ParseNumber(parent, name);
	} else {
		result = _ParseKeyword(parent, name);
	}

	fDepth--;
	return result;
}


bool
PCloudJson::Parser::_ParseObject(BMessage& into)
{
	if (!_Consume('{'))
		return false;
	_SkipWhitespace();
	if (_Consume('}'))
		return true;

	while (true) {
		_SkipWhitespace();
		BString name;
		if (!_ParseString(name))
			return false;
		_SkipWhitespace();
		if (!_Consume(':'))
			return false;
		_SkipWhitespace();
		if (!_ParseValue(into, name.String()))
			return false;
		_SkipWhitespace();

		if (_Consume('}'))
			return true;
		if (!_Consume(','))
			return false;
	}
}


bool
PCloudJson::Parser::_ParseArray(BMessage& into)
{
	if (!_Consume('['))
		return false;
	_SkipWhitespace();

	int32 count = 0;
	if (_Consume(']')) {
		into.AddInt32(kArrayCountField, 0);
		return true;
	}

	while (true) {
		BString name;
		name.SetToFormat("%d", count);
		if (!_ParseValue(into, name.String()))
			return false;
		count++;
		_SkipWhitespace();

		if (_Consume(']'))
			break;
		if (!_Consume(','))
			return false;
		_SkipWhitespace();
	}

	return into.AddInt32(kArrayCountField, count) == B_OK;
}


bool
PCloudJson::Parser::_ParseString(BString& value)
{
	if (!_Consume('"'))
		return false;

	value.Truncate(0);
	while (!_AtEnd()) {
		char character = fText[fPosition++];
		if (character == '"')
			return true;

		if ((unsigned char)character < 0x20)
			return false;

		if (character != '\\') {
			value.Append(&character, 1);
			continue;
		}

		if (_AtEnd())
			return false;
		char escape = fText[fPosition++];
		switch (escape) {
			case '"': value << '"'; break;
			case '\\': value << '\\'; break;
			case '/': value << '/'; break;
			case 'b': value << '\b'; break;
			case 'f': value << '\f'; break;
			case 'n': value << '\n'; break;
			case 'r': value << '\r'; break;
			case 't': value << '\t'; break;
			case 'u':
			{
				uint32 codePoint;
				if (!_ReadHex(codePoint))
					return false;
				if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
					// High surrogate: a low surrogate must follow.
					if (fPosition + 2 > fLength || fText[fPosition] != '\\'
						|| fText[fPosition + 1] != 'u') {
						return false;
					}
					fPosition += 2;
					uint32 low;
					if (!_ReadHex(low) || low < 0xdc00 || low > 0xdfff)
						return false;
					codePoint = 0x10000
						+ ((codePoint - 0xd800) << 10) + (low - 0xdc00);
				} else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
					return false;
				}
				if (!_AppendUtf8(value, codePoint))
					return false;
				break;
			}
			default:
				return false;
		}
	}
	return false;
}


bool
PCloudJson::Parser::_ParseNumber(BMessage& parent, const char* name)
{
	size_t start = fPosition;
	if (_Peek() == '-')
		fPosition++;
	if (_AtEnd())
		return false;
	if (_Peek() == '0') {
		fPosition++;
		if (!_AtEnd() && isdigit((unsigned char)_Peek()))
			return false;
	} else {
		if (!isdigit((unsigned char)_Peek()))
			return false;
		while (!_AtEnd() && isdigit((unsigned char)_Peek()))
			fPosition++;
	}

	bool isInteger = true;
	if (!_AtEnd() && _Peek() == '.') {
		isInteger = false;
		fPosition++;
		if (_AtEnd() || !isdigit((unsigned char)_Peek()))
			return false;
		while (!_AtEnd() && isdigit((unsigned char)_Peek()))
			fPosition++;
	}
	if (!_AtEnd() && (_Peek() == 'e' || _Peek() == 'E')) {
		isInteger = false;
		fPosition++;
		if (!_AtEnd() && (_Peek() == '+' || _Peek() == '-'))
			fPosition++;
		if (_AtEnd() || !isdigit((unsigned char)_Peek()))
			return false;
		while (!_AtEnd() && isdigit((unsigned char)_Peek()))
			fPosition++;
	}

	BString number;
	number.SetTo(fText + start, fPosition - start);
	if (name == NULL)
		return false;

	if (isInteger) {
		char* end = NULL;
		int64 value = strtoll(number.String(), &end, 10);
		if (end == NULL || *end != '\0')
			return false;
		return parent.AddInt64(name, value) == B_OK;
	}

	char* end = NULL;
	double value = strtod(number.String(), &end);
	if (end == NULL || *end != '\0')
		return false;
	return parent.AddDouble(name, value) == B_OK;
}


bool
PCloudJson::Parser::_ParseKeyword(BMessage& parent, const char* name)
{
	static const char kTrue[] = "true";
	static const char kFalse[] = "false";
	static const char kNull[] = "null";

	for (int32 keyword = 0; keyword < 3; keyword++) {
		const char* text = keyword == 0 ? kTrue
			: (keyword == 1 ? kFalse : kNull);
		size_t length = strlen(text);
		if (fPosition + length > fLength
			|| memcmp(fText + fPosition, text, length) != 0) {
			continue;
		}
		fPosition += length;
		if (name == NULL)
			return true;
		if (keyword == 2)
			return true;	// null: no field
		return parent.AddBool(name, keyword == 0) == B_OK;
	}
	return false;
}


void
PCloudJson::Parser::_SkipWhitespace()
{
	while (!_AtEnd()) {
		char character = fText[fPosition];
		if (character != ' ' && character != '\t' && character != '\r'
			&& character != '\n') {
			break;
		}
		fPosition++;
	}
}


bool
PCloudJson::Parser::_Consume(char expected)
{
	_SkipWhitespace();
	if (_AtEnd() || fText[fPosition] != expected)
		return false;
	fPosition++;
	return true;
}


bool
PCloudJson::Parser::_AtEnd() const
{
	return fPosition >= fLength;
}


char
PCloudJson::Parser::_Peek() const
{
	return _AtEnd() ? '\0' : fText[fPosition];
}


bool
PCloudJson::Parser::_ReadHex(uint32& value)
{
	if (fPosition + 4 > fLength)
		return false;
	value = 0;
	for (int32 i = 0; i < 4; i++) {
		char character = fText[fPosition++];
		value <<= 4;
		if (character >= '0' && character <= '9')
			value |= character - '0';
		else if (character >= 'a' && character <= 'f')
			value |= character - 'a' + 10;
		else if (character >= 'A' && character <= 'F')
			value |= character - 'A' + 10;
		else
			return false;
	}
	return true;
}


bool
PCloudJson::Parser::_AppendUtf8(BString& value, uint32 codePoint)
{
	char buffer[4];
	size_t length;
	if (codePoint <= 0x7f) {
		buffer[0] = (char)codePoint;
		length = 1;
	} else if (codePoint <= 0x7ff) {
		buffer[0] = (char)(0xc0 | (codePoint >> 6));
		buffer[1] = (char)(0x80 | (codePoint & 0x3f));
		length = 2;
	} else if (codePoint <= 0xffff) {
		buffer[0] = (char)(0xe0 | (codePoint >> 12));
		buffer[1] = (char)(0x80 | ((codePoint >> 6) & 0x3f));
		buffer[2] = (char)(0x80 | (codePoint & 0x3f));
		length = 3;
	} else if (codePoint <= 0x10ffff) {
		buffer[0] = (char)(0xf0 | (codePoint >> 18));
		buffer[1] = (char)(0x80 | ((codePoint >> 12) & 0x3f));
		buffer[2] = (char)(0x80 | ((codePoint >> 6) & 0x3f));
		buffer[3] = (char)(0x80 | (codePoint & 0x3f));
		length = 4;
	} else {
		return false;
	}
	value.Append(buffer, length);
	return true;
}
