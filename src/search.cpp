/*
 * Copyright (C) 2012-2013 Robin Haberkorn
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "expressions.h"
#include "undo.h"
#include "qregisters.h"
#include "ring.h"
#include "parser.h"
#include "search.h"

namespace States {
	StateSearch			search;
	StateSearchAll			searchall;
	StateSearchKill			searchkill;
	StateSearchDelete		searchdelete;

	StateReplace			replace;
	StateReplace_insert		replace_insert;
	StateReplace_ignore		replace_ignore;

	StateReplaceDefault		replacedefault;
	StateReplaceDefault_insert	replacedefault_insert;
	StateReplaceDefault_ignore	replacedefault_ignore;
}

/*
 * Command states
 */

/*$
 * S[pattern]$ -- Search for pattern
 * [n]S[pattern]$
 * -S[pattern]$
 * from,toS[pattern]$
 * :S[pattern]$ -> Success|Failure
 * [n]:S[pattern]$ -> Success|Failure
 * -:S[pattern]$ -> Success|Failure
 * from,to:S[pattern]$ -> Success|Failure
 *
 * Search for <pattern> in the current buffer/Q-Register.
 * Search order and range depends on the arguments given.
 * By default without any arguments, S will search forward
 * from dot till file end.
 * The optional single argument specifies the occurrence
 * to search (1 is the first occurrence, 2 the second, etc.).
 * Negative values for <n> perform backward searches.
 * If missing, the sign prefix is implied for <n>.
 * Therefore \(lq-S\(rq will search for the first occurrence
 * of <pattern> before dot.
 *
 * If two arguments are specified on the command,
 * search will be bounded in the character range <from> up to
 * <to>, and only the first occurrence will be searched.
 * <from> might be larger than <to> in which case a backward
 * search is performed in the selected range.
 *
 * After performing the search, the search <pattern> is saved
 * in the global search Q-Register \(lq_\(rq.
 * A success/failure condition boolean is saved in that
 * register's integer part.
 * <pattern> may be omitted in which case the pattern of
 * the last search or search and replace command will be
 * implied by using the contents of register \(lq_\(rq
 * (this could of course also be manually set).
 *
 * After a successful search, the pointer is positioned after
 * the found text in the buffer.
 * An unsuccessful search will display an error message but
 * not actually yield an error.
 * The message displaying is suppressed when executed from loops
 * and register \(lq_\(rq is the implied argument for break-commands
 * so that a search-break idiom can be implemented as follows:
 * .EX
 * <Sfoo$; ...>
 * .EE
 * Alternatively, S may be colon-modified in which case it returns
 * a condition boolean that may be directly evaluated by a
 * conditional or break-command.
 *
 * In interactive mode, searching will be performed immediately
 * (\(lqsearch as you type\(rq) highlighting matched text
 * on the fly.
 * Changing the <pattern> results in the search being reperformed
 * from the beginning.
 */
void
StateSearch::initial(void) throw (Error)
{
	tecoInt v1, v2;

	undo.push_var(parameters);

	parameters.dot = interface.ssm(SCI_GETCURRENTPOS);

	v2 = expressions.pop_num_calc();
	if (expressions.args()) {
		/* TODO: optional count argument? */
		v1 = expressions.pop_num_calc();
		if (v1 <= v2) {
			parameters.count = 1;
			parameters.from = (gint)v1;
			parameters.to = (gint)v2;
		} else {
			parameters.count = -1;
			parameters.from = (gint)v2;
			parameters.to = (gint)v1;
		}

		if (!Validate::pos(parameters.from) ||
		    !Validate::pos(parameters.to))
			throw RangeError("S");
	} else {
		parameters.count = (gint)v2;
		if (v2 >= 0) {
			parameters.from = parameters.dot;
			parameters.to = interface.ssm(SCI_GETLENGTH);
		} else {
			parameters.from = 0;
			parameters.to = parameters.dot;
		}
	}

	parameters.from_buffer = ring.current;
	parameters.to_buffer = NULL;
}

static inline const gchar *
regexp_escape_chr(gchar chr)
{
	static gchar escaped[] = {'\\', '\0', '\0'};

	escaped[1] = chr;
	return g_ascii_isalnum(chr) ? escaped + 1 : escaped;
}

