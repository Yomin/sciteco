#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include "sciteco.h"
#include "undo.h"
#include "expressions.h"
#include "goto.h"
#include "qbuffers.h"
#include "parser.h"

//#define DEBUG

gint macro_pc = 0;

namespace States {
	StateStart 		start;
	StateControl		control;
	StateFlowCommand	flowcommand;
	StateCondCommand	condcommand;
	StateECommand		ecommand;
	StateInsert		insert;
	StateSearch		search;

	State *current = &start;
}

namespace Modifiers {
	static bool colon = false;
	static bool at = false;
}

enum Mode mode = MODE_NORMAL;

/* FIXME: perhaps integrate into Mode */
static bool skip_else = false;

static gint nest_level = 0;

gchar *strings[2] = {NULL, NULL};
gchar escape_char = '\x1B';

bool
macro_execute(const gchar *macro)
{
	while (macro[macro_pc]) {
#ifdef DEBUG
		g_printf("EXEC(%d): input='%c'/%x, state=%p\n",
			 macro_pc, macro[macro_pc], macro[macro_pc],
			 States::current);
#endif

		if (!State::input(macro[macro_pc])) {
			message_display(GTK_MESSAGE_ERROR,
					"Syntax error \"%c\"",
					macro[macro_pc]);
			return false;
		}

		macro_pc++;
	}

	return true;
}

bool
file_execute(const gchar *filename)
{
	gchar *macro, *p = NULL;
	bool rc;

	macro_pc = 0;
	States::current = &States::start;

	if (!g_file_get_contents(filename, &macro, NULL, NULL))
		return false;
	/* only when executing files, ignore Hash-Bang line */
	if (macro[0] == '#')
		p = MAX(strchr(macro, '\r'), strchr(macro, '\n'));

	rc = macro_execute(p ? p+1 : macro);
	g_free(macro);
	if (!rc)
		return false;

	macro_pc = 0;
	States::current = &States::start;
	return true;
}

State::State()
{
	for (guint i = 0; i < G_N_ELEMENTS(transitions); i++)
		transitions[i] = NULL;
}

bool
State::eval_colon(void)
{
	if (!Modifiers::colon)
		return false;

	undo.push_var<bool>(Modifiers::colon);
	Modifiers::colon = false;
	return true;
}

bool
State::input(gchar chr)
{
	State *state = States::current;

	for (;;) {
		State *next = state->get_next_state(chr);

		if (!next)
			/* Syntax error */
			return false;

		if (next == state)
			break;

		state = next;
		chr = '\0';
	}

	if (state != States::current) {
		undo.push_var<State *>(States::current);
		States::current = state;
	}

	return true;
}

State *
State::get_next_state(gchar chr)
{
	State *next = NULL;
	guint upper = g_ascii_toupper(chr);

	if (upper < G_N_ELEMENTS(transitions))
		next = transitions[upper];
	if (!next)
		next = custom(chr);

	return next;
}

