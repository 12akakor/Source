/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_JSON_H
#define _PCLOUD_JSON_H


#include <Message.h>
#include <String.h>


/*! A small strict JSON reader.

	pCloud replies are JSON. The reader converts an object into a BMessage and
	an array into a BMessage with numbered fields plus the int32 field
	"_count". Numbers that fit in an int64 are stored as int64, other numbers
	as double. Input is bounded in size and nesting depth.
*/
class PCloudJson {
public:
		static status_t	Parse(const char* text, size_t length, BMessage& into);
		static status_t	Parse(const BString& text, BMessage& into);

		static bool		GetString(const BMessage& message, const char* name,
							BString& value);
		static bool		GetInt64(const BMessage& message, const char* name,
							int64& value);
		static bool		GetBool(const BMessage& message, const char* name,
							bool& value);
		static bool		Has(BMessage& message, const char* name);
		static int32	ArrayCount(const BMessage& array);
		static status_t	ArrayItem(BMessage& array, int32 index,
							BMessage& item);

private:
		class Parser {
		public:
							Parser(const char* text, size_t length);

			status_t		Parse(BMessage& into);

		private:
			bool			_ParseValue(BMessage& parent, const char* name);
			bool			_ParseObject(BMessage& into);
			bool			_ParseArray(BMessage& into);
			bool			_ParseString(BString& value);
			bool			_ParseNumber(BMessage& parent, const char* name);
			bool			_ParseKeyword(BMessage& parent, const char* name);

			void			_SkipWhitespace();
			bool			_Consume(char expected);
			bool			_AtEnd() const;
			char			_Peek() const;
			bool			_ReadHex(uint32& value);
			bool			_AppendUtf8(BString& value, uint32 codePoint);

		private:
				const char*	fText;
				size_t		fLength;
				size_t		fPosition;
				int32		fDepth;
		};
};


#endif	// _PCLOUD_JSON_H
