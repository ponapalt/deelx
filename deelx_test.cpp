// deelx_test.cpp
//
// ===========================================================================
// NOTES FOR CONTRIBUTORS AND AI AGENTS  (serves as CLAUDE.md / AGENTS.md)
// ===========================================================================
//
// Project
//   DEELX is a backtracking regular expression engine implemented entirely in
//   the single header deelx.h (templates, no .cpp, no external dependencies).
//   Main entry points: CRegexpT<CHART> (CRegexpA = char, CRegexpW = unsigned
//   short), MatchResult, CContext. The pattern is compiled by CBuilderT into a
//   tree of ElxInterface nodes whose Match()/MatchNext() pairs implement
//   matching and backtracking via CContext::m_stack.
//
// Hard constraints
//   * deelx.h MUST keep compiling with Visual C++ 6.0 as well as modern
//     compilers (g++, clang, recent MSVC). Therefore in deelx.h:
//       - no C++11 or later (no auto, nullptr, override, range-for, etc.)
//       - no STL containers/strings, no RTTI; only C headers plus <new>
//       - the only exception leaving deelx.h is std::bad_alloc, and only from
//         deelx_realloc() / deelx_check_new() (VC6's new returns 0 instead
//         of throwing). Keep objects consistent when an allocation throws:
//         update pointers/capacities only after the allocation succeeded.
//         deelx_step_limit_exceeded is thrown by CContext::Step() and always
//         caught in CRegexpT::Match()/MatchExact()
//       - this file and deelx.h stay ASCII (VC6 and code pages); write
//         non-ASCII test text as UTF-8 \x escapes and convert with U16()
//       - no member templates or partial specialization tricks VC6 can't parse
//       - VC6 leaks for-loop variables into the enclosing scope: do not
//         declare the same loop variable twice in one scope
//       - qualify base members in templates (CBufferRefT<ELT>::m_nSize):
//         modern compilers need it, VC6 accepts it
//   * Memory in CBufferT is managed with realloc/free (via deelx_realloc),
//     strings returned by Replace() must be freed with
//     CRegexpT::ReleaseString().
//   * No function-local or global mutable statics: a compiled CRegexpT must
//     be usable from several threads at once, each with its own CContext.
//   * Never pass a negative char or a value >= 256 to <ctype.h> functions:
//     use deelx_lt256() / deelx_toupper() / deelx_isspace() helpers.
//   * Patterns and subject strings may be passed with an explicit length and
//     need NOT be NUL-terminated; never read past the given length.
//   * Keep public API and the global names (CRegexpA, MatchResult, flags such
//     as IGNORECASE) source compatible; downstream projects include deelx.h.
//     Without UNICODE_MODE, existing patterns must behave as before (\w, \d,
//     \s, \b and IGNORECASE stay ASCII). Do not name anything UNICODE: it is
//     a Windows macro.
//   * The backtracking protocol: Match() pushes its state on m_stack only
//     when it succeeds; MatchNext() pops it, and pushes again only when it
//     succeeds. An element whose char length varies (UNICODE_MODE surrogate
//     pairs) pushes the length. Elements that loop or backtrack call
//     pContext->Step() so the step limit can stop them.
//   * The Unicode tables at the end of deelx.h are generated from the UCD
//     (UnicodeData.txt, CaseFolding.txt). Regenerate them rather than edit
//     them by hand, and check every code point against the UCD afterwards.
//
// Style
//   Tabs for indentation, braces on their own line, Hungarian-ish names as in
//   the existing code (m_nXxx, m_pXxx, bXxx). Comments in English. Match the
//   surrounding code rather than reformatting it.
//
// Building and running the tests (this file must also stay VC6-compatible)
//   g++  : g++ -Wall -Wextra -o deelx_test deelx_test.cpp && ./deelx_test
//   MSVC : cl /nologo /W3 /GX deelx_test.cpp && deelx_test.exe
//   VC6  : run VCVARS32.BAT first, then the MSVC line above
//   /GX (exception handling) is required with MSVC, also for users of
//   deelx.h, because allocation failure throws std::bad_alloc.
//   The program prints each failure and exits with non-zero status if any
//   check fails. Add a regression test here for every bug fixed in deelx.h.
//
// Known limitations (not bugs to "fix" casually)
//   * CRegexpA is not MBCS aware: Shift_JIS trail bytes may match ASCII, and
//     '.' etc. match single bytes. UNICODE_MODE has no effect on it.
//   * \w, \b, \d, \s and case folding are ASCII only unless UNICODE_MODE
//     (or (?u)) is given. With it, case-insensitive literals and
//     backreferences are compared per UTF-16 code unit, so chars above
//     U+FFFF (e.g. Deseret) are not case folded there; classes do fold them.
//   * Simple case folding only: "ss" does not match U+00DF.
//   * \p{...} supports general categories and Any, Assigned, ASCII, L&/LC,
//     White_Space/Space, Word, Xan, Xsp, Xps, Xwd; no scripts or blocks.
//     Unknown names match nothing (\P{...}: any char).
//   * \v is the vertical tab char (as before), not the vertical space class.
//   * \K inside lookaround is not supported.
//   * Invalid patterns are never reported: they compile to something (e.g.
//     "a**" takes the second '*' literally, an unknown backreference fails).
//   * Recursion ((?R), (?1), ...) is limited to 100 levels per match path;
//     deeper input simply does not match.
//   * Plain backtracking: patterns like (.*a){12}c can take exponential time.
//     A step limit that grows with the text length is on by default (see
//     CRegexpT::SetStepLimit()); a call that exceeds it reports "not matched"
//     with MatchResult::IsStepLimitExceeded() set.
//   * Explicit group numbers are limited to DEELX_MAX_GROUP_NUMBER (65535,
//     overridable); larger ones are treated as group names.
// ===========================================================================

#include "deelx.h"
#include <stdio.h>
#include <string.h>
#include <algorithm> // must compile after deelx.h: it used to define max/min macros

static int g_nChecks   = 0;
static int g_nFailures = 0;

