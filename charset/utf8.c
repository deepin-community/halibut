/*
 * utf8.c - routines to handle UTF-8.
 */

#ifndef ENUM_CHARSETS

#include "charset.h"
#include "internal.h"

/*
 * The internal read_utf8 and write_utf8 functions in this module
 * are not static, because they're also called internally from
 * iso2022.c.
 */

/*
 * UTF-8 has no associated data, so `charset' may be ignored.
 */

void read_utf8(charset_spec const *charset, long int input_chr,
	       charset_state *state,
	       void (*emit)(void *ctx, long int output), void *emitctx)
{
    UNUSEDARG(charset);

    /*
     * For reading UTF-8, the `state' word contains the character
     * being accumulated.  This is shifted left by six bits each
     * time a character is added, and there's a single '1' bit
     * in what would be bit 31 of the final character, which we
     * use to detect when it's complete.
     * 
     * As required, the state is zero when we are not in the middle
     * of a multibyte character at all.
     * 
     * For example, when reading E9 8D 8B, starting at state=0:
     * 
     *  - after E9, the state is 0x00080009
     *  - after 8D, the state is 0x0200024d
     *  - after 8B, the state conceptually becomes 0x8000934b, at
     *    which point we notice we've got as many characters as we
     *    were expecting, output U+934B, and reset the state to
     *    zero.
     *
     * If we detect an overlong sequence, we shift the marker bit
     * right one bit.  This is safe because an overlong sequence
     * can't encode a top-bit-set character.  Not that we worry
     * about what overlong sequences are trying to encode, but
     * it's nice to know that we could if we wanted to.
     *
     * Note that the maximum number of bits we might need to store
     * in the character value field is 25 (U+7FFFFFFF contains 31
     * bits, but we will never actually store its full value
     * because when we receive the last 6 bits in the final
     * continuation byte we will output it and revert the state to
     * zero). Hence we need 26 bits in total.
     */

    if (input_chr < 0x80) {
	/*
	 * Single-byte character. If the state is nonzero before
	 * coming here, output an error for an incomplete sequence.
	 * Then output the character.
	 */
	if (state->s0 != 0) {
	    emit(emitctx, ERROR);
	    state->s0 = 0;
	}
	emit(emitctx, input_chr);
    } else if (input_chr == 0xFE || input_chr == 0xFF) {
	/*
	 * FE and FF bytes should _never_ occur in UTF-8. They are
	 * automatic errors; if the state was nonzero to start
	 * with, output a further error for an incomplete sequence.
	 */
	if (state->s0 != 0) {
	    emit(emitctx, ERROR);
	    state->s0 = 0;
	}
	emit(emitctx, ERROR);
    } else if (input_chr >= 0x80 && input_chr < 0xC0) {
	/*
	 * Continuation byte. Output an error for an unexpected
	 * continuation byte, if the state is zero.
	 */
	if (state->s0 == 0) {
	    emit(emitctx, ERROR);
	} else {
	    unsigned long charval;

	    /*
	     * Otherwise, accumulate more of the character value.
	     */
	    charval = state->s0;
	    charval = (charval << 6) | (input_chr & 0x3F);

	    /*
	     * Detect overlong encodings.  We're looking for too many
	     * leading zeroes given our position in the character.  If
	     * we find an overlong encoding, clear the current marker
	     * bit and set the bit below it.  Overlong two-byte
	     * encodings are a special case, and are detected when we
	     * read their inital byte.
	     */
	    if ((charval & 0xffffffe0L) == 0x02000000L)
		charval ^= 0x03000000L;
	    else if ((charval & 0xfffffff0L) == 0x00080000L)
		charval ^= 0x000c0000L;
	    else if ((charval & 0xfffffff8L) == 0x00002000L)
		charval ^= 0x00003000L;
	    else if ((charval & 0xfffffffcL) == 0x00000080L)
		charval ^= 0x000000c0L;

	    /*
	     * Check the byte counts; if we have not reached the
	     * end of the character, update the state and return.
	     */
	    if (!(charval & 0xc0000000L)) {
		state->s0 = charval;
		return;
	    }

	    /*
	     * Clear the marker bit, or set it if it's clear,
	     * indicating an overlong sequence.
	     */
	    charval ^= 0x80000000L;

	    /*
	     * Now we know we've reached the end of the character.
	     * `charval' is the Unicode value. We should check for
	     * various invalid things, and then either output
	     * charval or an error. In all cases we reset the state
	     * to zero.
	     */
	    state->s0 = 0;

	    if (charval & 0x80000000L) {
		/* We got an overlong sequence. */
		emit(emitctx, ERROR);
	    } else if (charval >= 0xD800 && charval < 0xE000) {
		/*
		 * Surrogates (0xD800-0xDFFF) may never be encoded
		 * in UTF-8. A surrogate pair in Unicode should
		 * have been encoded as a single UTF-8 character
		 * occupying more than three bytes.
		 */
		emit(emitctx, ERROR);
	    } else if (charval == 0xFFFE || charval == 0xFFFF) {
		/*
		 * U+FFFE and U+FFFF are invalid Unicode characters
		 * and may never be encoded in UTF-8. (This is one
		 * reason why U+FFFF is our way of signalling an
		 * error to our `emit' function :-)
		 */
		emit(emitctx, ERROR);
	    } else {
		/*
		 * Oh, all right. We'll let this one off.
		 */
		emit(emitctx, charval);
	    }
	}

    } else {
	/*
	 * Lead byte. First output an error for an incomplete
	 * sequence, if the state is nonzero.
	 */
	if (state->s0 != 0)
	    emit(emitctx, ERROR);

	/*
	 * Now deal with the lead byte: work out the number of
	 * bytes we expect to see in this character, and extract
	 * the initial bits of it too.
	 */
	if (input_chr >= 0xC0 && input_chr < 0xC2) {
	    /* beginning of an overlong two-byte sequence */
	    state->s0 = 0x01000000L | (input_chr & 0x1F);
	} else if (input_chr >= 0xC2 && input_chr < 0xE0) {
	    state->s0 = 0x02000000L | (input_chr & 0x1F);
	} else if (input_chr >= 0xE0 && input_chr < 0xF0) {
	    state->s0 = 0x00080000L | (input_chr & 0x0F);
	} else if (input_chr >= 0xF0 && input_chr < 0xF8) {
	    state->s0 = 0x00002000L | (input_chr & 0x07);
	} else if (input_chr >= 0xF8 && input_chr < 0xFC) {
	    state->s0 = 0x00000080L | (input_chr & 0x03);
	} else if (input_chr >= 0xFC && input_chr < 0xFE) {
	    state->s0 = 0x00000002L | (input_chr & 0x01);
	}
    }
}