gchar *
StateSearch::class2regexp(MatchState &state, const gchar *&pattern,
			  bool escape_default)
{
	while (*pattern) {
		QRegister *reg;
		gchar *temp, *temp2;

		switch (state) {
		case STATE_START:
			switch (*pattern) {
			case CTL_KEY('S'):
				return g_strdup("[:^alnum:]");
			case CTL_KEY('E'):
				state = STATE_CTL_E;
				break;
			default:
				temp = escape_default
					? g_strdup(regexp_escape_chr(*pattern))
					: NULL;
				return temp;
			}
			break;

		case STATE_CTL_E:
			switch (g_ascii_toupper(*pattern)) {
			case 'A':
				state = STATE_START;
				return g_strdup("[:alpha:]");
			/* same as <CTRL/S> */
			case 'B':
				state = STATE_START;
				return g_strdup("[:^alnum:]");
			case 'C':
				state = STATE_START;
				return g_strdup("[:alnum:].$");
			case 'D':
				state = STATE_START;
				return g_strdup("[:digit:]");
			case 'G':
				state = STATE_ANYQ;
				break;
			case 'L':
				state = STATE_START;
				return g_strdup("\r\n\v\f");
			case 'R':
				state = STATE_START;
				return g_strdup("[:alnum:]");
			case 'V':
				state = STATE_START;
				return g_strdup("[:lower:]");
			case 'W':
				state = STATE_START;
				return g_strdup("[:upper:]");
			default:
				return NULL;
			}
			break;

		case STATE_ANYQ:
			reg = qreg_machine.input(*pattern);
			if (!reg)
				break;
			qreg_machine.reset();

			temp = reg->get_string();
			temp2 = g_regex_escape_string(temp, -1);
			g_free(temp);
			state = STATE_START;
			return temp2;

		default:
			return NULL;
		}

		pattern++;
	}

	return NULL;
}

gchar *
StateSearch::pattern2regexp(const gchar *&pattern,
			    bool single_expr)
{
	MatchState state = STATE_START;
	gchar *re = NULL;

	while (*pattern) {
		gchar *new_re, *temp;

		temp = class2regexp(state, pattern);
		if (temp) {
			new_re = g_strconcat(re ? : "", "[", temp, "]", NIL);
			g_free(temp);
			g_free(re);
			re = new_re;

			goto next;
		}
		if (!*pattern)
			break;

		switch (state) {
		case STATE_START:
			switch (*pattern) {
			case CTL_KEY('X'): String::append(re, "."); break;
			case CTL_KEY('N'): state = STATE_NOT; break;
			default:
				String::append(re, regexp_escape_chr(*pattern));
			}
			break;

		case STATE_NOT:
			state = STATE_START;
			temp = class2regexp(state, pattern, true);
			if (!temp)
				goto error;
			new_re = g_strconcat(re ? : "", "[^", temp, "]", NIL);
			g_free(temp);
			g_free(re);
			re = new_re;
			g_assert(state == STATE_START);
			break;

		case STATE_CTL_E:
			state = STATE_START;
			switch (g_ascii_toupper(*pattern)) {
			case 'M': state = STATE_MANY; break;
			case 'S': String::append(re, "\\s+"); break;
			/* same as <CTRL/X> */
			case 'X': String::append(re, "."); break;
			/* TODO: ASCII octal code!? */
			case '[':
				String::append(re, "(");
				state = STATE_ALT;
				break;
			default:
				goto error;
			}
			break;

		case STATE_MANY:
			temp = pattern2regexp(pattern, true);
			if (!temp)
				goto error;
			new_re = g_strconcat(re ? : "", "(", temp, ")+", NIL);
			g_free(temp);
			g_free(re);
			re = new_re;
			state = STATE_START;
			break;

		case STATE_ALT:
			switch (*pattern) {
			case ',':
				String::append(re, "|");
				break;
			case ']':
				String::append(re, ")");
				state = STATE_START;
				break;
			default:
				temp = pattern2regexp(pattern, true);
				if (!temp)
					goto error;
				String::append(re, temp);
				g_free(temp);
			}
			break;

		default:
			/* shouldn't happen */
			g_assert(true);
		}

next:
		if (single_expr && state == STATE_START)
			return re;

		pattern++;
	}

	if (state == STATE_ALT)
		String::append(re, ")");

	return re;

error:
	g_free(re);
	return NULL;
}