#define CHECK(expr) \
	do { \
		g_nChecks ++; \
		if( ! (expr) ) { g_nFailures ++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
	} while(0)

static int MatchA(const char * pattern, const char * text, int flags = 0)
{
	CRegexpA regexp(pattern, flags);
	return regexp.Match(text).IsMatched();
}

static int ReplaceEquals(const char * pattern, const char * text, const char * to, const char * expected, int ntimes = -1)
{
	CRegexpA regexp(pattern);
	char * result = regexp.Replace(text, to, -1, ntimes);
	int ok = result != 0 && strcmp(result, expected) == 0;
	if( ! ok ) printf("     Replace(\"%s\", \"%s\", \"%s\") = \"%s\", expected \"%s\"\n", pattern, text, to, result ? result : "(null)", expected);
	CRegexpA::ReleaseString(result);
	return ok;
}

//
// Basic behaviour (baseline, should never change)
//
static void TestBasic()
{
	CHECK(  MatchA("abc", "xxabcxx") );
	CHECK( !MatchA("abd", "xxabcxx") );
	CHECK(  MatchA("^a.c$", "abc") );
	CHECK(  MatchA("ABC", "abc", IGNORECASE) );
	CHECK(  MatchA("[a-c]+x", "zzcbax") );
	CHECK(  MatchA("[[:digit:]]{3}", "ab123") );
	CHECK(  MatchA("(?<=a)b", "ab") );
	CHECK( !MatchA("(?<!a)b", "ab") );
	CHECK(  MatchA("a(?=b)", "ab") );
	CHECK(  MatchA("(a)\\1", "aa") );
	CHECK(  MatchA("(?<x>a)\\k<x>", "aa") );
	CHECK(  MatchA("^(a)?(?(1)b|c)$", "ab") );
	CHECK(  MatchA("^(a)?(?(1)b|c)$", "c") );
	CHECK(  MatchA("^a*?b", "aaab") );
	CHECK(  MatchA("^a*+b", "aaab") );
	CHECK( !MatchA("^a*+a", "aaaa") );
	CHECK(  MatchA("\\x41", "A") );
	CHECK(  MatchA("\\u0041", "A") );
	CHECK(  MatchA("\\u{41}", "A") );
	CHECK(  MatchA("a(?#comment)b", "ab") );

	{
		CRegexpA regexp("(\\d+)-(?<tail>\\d+)");
		MatchResult result = regexp.Match("tel 12-345");
		CHECK( result.IsMatched() );
		CHECK( result.GetStart() == 4 && result.GetEnd() == 10 );
		CHECK( result.GetGroupStart(1) == 4 && result.GetGroupEnd(1) == 6 );

		int ntail = regexp.GetNamedGroupNumber("tail");
		CHECK( ntail == 2 );
		CHECK( result.GetGroupStart(ntail) == 7 );
		CHECK( strcmp(regexp.GetNamedGroupName(ntail), "tail") == 0 );
	}

	CHECK( ReplaceEquals("a", "banana", "o", "bonono") );
	CHECK( ReplaceEquals("(\\w+)@(\\w+)", "me@host", "$2 at $1", "host at me") );
	CHECK( ReplaceEquals("(?<u>\\w+)@", "me@", "${u}!", "me!") );
	CHECK( ReplaceEquals("b", "abc", "[$`|$&|$']", "a[a|b|c]c") );
	CHECK( ReplaceEquals("x", "axa", "$$", "a$a") );
	CHECK( ReplaceEquals("a", "aaa", "b", "baa", 1) );
}

//
// Regression tests for fixed bugs
//
static void TestQuantifierDigits()
{
	// ReadDec used to read at most sizeof(CHART)*3 digits: {1000} became {100}
	char text[1001];
	memset(text, 'a', 1000);
	text[1000] = 0;

	CRegexpA regexp("^a{1000}$");
	CHECK( regexp.Match(text).IsMatched() );

	text[100] = 0;
	CHECK( ! regexp.Match(text).IsMatched() );

	// explicit group number with more than 3 digits
	CRegexpA numbered("(?<1234>x)");
	MatchResult result = numbered.Match("x");
	CHECK( numbered.GetNamedGroupNumber("1234") < 0 );
	CHECK( result.MaxGroupNumber() == 1234 );
	CHECK( result.GetGroupStart(1234) == 0 );
}

static void TestSelfAssignment()
{
	CRegexpA regexp("b");
	MatchResult result = regexp.Match("abc");
	MatchResult & alias = result;
	result = alias;
	CHECK( result.IsMatched() );
	CHECK( result.GetStart() == 1 );
}

static void TestReplaceResultWithLimit()
{
	// with ntimes reached, result info used to be appended to the last match
	CRegexpA regexp("a");
	MatchResult result;
	char * replaced = regexp.Replace("aXaXa", "bb", -1, 1, &result);
	CHECK( strcmp(replaced, "bbXaXa") == 0 );
	CHECK( result.IsMatched()      == 6 ); // result length
	CHECK( result.MaxGroupNumber() == 1 ); // times replaced
	CHECK( result.GetStart() == 0 && result.GetEnd() == 2 );
	CRegexpA::ReleaseString(replaced);
}

static void TestUnterminatedRemark()
{
	// the token before an unterminated "(?#" used to be duplicated
	CHECK(  MatchA("^ab(?#xyz", "ab") );
	CHECK(  MatchA("^ab(?#xyz", "abb") ); // no '$': still a prefix match
	CHECK( !MatchA("^ab(?#xyz", "a") );
	CRegexpA regexp("^ab(?#xyz");
	CHECK( regexp.MatchExact("ab").IsMatched() );
	CHECK( ! regexp.MatchExact("abb").IsMatched() );
}

static void TestLengthBoundedInput()
{
	// replacement "$12" passed with length 2 must be read as "$1"
	{
		CRegexpA regexp("(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)");
		int length = 0;
		char * replaced = regexp.Replace("abcdefghijkl", 12, "$12", 2, length);
		CHECK( replaced != 0 && strcmp(replaced, "a") == 0 );
		CHECK( length == 1 );
		CRegexpA::ReleaseString(replaced);
	}

	// pattern "\x4" passed with length 3 out of "\x41" must not read the '1'
	{
		CRegexpA regexp("\\x41", 3, 0);
		CHECK(   regexp.Match("\x04").IsMatched() );
		CHECK( ! regexp.Match("A"   ).IsMatched() );
	}
}

static void TestRecompileClearsNames()
{
	// m_namedlist used to keep dangling pointers after Compile(0)
	CRegexpA regexp("(?<name>a)");
	CHECK( regexp.GetNamedGroupNumber("name") == 1 );

	regexp.Compile(0);
	CHECK( regexp.GetNamedGroupNumber("name") < 0 );
	CHECK( ! regexp.Match("a").IsMatched() );

	regexp.Compile("(?<other>b)");
	CHECK( regexp.GetNamedGroupNumber("name") < 0 );
	CHECK( regexp.GetNamedGroupNumber("other") == 1 );
}

static void TestBalancingOutOfRange()
{
	// balancing to a group number beyond max group used to read out of range
	CHECK( ! MatchA("(?<a-99>x)", "x") );
	CHECK( ! MatchA("(?<-99>x)",  "x") );
}

static void TestBufferCopy()
{
	CBufferT <int> a;
	a.Push(1); a.Push(2); a.Push(3);

	CBufferT <int> b(a);
	CBufferT <int> c;
	c = a;
	a[0] = 100;

	CHECK( b.GetSize() == 3 && b[0] == 1 && b[2] == 3 );
	CHECK( c.GetSize() == 3 && c[0] == 1 && c[2] == 3 );

	CBufferT <int> & self = c; // not "c = c", which clang warns about
	c = self;
	CHECK( c.GetSize() == 3 && c[1] == 2 );

	CBufferT <int> empty, copied(empty);
	CHECK( copied.GetSize() == 0 );

	// CContext copies deeply too (no double free at scope exit)
	CContext ctx1;
	ctx1.m_stack.Push(7);
	CContext ctx2(ctx1);
	ctx1.m_stack[0] = 8;
	CHECK( ctx2.m_stack[0] == 7 );
}

static void TestBufferInsertAndPrepare()
{
	// PrepareInsert past the end used to allocate too little
	CBufferT <int> a;
	int values[3] = { 7, 8, 9 };
	a.Insert(10, values, 3);
	CHECK( a.GetSize() == 13 );
	CHECK( a[10] == 7 && a[12] == 9 );

	// Prepare used memset, wrong for fill values other than 0 and -1
	CBufferT <int> b;
	b.Prepare(4, 1);
	CHECK( b.GetSize() == 5 && b[0] == 1 && b[4] == 1 );

	// Pop(buffer) used to read the size through an int* cast
	CBufferT <char> stack, popped;
	CBufferT <char> data("xyz");
	stack.Push(data);
	CHECK( stack.Pop(popped) );
	CHECK( popped.GetSize() == 3 && popped[0] == 'x' && popped[2] == 'z' );
}

static void TestNonAsciiChars()
{
	// bytes >= 0x80 are negative chars: must not reach toupper()/isspace()
	const char sjis[] = "\x82\xa0\x82\xa2"; // Shift_JIS "aiu" hiragana
	CHECK( MatchA(sjis, sjis, IGNORECASE) );
	CHECK( MatchA("\\U\x82\xa0" "a\\E", "\x82\xa0" "A") );
	CHECK( MatchA("(?R \x82)|x", "x") );

	// wide chars >= 256 (wchar_t is not always 16-bit, so spell them out)
	const unsigned short wpattern[] = { 0x3042, 0 };
	const unsigned short wtext   [] = { 'a', 0x3042, 0 };
	CRegexpW wregexp(wpattern, IGNORECASE);
	CHECK( wregexp.Match(wtext).GetStart() == 1 );
}

static void TestReplaceUnmatchedGroup()
{
	CHECK( ReplaceEquals("(a)|b", "b", "[$1]", "[]") );
	CHECK( ReplaceEquals("(?<n>a)|b", "b", "[${n}]", "[]") );
}

static void TestReplaceTokens()
{
	// replacement strings are parsed by FindReplaceToken (no static regexp)
	CHECK( ReplaceEquals("(a)", "a", "$", "$") );
	CHECK( ReplaceEquals("(a)", "a", "x$", "x$") );
	CHECK( ReplaceEquals("(a)", "a", "$x", "$x") );
	CHECK( ReplaceEquals("(a)", "a", "$0$1", "aa") );
	CHECK( ReplaceEquals("(a)", "a", "$12", "a2") );     // only group 1 exists
	CHECK( ReplaceEquals("(a)", "a", "$9", "$9") );      // no such group
	CHECK( ReplaceEquals("(a)", "a", "$$1", "$1") );
	CHECK( ReplaceEquals("(a)", "a", "${}", "${}") );
	CHECK( ReplaceEquals("(a)", "a", "${zz}", "${zz}") ); // unknown name
	CHECK( ReplaceEquals("(a)", "a", "${a\n}", "${a\n}") );
	CHECK( ReplaceEquals("(a)", "a", "${", "${") );
	CHECK( ReplaceEquals("(?<n>a)", "a", "<${n}${n}>", "<aa>") );
	CHECK( ReplaceEquals("(?<n>a)", "a", "${x${n}", "${x${n}") ); // name is "x${n"
	CHECK( ReplaceEquals("b", "abc", "$_", "aabcc") );
	CHECK( ReplaceEquals("(a)(b)?", "a", "$+", "a") );
}

static void TestAsciiClass()
{
	CHECK(  MatchA("^[[:ascii:]]+$", "abc~") );
	CHECK( !MatchA("[[:ascii:]]", "\x82\xa0") );
	CHECK(  MatchA("[[:^ascii:]]", "a\x82") );
}

// Compilers must not see through the huge allocation: clang removes an
// allocation whose result is unused and assumes it succeeded (so nothing is
// thrown), and g++ warns about a constant size that large.
static void * volatile g_pAllocSink = 0;
static volatile size_t g_nHugeSize  = (size_t)-1;

static void TestAllocationFailure()
{
	int thrown = 0;

	try { g_pAllocSink = deelx_realloc(0, g_nHugeSize); }
	catch(std::bad_alloc &) { thrown = 1; }
	CHECK( thrown );
	free(g_pAllocSink);
	g_pAllocSink = 0;

	thrown = 0;
	try { deelx_check_new((int *)0); }
	catch(std::bad_alloc &) { thrown = 1; }
	CHECK( thrown );

	// zero-size realloc is not a failure
	thrown = 0;
	try { free(deelx_realloc(0, 0)); }
	catch(std::bad_alloc &) { thrown = 1; }
	CHECK( ! thrown );
}

static void TestGroupNumberLimit()
{
	// huge explicit numbers used to overflow per-group arrays and crash
	CRegexpA a("(?<2147483647>x)");
	CHECK( a.Match("x").IsMatched() );
	CHECK( a.GetNamedGroupNumber("2147483647") == 1 ); // taken as a name

	CHECK( MatchA("(?<a-2147483647>x)|y", "y") );
	CHECK( ! MatchA("\\k<99999999999>", "x") );

	CRegexpA b("(?<65535>x)");
	MatchResult result = b.Match("x");
	CHECK( result.MaxGroupNumber() == 65535 );
	CHECK( result.GetGroupStart(65535) == 0 );

	CRegexpA c("(?<65536>x)");
	CHECK( c.GetNamedGroupNumber("65536") == 1 );
}

static void TestMatchExactBacktracking()
{
	// MatchExact used to give up after two consecutive empty attempts
	CRegexpA a("(?:|a)(?:||)");
	CHECK( a.MatchExact("a").IsMatched() );

	CRegexpA b("(a)\\1|(?:||)|aa*");
	CHECK( b.MatchExact("a").IsMatched() );
	CHECK( b.MatchExact("").IsMatched() );
	CHECK( ! b.MatchExact("b").IsMatched() );
}

static void TestSpaceClass()
{
	// \s lacked \f and \v left-to-right, but had them right-to-left
	CHECK(  MatchA("^\\s+$", " \t\r\n\f\v") );
	CHECK(  MatchA("^\\s+$", " \t\r\n\f\v", RIGHTTOLEFT) );
	CHECK(  MatchA("(?<=\\s)x", "\fx") );
	CHECK( !MatchA("\\S", "\f\v") );
}

static void TestInvalidQuantifierIsLiteral()
{
	// a '{' not starting {n}, {n,} or {n,m} used to make the atom vanish
	CHECK(  MatchA("^a{,5}$", "a{,5}") );
	CHECK( !MatchA("^a{,5}$", "") );
	CHECK(  MatchA("^x{abc}$", "x{abc}") );
	CHECK(  MatchA("^x{1$", "x{1") );
	CHECK(  MatchA("^x{1,2,3}$", "x{1,2,3}") );
	CHECK(  MatchA("^{$", "{") );

	// valid forms still work
	CHECK(  MatchA("^a{2}$", "aa") );
	CHECK(  MatchA("^a{2,}$", "aaaa") );
	CHECK(  MatchA("^a{1,2}$", "aa") );
	CHECK( !MatchA("^a{1,2}$", "aaa") );
	CHECK(  MatchA("^a{ 1 , 2 }$", "aa") );
}

static void TestNegativeGroupNumber()
{
	// GetGroupStart(-3) used to read before the result buffer
	CRegexpA regexp("(a)");
	MatchResult result = regexp.Match("a");
	int n = regexp.GetNamedGroupNumber("nosuchname");
	CHECK( n < 0 );
	CHECK( result.GetGroupStart(n) == -1 );
	CHECK( result.GetGroupEnd(n) == -1 );
}

static void TestUnresolvedRecursion()
{
	// recursion to a missing group used to match the empty string
	CHECK( !MatchA("^(?R99)$", "") );
	CHECK( !MatchA("^(?R<nosuchname>)x$", "x") );
	CHECK(  MatchA("^(?R<nosuchname>)x$|y", "y") );
	CHECK(  MatchA("^(\\((?1)*\\))$", "(()())") );
}

static void TestExtendedCharset()
{
	// in EXTENDED mode, white space and '#' inside [...] are literal
	CHECK(  MatchA("^[ ]$", " ", EXTENDED) );
	CHECK(  MatchA("^[#]$", "#", EXTENDED) );
	CHECK(  MatchA("^a b # comment\n c$", "abc", EXTENDED) );
	CHECK( !MatchA("^a b$", "a b", EXTENDED) );
}

static void TestContextInitialized()
{
	CContext ctx;
	CHECK( ctx.m_nCurrentPos == 0 && ctx.m_pMatchString == 0 && ctx.m_pMatchStringLength == 0 );
}

// widen an ASCII string for CRegexpW (at most 63 chars)
static const unsigned short * Wide(const char * s, unsigned short * buf)
{
	int i;
	for(i=0; s[i] != 0 && i < 63; i++) buf[i] = (unsigned char)s[i];
	buf[i] = 0;
	return buf;
}

static int WideEquals(const unsigned short * w, const char * s)
{
	int i;
	for(i=0; s[i] != 0; i++)
		if(w[i] != (unsigned char)s[i]) return 0;
	return w[i] == 0;
}

static void TestMinMaxNotMacros()
{
	// deelx.h defined max(a, b) and min(a, b) macros, which broke <algorithm>
#if defined(max) || defined(min)
	CHECK( ! "max or min is defined as a macro" );
#endif

	int k = 0;
	CHECK( deelx_max(3, 5) == 5 && deelx_max(5, 3) == 5 );
	CHECK( deelx_min(3, 5) == 3 && deelx_min(5, 3) == 3 );
	CHECK( deelx_max(k++, -1) == 0 && k == 1 ); // argument evaluated once

	// deelx_max/deelx_min clip ranges to A-Z/a-z when IGNORECASE adds the other case
	CHECK(  MatchA("^[a-f]$", "D", IGNORECASE) );
	CHECK( !MatchA("^[a-f]$", "G", IGNORECASE) );
	CHECK(  MatchA("^[D-K]$", "h", IGNORECASE) );
	CHECK(  MatchA("^[A-c]$", "x", IGNORECASE) );
	CHECK(  MatchA("^[X-c]$", "B", IGNORECASE) );
	CHECK( !MatchA("^[X-c]$", "d", IGNORECASE) );

	unsigned short p[64], t[64];
	CRegexpW regexp(Wide("^[d-k]+$", p), IGNORECASE);
	CHECK(  regexp.Match(Wide("DeFk", t)).IsMatched() );
	CHECK( !regexp.Match(Wide("DeFl", t)).IsMatched() );
}

static void TestNamedGroupName()
{
	{
		CRegexpA regexp("(\\d+)-(?<tail>\\d+)");
		CHECK( strcmp(regexp.GetNamedGroupName(2), "tail") == 0 );
		CHECK( strcmp(regexp.GetNamedGroupName(1), "") == 0 ); // unnamed
		CHECK( strcmp(regexp.GetNamedGroupName(0), "") == 0 );
		CHECK( strcmp(regexp.GetNamedGroupName(9), "") == 0 ); // no such group
		CHECK( strcmp(regexp.GetNamedGroupName(-1), "") == 0 );
	}
	{
		// named groups are numbered after the unnamed ones, ignoring explicit
		// numbers, so "n" gets 1 too. (?<1>...) and (?<2>...) are kept in the
		// same list without a name and must not hide "n".
		CRegexpA regexp("(?<2>a)(?<1>b)(?<n>c)");
		CHECK( regexp.GetNamedGroupNumber("n") == 1 );
		CHECK( strcmp(regexp.GetNamedGroupName(1), "n") == 0 );
		CHECK( strcmp(regexp.GetNamedGroupName(2), "") == 0 );
	}
	{
		CRegexpA regexp("(?'q'a)(?P<p>b)(?<q>c)"); // all name syntaxes, a repeated name
		CHECK( strcmp(regexp.GetNamedGroupName(1), "q") == 0 );
		CHECK( strcmp(regexp.GetNamedGroupName(2), "p") == 0 );
	}
	{
		CRegexpA regexp("(?<old>a)");
		regexp.Compile("(?<new>a)");
		CHECK( strcmp(regexp.GetNamedGroupName(1), "new") == 0 );
		regexp.Compile(0);
		CHECK( strcmp(regexp.GetNamedGroupName(1), "") == 0 );
	}
	{
		unsigned short p[64];
		CRegexpW regexp(Wide("(?<wide>a)", p));
		CHECK( WideEquals(regexp.GetNamedGroupName(1), "wide") );
		CHECK( WideEquals(regexp.GetNamedGroupName(2), "") );
	}
}

static void TestSortedBufferFind()
{
	CSortedBufferT <int> sorted;
	sorted.Add(30); sorted.Add(10); sorted.Add(20);

	CHECK( sorted.Find(10) == 0 );
	CHECK( sorted.Find(20) == 1 );
	CHECK( sorted.Find(30) == 2 );
	CHECK( sorted.Find(15) == -1 );
}

static void TestReplaceSegmentLengths()
{
	// Replace stores segment lengths in pointer slots; check both directions
	// and that every segment (before, replacement, after) keeps its length
	CRegexpA ltr("n"), rtl("n", RIGHTTOLEFT);
	char * s;

	s = ltr.Replace("banana", "NN");
	CHECK( s != 0 && strcmp(s, "baNNaNNa") == 0 ); CRegexpA::ReleaseString(s);
	s = rtl.Replace("banana", "NN");
	CHECK( s != 0 && strcmp(s, "baNNaNNa") == 0 ); CRegexpA::ReleaseString(s);
	s = rtl.Replace("banana", "N", -1, 1);
	CHECK( s != 0 && strcmp(s, "banaNa") == 0 ); CRegexpA::ReleaseString(s);

	CRegexpA rtlgroup("(\\w+)@(\\w+)", RIGHTTOLEFT);
	s = rtlgroup.Replace("me@host x@y", "$2 at $1", -1, 1);
	CHECK( s != 0 && strcmp(s, "me@host y at x") == 0 ); CRegexpA::ReleaseString(s);

	CRegexpA empty("");
	s = empty.Replace("ab", "-");
	CHECK( s != 0 && strcmp(s, "-a-b-") == 0 ); CRegexpA::ReleaseString(s);

	unsigned short p[64], t[64], to[64];
	CRegexpW wide(Wide("(\\w)(\\d)", p));
	unsigned short * ws = wide.Replace(Wide("a1 b2 c", t), Wide("<$2$1>", to));
	CHECK( ws != 0 && WideEquals(ws, "<1a> <2b> c") );
	CRegexpW::ReleaseString(ws);
}

//
// UTF-8 (written with \x escapes, so this file stays ASCII) to UTF-16
//
static const unsigned short * U16(const char * s, unsigned short * buf)
{
	const unsigned char * p = (const unsigned char *)s;
	int n = 0;

	while(*p != 0 && n < 60)
	{
		unsigned int cp;

		if     (p[0] < 0x80) { cp = p[0]; p += 1; }
		else if(p[0] < 0xE0) { cp = ((p[0] & 0x1F) <<  6) |  (p[1] & 0x3F); p += 2; }
		else if(p[0] < 0xF0) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) <<  6) |  (p[2] & 0x3F); p += 3; }
		else                 { cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4; }

		if(cp >= 0x10000)
		{
			buf[n++] = (unsigned short)(0xD800 + ((cp - 0x10000) >> 10));
			buf[n++] = (unsigned short)(0xDC00 + ((cp - 0x10000) & 0x3FF));
		}
		else
			buf[n++] = (unsigned short)cp;
	}

	buf[n] = 0;
	return buf;
}