/*
 * UTF-8 is a stateless multi-byte encoding (in the sense that just
 * after any character has been completed, the state is always the
 * same); hence when writing it, there is no need to use the
 * charset_state.
 */

bool write_utf8(charset_spec const *charset, long int input_chr,
                charset_state *state,
                void (*emit)(void *ctx, long int output),
                void *emitctx)
{
    UNUSEDARG(charset);
    UNUSEDARG(state);

    if (input_chr == -1)
	return true;		       /* stateless; no cleanup required */

    /*
     * Refuse to output any illegal code points.
     */
    if (input_chr == 0xFFFE || input_chr == 0xFFFF ||
	(input_chr >= 0xD800 && input_chr < 0xE000)) {
	return false;
    } else if (input_chr < 0x80) {     /* one-byte character */
	emit(emitctx, input_chr);
	return true;
    } else if (input_chr < 0x800) {    /* two-byte character */
	emit(emitctx, 0xC0 | (0x1F & (input_chr >>  6)));
	emit(emitctx, 0x80 | (0x3F & (input_chr      )));
	return true;
    } else if (input_chr < 0x10000) {  /* three-byte character */
	emit(emitctx, 0xE0 | (0x0F & (input_chr >> 12)));
	emit(emitctx, 0x80 | (0x3F & (input_chr >>  6)));
	emit(emitctx, 0x80 | (0x3F & (input_chr      )));
	return true;
    } else if (input_chr < 0x200000) { /* four-byte character */
	emit(emitctx, 0xF0 | (0x07 & (input_chr >> 18)));
	emit(emitctx, 0x80 | (0x3F & (input_chr >> 12)));
	emit(emitctx, 0x80 | (0x3F & (input_chr >>  6)));
	emit(emitctx, 0x80 | (0x3F & (input_chr      )));
	return true;
    } else if (input_chr < 0x4000000) {/* five-byte character */
	emit(emitctx, 0xF8 | (0x03 & (input_chr >> 24)));
	emit(emitctx, 0x80 | (0x3F & (input_chr >> 18)));
	emit(emitctx, 0x80 | (0x3F & (input_chr >> 12)));
	emit(emitctx, 0x80 | (0x3F & (input_chr >>  6)));
	emit(emitctx, 0x80 | (0x3F & (input_chr      )));
	return true;
    } else {			       /* six-byte character */
	emit(emitctx, 0xFC | (0x01 & (input_chr >> 30)));
	emit(emitctx, 0x80 | (0x3F & (input_chr >> 24)));
	emit(emitctx, 0x80 | (0x3F & (input_chr >> 18)));
	emit(emitctx, 0x80 | (0x3F & (input_chr >> 12)));
	emit(emitctx, 0x80 | (0x3F & (input_chr >>  6)));
	emit(emitctx, 0x80 | (0x3F & (input_chr      )));
	return true;
    }
}