void
StateSearch::do_search(GRegex *re, gint from, gint to, gint &count)
{
	GMatchInfo *info;
	const gchar *buffer;

	gint matched_from = -1, matched_to = -1;

	buffer = (const gchar *)interface.ssm(SCI_GETCHARACTERPOINTER);
	g_regex_match_full(re, buffer, (gssize)to, from,
			   (GRegexMatchFlags)0, &info, NULL);

	if (count >= 0) {
		while (g_match_info_matches(info) && --count)
			g_match_info_next(info, NULL);

		if (!count)
			/* successful */
			g_match_info_fetch_pos(info, 0,
					       &matched_from, &matched_to);
	} else {
		/* only keep the last `count' matches, in a circular stack */
		struct Range {
			gint from, to;
		};
		Range *matched = new Range[-count];
		gint matched_total = 0, i = 0;

		while (g_match_info_matches(info)) {
			g_match_info_fetch_pos(info, 0,
					       &matched[i].from,
					       &matched[i].to);

			g_match_info_next(info, NULL);
			i = ++matched_total % -count;
		}

		count = MIN(count + matched_total, 0);
		if (!count) {
			/* successful, i points to stack bottom */
			matched_from = matched[i].from;
			matched_to = matched[i].to;
		}

		delete matched;
	}

	g_match_info_free(info);

	if (matched_from >= 0 && matched_to >= 0)
		/* match success */
		interface.ssm(SCI_SETSEL, matched_from, matched_to);
}

void
StateSearch::process(const gchar *str, gint new_chars) throw (Error)
{
	static const gint flags = G_REGEX_CASELESS | G_REGEX_MULTILINE |
				  G_REGEX_DOTALL | G_REGEX_RAW;

	QRegister *search_reg = QRegisters::globals["_"];

	gchar *re_pattern;
	GRegex *re;

	gint count = parameters.count;

	undo.push_msg(SCI_SETSEL,
		      interface.ssm(SCI_GETANCHOR),
		      interface.ssm(SCI_GETCURRENTPOS));

	search_reg->undo_set_integer();
	search_reg->set_integer(FAILURE);

	/* NOTE: pattern2regexp() modifies str pointer */
	re_pattern = pattern2regexp(str);
	qreg_machine.reset();
#ifdef DEBUG
	g_printf("REGEXP: %s\n", re_pattern);
#endif
	if (!re_pattern)
		goto failure;
	re = g_regex_new(re_pattern, (GRegexCompileFlags)flags,
			 (GRegexMatchFlags)0, NULL);
	g_free(re_pattern);
	if (!re)
		goto failure;

	if (ring.current != parameters.from_buffer) {
		ring.undo_edit();
		parameters.from_buffer->edit();
	}

	do_search(re, parameters.from, parameters.to, count);

	if (parameters.to_buffer && count) {
		Buffer *buffer = parameters.from_buffer;

		if (ring.current == buffer)
			ring.undo_edit();

		if (count > 0) {
			do {
				buffer = buffer->next() ? : ring.first();
				buffer->edit();

				if (buffer == parameters.to_buffer) {
					do_search(re, 0, parameters.dot, count);
					break;
				}

				do_search(re, 0, interface.ssm(SCI_GETLENGTH),
					  count);
			} while (count);
		} else /* count < 0 */ {
			do {
				buffer = buffer->prev() ? : ring.last();
				buffer->edit();

				if (buffer == parameters.to_buffer) {
					do_search(re, parameters.dot,
						  interface.ssm(SCI_GETLENGTH),
						  count);
					break;
				}

				do_search(re, 0, interface.ssm(SCI_GETLENGTH),
					  count);
			} while (count);
		}

		ring.current = buffer;
	}

	search_reg->set_integer(TECO_BOOL(!count));

	g_regex_unref(re);

	if (!count)
		return;

failure:
	interface.ssm(SCI_GOTOPOS, parameters.dot);
}

State *
StateSearch::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	QRegister *search_reg = QRegisters::globals["_"];

	if (*str) {
		/* workaround: preserve selection (also on rubout) */
		gint anchor = interface.ssm(SCI_GETANCHOR);
		undo.push_msg(SCI_SETANCHOR, anchor);

		search_reg->undo_set_string();
		search_reg->set_string(str);

		interface.ssm(SCI_SETANCHOR, anchor);
	} else {
		gchar *search_str = search_reg->get_string();
		process(search_str, 0 /* unused */);
		g_free(search_str);
	}

	if (eval_colon())
		expressions.push(search_reg->get_integer());
	else if (IS_FAILURE(search_reg->get_integer()) &&
		 !expressions.find_op(Expressions::OP_LOOP) /* not in loop */)
		interface.msg(Interface::MSG_ERROR, "Search string not found!");

	return &States::start;
}