// pattern is ASCII (use \u{...}), text is UTF-8
static MatchResult MatchW(const char * pattern, const char * text, int flags = 0)
{
	unsigned short p[64], t[64];
	CRegexpW regexp(Wide(pattern, p), flags);
	return regexp.Match(U16(text, t));
}

static int MatchSpanW(const char * pattern, const char * text, int flags, int start, int end)
{
	MatchResult result = MatchW(pattern, text, flags);
	return result.IsMatched() && result.GetStart() == start && result.GetEnd() == end;
}

// some chars as UTF-8
#define E_ACUTE   "\xc3\xa9"         // U+00E9
#define E_ACUTE_U "\xc3\x89"         // U+00C9
#define HIRA_A    "\xe3\x81\x82"     // U+3042
#define FW_ONE    "\xef\xbc\x91"     // U+FF11 fullwidth digit one
#define IDEO_SP   "\xe3\x80\x80"     // U+3000 ideographic space
#define CYR_DE    "\xd0\xb4"         // U+0434
#define CYR_DE_U  "\xd0\x94"         // U+0414
#define SIGMA     "\xcf\x83"         // U+03C3
#define SIGMA_F   "\xcf\x82"         // U+03C2 final sigma
#define SIGMA_U   "\xce\xa3"         // U+03A3
#define KELVIN    "\xe2\x84\xaa"     // U+212A, folds to 'k'
#define ARABIC_3  "\xd9\xa3"         // U+0663 Arabic-Indic digit three
#define LINE_SEP  "\xe2\x80\xa8"     // U+2028
#define UNASSIGN  "\xcd\xb8"         // U+0378, unassigned
#define GRIN      "\xf0\x9f\x98\x80" // U+1F600, a surrogate pair in UTF-16
#define GRIN2     "\xf0\x9f\x98\x81" // U+1F601

