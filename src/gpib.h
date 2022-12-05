#include <stddef.h>

int gpib_open(const char *name);
int gpib_close(int dev);
int gpib_read(int dev, char *buf, size_t buf_length);
int gpib_write(int dev, const char *str);
int gpib_print(int dev, const char *format, ...);
void gpib_print_error(int dev);