/*$
 * [n]N[pattern]$ -- Search over buffer-boundaries
 * -N[pattern]$
 * from,toN[pattern]$
 * [n]:N[pattern]$ -> Success|Failure
 * -:N[pattern]$ -> Success|Failure
 * from,to:N[pattern]$ -> Success|Failure
 *
 * Search for <pattern> over buffer boundaries.
 * This command is similar to the regular search command
 * (S) but will continue to search for occurrences of
 * pattern when the end or beginning of the current buffer
 * is reached.
 * Occurrences of <pattern> spanning over buffer boundaries
 * will not be found.
 * When searching forward N will start in the current buffer
 * at dot, continue with the next buffer in the ring searching
 * the entire buffer until it reaches the end of the buffer
 * ring, continue with the first buffer in the ring until
 * reaching the current file again where it searched from the
 * beginning of the buffer up to its current dot.
 * Searching backwards does the reverse.
 *
 * N also differs from S in the interpretation of two arguments.
 * Using two arguments the search will be bounded between the
 * buffer with number <from>, up to the buffer with number
 * <to>.
 * When specifying buffer ranges, the entire buffers are searched
 * from beginning to end.
 * <from> may be greater than <to> in which case, searching starts
 * at the end of buffer <from> and continues backwards until the
 * beginning of buffer <to> has been reached.
 * Furthermore as with all buffer numbers, the buffer ring
 * is considered a circular structure and it is possible
 * to search over buffer ring boundaries by specifying
 * buffer numbers greater than the number of buffers in the
 * ring.
 */
void
StateSearchAll::initial(void) throw (Error)
{
	tecoInt v1, v2;

	undo.push_var(parameters);

	parameters.dot = interface.ssm(SCI_GETCURRENTPOS);

	v2 = expressions.pop_num_calc();
	if (expressions.args()) {
		/* TODO: optional count argument? */
		v1 = expressions.pop_num_calc();
		if (v1 <= v2) {
			parameters.count = 1;
			parameters.from_buffer = ring.find(v1);
			parameters.to_buffer = ring.find(v2);
		} else {
			parameters.count = -1;
			parameters.from_buffer = ring.find(v2);
			parameters.to_buffer = ring.find(v1);
		}

		if (!parameters.from_buffer || !parameters.to_buffer)
			throw RangeError("N");
	} else {
		parameters.count = (gint)v2;
		/* NOTE: on Q-Registers, behave like "S" */
		parameters.from_buffer = parameters.to_buffer = ring.current;
	}

	if (parameters.count >= 0) {
		parameters.from = parameters.dot;
		parameters.to = interface.ssm(SCI_GETLENGTH);
	} else {
		parameters.from = 0;
		parameters.to = parameters.dot;
	}
}

State *
StateSearchAll::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	StateSearch::done(str);
	QRegisters::hook(QRegisters::HOOK_EDIT);

	return &States::start;
}

/*$
 * FK[pattern]$ -- Delete up to occurrence of pattern
 * [n]FK[pattern]$
 * -FK[pattern]$
 * from,toFK[pattern]$
 * :FK[pattern]$ -> Success|Failure
 * [n]:FK[pattern]$ -> Success|Failure
 * -:FK[pattern]$ -> Success|Failure
 * from,to:FK[pattern]$ -> Success|Failure
 *
 * FK searches for <pattern> just like the regular search
 * command (S) but when found deletes all text from dot
 * up to but not including the found text instance.
 * When searching backwards the characters beginning after
 * the occurrence of <pattern> up to dot are deleted.
 *
 * In interactive mode, deletion is not performed
 * as-you-type but only on command termination.
 */
State *
StateSearchKill::done(const gchar *str) throw (Error)
{
	gint dot;

	BEGIN_EXEC(&States::start);

	QRegister *search_reg = QRegisters::globals["_"];

	StateSearch::done(str);

	if (IS_FAILURE(search_reg->get_integer()))
		return &States::start;

	dot = interface.ssm(SCI_GETCURRENTPOS);

	interface.ssm(SCI_BEGINUNDOACTION);
	if (parameters.dot < dot) {
		/* kill forwards */
		gint anchor = interface.ssm(SCI_GETANCHOR);

		undo.push_msg(SCI_GOTOPOS, dot);
		interface.ssm(SCI_GOTOPOS, anchor);

		interface.ssm(SCI_DELETERANGE,
			      parameters.dot, anchor - parameters.dot);
	} else {
		/* kill backwards */
		interface.ssm(SCI_DELETERANGE, dot, parameters.dot - dot);
	}
	interface.ssm(SCI_ENDUNDOACTION);
	ring.dirtify();

	undo.push_msg(SCI_UNDO);

	return &States::start;
}