static void TestStepLimit()
{
	// (.*a){12}c takes exponential time on "aaa...ab"
	char text[64];
	memset(text, 'a', 30);
	text[30] = 'b';
	text[31] = 0;

	CRegexpA regexp("(.*a){12}c");
	CHECK( regexp.GetStepLimit() == CRegexpA::STEP_LIMIT_AUTO );
	regexp.SetStepLimit(100000);
	CHECK( regexp.GetStepLimit() == 100000 );

	MatchResult result = regexp.Match(text);
	CHECK( !result.IsMatched() && result.IsStepLimitExceeded() );

	result = regexp.MatchExact(text);
	CHECK( !result.IsMatched() && result.IsStepLimitExceeded() );

	// a copy keeps the flag
	MatchResult copy = result;
	CHECK( copy.IsStepLimitExceeded() );

	// the context is spent: matching with it again finds nothing
	CContext * pContext = regexp.PrepareMatch(text);
	CHECK( regexp.Match(pContext).IsStepLimitExceeded() );
	result = regexp.Match(pContext);
	CHECK( !result.IsMatched() && !result.IsStepLimitExceeded() );
	CRegexpA::ReleaseContext(pContext);

	// Replace gives up too and returns the text unchanged
	MatchResult replaced;
	char * s = regexp.Replace(text, "x", -1, -1, &replaced);
	CHECK( s != 0 && strcmp(s, text) == 0 && replaced.IsStepLimitExceeded() );
	CRegexpA::ReleaseString(s);

	// cheap matches are not affected, and the counter starts again per Match
	CRegexpA simple("(a|b)+c");
	simple.SetStepLimit(1000);
	CHECK( simple.Match("ababc").IsMatched() );
	CHECK( !simple.Match("ababc").IsStepLimitExceeded() );
	s = simple.Replace("abc abc abc", "x", -1, -1, &replaced);
	CHECK( s != 0 && strcmp(s, "x x x") == 0 && !replaced.IsStepLimitExceeded() );
	CRegexpA::ReleaseString(s);

	// 0: no limit, negative: automatic
	simple.SetStepLimit(0);
	CHECK( simple.GetStepLimit() == CRegexpA::STEP_LIMIT_NONE );
	CHECK( simple.GetEffectiveStepLimit(100) == 0 );
	simple.SetStepLimit(-5);
	CHECK( simple.GetStepLimit() == CRegexpA::STEP_LIMIT_AUTO );

	// recursion counts too
	CRegexpA recursive("^(a|(?R)a)*$");
	recursive.SetStepLimit(1000);
	CHECK( recursive.Match("aaaaaaaaaaaaaaaaaaaab").IsStepLimitExceeded() );
}