gchar *
StateExpectString::machine_input(gchar chr)
{
	switch (machine.mode) {
	case Machine::MODE_UPPER:
		chr = g_ascii_toupper(chr);
		break;
	case Machine::MODE_LOWER:
		chr = g_ascii_tolower(chr);
		break;
	default:
		break;
	}

	if (machine.toctl) {
		chr = CTL_KEY(g_ascii_toupper(chr));
		machine.toctl = false;
	}

	if (machine.state == Machine::STATE_ESCAPED) {
		machine.state = Machine::STATE_START;
		goto original;
	}

	if (chr == '^') {
		machine.toctl = true;
		return NULL;
	}

	switch (machine.state) {
	case Machine::STATE_START:
		switch (chr) {
		case CTL_KEY('Q'):
		case CTL_KEY('R'): machine.state = Machine::STATE_ESCAPED; break;
		case CTL_KEY('V'): machine.state = Machine::STATE_LOWER; break;
		case CTL_KEY('W'): machine.state = Machine::STATE_UPPER; break;
		case CTL_KEY('E'): machine.state = Machine::STATE_CTL_E; break;
		default:
			goto original;
		}
		break;

	case Machine::STATE_LOWER:
		machine.state = Machine::STATE_START;
		if (chr != CTL_KEY('V'))
			return g_strdup((gchar []){g_ascii_tolower(chr), '\0'});
		machine.mode = Machine::MODE_LOWER;
		break;

	case Machine::STATE_UPPER:
		machine.state = Machine::STATE_START;
		if (chr != CTL_KEY('W'))
			return g_strdup((gchar []){g_ascii_toupper(chr), '\0'});
		machine.mode = Machine::MODE_UPPER;
		break;

	case Machine::STATE_CTL_E:
		switch (g_ascii_toupper(chr)) {
		case 'Q': machine.state = Machine::STATE_CTL_EQ; break;
		case 'U': machine.state = Machine::STATE_CTL_EU; break;
		default:
			machine.state = Machine::STATE_START;
			return g_strdup((gchar []){CTL_KEY('E'), chr, '\0'});
		}
		break;

	/*
	 * FIXME: Q-Register specifications might get more complicated
	 */
	case Machine::STATE_CTL_EU:
	case Machine::STATE_CTL_EQ: {
		QRegister *reg;
		Machine::State state = machine.state;
		machine.state = Machine::STATE_START;

		reg = qregisters[(gchar []){g_ascii_toupper(chr), '\0'}];
		if (!reg)
			return NULL;

		return state == Machine::STATE_CTL_EQ
			? reg->get_string()
			: g_strdup((gchar []){(gchar)reg->integer, '\0'});
	}

	default:
		g_assert(TRUE);
	}

	return NULL;

original:
	return g_strdup((gchar []){chr, '\0'});
}

State *
StateExpectString::custom(gchar chr)
{
	gchar *insert, *new_str;

	if (chr == '\0') {
		BEGIN_EXEC(this);
		initial();
		return this;
	}

	/*
	 * String termination handling
	 */
	if (Modifiers::at) {
		undo.push_var<bool>(Modifiers::at);
		Modifiers::at = false;
		undo.push_var<gchar>(escape_char);
		escape_char = g_ascii_toupper(chr);

		return this;
	}

	if (g_ascii_toupper(chr) == escape_char) {
		State *next = done(strings[0] ? : "");

		undo.push_var<gchar>(escape_char);
		escape_char = '\x1B';
		undo.push_str(strings[0]);
		g_free(strings[0]);
		strings[0] = NULL;

		undo.push_var<Machine>(machine);
		machine.state = Machine::STATE_START;
		machine.mode = Machine::MODE_NORMAL;
		machine.toctl = false;

		return next;
	}

	BEGIN_EXEC(this);

	/*
	 * String building characters
	 */
	undo.push_var<Machine>(machine);
	insert = machine_input(chr);
	if (!insert)
		return this;

	/*
	 * String accumulation
	 */
	undo.push_str(strings[0]);
	new_str = g_strconcat(strings[0] ? : "", insert, NULL);
	g_free(strings[0]);
	strings[0] = new_str;

	process(strings[0], strlen(insert));
	g_free(insert);
	return this;
}

StateExpectQReg::StateExpectQReg() : State()
{
	transitions['\0'] = this;
}

State *
StateExpectQReg::custom(gchar chr)
{
	QRegister *reg = qregisters[(gchar []){g_ascii_toupper(chr), '\0'}];

	if (!reg)
		return NULL;

	return got_register(reg);
}

StateStart::StateStart() : State()
{
	transitions['\0'] = this;
	init(" \f\r\n\v");

	transitions['!'] = &States::label;
	transitions['O'] = &States::gotocmd;
	transitions['^'] = &States::control;
	transitions['F'] = &States::flowcommand;
	transitions['"'] = &States::condcommand;
	transitions['E'] = &States::ecommand;
	transitions['I'] = &States::insert;
	transitions['S'] = &States::search;
	transitions['Q'] = &States::getqreginteger;
	transitions['U'] = &States::setqreginteger;
	transitions['%'] = &States::increaseqreg;
	transitions['M'] = &States::macro;
	transitions['X'] = &States::copytoqreg;
}

void
StateStart::move(gint64 n)
{
	sptr_t pos = editor_msg(SCI_GETCURRENTPOS);
	editor_msg(SCI_GOTOPOS, pos + n);
	undo.push_msg(SCI_GOTOPOS, pos);
}

