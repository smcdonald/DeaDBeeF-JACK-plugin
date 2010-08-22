all:
	gcc -I/usr/local/include  -std=c99 -shared -O2 -o jack.so -ljack jack.c -fPIC