static void TestAutoStepLimit()
{
	// on by default, and it grows with the text length
	CRegexpA regexp("(.*a){12}c");
	int small = regexp.GetEffectiveStepLimit(10);
	int large = regexp.GetEffectiveStepLimit(1000000);
	CHECK( small >= CRegexpA::STEP_LIMIT_AUTO_BASE && large > small );
	CHECK( regexp.GetEffectiveStepLimit(INT_MAX) == INT_MAX );
	CHECK( regexp.GetEffectiveStepLimit(-1) == small - 10 * CRegexpA::STEP_LIMIT_AUTO_PER_CHAR );

	// runaway backtracking is stopped without any setting
	char text[64];
	memset(text, 'a', 40);
	text[40] = 'b';
	text[41] = 0;
	CHECK( regexp.Match(text).IsStepLimitExceeded() );
	CHECK( regexp.MatchExact(text).IsStepLimitExceeded() );

	// an explicit limit and "no limit" win over the automatic one
	regexp.SetStepLimit(500);
	CHECK( regexp.GetEffectiveStepLimit(1000000) == 500 );
	regexp.SetStepLimit(0);
	CHECK( regexp.GetEffectiveStepLimit(1000000) == 0 );

	// scanning a long text for something that is not there is not a runaway
	// (short words: one long run of word characters would make \w+@ quadratic)
	const char unit[] = "the quick brown fox jumps over the lazy dog\n";
	int n = 4000000;
	char * big = (char *)malloc(n + 1);
	CHECK( big != 0 );
	if(big)
	{
		for(int i = 0; i < n; i ++) big[i] = unit[i % (sizeof(unit) - 1)];
		big[n] = 0;
		CRegexpA alt("(foo|bar)baz");
		MatchResult result = alt.Match(big);
		CHECK( !result.IsMatched() && !result.IsStepLimitExceeded() );
		CRegexpA word("\\w+@\\w+\\.com");
		result = word.Match(big);
		CHECK( !result.IsMatched() && !result.IsStepLimitExceeded() );
		CRegexpA lines("^.*zzz$", MULTILINE);
		result = lines.Match(big);
		CHECK( !result.IsMatched() && !result.IsStepLimitExceeded() );
		free(big);
	}

	// quadratic on one long run: \w+@ retries from every character of it
	n = 200000;
	big = (char *)malloc(n + 1);
	CHECK( big != 0 );
	if(big)
	{
		memset(big, 'x', n);
		big[n] = 0;
		CRegexpA email("\\w+@");
		CHECK( email.Match(big).IsStepLimitExceeded() );
		free(big);
	}
}