void
StateStart::move_lines(gint64 n)
{
	sptr_t pos = editor_msg(SCI_GETCURRENTPOS);
	sptr_t line = editor_msg(SCI_LINEFROMPOSITION, pos);
	editor_msg(SCI_GOTOPOS, editor_msg(SCI_POSITIONFROMLINE, line + n));
	undo.push_msg(SCI_GOTOPOS, pos);
}

State *
StateStart::custom(gchar chr)
{
	/*
	 * <CTRL/x> commands implemented in StateCtrlCmd
	 */
	if (IS_CTL(chr))
		return States::control.get_next_state(CTL_ECHO(chr));

	/*
	 * arithmetics
	 */
	if (g_ascii_isdigit(chr)) {
		BEGIN_EXEC(this);
		expressions.add_digit(chr);
		return this;
	}

	chr = g_ascii_toupper(chr);
	switch (chr) {
	case '/':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_DIV);
		break;

	case '*':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_MUL);
		break;

	case '+':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_ADD);
		break;

	case '-':
		BEGIN_EXEC(this);
		if (!expressions.args())
			expressions.set_num_sign(-expressions.num_sign);
		else
			expressions.push_calc(Expressions::OP_SUB);
		break;

	case '&':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_AND);
		break;

	case '#':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_OR);
		break;

	case '(':
		BEGIN_EXEC(this);
		if (expressions.num_sign < 0) {
			expressions.set_num_sign(1);
			expressions.eval();
			expressions.push(-1);
			expressions.push_calc(Expressions::OP_MUL);
		}
		expressions.push(Expressions::OP_BRACE);
		break;

	case ')':
		BEGIN_EXEC(this);
		expressions.eval(true);
		break;

	case ',':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(Expressions::OP_NEW);
		break;

	case '.':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(editor_msg(SCI_GETCURRENTPOS));
		break;

	case 'Z':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(editor_msg(SCI_GETLENGTH));
		break;

	case 'H':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(0);
		expressions.push(editor_msg(SCI_GETLENGTH));
		break;

	/*
	 * control structures (loops)
	 */
	case '<':
		if (mode == MODE_PARSE_ONLY_LOOP) {
			undo.push_var<gint>(nest_level);
			nest_level++;
			return this;
		}
		BEGIN_EXEC(this);

		expressions.eval();
		if (!expressions.args())
			/* infinite loop */
			expressions.push(-1);

		if (!expressions.peek_num()) {
			expressions.pop_num();

			/* skip to end of loop */
			undo.push_var<Mode>(mode);
			mode = MODE_PARSE_ONLY_LOOP;
		} else {
			expressions.push(macro_pc);
			expressions.push(Expressions::OP_LOOP);
		}
		break;

	case '>':
		if (mode == MODE_PARSE_ONLY_LOOP) {
			if (!nest_level) {
				undo.push_var<Mode>(mode);
				mode = MODE_NORMAL;
			} else {
				undo.push_var<gint>(nest_level);
				nest_level--;
			}
		} else {
			BEGIN_EXEC(this);
			gint64 loop_pc, loop_cnt;

			expressions.discard_args();
			g_assert(expressions.pop_op() == Expressions::OP_LOOP);
			loop_pc = expressions.pop_num();
			loop_cnt = expressions.pop_num();

			if (loop_cnt != 1) {
				/* repeat loop */
				macro_pc = loop_pc;
				expressions.push(MAX(loop_cnt - 1, -1));
				expressions.push(loop_pc);
				expressions.push(Expressions::OP_LOOP);
			}
		}
		break;

	case ';': {
		gint64 v;
		BEGIN_EXEC(this);

		v = expressions.pop_num_calc(1, qregisters["_"]->integer);
		if (eval_colon())
			v = ~v;

		if (IS_FAILURE(v)) {
			expressions.discard_args();
			g_assert(expressions.pop_op() == Expressions::OP_LOOP);
			expressions.pop_num(); /* pc */
			expressions.pop_num(); /* counter */

			/* skip to end of loop */
			undo.push_var<Mode>(mode);
			mode = MODE_PARSE_ONLY_LOOP;
		}
		break;
	}

	/*
	 * control structures (conditionals)
	 */
	case '|':
		if (mode == MODE_PARSE_ONLY_COND) {
			if (!skip_else && !nest_level) {
				undo.push_var<Mode>(mode);
				mode = MODE_NORMAL;
			}
			return this;
		}
		BEGIN_EXEC(this);

		/* skip to end of conditional; skip ELSE-part */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY_COND;
		break;

	case '\'':
		if (mode != MODE_PARSE_ONLY_COND)
			break;

		if (!nest_level) {
			undo.push_var<Mode>(mode);
			mode = MODE_NORMAL;
			undo.push_var<bool>(skip_else);
			skip_else = false;
		} else {
			undo.push_var<gint>(nest_level);
			nest_level--;
		}
		break;

	/*
	 * modifiers
	 */
	case '@':
		/*
		 * @ modifier has syntactic significance, so set it even
		 * in PARSE_ONLY* modes
		 */
		undo.push_var<bool>(Modifiers::at);
		Modifiers::at = true;
		break;

	case ':':
		BEGIN_EXEC(this);
		undo.push_var<bool>(Modifiers::colon);
		Modifiers::colon = true;
		break;

	/*
	 * commands
	 */
	case 'J':
		BEGIN_EXEC(this);
		undo.push_msg(SCI_GOTOPOS, editor_msg(SCI_GETCURRENTPOS));
		editor_msg(SCI_GOTOPOS, expressions.pop_num_calc(1, 0));
		break;

	case 'C':
		BEGIN_EXEC(this);
		move(expressions.pop_num_calc());
		break;

	case 'R':
		BEGIN_EXEC(this);
		move(-expressions.pop_num_calc());
		break;

	case 'L':
		BEGIN_EXEC(this);
		move_lines(expressions.pop_num_calc());
		break;

	case 'B':
		BEGIN_EXEC(this);
		move_lines(-expressions.pop_num_calc());
		break;

	case '=':
		BEGIN_EXEC(this);
		message_display(GTK_MESSAGE_OTHER, "%" G_GINT64_FORMAT,
				expressions.pop_num_calc());
		break;

	case 'K':
	case 'D': {
		gint64 from, len;

		BEGIN_EXEC(this);
		expressions.eval();

		if (expressions.args() <= 1) {
			from = editor_msg(SCI_GETCURRENTPOS);
			if (chr == 'D') {
				len = expressions.pop_num_calc();
			} else /* chr == 'K' */ {
				sptr_t line = editor_msg(SCI_LINEFROMPOSITION, from) +
					      expressions.pop_num_calc();
				len = editor_msg(SCI_POSITIONFROMLINE, line) - from;
			}
			if (len < 0) {
				from += len;
				len *= -1;
			}
		} else {
			gint64 to = expressions.pop_num();
			from = expressions.pop_num();
			len = to - from;
		}

		if (len > 0) {
			undo.push_msg(SCI_GOTOPOS,
				      editor_msg(SCI_GETCURRENTPOS));
			undo.push_msg(SCI_UNDO);

			editor_msg(SCI_BEGINUNDOACTION);
			editor_msg(SCI_DELETERANGE, from, len);
			editor_msg(SCI_ENDUNDOACTION);
		}
		break;
	}

	default:
		return NULL;
	}

	return this;
}