/*$
 * FD[pattern]$ -- Delete occurrence of pattern
 * [n]FD[pattern]$
 * -FD[pattern]$
 * from,toFD[pattern]$
 * :FD[pattern]$ -> Success|Failure
 * [n]:FD[pattern]$ -> Success|Failure
 * -:FD[pattern]$ -> Success|Failure
 * from,to:FD[pattern]$ -> Success|Failure
 *
 * Searches for <pattern> just like the regular search command
 * (S) but when found deletes the entire occurrence.
 */
State *
StateSearchDelete::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	QRegister *search_reg = QRegisters::globals["_"];

	StateSearch::done(str);

	if (IS_SUCCESS(search_reg->get_integer())) {
		interface.ssm(SCI_BEGINUNDOACTION);
		interface.ssm(SCI_REPLACESEL, 0, (sptr_t)"");
		interface.ssm(SCI_ENDUNDOACTION);
		ring.dirtify();

		undo.push_msg(SCI_UNDO);
	}

	return &States::start;
}

/*$
 * FS[pattern]$[string]$ -- Search and replace
 * [n]FS[pattern]$[string]$
 * -FS[pattern]$[string]$
 * from,toFS[pattern]$[string]$
 * :FS[pattern]$[string]$ -> Success|Failure
 * [n]:FS[pattern]$[string]$ -> Success|Failure
 * -:FS[pattern]$[string]$ -> Success|Failure
 * from,to:FS[pattern]$[string]$ -> Success|Failure
 *
 * Search for <pattern> just like the regular search command
 * (S) does but replace it with <string> if found.
 * If <string> is empty, the occurrence will always be
 * deleted so \(lqFS[pattern]$$\(rq is similar to
 * \(lqFD[pattern]$\(rq.
 * The global replace register is \fBnot\fP touched
 * by the FS command.
 *
 * In interactive mode, the replacement will be performed
 * immediately and interactively.
 */
State *
StateReplace::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::replace_ignore);

	QRegister *search_reg = QRegisters::globals["_"];

	StateSearchDelete::done(str);

	return IS_SUCCESS(search_reg->get_integer())
		? (State *)&States::replace_insert
		: (State *)&States::replace_ignore;
}

State *
StateReplace_ignore::done(const gchar *str) throw (Error)
{
	return &States::start;
}

/*$
 * FR[pattern]$[string]$ -- Search and replace with default
 * [n]FR[pattern]$[string]$
 * -FR[pattern]$[string]$
 * from,toFR[pattern]$[string]$
 * :FR[pattern]$[string]$ -> Success|Failure
 * [n]:FR[pattern]$[string]$ -> Success|Failure
 * -:FR[pattern]$[string]$ -> Success|Failure
 * from,to:FR[pattern]$[string]$ -> Success|Failure
 *
 * The FR command is similar to the FS command.
 * It searches for <pattern> just like the regular search
 * command (S) and replaces the occurrence with <string>
 * similar to what FS does.
 * It differs from FS in the fact that the replacement
 * string is saved in the global replacement register
 * \(lq-\(rq.
 * If <string> is empty the string in the global replacement
 * register is implied instead.
 */
State *
StateReplaceDefault::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::replacedefault_ignore);

	QRegister *search_reg = QRegisters::globals["_"];

	StateSearchDelete::done(str);

	return IS_SUCCESS(search_reg->get_integer())
		? (State *)&States::replacedefault_insert
		: (State *)&States::replacedefault_ignore;
}

State *
StateReplaceDefault_insert::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	QRegister *replace_reg = QRegisters::globals["-"];

	if (*str) {
		replace_reg->undo_set_string();
		replace_reg->set_string(str);
	} else {
		gchar *replace_str = replace_reg->get_string();
		StateInsert::process(replace_str, strlen(replace_str));
		g_free(replace_str);
	}

	return &States::start;
}

State *
StateReplaceDefault_ignore::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	if (*str) {
		QRegister *replace_reg = QRegisters::globals["-"];

		replace_reg->undo_set_string();
		replace_reg->set_string(str);
	}

	return &States::start;
}