static void TestInlineExtended()
{
	CHECK(  MatchA("^(?x) a b c $", "abc") );
	CHECK( !MatchA("^(?x) a b c $", "a b c") );
	CHECK(  MatchA("^(?x)a # comment\n b$", "ab") );
	CHECK(  MatchA("^(?x)a +$", "aaa") );                 // white space before a quantifier
	CHECK(  MatchA("^a(?x: b c )d$", "abcd") );
	CHECK(  MatchA("^(?x: b ) c$", "b c") );              // ends with the group
	CHECK( !MatchA("^(?x: b ) c$", "bc") );
	CHECK(  MatchA("^(a(?x) b) c$", "ab c") );            // (?x) ends with its group
	CHECK( !MatchA("^(a(?x) b) c$", "abc") );
	CHECK(  MatchA("^a(?-x) b$", "a b", EXTENDED) );
	CHECK(  MatchA("^(?x)[ ]a$", " a") );                 // literal in [...]
	CHECK(  MatchA("^(?x)a(?-x) b$", "a b") );
	CHECK(  MatchA("^(?x)a{2} b$", "aab") );
	CHECK(  MatchA("^(?x: a | b )$", "b") );
	CHECK(  MatchA("^(?(?=a)(?x) a b| c d)$", "ab") );    // yes branch only
	CHECK(  MatchA("^(?(?=a)(?x) a b| c d)$", " c d") );
}

static void TestHorizontalSpace()
{
	CHECK(  MatchA("^\\h+$", " \t") );
	CHECK( !MatchA("\\h", "\n\r\v\f") );
	CHECK(  MatchA("^\\H+$", "ab\n") );
	CHECK(  MatchA("^[\\hx]+$", " x\t") );
	CHECK( !MatchA("\\h", "\xa0") );                     // CRegexpA: 0xA0 is a byte of a multibyte char
	CHECK(  MatchW("^\\h$", IDEO_SP).IsMatched() );
	CHECK(  MatchW("^\\h$", "\xc2\xa0").IsMatched() );
	CHECK( !MatchW("\\H", IDEO_SP).IsMatched() );
	CHECK(  MatchA("^h\\h$", "h ", RIGHTTOLEFT) );
}

static void TestLineBreak()
{
	CHECK(  MatchA("^a\\Rb$", "a\r\nb") );
	CHECK(  MatchA("^a\\Rb$", "a\nb") );
	CHECK(  MatchA("^a\\Rb$", "a\rb") );
	CHECK(  MatchA("^a\\Rb$", "a\fb") );
	CHECK( !MatchA("^a\\R\\nb$", "a\r\nb") );           // atomic, as in Perl
	CHECK(  MatchA("^a\\R{2}b$", "a\n\r\nb") );
	CHECK(  MatchA("^a\\Rb$", "a\r\nb", RIGHTTOLEFT) );
	CHECK(  MatchA("^[\\R]$", "R") );                    // literal in [...]
	CHECK(  MatchW("^a\\Rb$", "a" LINE_SEP "b").IsMatched() );
}

static void TestKeep()
{
	CRegexpA regexp("foo=\\Kbar");
	MatchResult result = regexp.Match("x foo=bar");
	CHECK( result.IsMatched() && result.GetStart() == 6 && result.GetEnd() == 9 );

	CHECK( ReplaceEquals("foo=\\Kbar", "foo=bar", "X", "foo=X") );
	CHECK( ReplaceEquals("a\\K", "aaa", "-", "a-a-a-") );
	CHECK( ReplaceEquals("\\d+\\K(?=px)", "10px 2px", "!", "10!px 2!px") );

	// undone by backtracking
	CRegexpA back("a\\Kb|ac");
	result = back.Match("ac");
	CHECK( result.IsMatched() && result.GetStart() == 0 && result.GetEnd() == 2 );

	CHECK(  MatchA("^[\\K]$", "K") );                    // literal in [...]

	CRegexpA exact("a\\Kb");
	result = exact.MatchExact("ab");
	CHECK( result.IsMatched() && result.GetStart() == 1 && result.GetEnd() == 2 );
}