StateFlowCommand::StateFlowCommand() : State()
{
	transitions['\0'] = this;
}

State *
StateFlowCommand::custom(gchar chr)
{
	switch (chr) {
	/*
	 * loop flow control
	 */
	case '<':
		BEGIN_EXEC(&States::start);
		/* FIXME: what if in brackets? */
		expressions.discard_args();
		if (expressions.peek_op() == Expressions::OP_LOOP)
			/* repeat loop */
			macro_pc = expressions.peek_num();
		else
			macro_pc = -1;
		break;

	case '>': {
		gint64 loop_pc, loop_cnt;

		BEGIN_EXEC(&States::start);
		/* FIXME: what if in brackets? */
		expressions.discard_args();
		g_assert(expressions.pop_op() == Expressions::OP_LOOP);
		loop_pc = expressions.pop_num();
		loop_cnt = expressions.pop_num();

		if (loop_cnt != 1) {
			/* repeat loop */
			macro_pc = loop_pc;
			expressions.push(MAX(loop_cnt - 1, -1));
			expressions.push(loop_pc);
			expressions.push(Expressions::OP_LOOP);
		} else {
			/* skip to end of loop */
			undo.push_var<Mode>(mode);
			mode = MODE_PARSE_ONLY_LOOP;
		}
		break;
	}

	/*
	 * conditional flow control
	 */
	case '\'':
		BEGIN_EXEC(&States::start);
		/* skip to end of conditional */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY_COND;
		undo.push_var<bool>(skip_else);
		skip_else = true;
		break;

	case '|':
		BEGIN_EXEC(&States::start);
		/* skip to ELSE-part or end of conditional */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY_COND;
		break;

	default:
		return NULL;
	}

	return &States::start;
}

