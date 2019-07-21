CFLAGS=-O2 -fPIC -g3 -std=c99
LDFLAGS=-shared

all :
	if ! [ -d "bin" ]; then mkdir "bin"; fi
	gcc $(CFLAGS) -c -o bin/beefmote.o src/beefmote.c
	gcc $(LDFLAGS) $(CFLAGS) -o bin/beefmote.so bin/beefmote.o

install :   
	cp bin/beefmote.so ~/.local/lib64/deadbeef/
                
clean :
	rm bin/beefmote.so bin/*.o