static void TestBranchReset()
{
	CHECK(  MatchA("^(?|(a)|(b))\\1$", "aa") );
	CHECK(  MatchA("^(?|(a)|(b))\\1$", "bb") );
	CHECK( !MatchA("^(?|(a)|(b))\\1$", "ab") );

	// the group after continues from the largest number
	CRegexpA regexp("(?|(a)|(b)(c))(d)");
	MatchResult result = regexp.Match("bcd");
	CHECK( result.IsMatched() && result.MaxGroupNumber() == 3 );
	CHECK( result.GetGroupStart(1) == 0 && result.GetGroupStart(2) == 1 && result.GetGroupStart(3) == 2 );

	result = regexp.Match("ad");
	CHECK( result.IsMatched() && result.GetGroupStart(1) == 0 && result.GetGroupStart(2) == -1 && result.GetGroupStart(3) == 1 );

	// a subroutine call goes to the first group with the number
	CHECK(  MatchA("^(?|(a)|(b))(?1)$", "ba") );
	CHECK( !MatchA("^(?|(a)|(b))(?1)$", "bb") );

	CHECK(  MatchA("^(?|)x$", "x") );
	CHECK(  ReplaceEquals("(?|(\\d+)|([a-z]+))", "12 ab", "<$1>", "<12> <ab>") );
}

static void TestUnicodeProperty()
{
	// \p{...} works without UNICODE_MODE too
	CHECK(  MatchW("^\\p{L}$", "a").IsMatched() );
	CHECK(  MatchW("^\\pL$", HIRA_A).IsMatched() );
	CHECK( !MatchW("\\p{L}", "1").IsMatched() );
	CHECK(  MatchW("^\\P{L}$", "1").IsMatched() );
	CHECK(  MatchW("^\\p{^L}$", "1").IsMatched() );
	CHECK( !MatchW("\\P{^L}", "1").IsMatched() );
	CHECK(  MatchW("^\\p{Lu}$", "A").IsMatched() );
	CHECK( !MatchW("\\p{Lu}", "a").IsMatched() );
	CHECK(  MatchW("^\\p{Lu}$", E_ACUTE_U).IsMatched() );
	CHECK(  MatchW("^\\p{Nd}$", ARABIC_3).IsMatched() );
	CHECK(  MatchW("^\\p{N}$", FW_ONE).IsMatched() );
	CHECK(  MatchW("^\\p{Zs}$", IDEO_SP).IsMatched() );
	CHECK(  MatchW("^\\p{L&}+$", "aB").IsMatched() );
	CHECK( !MatchW("\\p{LC}", HIRA_A).IsMatched() );
	CHECK(  MatchW("^\\p{Any}$", HIRA_A).IsMatched() );
	CHECK(  MatchW("^\\p{Assigned}$", HIRA_A).IsMatched() );
	CHECK( !MatchW("\\p{Assigned}", UNASSIGN).IsMatched() );
	CHECK(  MatchW("^\\p{Cn}$", UNASSIGN).IsMatched() );
	CHECK(  MatchW("^\\p{ASCII}+$", "a~").IsMatched() );
	CHECK( !MatchW("\\p{ASCII}", E_ACUTE).IsMatched() );
	CHECK(  MatchW("^\\p{White_Space}+$", " \t" IDEO_SP).IsMatched() );
	CHECK(  MatchW("^\\p{Space}$", LINE_SEP).IsMatched() );
	CHECK(  MatchW("^\\p{Word}+$", "a_1" HIRA_A).IsMatched() );
	CHECK(  MatchW("^\\p{Xan}+$", "a1" FW_ONE).IsMatched() );
	CHECK( !MatchW("\\p{Xan}", "_").IsMatched() );
	CHECK(  MatchW("^\\p{Xwd}$", "_").IsMatched() );

	// in [...], negated, with other members
	CHECK(  MatchW("^[\\p{Lu}\\d]+$", "A1" E_ACUTE_U).IsMatched() );
	CHECK( !MatchW("^[\\p{Lu}\\d]+$", "Aa").IsMatched() );
	CHECK(  MatchW("^[^\\p{L}]+$", "12 ").IsMatched() );
	CHECK(  MatchW("^[\\P{L}a]+$", "1a").IsMatched() );

	// properties ignore IGNORECASE, as in PCRE
	CHECK( !MatchW("\\p{Lu}", "a", IGNORECASE).IsMatched() );

	// unknown names match nothing, \P of them anything
	CHECK( !MatchW("\\p{Hiragana}", HIRA_A).IsMatched() );
	CHECK(  MatchW("^\\P{Hiragana}$", HIRA_A).IsMatched() );
	CHECK( !MatchW("\\p{}", "a").IsMatched() );

	// not a property: literal
	CHECK(  MatchW("^\\p{L$", "p{L").IsMatched() );
	CHECK(  MatchA("^\\p$", "p") );

	// CRegexpA: bytes as U+0000..U+00FF
	CHECK(  MatchA("^\\p{Lu}+$", "AB") );
	CHECK( !MatchA("\\p{Lu}", "ab") );

	// right to left
	CHECK(  MatchW("^\\p{L}\\p{N}$", "a1", RIGHTTOLEFT).IsMatched() );

	// without UNICODE_MODE the halves of a surrogate pair are separate chars
	CHECK(  MatchW("^\\p{Cs}\\p{Cs}$", GRIN).IsMatched() );
	CHECK(  MatchW("^\\p{So}$", GRIN, UNICODE_MODE).IsMatched() );
}