#ifdef TESTMODE

#include <stdio.h>
#include <stdarg.h>

int total_errs = 0;

void utf8_emit(void *ctx, long output)
{
    wchar_t **p = (wchar_t **)ctx;
    *(*p)++ = output;
}

void utf8_read_test(int line, char *input, int inlen, ...)
{
    va_list ap;
    wchar_t *p, str[512];
    int i;
    charset_state state;
    unsigned long l;

    state.s0 = 0;
    p = str;

    for (i = 0; i < inlen; i++)
	read_utf8(NULL, input[i] & 0xFF, &state, utf8_emit, &p);

    va_start(ap, inlen);
    l = 0;
    for (i = 0; i < p - str; i++) {
	l = va_arg(ap, long int);
	if (l == -1) {
	    printf("%d: correct string shorter than output\n", line);
	    total_errs++;
	    break;
	}
	if (l != str[i]) {
	    printf("%d: char %d came out as %08x, should be %08x\n",
                   line, i, str[i], (unsigned)l);
	    total_errs++;
	}
    }
    if (l != -1) {
	l = va_arg(ap, long int);
	if (l != -1) {
	    printf("%d: correct string longer than output\n", line);
	    total_errs++;
	}
    }
    va_end(ap);
}

void utf8_write_test(int line, const long *input, int inlen, ...)
{
    va_list ap;
    wchar_t *p, str[512];
    int i;
    charset_state state;
    unsigned long l;

    state.s0 = 0;
    p = str;

    for (i = 0; i < inlen; i++) {
	if (!write_utf8(NULL, input[i], &state, utf8_emit, &p))
            utf8_emit(&p, ERROR);
    }

    va_start(ap, inlen);
    l = 0;
    for (i = 0; i < p - str; i++) {
	l = va_arg(ap, long int);
	if (l == -1) {
	    printf("%d: correct string shorter than output\n", line);
	    total_errs++;
	    break;
	}
	if (l != str[i]) {
	    printf("%d: char %d came out as %08x, should be %08x\n",
		    line, i, str[i], (unsigned)l);
	    total_errs++;
	}
    }
    if (l != -1) {
	l = va_arg(ap, long int);
	if (l != -1) {
	    printf("%d: correct string longer than output\n", line);
	    total_errs++;
	}
    }
    va_end(ap);
}

/* Macro to concoct the first three parameters of utf8_read_test. */
#define TESTSTR(x) __LINE__, x, lenof(x)

