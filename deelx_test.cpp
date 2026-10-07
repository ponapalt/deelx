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
//       - the only exception thrown is std::bad_alloc, and only through
//         deelx_realloc() / deelx_check_new() (VC6's new returns 0 instead
//         of throwing). Keep objects consistent when an allocation throws:
//         update pointers/capacities only after the allocation succeeded
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
//   * Not MBCS aware: Shift_JIS trail bytes may match ASCII in some cases.
//   * \w, \b, \d and case folding are ASCII only, also for CRegexpW.
//   * Invalid patterns are never reported: they compile to something (e.g.
//     "a**" takes the second '*' literally, an unknown backreference fails).
//   * Recursion ((?R), (?1), ...) is limited to 100 levels per match path;
//     deeper input simply does not match.
//   * Plain backtracking without step limit: patterns like (a*)*b can take
//     exponential time. Do not run untrusted patterns on untrusted input.
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

	printf("%d checks, %d failures\n", g_nChecks, g_nFailures);
	return g_nFailures == 0 ? 0 : 1;
}
