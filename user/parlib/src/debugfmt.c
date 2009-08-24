#include <arch/types.h>
#include <ros/error.h>
#include <debug.h>

/*
 * Print a number (base <= 16) in reverse order,
 * using specified putch function and associated pointer putdat.
 */
#ifdef __DEPUTY__
static void printnum(void (*putch)(int, TV(t)), TV(t) putdat,
	                 unsigned long long num, unsigned base, int width, int padc)
#else
static void printnum(void (*putch)(int, void**), void **putdat,
	                 unsigned long long num, unsigned base, int width, int padc)
#endif
{
	// first recursively print all preceding (more significant) digits
	if (num >= base) {
		printnum(putch, putdat, num / base, base, width - 1, padc);
	} else {
		// print any needed pad characters before first digit
		while (--width > 0)
			putch(padc, putdat);
	}

	// then print this (the least significant) digit
	putch("0123456789abcdef"[num % base], putdat);
}

// Get an unsigned int of various possible sizes from a varargs list,
// depending on the lflag parameter.
static unsigned long long getuint(va_list *ap, int lflag)
{
	if (lflag >= 2)
		return va_arg(*ap, unsigned long long);
	else if (lflag)
		return va_arg(*ap, unsigned long);
	else
		return va_arg(*ap, unsigned int);
}

// Same as getuint but signed - can't use getuint
// because of sign extension
static long long getint(va_list *ap, int lflag)
{
	if (lflag >= 2)
		return va_arg(*ap, long long);
	else if (lflag)
		return va_arg(*ap, long);
	else
		return va_arg(*ap, int);
}


// Main function to format and print a string.
#ifdef __DEPUTY__
void debugfmt(void (*putch)(int, TV(t)), TV(t) putdat, const char *fmt, ...);
void vdebugfmt(void (*putch)(int, TV(t)), TV(t) putdat, const char *fmt, va_list ap)
#else
void debugfmt(void (*putch)(int, void**), void **putdat, const char *fmt, ...);
void vdebugfmt(void (*putch)(int, void**), void **putdat, const char *fmt, va_list ap)
#endif
{
	register const char *NTS p;
	const char *NTS last_fmt;
	register int ch, err;
	unsigned long long num;
	int base, lflag, width, precision, altflag;
	char padc;

	while (1) {
		while ((ch = *(unsigned char *) fmt) != '%') {
			if (ch == '\0')
				return;
			fmt++;
			putch(ch, putdat);
		}
		fmt++;

		// Process a %-escape sequence
		last_fmt = fmt;
		padc = ' ';
		width = -1;
		precision = -1;
		lflag = 0;
		altflag = 0;
	reswitch:
		switch (ch = *(unsigned char *) fmt++) {

		// flag to pad on the right
		case '-':
			padc = '-';
			goto reswitch;
			
		// flag to pad with 0's instead of spaces
		case '0':
			padc = '0';
			goto reswitch;

		// width field
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			for (precision = 0; ; ++fmt) {
				precision = precision * 10 + ch - '0';
				ch = *fmt;
				if (ch < '0' || ch > '9')
					break;
			}
			goto process_precision;

		case '*':
			precision = va_arg(ap, int);
			goto process_precision;

		case '.':
			if (width < 0)
				width = 0;
			goto reswitch;

		case '#':
			altflag = 1;
			goto reswitch;

		process_precision:
			if (width < 0)
				width = precision, precision = -1;
			goto reswitch;

		// long flag (doubled for long long)
		case 'l':
			lflag++;
			goto reswitch;

		// character
		case 'c':
			putch(va_arg(ap, int), putdat);
			break;

		// error message
		case 'e':
			err = va_arg(ap, int);
			if (err < 0)
				err = -err;
			if (err >= NUMERRORS)
				debugfmt(putch, putdat, "error %d", err);
			else
				debugfmt(putch, putdat, "%s", error_string[err]);
			break;

		// string
		case 's':
			if ((p = va_arg(ap, char *NT)) == NULL)
				p = "(null)";
			if (width > 0 && padc != '-')
				for (width -= strnlen(p, precision); width > 0; width--)
					putch(padc, putdat);
			for (; (ch = *p) != '\0' && (precision < 0 || --precision >= 0); width--) {
				if (altflag && (ch < ' ' || ch > '~'))
					putch('?', putdat);
				else
					putch(ch, putdat);
				// zra: make sure *p isn't '\0' before inc'ing
				p++;
			}
			for (; width > 0; width--)
				putch(' ', putdat);
			break;

		// (signed) decimal
		case 'd':
			num = getint(&ap, lflag);
			if ((long long) num < 0) {
				putch('-', putdat);
				num = -(long long) num;
			}
			base = 10;
			goto number;

		// unsigned decimal
		case 'u':
			num = getuint(&ap, lflag);
			base = 10;
			goto number;

		// (unsigned) octal
		case 'o':
			// should do something with padding so it's always 3 octits
			num = getuint(&ap, lflag);
			base = 8;
			goto number;

		// pointer
		case 'p':
			putch('0', putdat);
			putch('x', putdat);
			num = (unsigned long long)
				(uintptr_t) va_arg(ap, void *);
			base = 16;
			goto number;

		// (unsigned) hexadecimal
		case 'x':
			num = getuint(&ap, lflag);
			base = 16;
		number:
			printnum(putch, putdat, num, base, width, padc);
			break;

		// escaped '%' character
		case '%':
			putch(ch, putdat);
			break;
			
		// unrecognized escape sequence - just print it literally
		default:
			putch('%', putdat);
			fmt = last_fmt;
			//for (fmt--; fmt[-1] != '%'; fmt--)
				/* do nothing */;
			break;
		}
	}
}

#ifdef __DEPUTY__
void debugfmt(void (*putch)(int, TV(t)), TV(t) putdat, const char *fmt, ...)
#else
void debugfmt(void (*putch)(int, void**), void **putdat, const char *fmt, ...)
#endif
{
	va_list ap;

	va_start(ap, fmt);
	vdebugfmt(putch, putdat, fmt, ap);
	va_end(ap);
}