int main(void)
{
    printf("read tests beginning\n");
    utf8_read_test(TESTSTR("\xCE\xBA\xE1\xBD\xB9\xCF\x83\xCE\xBC\xCE\xB5"),
		   0x000003BAL, /* GREEK SMALL LETTER KAPPA */
		   0x00001F79L, /* GREEK SMALL LETTER OMICRON WITH OXIA */
		   0x000003C3L, /* GREEK SMALL LETTER SIGMA */
		   0x000003BCL, /* GREEK SMALL LETTER MU */
		   0x000003B5L, /* GREEK SMALL LETTER EPSILON */
		   0L, -1L);
    utf8_read_test(TESTSTR("\x00"),
		   0x00000000L, /* <control> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xC2\x80"),
		   0x00000080L, /* <control> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xE0\xA0\x80"),
		   0x00000800L, /* <no name available> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF0\x90\x80\x80"),
		   0x00010000L, /* <no name available> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF8\x88\x80\x80\x80"),
		   0x00200000L, /* <no name available> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFC\x84\x80\x80\x80\x80"),
		   0x04000000L, /* <no name available> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\x7F"),
		   0x0000007FL, /* <control> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xDF\xBF"),
		   0x000007FFL, /* <no name available> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xEF\xBF\xBD"),
		   0x0000FFFDL, /* REPLACEMENT CHARACTER */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xEF\xBF\xBF"),
		   ERROR,      /* <no name available> (invalid char) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF7\xBF\xBF\xBF"),
		   0x001FFFFFL, /* <no name available> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFB\xBF\xBF\xBF\xBF"),
		   0x03FFFFFFL, /* <no name available> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFD\xBF\xBF\xBF\xBF\xBF"),
		   0x7FFFFFFFL, /* <no name available> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\x9F\xBF"),
		   0x0000D7FFL, /* <no name available> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xEE\x80\x80"),
		   0x0000E000L, /* <Private Use, First> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xEF\xBF\xBD"),
		   0x0000FFFDL, /* REPLACEMENT CHARACTER */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF4\x8F\xBF\xBF"),
		   0x0010FFFFL, /* <no name available> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF4\x90\x80\x80"),
		   0x00110000L, /* <no name available> */
		   0L, -1L);
    utf8_read_test(TESTSTR("\x80"),
		   ERROR,      /* (unexpected continuation byte) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xBF"),
		   ERROR,      /* (unexpected continuation byte) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\x80\xBF"),
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\x80\xBF\x80"),
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\x80\xBF\x80\xBF"),
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\x80\xBF\x80\xBF\x80"),
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\x80\xBF\x80\xBF\x80\xBF"),
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\x80\xBF\x80\xBF\x80\xBF\x80"),
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8A\x8B\x8C\x8D\x8E\x8F\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9A\x9B\x9C\x9D\x9E\x9F\xA0\xA1\xA2\xA3\xA4\xA5\xA6\xA7\xA8\xA9\xAA\xAB\xAC\xAD\xAE\xAF\xB0\xB1\xB2\xB3\xB4\xB5\xB6\xB7\xB8\xB9\xBA\xBB\xBC\xBD\xBE\xBF"),
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   ERROR,      /* (unexpected continuation byte) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xC0\x20\xC1\x20\xC2\x20\xC3\x20\xC4\x20\xC5\x20\xC6\x20\xC7\x20"),
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xE0\x20\xE1\x20\xE2\x20\xE3\x20\xE4\x20\xE5\x20\xE6\x20\xE7\x20\xE8\x20\xE9\x20\xEA\x20\xEB\x20\xEC\x20\xED\x20\xEE\x20\xEF\x20"),
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF0\x20\xF1\x20\xF2\x20\xF3\x20\xF4\x20\xF5\x20\xF6\x20\xF7\x20"),
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF8\x20\xF9\x20\xFA\x20\xFB\x20"),
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFC\x20\xFD\x20"),
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   ERROR,      /* (incomplete sequence) */
		   0x00000020L, /* SPACE */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xC0"),
		   ERROR,      /* (incomplete sequence) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xE0\x80"),
		   ERROR,      /* (incomplete sequence) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF0\x80\x80"),
		   ERROR,      /* (incomplete sequence) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF8\x80\x80\x80"),
		   ERROR,      /* (incomplete sequence) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFC\x80\x80\x80\x80"),
		   ERROR,      /* (incomplete sequence) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xDF"),
		   ERROR,      /* (incomplete sequence) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xEF\xBF"),
		   ERROR,      /* (incomplete sequence) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF7\xBF\xBF"),
		   ERROR,      /* (incomplete sequence) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFB\xBF\xBF\xBF"),
		   ERROR,      /* (incomplete sequence) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFD\xBF\xBF\xBF\xBF"),
		   ERROR,      /* (incomplete sequence) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xC0\xE0\x80\xF0\x80\x80\xF8\x80\x80\x80\xFC\x80\x80\x80\x80\xDF\xEF\xBF\xF7\xBF\xBF\xFB\xBF\xBF\xBF\xFD\xBF\xBF\xBF\xBF"),
		   ERROR,      /* (incomplete sequence) */
		   ERROR,      /* (incomplete sequence) */
		   ERROR,      /* (incomplete sequence) */
		   ERROR,      /* (incomplete sequence) */
		   ERROR,      /* (incomplete sequence) */
		   ERROR,      /* (incomplete sequence) */
		   ERROR,      /* (incomplete sequence) */
		   ERROR,      /* (incomplete sequence) */
		   ERROR,      /* (incomplete sequence) */
		   ERROR,      /* (incomplete sequence) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFE"),
		   ERROR,      /* (invalid UTF-8 byte) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFF"),
		   ERROR,      /* (invalid UTF-8 byte) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFE\xFE\xFF\xFF"),
		   ERROR,      /* (invalid UTF-8 byte) */
		   ERROR,      /* (invalid UTF-8 byte) */
		   ERROR,      /* (invalid UTF-8 byte) */
		   ERROR,      /* (invalid UTF-8 byte) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xC0\xAF"),
		   ERROR,      /* SOLIDUS (overlong form of 2F) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xE0\x80\xAF"),
		   ERROR,      /* SOLIDUS (overlong form of 2F) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF0\x80\x80\xAF"),
		   ERROR,      /* SOLIDUS (overlong form of 2F) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF8\x80\x80\x80\xAF"),
		   ERROR,      /* SOLIDUS (overlong form of 2F) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFC\x80\x80\x80\x80\xAF"),
		   ERROR,      /* SOLIDUS (overlong form of 2F) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xC1\xBF"),
		   ERROR,      /* <control> (overlong form of 7F) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xE0\x9F\xBF"),
		   ERROR,      /* <no name available> (overlong form of DF BF) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF0\x8F\xBF\xBF"),
		   ERROR,      /* <no name available> (overlong form of EF BF BF) (invalid char) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF8\x87\xBF\xBF\xBF"),
		   ERROR,      /* <no name available> (overlong form of F7 BF BF BF) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFC\x83\xBF\xBF\xBF\xBF"),
		   ERROR,      /* <no name available> (overlong form of FB BF BF BF BF) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xC0\x80"),
		   ERROR,      /* <control> (overlong form of 00) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xE0\x80\x80"),
		   ERROR,      /* <control> (overlong form of 00) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF0\x80\x80\x80"),
		   ERROR,      /* <control> (overlong form of 00) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xF8\x80\x80\x80\x80"),
		   ERROR,      /* <control> (overlong form of 00) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xFC\x80\x80\x80\x80\x80"),
		   ERROR,      /* <control> (overlong form of 00) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xA0\x80"),
		   ERROR,      /* <Non Private Use High Surrogate, First> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xAD\xBF"),
		   ERROR,      /* <Non Private Use High Surrogate, Last> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xAE\x80"),
		   ERROR,      /* <Private Use High Surrogate, First> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xAF\xBF"),
		   ERROR,      /* <Private Use High Surrogate, Last> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xB0\x80"),
		   ERROR,      /* <Low Surrogate, First> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xBE\x80"),
		   ERROR,      /* <no name available> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xBF\xBF"),
		   ERROR,      /* <Low Surrogate, Last> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xA0\x80\xED\xB0\x80"),
		   ERROR,      /* <Non Private Use High Surrogate, First> (surrogate) */
		   ERROR,      /* <Low Surrogate, First> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xA0\x80\xED\xBF\xBF"),
		   ERROR,      /* <Non Private Use High Surrogate, First> (surrogate) */
		   ERROR,      /* <Low Surrogate, Last> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xAD\xBF\xED\xB0\x80"),
		   ERROR,      /* <Non Private Use High Surrogate, Last> (surrogate) */
		   ERROR,      /* <Low Surrogate, First> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xAD\xBF\xED\xBF\xBF"),
		   ERROR,      /* <Non Private Use High Surrogate, Last> (surrogate) */
		   ERROR,      /* <Low Surrogate, Last> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xAE\x80\xED\xB0\x80"),
		   ERROR,      /* <Private Use High Surrogate, First> (surrogate) */
		   ERROR,      /* <Low Surrogate, First> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xAE\x80\xED\xBF\xBF"),
		   ERROR,      /* <Private Use High Surrogate, First> (surrogate) */
		   ERROR,      /* <Low Surrogate, Last> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xAF\xBF\xED\xB0\x80"),
		   ERROR,      /* <Private Use High Surrogate, Last> (surrogate) */
		   ERROR,      /* <Low Surrogate, First> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xED\xAF\xBF\xED\xBF\xBF"),
		   ERROR,      /* <Private Use High Surrogate, Last> (surrogate) */
		   ERROR,      /* <Low Surrogate, Last> (surrogate) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xEF\xBF\xBE"),
		   ERROR,      /* <no name available> (invalid char) */
		   0L, -1L);
    utf8_read_test(TESTSTR("\xEF\xBF\xBF"),
		   ERROR,      /* <no name available> (invalid char) */
		   0L, -1L);
    printf("read tests completed\n");
    printf("write tests beginning\n");
    {
	const static long str[] =
	{0x03BAL, 0x1F79L, 0x03C3L, 0x03BCL, 0x03B5L, 0};
	utf8_write_test(TESTSTR(str),
			0xCEL, 0xBAL,
			0xE1L, 0xBDL, 0xB9L,
			0xCFL, 0x83L,
			0xCEL, 0xBCL,
			0xCEL, 0xB5L,
			0L, -1L);
    }
    {
	const static long str[] = {0x0000L, 0};
	utf8_write_test(TESTSTR(str),
			0x00L,
			0L, -1L);
    }
    {
	const static long str[] = {0x0080L, 0};
	utf8_write_test(TESTSTR(str),
			0xC2L, 0x80L,
			0L, -1L);
    }
    {
	const static long str[] = {0x0800L, 0};
	utf8_write_test(TESTSTR(str),
			0xE0L, 0xA0L, 0x80L,
			0L, -1L);
    }
    {
	const static long str[] = {0x00010000L, 0};
	utf8_write_test(TESTSTR(str),
			0xF0L, 0x90L, 0x80L, 0x80L,
			0L, -1L);
    }
    {
	const static long str[] = {0x00200000L, 0};
	utf8_write_test(TESTSTR(str),
			0xF8L, 0x88L, 0x80L, 0x80L, 0x80L,
			0L, -1L);
    }
    {
	const static long str[] = {0x04000000L, 0};
	utf8_write_test(TESTSTR(str),
			0xFCL, 0x84L, 0x80L, 0x80L, 0x80L, 0x80L,
			0L, -1L);
    }
    {
	const static long str[] = {0x007FL, 0};
	utf8_write_test(TESTSTR(str),
			0x7FL,
			0L, -1L);
    }
    {
	const static long str[] = {0x07FFL, 0};
	utf8_write_test(TESTSTR(str),
			0xDFL, 0xBFL,
			0L, -1L);
    }
    {
	const static long str[] = {0xFFFDL, 0};
	utf8_write_test(TESTSTR(str),
			0xEFL, 0xBFL, 0xBDL,
			0L, -1L);
    }
    {
	const static long str[] = {0xFFFFL, 0};
	utf8_write_test(TESTSTR(str),
			ERROR,
			0L, -1L);
    }
    {
	const static long str[] = {0x001FFFFFL, 0};
	utf8_write_test(TESTSTR(str),
			0xF7L, 0xBFL, 0xBFL, 0xBFL,
			0L, -1L);
    }
    {
	const static long str[] = {0x03FFFFFFL, 0};
	utf8_write_test(TESTSTR(str),
			0xFBL, 0xBFL, 0xBFL, 0xBFL, 0xBFL,
			0L, -1L);
    }
    {
	const static long str[] = {0x7FFFFFFFL, 0};
	utf8_write_test(TESTSTR(str),
			0xFDL, 0xBFL, 0xBFL, 0xBFL, 0xBFL, 0xBFL,
			0L, -1L);
    }
    {
	const static long str[] = {0xD7FFL, 0};
	utf8_write_test(TESTSTR(str),
			0xEDL, 0x9FL, 0xBFL,
			0L, -1L);
    }
    {
	const static long str[] = {0xD800L, 0};
	utf8_write_test(TESTSTR(str),
			ERROR,
			0L, -1L);
    }
    {
	const static long str[] = {0xD800L, 0xDC00L, 0};
	utf8_write_test(TESTSTR(str),
			ERROR,
			ERROR,
			0L, -1L);
    }
    {
	const static long str[] = {0xDFFFL, 0};
	utf8_write_test(TESTSTR(str),
			ERROR,
			0L, -1L);
    }
    {
	const static long str[] = {0xE000L, 0};
	utf8_write_test(TESTSTR(str),
			0xEEL, 0x80L, 0x80L,
			0L, -1L);
    }
    printf("write tests completed\n");

    printf("total: %d errors\n", total_errs);
    return (total_errs != 0);
}
#endif /* TESTMODE */

const charset_spec charset_CS_UTF8 = {
    CS_UTF8, read_utf8, write_utf8, NULL
};

#else /* ENUM_CHARSETS */

ENUM_CHARSET(CS_UTF8)

#endif /* ENUM_CHARSETS */