StateCondCommand::StateCondCommand() : State()
{
	transitions['\0'] = this;
}

State *
StateCondCommand::custom(gchar chr)
{
	gint64 value;
	bool result;

	switch (mode) {
	case MODE_PARSE_ONLY_COND:
		undo.push_var<gint>(nest_level);
		nest_level++;
		break;
	case MODE_NORMAL:
		value = expressions.pop_num_calc();
		break;
	default:
		break;
	}

	switch (g_ascii_toupper(chr)) {
	case 'A':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isalpha((gchar)value);
		break;
	case 'C':
		BEGIN_EXEC(&States::start);
		/* FIXME */
		result = g_ascii_isalnum((gchar)value);
		break;
	case 'D':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isdigit((gchar)value);
		break;
	case 'E':
	case 'F':
	case 'U':
	case '=':
		BEGIN_EXEC(&States::start);
		result = value == 0;
		break;
	case 'G':
	case '>':
		BEGIN_EXEC(&States::start);
		result = value > 0;
		break;
	case 'L':
	case 'S':
	case 'T':
	case '<':
		BEGIN_EXEC(&States::start);
		result = value < 0;
		break;
	case 'N':
		BEGIN_EXEC(&States::start);
		result = value != 0;
		break;
	case 'R':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isalnum((gchar)value);
		break;
	case 'V':
		BEGIN_EXEC(&States::start);
		result = g_ascii_islower((gchar)value);
		break;
	case 'W':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isupper((gchar)value);
		break;
	default:
		return NULL;
	}

	if (!result) {
		/* skip to ELSE-part or end of conditional */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY_COND;
	}

	return &States::start;
}

StateControl::StateControl() : State()
{
	transitions['\0'] = this;
	transitions['U'] = &States::ctlucommand;
}

State *
StateControl::custom(gchar chr)
{
	switch (g_ascii_toupper(chr)) {
	case 'O':
		BEGIN_EXEC(&States::start);
		expressions.set_radix(8);
		break;

	case 'D':
		BEGIN_EXEC(&States::start);
		expressions.set_radix(10);
		break;

	case 'R':
		BEGIN_EXEC(&States::start);
		expressions.eval();
		if (!expressions.args())
			expressions.push(expressions.radix);
		else
			expressions.set_radix(expressions.pop_num_calc());
		break;

	/*
	 * Alternatives: ^i, ^I, <CTRL/I>, <TAB>
	 */
	case 'I':
		BEGIN_EXEC(&States::insert);
		expressions.eval();
		expressions.push('\t');
		return &States::insert;

	/*
	 * Alternatives: ^[, <CTRL/[>, <ESC>
	 */
	case '[':
		BEGIN_EXEC(&States::start);
		expressions.discard_args();
		break;

	/*
	 * Additional numeric operations
	 */
	case '_':
		BEGIN_EXEC(&States::start);
		expressions.push(~expressions.pop_num_calc());
		break;

	case '*':
		BEGIN_EXEC(&States::start);
		expressions.push_calc(Expressions::OP_POW);
		break;

	case '/':
		BEGIN_EXEC(&States::start);
		expressions.push_calc(Expressions::OP_MOD);
		break;

	default:
		return NULL;
	}

	return &States::start;
}

StateECommand::StateECommand() : State()
{
	transitions['\0'] = this;
	transitions['B'] = &States::file;
	transitions['Q'] = &States::eqcommand;
}

State *
StateECommand::custom(gchar chr)
{
	switch (g_ascii_toupper(chr)) {
	case 'X':
		BEGIN_EXEC(&States::start);
		undo.push_var<bool>(quit_requested);
		quit_requested = true;
		break;

	default:
		return NULL;
	}

	return &States::start;
}