static void TestUnicodeMode()
{
	// \w \d \s \b
	CHECK(  MatchW("^\\w$", E_ACUTE, UNICODE_MODE).IsMatched() );
	CHECK( !MatchW("\\w", E_ACUTE).IsMatched() );          // ASCII only without the flag
	CHECK(  MatchW("^\\w+$", HIRA_A "x" FW_ONE, UNICODE_MODE).IsMatched() );
	CHECK( !MatchW("\\W", HIRA_A, UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^\\d$", FW_ONE, UNICODE_MODE).IsMatched() );
	CHECK( !MatchW("\\d", FW_ONE).IsMatched() );
	CHECK(  MatchW("^\\D$", HIRA_A, UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^\\s$", IDEO_SP, UNICODE_MODE).IsMatched() );
	CHECK( !MatchW("\\S", IDEO_SP, UNICODE_MODE).IsMatched() );
	CHECK(  MatchSpanW("\\b\\w+\\b", " " HIRA_A HIRA_A " ", UNICODE_MODE, 1, 3) );
	CHECK( !MatchW("\\b", HIRA_A).IsMatched() );          // no word chars without the flag
	CHECK(  MatchW("^[\\w]$", E_ACUTE, UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("(?u)^\\w$", E_ACUTE).IsMatched() );
	CHECK( !MatchW("(?-u)\\w", E_ACUTE, UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^(?u:\\w)\\w$", E_ACUTE "a").IsMatched() );
	CHECK( !MatchW("^(?u:\\w)\\w$", "a" E_ACUTE).IsMatched() );

	// case folding
	CHECK(  MatchW("^\\u0434$", CYR_DE_U, IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK( !MatchW("^\\u0434$", CYR_DE_U, IGNORECASE).IsMatched() ); // ASCII only without the flag
	CHECK(  MatchW("^\\u00e9$", E_ACUTE_U, IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^\\u03c2$", SIGMA_U, IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^\\u03a3$", SIGMA_F, IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^k$", KELVIN, IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^[\\u0430-\\u044f]+$", CYR_DE_U CYR_DE, IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK( !MatchW("[\\u0430-\\u044f]", CYR_DE_U, UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^[\\u03c2]$", SIGMA, IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^[\\u03a3]$", SIGMA_F, IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^[a-z]$", KELVIN, IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^[^\\u03c3]$", "x", IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK( !MatchW("^[^\\u03c3]$", SIGMA_U, IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^(\\u0434)\\1$", CYR_DE CYR_DE_U, IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^ABC$", "abc", IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^\\u0434$", CYR_DE_U, IGNORECASE | UNICODE_MODE | RIGHTTOLEFT).IsMatched() );

	// surrogate pairs are one char
	CHECK(  MatchW("^.$", GRIN, UNICODE_MODE).IsMatched() );
	CHECK( !MatchW("^.$", GRIN).IsMatched() );
	CHECK( !MatchW("^..$", GRIN, UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^.{2}$", GRIN GRIN2, UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^.*b$", GRIN GRIN "b", UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^(.)+?b$", GRIN GRIN "b", UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^[^a]$", GRIN, UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^\\W$", GRIN, UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^[[:^alpha:]]$", GRIN, UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^[\\u{1F600}-\\u{1F64F}]+$", GRIN GRIN2, UNICODE_MODE).IsMatched() );
	CHECK( !MatchW("^[\\u{1F600}-\\u{1F64F}]+$", "a", UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^\\u{1F600}+$", GRIN GRIN, UNICODE_MODE).IsMatched() );
	CHECK( !MatchW("^\\u{1F600}+$", GRIN GRIN).IsMatched() ); // + applies to the low half only
	CHECK(  MatchW("^\\u{1F600}$", GRIN).IsMatched() );       // \u{...} above U+FFFF is a pair
	CHECK(  MatchW("^x\\u{1F600}{2}y$", "x" GRIN GRIN "y", UNICODE_MODE).IsMatched() );
	CHECK(  MatchSpanW("(?<=.)x", GRIN "x", UNICODE_MODE, 2, 3) );
	CHECK(  MatchSpanW(".", "a" GRIN, UNICODE_MODE | RIGHTTOLEFT, 1, 3) );
	CHECK( !MatchW("\\b", GRIN, UNICODE_MODE).IsMatched() ); // not a word char

	// no match starts between the halves of a pair
	CHECK( !MatchW("\\p{Cs}", GRIN, UNICODE_MODE).IsMatched() );
	CHECK(  MatchSpanW("", GRIN, UNICODE_MODE, 0, 0) );

	{
		unsigned short p[64], t[64], to[64];
		CRegexpW regexp(Wide("", p), UNICODE_MODE);
		unsigned short * s = regexp.Replace(U16(GRIN "a", t), Wide("-", to));
		unsigned short expected[64];
		U16("-" GRIN "-a-", expected);
		int ok = s != 0;
		for(int i=0; ok && (s[i] != 0 || expected[i] != 0); i++) ok = s[i] == expected[i];
		CHECK( ok );
		CRegexpW::ReleaseString(s);
	}

	// classes fold chars above U+FFFF too (Deseret U+10400 / U+10428)
	CHECK(  MatchW("^[\\u{10400}]$", "\xf0\x90\x90\xa8", IGNORECASE | UNICODE_MODE).IsMatched() );
	CHECK(  MatchW("^[\\u{10428}]$", "\xf0\x90\x90\x80", IGNORECASE | UNICODE_MODE).IsMatched() );

	// the tables (checked against the whole UCD when generated)
	CHECK( deelx_unicode_category(0x41)    == DEELX_UC_Lu );
	CHECK( deelx_unicode_category(0x3042)  == DEELX_UC_Lo );
	CHECK( deelx_unicode_category(0x0378)  == DEELX_UC_Cn );
	CHECK( deelx_unicode_category(0x1F600) == DEELX_UC_So );
	CHECK( deelx_unicode_category(0x10FFFF) == DEELX_UC_Cn );
	CHECK( deelx_unicode_category(0x110000) == DEELX_UC_Cn );
	CHECK( deelx_unicode_fold(0x41)   == 0x61 );
	CHECK( deelx_unicode_fold(0x61)   == 0x61 );
	CHECK( deelx_unicode_fold(0x3A3)  == 0x3C3 );
	CHECK( deelx_unicode_fold(0x3C2)  == 0x3C3 );
	CHECK( deelx_unicode_fold(0x212A) == 0x6B );
	CHECK( deelx_unicode_fold(0x10400) == 0x10428 );
	CHECK( deelx_unicode_fold(0x101)  == 0x101 );  // between the chars of a step-2 range
	CHECK( deelx_unicode_fold(0x100)  == 0x101 );

	// CRegexpA ignores the flag
	CHECK( !MatchA("^\\w$", "\xe9", UNICODE_MODE) );
	CHECK( !MatchA("^\\xe9$", "\xc9", IGNORECASE | UNICODE_MODE) );
}

int main()
{
	setvbuf(stdout, 0, _IONBF, 0); // keep output even if a test crashes

	TestBasic();
	TestQuantifierDigits();
	TestSelfAssignment();
	TestReplaceResultWithLimit();
	TestUnterminatedRemark();
	TestLengthBoundedInput();
	TestRecompileClearsNames();
	TestBalancingOutOfRange();
	TestBufferCopy();
	TestBufferInsertAndPrepare();
	TestNonAsciiChars();
	TestReplaceUnmatchedGroup();
	TestReplaceTokens();
	TestAsciiClass();
	TestAllocationFailure();
	TestGroupNumberLimit();
	TestMatchExactBacktracking();
	TestSpaceClass();
	TestInvalidQuantifierIsLiteral();
	TestNegativeGroupNumber();
	TestUnresolvedRecursion();
	TestExtendedCharset();
	TestContextInitialized();
	TestMinMaxNotMacros();
	TestNamedGroupName();
	TestSortedBufferFind();
	TestReplaceSegmentLengths();
	TestStepLimit();
	TestAutoStepLimit();
	TestInlineExtended();
	TestHorizontalSpace();
	TestLineBreak();
	TestKeep();
	TestBranchReset();
	TestUnicodeProperty();
	TestUnicodeMode();

	printf("%d checks, %d failures\n", g_nChecks, g_nFailures);
	return g_nFailures == 0 ? 0 : 1;
}
