#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <gpib/ib.h>
#include "gpib.h"

int gpib_open(const char *name)
{
	int r;

	r = ibfind(name);
	if (r == -1)
	{
		fprintf(stderr, "# E: unable to open gpib (ibsta = %d, iberr = %d)\n", ibsta, iberr);
	}

	return r;
}

int gpib_close(int dev)
{
	int r;
	int ret = 0;

	r = ibclr(dev);
	if (r & 0x8000)
	{
		ret = -1;
		fprintf(stderr, "# E: unable to clr gpib (ibsta = %d, iberr = %d)\n", ibsta, iberr);
	}

	r = gpib_write(dev, "*cls");
	if (r == -1)
	{
		ret = r;
	}

	sleep(1);

	r = ibloc(dev);
	if (r & 0x8000)
	{
		ret = -1;
		fprintf(stderr, "# E: unable to loc gpib (ibsta = %d, iberr = %d)\n", ibsta, iberr);
	}

	return ret;
}

int gpib_read(int dev, char *buf, size_t buf_length)
{
	int r;
	int ret = 0;

	r = ibrd(dev, buf, buf_length);
	if (r & 0x8000)
	{
		ret = -1;
		fprintf(stderr, "# E: unable to write to gpib (ibsta = %d, iberr = %d)\n", ibsta, iberr);
	}
	else
	{
		ret = ibcnt;
		if (ibcnt < (int)buf_length)
		{
			buf[ibcnt] = 0;
		}
	}

	return ret;
}

int gpib_write(int dev, const char *str)
{
	int r;
	int ret = 0;

	r = ibwrt(dev, str, strlen(str));
	if (r & 0x8000)
	{
		ret = -1;
		fprintf(stderr, "# E: unable to write to gpib (ibsta = %d, iberr = %d)\n", ibsta, iberr);
	}
	else
	{
		ret = ibcnt;
	}

	return ret;
}

int gpib_print(int dev, const char *format, ...)
{
	int r;
	va_list args;
	char buf[100];
	const size_t bufsize = 100;

	va_start(args, format);
	r = vsnprintf(buf, bufsize, format, args);
	if (r < 0)
	{
		fprintf(stderr, "# E: unable to printf to buffer (%s)\n", strerror(errno));
		goto gpib_print_vsnprintf;
	}
	r = gpib_write(dev, buf);
	gpib_print_vsnprintf:
	va_end(args);

	return r;
}

void gpib_print_error(int dev)
{
	char buf[100] = {0};
	gpib_write(dev, "system:error?");
	gpib_read(dev, buf, 100);
	fprintf(stderr, "# [debug] error = %s\n", buf);
}