/*
 * NOTE: cannot support VideoTECO's <n>I because
 * beginning and end of strings must be determined
 * syntactically
 */
void
StateInsert::initial(void)
{
	int args;

	expressions.eval();
	args = expressions.args();
	if (!args)
		return;

	editor_msg(SCI_BEGINUNDOACTION);
	for (int i = args; i > 0; i--) {
		gchar chr = (gchar)expressions.peek_num(i);
		editor_msg(SCI_ADDTEXT, 1, (sptr_t)&chr);
	}
	for (int i = args; i > 0; i--)
		expressions.pop_num_calc();
	editor_msg(SCI_ENDUNDOACTION);

	undo.push_msg(SCI_UNDO);
}

void
StateInsert::process(const gchar *str, gint new_chars)
{
	editor_msg(SCI_BEGINUNDOACTION);
	editor_msg(SCI_ADDTEXT, new_chars,
		   (sptr_t)(str + strlen(str) - new_chars));
	editor_msg(SCI_ENDUNDOACTION);

	undo.push_msg(SCI_UNDO);
}

State *
StateInsert::done(const gchar *str __attribute__((unused)))
{
	/* nothing to be done when done */
	return &States::start;
}

void
StateSearch::initial(void)
{
	gint64 v;

	undo.push_var<Parameters>(parameters);

	parameters.dot = editor_msg(SCI_GETCURRENTPOS);
	v = expressions.pop_num_calc();
	if (expressions.args()) {
		/* TODO: optional count argument? */
		parameters.count = 1;
		parameters.from = (gint)v;
		parameters.to = (gint)expressions.pop_num_calc();
	} else {
		parameters.count = (gint)v;
		if (v >= 0) {
			parameters.from = parameters.dot;
			parameters.to = editor_msg(SCI_GETLENGTH);
		} else {
			parameters.from = 0;
			parameters.to = parameters.dot;
		}
	}
}

void
StateSearch::process(const gchar *str, gint new_chars __attribute__((unused)))
{
	static const gint flags = G_REGEX_CASELESS | G_REGEX_MULTILINE |
				  G_REGEX_DOTALL | G_REGEX_RAW;

	QRegister *search_reg = qregisters["_"];
	GRegex *re;
	GMatchInfo *info;
	const gchar *buffer;

	gint matched_from = -1, matched_to = -1;

	undo.push_var<gint64>(search_reg->integer);

	re = g_regex_new(str, (GRegexCompileFlags)flags,
			 (GRegexMatchFlags)0, NULL);
	if (!re) {
		search_reg->integer = FAILURE;
		return;
	}

	buffer = (const gchar *)editor_msg(SCI_GETCHARACTERPOINTER);
	g_regex_match_full(re, buffer, (gssize)parameters.to, parameters.from,
			   (GRegexMatchFlags)0, &info, NULL);

	if (parameters.count >= 0) {
		gint count = parameters.count;

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
		gint count = -parameters.count;
		Range *matched = new Range[count];
		gint matched_total = 0, i = 0;

		while (g_match_info_matches(info)) {
			g_match_info_fetch_pos(info, 0,
					       &matched[i].from,
					       &matched[i].to);

			g_match_info_next(info, NULL);
			i = ++matched_total % count;
		}

		if (matched_total >= count) {
			/* successful, i points to stack bottom */
			matched_from = matched[i].from;
			matched_to = matched[i].to;
		}

		delete matched;
	}

	g_match_info_free(info);

	if (matched_from >= 0 && matched_to >= 0) {
		/* match success */
		search_reg->integer = SUCCESS;

		undo.push_msg(SCI_GOTOPOS, editor_msg(SCI_GETCURRENTPOS));
		editor_msg(SCI_SETSEL, matched_from, matched_to);
	} else {
		search_reg->integer = FAILURE;
	}

	g_regex_unref(re);
}

State *
StateSearch::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	QRegister *search_reg = qregisters["_"];

	if (*str) {
		undo.push_var<gint>(search_reg->dot);
		undo.push_msg(SCI_UNDO);
		search_reg->set_string(str);
	} else {
		gchar *search_str = search_reg->get_string();
		process(search_str, 0);
		g_free(search_str);
	}

	return &States::start;
}
